#include "zetta/query/executor.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

#include "zetta/api/serving.hpp"
#include "zetta/cloud/cloud_inspector.hpp"
#include "zetta/cloud/cloud_provider.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/data/table.hpp"

namespace zetta::query {
namespace {

// The virtual `system` database holds read-only system tables (à la Postgres'
// pg_catalog). Its tables are computed at query time, not stored/replicated.
//  - `tables` — the catalog of these system tables themselves (#684): one row
//    per table with its name + one-line description, sourced from
//    kSystemTableCatalog below. "Which system tables exist" as a read view
//    (RULE-7) — the same list system_table_names() exposes and the headed
//    explorer renders.
//  - `nodes` — cluster membership + replication status (Cluster::nodes()).
//    `SELECT * FROM system.nodes` (bare `zetta_nodes` still works, back-compat).
//  - `deployments` — recorded graph→target placements (Node::deployments()).
//  - `models` — the model registry (Node::models()): trained models +
//    their kind / metrics / weights fingerprint.
//  - `indexes` — the index registry (Node::indexes()): one row per registered
//    index over a table — its kind / backend / fields (ENT-39 / ENT-40).
//  - `jobs` — the compute-job registry (Node::jobs()): one row per unit of
//    native distributed accelerated compute + its lifecycle (ARC-11 / ENT-31).
//  - `cloud_providers` — the deployment-target / provider catalog
//    (CloudProviderCatalog::builtin()): pure metadata, reads nothing from any
//    cloud. Surfaces what the Cloud Inspector would touch (RULE-7).
//  - `cloud_status` — cached results of consent-gated cloud inspects
//    (Node::cloud_inspections()). Querying it NEVER triggers a cloud call: it
//    only reflects inspects the user already consented to in the Cloud Inspector
//    (empty until then). Ephemeral, not replicated.
//  - `provisioners` — cloud-burst provisioners detected from this node's own
//    local CLIs (Node::provisioner_configs()): one row per wired target with its
//    non-secret deployment context (subscription / region / …). Carries NO
//    credentials (the CLI keeps its own). Ephemeral, not replicated — it reflects
//    this node's CLI auth + defaults, and drives the headed Add-nodes control.
//  - `runs` — the run registry (Node::runs()): one row per graph execution
//    + its lifecycle / pipeline refs / lineage (FEAT-5).
//  - `principals` — the authenticated actors (Node::principals(); ENT-20).
//  - `keys` — the access keys the principals own (ENT-21/22): each key's
//    scope (resources) + capabilities (rights), no secret material.
//  - `audit` — the immutable, append-only audit trail (Node::audit_events();
//    ENT-23 / FEAT-9).
//  - `activity` — the merged, newest-first activity feed (#622): the audit trail
//    + runs composed chronologically at query time
//    (zetta::observability::activity_entries), each row with its actor, surface
//    (local / console / api), action, target, and outcome. The engine source the
//    headed Activity view renders too (RULE-7).
//  - `dataset_partitions` — the partition catalog (Node::dataset_partitions()):
//    one row per recorded dataset partition (where a slice of a table's data lives
//    — its table / blob / row range / stats / owner nodes). Replicate-to-all, so
//    every node sees the same placement (ENT-36 / TBL-13).
//  - `datasets` — datasets surfaced as a first-class primitive (Node::datasets();
//    #585). One row per dataset (a table + its data) aggregating its schema, row
//    count, on-disk size, storage format, replication strategy, and partition
//    placement across the cluster (which node owns which partition — joined from the
//    replicate-to-all partition catalog). Computed live from the tree + the catalog,
//    so it never drifts (RULE-7).
//  - `checkpoints` — burst-job eviction tolerance (Node::checkpoints()): one row per
//    job with a durable resume checkpoint in this node's LOCAL store (its kind /
//    sequence / total / progress). Ephemeral, local, NOT replicated (RULE-5 / #468).
//  - `node_metrics` — this node's live host resource utilization
//    (Node::sample_node_metrics()): CPU / memory / GPU + VRAM / storage / network,
//    sampled from OS counters at query time. Ephemeral, observed, NOT replicated
//    (like cloud_status), the engine source for the animated per-node gauges
//    (#575 area 2). A node measures only its own host, so this returns one row —
//    the node the query hits; counters a host can't report are null (the GPU
//    group is absent in a CPU-only core build).
//  - `spans` — one row per span in this node's span ring (Node::span_recorder();
//    docs/architecture/observability.md, #640): every traced operation's
//    intent / plan / performance / result shape, oldest first. Ephemeral,
//    observed, NOT replicated — the same ring the list_traces / get_trace
//    capabilities and the GUI trace pane read (RULE-7: one source).
//  - `traces` — one row per trace aggregated from the same ring: the root
//    span's name + intent, earliest start, duration, span count, and whether
//    every span succeeded. The "what happened" summary the GUI activity feed
//    renders (#622).
//  - `crashes` — one row per crash report under <data_dir>/crashes/ (#640):
//    the fatal signal, stacktrace, recent spans, version + platform a dying
//    node wrote on its way down. Read from the local files at query time
//    (the checkpoints precedent) — ephemeral, local, NOT replicated. A
//    malformed report file surfaces as a row with valid=false + the reason,
//    never silently skipped.
// Together with the GUI Runs / Admin panels these satisfy RULE-7 (observable
// state defaults to a system.<table> read view, never a GUI-only surface).
// Future: system.leadership, etc.
//
// Naming: the `system` database is reserved + zetta-controlled, so a `zetta_`
// prefix on its tables is redundant — system tables are named plainly
// (`nodes`, `dataset_partitions`). The tables originally shipped as
// `system.zetta_<table>`; those legacy spellings still resolve as back-compat
// aliases (canonical_system_table below, #538) for at least one cycle.
constexpr std::string_view kSystemDatabase = "system";
constexpr std::string_view kNodesTable = "nodes";
constexpr std::string_view kDeploymentsTable = "deployments";
constexpr std::string_view kModelsTable = "models";
constexpr std::string_view kIndexesTable = "indexes";
constexpr std::string_view kJobsTable = "jobs";
constexpr std::string_view kCloudProvidersTable = "cloud_providers";
constexpr std::string_view kCloudStatusTable = "cloud_status";
constexpr std::string_view kProvisionersTable = "provisioners";
constexpr std::string_view kConsentRequestsTable = "consent_requests";
constexpr std::string_view kRunsTable = "runs";
constexpr std::string_view kPrincipalsTable = "principals";
constexpr std::string_view kKeysTable = "keys";
constexpr std::string_view kAuditTable = "audit";
constexpr std::string_view kActivityTable = "activity";
constexpr std::string_view kPartitionsTable = "dataset_partitions";
constexpr std::string_view kDatasetsTable = "datasets";
constexpr std::string_view kCheckpointsTable = "checkpoints";
constexpr std::string_view kNodeMetricsTable = "node_metrics";
constexpr std::string_view kPipelineJobsTable = "pipeline_jobs";
constexpr std::string_view kCollectiveRoundsTable = "collective_rounds";
constexpr std::string_view kSpansTable = "spans";
constexpr std::string_view kTracesTable = "traces";
constexpr std::string_view kCrashesTable = "crashes";
constexpr std::string_view kTablesTable = "tables";

// The system-table catalog: every table the `system` database serves, with a
// one-line description, alphabetical. THE single list (#684) — the
// `system.tables` read view, the public system_table_names() accessor (which
// the headed explorer renders), and the unknown-table error's suggestions all
// derive from it, so a new system table registered here appears everywhere at
// once. Adding a table = one entry here + its dispatch branch in execute().
struct SystemTableInfo {
    std::string_view name;
    std::string_view description;
};
constexpr std::array kSystemTableCatalog = {
    SystemTableInfo{kActivityTable,
                    "the merged, newest-first activity feed: audit trail + "
                    "runs composed chronologically"},
    SystemTableInfo{kAuditTable, "the immutable, append-only audit trail"},
    SystemTableInfo{kCheckpointsTable,
                    "jobs with a durable resume checkpoint in this node's "
                    "local store (ephemeral, not replicated)"},
    SystemTableInfo{kCloudProvidersTable,
                    "the deployment-target / provider catalog: pure metadata, "
                    "reads nothing from any cloud"},
    SystemTableInfo{kCloudStatusTable,
                    "cached results of consent-gated cloud inspects; querying "
                    "never triggers a cloud call"},
    SystemTableInfo{kCollectiveRoundsTable,
                    "this node's live collective-plane counters (ephemeral, "
                    "not replicated)"},
    SystemTableInfo{kCrashesTable,
                    "crash reports found under this node's data_dir "
                    "(ephemeral, local)"},
    SystemTableInfo{kPartitionsTable,
                    "the partition catalog: where each slice of a table's "
                    "data lives"},
    SystemTableInfo{kDatasetsTable,
                    "datasets as a first-class primitive: schema, rows, size, "
                    "format, partition placement"},
    SystemTableInfo{kDeploymentsTable,
                    "recorded graph-to-target placements"},
    SystemTableInfo{kIndexesTable,
                    "the index registry: one row per registered index over a "
                    "table"},
    SystemTableInfo{kJobsTable,
                    "the compute-job registry: one row per unit of native "
                    "distributed compute + its lifecycle"},
    SystemTableInfo{kKeysTable,
                    "the access keys the principals own: scope + capabilities, "
                    "no secret material"},
    SystemTableInfo{kModelsTable,
                    "the model registry: trained models + their kind, "
                    "metrics, weights fingerprint"},
    SystemTableInfo{kNodeMetricsTable,
                    "this node's live host resource utilization, sampled at "
                    "query time"},
    SystemTableInfo{kNodesTable,
                    "cluster membership + replication status (needs a "
                    "cluster)"},
    SystemTableInfo{kPipelineJobsTable,
                    "recorded near-data compute jobs (ephemeral, not "
                    "replicated)"},
    SystemTableInfo{kPrincipalsTable, "the authenticated actors"},
    SystemTableInfo{kProvisionersTable,
                    "cloud-burst provisioners detected from this node's local "
                    "CLIs; carries no credentials"},
    SystemTableInfo{kRunsTable,
                    "the run registry: one row per graph execution + its "
                    "lifecycle and lineage"},
    SystemTableInfo{kSpansTable,
                    "one row per span in this node's span ring: every traced "
                    "operation's intent, plan, timing"},
    SystemTableInfo{kTablesTable,
                    "this catalog: every system table + what it holds"},
    SystemTableInfo{kTracesTable,
                    "one row per trace aggregated from the span ring: the "
                    "what-happened summary"},
};

// One JSON row per system table for the system.tables catalog view (#684).
std::vector<nlohmann::json> system_table_rows() {
    std::vector<nlohmann::json> rows;
    for (const SystemTableInfo& info : kSystemTableCatalog) {
        nlohmann::json row;
        row["table"] = std::string{info.name};
        row["description"] = std::string{info.description};
        rows.push_back(std::move(row));
    }
    return rows;
}

// The names alone, derived from the catalog for the public accessor.
constexpr auto kSystemTableNames = [] {
    std::array<std::string_view, kSystemTableCatalog.size()> names{};
    for (std::size_t i = 0; i < names.size(); ++i) {
        names[i] = kSystemTableCatalog[i].name;
    }
    return names;
}();

// The pre-#538 table names: `system.zetta_<table>` still resolves to
// `system.<table>` for the tables that shipped under the legacy prefix.
// Only those twelve alias — newer tables never had a prefixed spelling.
std::string canonical_system_table(const std::string& name) {
    constexpr std::string_view kLegacyPrefix = "zetta_";
    if (!name.starts_with(kLegacyPrefix)) return name;
    const std::string_view plain =
        std::string_view{name}.substr(kLegacyPrefix.size());
    for (const std::string_view renamed :
         {kNodesTable, kDeploymentsTable, kModelsTable, kIndexesTable,
          kJobsTable, kCloudProvidersTable, kCloudStatusTable,
          kProvisionersTable, kRunsTable, kPrincipalsTable, kKeysTable,
          kAuditTable}) {
        if (plain == renamed) return std::string{plain};
    }
    return name;
}

// The system-table name a TableRef refers to, or empty if it's a normal table.
std::string system_table_of(const TableRef& ref) {
    if (ref.database == kSystemDatabase) {
        return canonical_system_table(ref.table);                     // system.<t>
    }
    // The historical bare spelling of the nodes table (no qualifier). Only the
    // legacy name aliases — a bare `nodes` would shadow user tables.
    if (ref.database.empty() && ref.table == "zetta_nodes") {
        return std::string{kNodesTable};
    }
    return {};
}

}  // namespace

std::span<const std::string_view> system_table_names() {
    return kSystemTableNames;
}

namespace {

template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

QueryResult error(std::string message) {
    QueryResult r;
    r.error = std::move(message);
    return r;
}

// One JSON row per node for the nodes system table. Unknown replication
// fields (e.g. a peer that hasn't reported yet, or self's sync state) are null.
std::vector<nlohmann::json> cluster_node_rows(const zetta::cluster::Cluster& cluster) {
    std::vector<nlohmann::json> rows;
    for (const zetta::cluster::NodeStatus& node : cluster.nodes()) {
        const zetta::cluster::NodeReplication& replication = node.replication;
        nlohmann::json row;
        row["node_id"] = node.node_id;
        row["address"] = node.address;
        row["role"] = to_string(node.role);
        row["is_self"] = node.is_self;
        row["connected"] = node.connected;
        row["entries"] = replication.known ? nlohmann::json(replication.entries)
                                           : nlohmann::json();
        row["version"] = replication.known ? nlohmann::json(replication.version)
                                           : nlohmann::json();
        row["in_sync"] = (replication.known && !node.is_self)
                             ? nlohmann::json(replication.in_sync)
                             : nlohmann::json();
        row["last_sync_seconds"] =
            (replication.known && !node.is_self)
                ? nlohmann::json(replication.seconds_since_sync)
                : nlohmann::json();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per recorded deployment for the deployments system table.
// Sourced from Node::deployments() (the placements recorded by deploy_graph).
std::vector<nlohmann::json> deployment_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& deployment : node.deployments()) {
        const zetta::pipeline::DeploymentTarget& target = deployment->target();
        nlohmann::json row;
        row["deployment_id"] = deployment->id();
        row["graph_name"] = deployment->graph_name();
        row["target_kind"] = to_string(target.kind);
        row["binding"] = target.binding;
        row["credentials_ref"] = target.credentials_ref;
        row["node_id"] = deployment->node_id();
        row["node_count"] = deployment->node_count();
        row["edge_count"] = deployment->edge_count();
        row["created"] = deployment->created();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per registered model for the models system table — the
// model registry. Sourced from Node::models(). The serialized weights body is
// not in the registry (it's a large binary that moves over the body seam); its
// fingerprint (size + sha256) and presence are, so the registry shows what is
// trained and whether its weights have landed here.
std::vector<nlohmann::json> model_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& model : node.models()) {
        nlohmann::json row;
        row["model_id"] = model->id();
        row["name"] = model->name();
        row["kind"] = model->kind();
        row["project"] = model->project();
        row["run"] = model->run();
        row["feature_schema"] = model->feature_schema();
        row["metrics"] = model->metrics();
        row["created"] = model->created();
        const bool has_weights = model->has_weights();
        row["has_weights"] = has_weights;
        row["weights_size"] =
            has_weights ? nlohmann::json(model->weights()->size)
                        : nlohmann::json();
        row["weights_sha256"] =
            has_weights ? nlohmann::json(model->weights()->sha256)
                        : nlohmann::json();
        row["weights_present"] = model->has_weights_body();
        // Governed serving (#648): is the model's HTTP predict endpoint
        // published, and at which route (the flag is replicated state, so
        // every node reports the same answer).
        row["exposed"] = model->exposed();
        row["endpoint"] = model->exposed()
                              ? nlohmann::json(zetta::api::model_endpoint_path(
                                    model->id()))
                              : nlohmann::json();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per registered index for the indexes system table — the
// index registry (ENT-39 / ENT-40). Sourced from Node::indexes() (every index
// replicates, so each node sees the same set). An index is metadata only: this
// exposes its definition — the source table, the index kind (search / vector /
// feature), the backend that builds it, and the fields it indexes. `fields` rides
// as a JSON array (like model_rows' feature_schema), with `field_count` for a
// quick scalar.
std::vector<nlohmann::json> index_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& index : node.indexes()) {
        nlohmann::json row;
        row["index_id"] = index->id();
        row["table"] = index->table();
        row["kind"] = std::string(index->kind_name());
        row["backend"] = index->backend();
        row["fields"] = index->fields();
        row["field_count"] = static_cast<int>(index->fields().size());
        row["created"] = index->created();
        // Governed serving (#648): is the index's HTTP search endpoint
        // published, and at which route.
        row["exposed"] = index->exposed();
        row["endpoint"] = index->exposed()
                              ? nlohmann::json(zetta::api::index_endpoint_path(
                                    index->id()))
                              : nlohmann::json();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per compute job for the jobs system table — the compute-job
// registry (ARC-11 / ENT-31). Sourced from Node::jobs() (every job replicates, so
// each node sees the same set). Exposes the job's identity + lifecycle (id / kind
// / status / created / updated / detail) and its optional id references (project /
// graph / run / scaling_policy — the reward model). `node` is the **observing**
// node — the node whose replicated view these rows are read from; per-job
// scheduler placement (which node actually runs a job) is a later slice, so until
// then this is "which node's registry you queried", not a recorded assignment.
std::vector<nlohmann::json> compute_job_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& job : node.jobs()) {
        nlohmann::json row;
        row["job_id"] = job->id();
        row["kind"] = to_string(job->kind());
        row["status"] = to_string(job->status());
        row["project"] = job->project();
        row["graph"] = job->graph();
        row["run"] = job->run();
        row["scaling_policy"] = job->scaling_policy();
        row["created"] = job->created();
        row["updated"] = job->updated();
        row["detail"] = job->detail();
        row["node"] = node.node_id();
        rows.push_back(std::move(row));
    }
    return rows;
}

// Whether `cell` <op> `target` holds. Equality/inequality work over any JSON
// value (nlohmann compares numbers across int/float); the ordering comparisons
// apply numerically when both sides are numbers and lexicographically when both
// are strings, and otherwise do not match (mixed/unsupported types).
bool compare_cell(const nlohmann::json& cell, CompareOp op,
                  const nlohmann::json& target) {
    switch (op) {
        case CompareOp::Equal:
            return cell == target;
        case CompareOp::NotEqual:
            return cell != target;
        default:
            break;
    }
    if (cell.is_number() && target.is_number()) {
        const double left = cell.get<double>();
        const double right = target.get<double>();
        switch (op) {
            case CompareOp::Less:         return left < right;
            case CompareOp::LessEqual:    return left <= right;
            case CompareOp::Greater:      return left > right;
            case CompareOp::GreaterEqual: return left >= right;
            default:                      return false;
        }
    }
    if (cell.is_string() && target.is_string()) {
        const std::string& left = cell.get_ref<const std::string&>();
        const std::string& right = target.get_ref<const std::string&>();
        switch (op) {
            case CompareOp::Less:         return left < right;
            case CompareOp::LessEqual:    return left <= right;
            case CompareOp::Greater:      return left > right;
            case CompareOp::GreaterEqual: return left >= right;
            default:                      return false;
        }
    }
    return false;
}

// Whether a row satisfies an optional WHERE clause. A row lacking the column never
// matches (it is excluded), preserving the original equality behavior.
bool row_matches(const nlohmann::json& row,
                 const std::optional<WhereClause>& where) {
    if (!where) return true;
    if (!row.contains(where->column)) return false;
    return compare_cell(row.at(where->column), where->op, where->value);
}

// Join strings with ", " for the flat, scalar cells a SELECT row shows.
std::string join_comma(const std::vector<std::string>& items) {
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) joined += ", ";
        joined += item;
    }
    return joined;
}

// One JSON row per provider for the cloud_providers system table — the
// deployment-target / provider catalog (CloudProviderCatalog::builtin()). **Pure
// metadata: reads nothing from any cloud.** It surfaces, queryable via SQL, what
// the Cloud Inspector would touch (the config files + read-only commands) and
// whether inspecting requires consent / can run here (RULE-7).
std::vector<nlohmann::json> cloud_provider_rows() {
    std::vector<nlohmann::json> rows;
    for (const zetta::cloud::CloudProvider& provider :
         zetta::cloud::CloudProviderCatalog::builtin().providers()) {
        std::vector<std::string> commands;
        for (const zetta::cloud::InspectCommand& command : provider.inspect_commands) {
            commands.push_back(command.display);
        }
        nlohmann::json row;
        row["provider"] = provider.id;
        row["name"] = provider.name;
        row["category"] = provider.category;
        row["cli"] = provider.cli;
        row["requires_consent"] = provider.requires_consent();
        row["runnable"] = provider.runnable();
        row["config_paths"] = join_comma(provider.config_paths);
        row["inspect_commands"] = join_comma(commands);
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per cached cloud inspect for the cloud_status system table —
// the results of consent-gated inspects (Node::cloud_inspections()). **Reading
// this never triggers a cloud call**: it only reflects inspects the user already
// ran (and consented to) in the Cloud Inspector, so it is empty until then.
std::vector<nlohmann::json> cloud_status_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::cloud::CloudInspectReport& report : node.cloud_inspections()) {
        std::size_t commands_ok = 0;
        for (const zetta::cloud::CloudInspectCommand& command : report.commands) {
            if (command.ok) ++commands_ok;
        }
        nlohmann::json row;
        row["provider"] = report.provider_id;
        row["name"] = report.provider_name;
        row["reachable"] = report.reachable;
        row["authenticated"] = report.authenticated;
        row["identity"] = report.identity;
        row["regions"] = join_comma(report.locations);
        row["resources"] = join_comma(report.resources);
        row["commands_ok"] = static_cast<int>(commands_ok);
        row["commands_total"] = static_cast<int>(report.commands.size());
        row["checked_at"] = report.checked_at;
        row["detail"] = report.detail;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per detected provisioner for the provisioners system table —
// the cloud-burst targets this node can drive, derived from its own local CLIs
// (Node::provisioner_configs()). **Reading this never triggers a cloud call** and
// **exposes no credentials**: it reflects only the non-secret deployment context
// the CLI reported (subscription / region / resource group / …), pulled tolerantly
// from the provider's validated config JSON. Empty until detection wires a target.
std::vector<nlohmann::json> provisioner_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::cloud::ProvisionerConfig& provisioner : node.provisioner_configs()) {
        const nlohmann::json& config = provisioner.config;
        nlohmann::json row;
        row["target"] = provisioner.target;
        row["cli"] = provisioner.cli;
        // Common non-secret targeting fields, read tolerantly (each provider's
        // config shape differs; absent fields are simply empty).
        row["subscription"] = config.value("subscription_id", std::string{});
        row["project"] = config.value("project_id", std::string{});
        row["region"] = config.contains("location")
                            ? config.value("location", std::string{})
                            : config.value("region", std::string{});
        row["resource_group"] = config.value("resource_group", std::string{});
        row["pool"] = config.value("pool_name", std::string{});
        row["spot"] = config.value("spot", false);
        row["summary"] = provisioner.summary;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per consent challenge for the consent_requests system table —
// the node's remote consent ledger (#621, Node::consent_ledger()): pending
// challenges awaiting a distinctly-privileged approval, plus decided ones with
// their outcome. **Ephemeral, never replicated** (consent is per-action and
// never remembered); reading this never triggers a cloud call.
std::vector<nlohmann::json> consent_request_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& challenge : node.consent_ledger().all()) {
        const nlohmann::json value = zetta::governance::to_json(*challenge);
        nlohmann::json row;
        row["id"] = challenge->id;
        row["capability"] = challenge->capability;
        row["summary"] = challenge->action.summary;
        row["requested_by"] = value.value("requested_by", std::string{});
        row["status"] = value.value("status", std::string{});
        row["created_at"] = challenge->created_at;
        row["expires_at"] = challenge->expires_at;
        row["decided_by"] = challenge->decided_by;
        row["detail"] = challenge->detail;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per run for the runs system table — the run registry
// (FEAT-5). Sourced from Node::runs() (every run replicates, so each node sees the
// same set). Exposes the run's identity + lifecycle + its place in the pipeline
// (project / graph / deployment) and summary counts of its logs / outputs plus the
// lineage confidence + freshness — the queryable read view of the GUI Runs panel.
std::vector<nlohmann::json> run_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& run : node.runs()) {
        nlohmann::json row;
        row["run_id"] = run->id();
        row["project"] = run->project();
        row["graph"] = run->graph();
        row["deployment"] = run->deployment();
        row["status"] = std::string(to_string(run->status()));
        row["created"] = run->created();
        row["updated"] = run->updated();
        row["log_count"] = static_cast<int>(run->logs().size());
        row["output_count"] = static_cast<int>(run->outputs().size());
        row["confidence"] = run->lineage().confidence;
        row["freshness"] = run->lineage().freshness;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per principal for the principals system table — the
// authenticated actors (ENT-20). Sourced from Node::principals(): identity + kind
// + created + how many access keys it owns; the keys themselves are keys.
std::vector<nlohmann::json> principal_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& principal : node.principals()) {
        nlohmann::json row;
        row["principal_id"] = principal->id();
        row["name"] = principal->name();
        row["kind"] = std::string(to_string(principal->kind()));
        row["created"] = principal->created();
        row["key_count"] = static_cast<int>(principal->key_count());
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per access key for the keys system table — the credentials
// the principals own (ENT-21/22), the queryable read view of the Admin panel. Each
// key's reach is two human-readable strings: its `scope` (the resources it may
// touch — "*" for an unrestricted/admin key) and its `capabilities` (the rights it
// may exercise, each "<domain>:<action>", with an "@<ttl>" suffix when TTL'd). A
// key record holds no secret material (scope + capabilities only), so this exposes
// nothing the Admin panel doesn't already show.
std::vector<nlohmann::json> access_key_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& principal : node.principals()) {
        for (const auto& key : principal->keys()) {
            std::vector<std::string> scope_parts;
            const zetta::governance::Scope& scope = key->scope();
            if (scope.unrestricted) {
                scope_parts.push_back("*");
            } else {
                for (const zetta::governance::Resource& resource : scope.grants) {
                    scope_parts.push_back(std::string(to_string(resource.kind)) +
                                          ":" + resource.name);
                }
            }
            std::vector<std::string> capability_parts;
            for (const zetta::governance::Capability& capability : key->capabilities().grants) {
                std::string text = std::string(to_string(capability.domain)) + ":" +
                                   capability.action;
                if (!capability.expires_at.empty()) text += "@" + capability.expires_at;
                capability_parts.push_back(std::move(text));
            }
            nlohmann::json row;
            row["key_id"] = key->id();
            row["principal_id"] = key->principal_id();
            row["principal"] = principal->name();
            row["unrestricted"] = scope.unrestricted;
            row["scope"] = join_comma(scope_parts);
            row["capabilities"] = join_comma(capability_parts);
            row["created"] = key->created();
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

// One JSON row per audit event for the audit system table — the immutable,
// append-only audit trail (ENT-23 / FEAT-9), the queryable read view of the Admin
// panel's audit log. Sourced from Node::audit_events() (chronological).
std::vector<nlohmann::json> audit_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& event : node.audit_events()) {
        nlohmann::json row;
        row["audit_id"] = event->id();
        row["action"] = std::string(to_string(event->action()));
        row["actor"] = event->actor();
        row["target"] = event->target();
        row["allowed"] = event->allowed();
        row["detail"] = event->detail();
        row["time"] = event->time();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per activity entry for the system.activity table — the merged,
// newest-first "what happened on this cluster" feed (#622): the audit trail +
// runs, composed at query time by zetta::observability::activity_entries
// (RULE-7 — no stored copy; the GUI's Activity view renders the same source).
std::vector<nlohmann::json> activity_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::observability::ActivityEntry& entry :
         zetta::observability::activity_entries(node)) {
        nlohmann::json row;
        row["time"] = entry.time;
        row["surface"] = std::string(to_string(entry.surface));
        row["actor"] = entry.actor;
        row["action"] = entry.action;
        row["target"] = entry.target;
        row["outcome"] = std::string(to_string(entry.outcome));
        row["detail"] = entry.detail;
        row["ref_kind"] = entry.ref_kind;
        row["ref_id"] = entry.ref_id;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per dataset partition for the system.dataset_partitions table — the
// partition catalog (ENT-36 / TBL-13). Sourced from Node::dataset_partitions()
// (every partition replicates, so each node sees the same placement). Exposes the
// partition's identity + owning table + the large-object body holding its bytes +
// the row range it covers + its column stats (free-form object) + the owner nodes
// (flattened to a readable string + a count) — the queryable read view of the
// replicate-to-all catalog.
std::vector<nlohmann::json> partition_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& partition : node.dataset_partitions()) {
        const zetta::data::PartitionRowRange& range = partition->row_range();
        nlohmann::json row;
        row["partition_id"] = partition->id();
        row["table"] = partition->table();
        row["blob_ref"] = partition->blob_ref();
        row["row_start"] = range.start;
        row["row_end"] = range.end;
        row["row_count"] = range.end >= range.start ? range.end - range.start : 0;
        row["owner_nodes"] = join_comma(partition->owner_nodes());
        row["owner_count"] = static_cast<int>(partition->owner_nodes().size());
        row["stats"] = partition->stats();
        row["created"] = partition->created();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per dataset for the system.datasets table — datasets surfaced as a
// first-class primitive (#585; the engine source for the area-3 dataset view in epic
// #575). Sourced from Node::datasets() (computed live from the tree + the
// replicate-to-all partition catalog), so every node reports the same per-dataset
// schema / row count / on-disk size / storage format / replication strategy and the
// cross-cluster partition placement. `columns` is the inferred schema (comma-joined
// field names) with `column_count` for a scalar; `placement` flattens each partition
// to "[start,end)@owner1|owner2" (which slice → which owners) joined with ", " (empty
// for a non-partitioned, replicate-to-all dataset), and `owner_nodes` is the
// de-duplicated set of nodes holding any of its partitions. It reads already-built
// DatasetSummary values, so it adds no new JSON boundary.
std::vector<nlohmann::json> dataset_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::data::DatasetSummary& dataset : node.datasets()) {
        // Flatten the per-partition placement into a readable per-slice string plus
        // the de-duplicated union of owner nodes (in first-seen order).
        std::vector<std::string> owner_union;
        std::unordered_set<std::string> owner_seen;
        std::vector<std::string> placement_parts;
        for (const zetta::data::DatasetPlacement& placement : dataset.partitions) {
            std::string owners;
            for (const std::string& owner : placement.owner_nodes) {
                if (!owners.empty()) owners += "|";
                owners += owner;
                if (owner_seen.insert(owner).second) owner_union.push_back(owner);
            }
            placement_parts.push_back(
                "[" + std::to_string(placement.row_start) + "," +
                std::to_string(placement.row_end) + ")@" +
                (owners.empty() ? std::string("unplaced") : owners));
        }

        nlohmann::json row;
        row["dataset"] = dataset.name;
        row["database"] = dataset.database;
        row["table_id"] = dataset.table_id;
        row["format"] = dataset.format;
        row["replication"] = dataset.replication;
        row["rows"] = dataset.rows;
        row["columns"] = join_comma(dataset.columns);
        row["column_count"] = static_cast<int>(dataset.columns.size());
        row["size_bytes"] = dataset.size_bytes;
        row["partitioned"] = !dataset.partitions.empty();
        row["partition_count"] = static_cast<int>(dataset.partitions.size());
        row["owner_nodes"] = join_comma(owner_union);
        row["placement"] = join_comma(placement_parts);
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per checkpointed job for the system.checkpoints table — burst-job
// eviction tolerance (RULE-5 / #468). Sourced from Node::checkpoints() (the latest
// checkpoint per job in this node's LOCAL store), so it shows what is durably
// resumable here. Checkpoints are ephemeral, local durability artifacts — NOT
// replicated state (like system.cloud_status), so this view has no CRDT
// backing and querying it touches only the local filesystem.
std::vector<nlohmann::json> checkpoint_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::compute::JobCheckpoint& checkpoint : node.checkpoints()) {
        nlohmann::json row;
        row["job_id"] = checkpoint.job_id;
        row["kind"] = std::string(zetta::compute::to_string(checkpoint.kind));
        row["sequence"] = checkpoint.sequence;
        row["total"] = checkpoint.total;
        row["progress"] = checkpoint.progress();
        row["created"] = checkpoint.created;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The single live-telemetry row for the system.node_metrics table — this node's
// host resource utilization (Node::sample_node_metrics()), sampled from OS
// counters at query time (#575 area 2). **Ephemeral, observed, NOT replicated**
// (like cloud_status / checkpoints): the row has no CRDT backing and reflects only
// the local host. A node measures only its own host, so this is exactly one row;
// counters a host can't report are emitted as null (graceful degradation — the
// GPU group is absent in a CPU-only core build). Percentages are 0..100.
std::vector<nlohmann::json> node_metrics_rows(Node& node) {
    const zetta::cluster::NodeMetrics metrics = node.sample_node_metrics();
    auto number_or_null = [](bool available, auto value) {
        return available ? nlohmann::json(value) : nlohmann::json();
    };

    nlohmann::json row;
    row["node_id"] = node.node_id();
    // CPU
    row["cpu_cores"] =
        metrics.cpu_cores > 0 ? nlohmann::json(metrics.cpu_cores) : nlohmann::json();
    row["cpu_percent"] =
        number_or_null(metrics.cpu_percent_available, metrics.cpu_percent);
    // Memory
    row["memory_used_bytes"] =
        number_or_null(metrics.memory_available, metrics.memory_used_bytes);
    row["memory_total_bytes"] =
        number_or_null(metrics.memory_available, metrics.memory_total_bytes);
    row["memory_percent"] =
        number_or_null(metrics.memory_available, metrics.memory_percent);
    // GPU + VRAM (absent in a CPU-only core build → null)
    row["gpu_present"] = metrics.gpu_available;
    row["gpu_percent"] =
        number_or_null(metrics.gpu_available, metrics.gpu_percent);
    row["vram_used_bytes"] =
        number_or_null(metrics.gpu_available, metrics.vram_used_bytes);
    row["vram_total_bytes"] =
        number_or_null(metrics.gpu_available, metrics.vram_total_bytes);
    // Storage (the data_dir filesystem)
    row["storage_used_bytes"] =
        number_or_null(metrics.storage_available, metrics.storage_used_bytes);
    row["storage_total_bytes"] =
        number_or_null(metrics.storage_available, metrics.storage_total_bytes);
    row["storage_percent"] =
        number_or_null(metrics.storage_available, metrics.storage_percent);
    // Network (rates since the previous sample; utilization needs a link cap)
    row["net_rx_bytes_per_sec"] =
        number_or_null(metrics.network_available, metrics.net_rx_bytes_per_sec);
    row["net_tx_bytes_per_sec"] =
        number_or_null(metrics.network_available, metrics.net_tx_bytes_per_sec);
    row["net_percent"] =
        number_or_null(metrics.net_utilization_available, metrics.net_percent);
    row["sampled_at"] = metrics.sampled_at;

    return {std::move(row)};
}

// One JSON row per recorded near-data compute job for the system.pipeline_jobs
// table — the live pipeline instrumentation (#589, epic #575 area 5; the engine
// source the algorithm-at-work animation reads from). Sourced from
// Node::pipeline_jobs() (the bounded, in-memory ring the near-data runners record
// into). **Ephemeral, observed, NOT replicated** (like node_metrics / checkpoints):
// the rows reflect what THIS node did and have no CRDT backing. Each row exposes the
// near-data acceptance directly — `local_bytes_read` (partition body bytes read near
// data, not moved) vs `wire_bytes_moved` (the collective-plane partials), so a query
// can confirm bytes-moved ≈ partials, not ≈ dataset.
std::vector<nlohmann::json> pipeline_job_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::compute::PipelineJobInstrumentation& job : node.pipeline_jobs()) {
        nlohmann::json row;
        row["job_id"] = job.job_id;
        row["kind"] = std::string(to_string(job.kind));
        row["node"] = job.node;
        row["partitions_read"] = job.partitions_read;
        row["rows_read"] = job.rows_read;
        row["local_bytes_read"] = job.local_bytes_read;
        row["wire_bytes_moved"] = job.wire_bytes_moved;
        row["status"] = std::string(to_string(job.status));
        row["recorded_at"] = job.recorded_at;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The single live-collective-plane row for the system.collective_rounds table —
// the live progress of this node's long-lived compute coordinator (#589, epic #575
// area 5). Computed from the live JobCoordinator's Collectives (the #579 spine):
// `rounds_completed` (allreduce/broadcast/barrier/gather that succeeded) and
// `wire_bytes_moved` (cumulative collective-plane payload bytes) both tick up during
// ANY distributed run — including the interactive churn run — so the GUI can watch a
// run advance. **Ephemeral, observed, NOT replicated** (live meters, no CRDT). A
// node with no attached coordinator (standalone, or before attach_cluster) reports
// `attached=false` with zeroed counters — always exactly one row, like node_metrics.
std::vector<nlohmann::json> collective_round_rows(Node& node) {
    nlohmann::json row;
    row["node"] = node.node_id();
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator) {
        const zetta::cluster::Collectives& collectives = coordinator->collectives();
        row["attached"] = true;
        row["participants"] =
            static_cast<int>(collectives.participants().size());
        row["rounds_completed"] = collectives.rounds_completed();
        row["wire_bytes_moved"] = collectives.wire_bytes();
    } else {
        row["attached"] = false;
        row["participants"] = 0;
        row["rounds_completed"] = static_cast<std::uint64_t>(0);
        row["wire_bytes_moved"] = static_cast<std::uint64_t>(0);
    }
    return {std::move(row)};
}

// One JSON row per span in the node's span ring for the system.spans table
// (docs/architecture/observability.md, #640). Sourced from
// Node::span_recorder().snapshot() (the bounded, mutex-guarded ring every
// traced operation records into — API calls, distributed runs, testkit cases).
// **Ephemeral, observed, NOT replicated** (like pipeline_jobs / node_metrics):
// the rows reflect what THIS node recorded and have no CRDT backing. The four
// captures ride in `attributes`, with `intent` / `plan` / `result_shape`
// surfaced as first-class columns so a query can filter on them directly.
std::vector<nlohmann::json> span_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::observability::Span& span :
         node.span_recorder().snapshot()) {
        nlohmann::json row;
        row["trace_id"] = span.trace_id;
        row["span_id"] = span.span_id;
        row["parent_span_id"] = span.parent_span_id;
        row["name"] = span.name;
        row["started_at"] = span.started_at;
        row["duration_ms"] = span.duration_ms;
        row["ok"] = span.ok;
        row["error"] = span.error;
        row["intent"] = span.attributes.value("intent", nlohmann::json());
        row["plan"] = span.attributes.value("plan", nlohmann::json());
        row["result_shape"] =
            span.attributes.value("result_shape", nlohmann::json());
        row["attributes"] = span.attributes;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per trace for the system.traces table — the "what happened"
// summary aggregated from the same span ring (#640): the trace's root span
// (the recorded span whose parent is not itself recorded — the inbound
// context's span id is synthetic and never recorded), its name + intent, the
// earliest start, the root's duration, the span count, and whether every span
// succeeded (`error` carries the first failing span's reason). Traces appear
// oldest first. **Ephemeral, observed, NOT replicated** — same source, so the
// GUI activity feed (#622), the SQL surface, and MCP agents all read one ring.
std::vector<nlohmann::json> trace_rows(const Node& node) {
    const std::vector<zetta::observability::Span> spans =
        node.span_recorder().snapshot();

    std::unordered_set<std::string> recorded_span_ids;
    for (const zetta::observability::Span& span : spans) {
        recorded_span_ids.insert(span.span_id);
    }

    // Group by trace id, preserving first-recorded order.
    std::vector<std::string> trace_order;
    std::unordered_map<std::string,
                       std::vector<const zetta::observability::Span*>>
        traces;
    for (const zetta::observability::Span& span : spans) {
        auto [entry, inserted] = traces.try_emplace(span.trace_id);
        if (inserted) trace_order.push_back(span.trace_id);
        entry->second.push_back(&span);
    }

    std::vector<nlohmann::json> rows;
    rows.reserve(trace_order.size());
    for (const std::string& trace_id : trace_order) {
        const auto& trace_spans = traces.at(trace_id);
        const zetta::observability::Span* root = nullptr;
        std::string started_at;
        bool all_ok = true;
        std::string first_error;
        for (const zetta::observability::Span* span : trace_spans) {
            const bool is_root_candidate =
                span->parent_span_id.empty() ||
                !recorded_span_ids.contains(span->parent_span_id);
            if (is_root_candidate && !root) root = span;
            if (started_at.empty() || span->started_at < started_at) {
                started_at = span->started_at;  // ISO-8601 sorts lexically
            }
            if (!span->ok) {
                all_ok = false;
                if (first_error.empty()) first_error = span->error;
            }
        }
        if (!root) root = trace_spans.front();

        nlohmann::json row;
        row["trace_id"] = trace_id;
        row["root_span_id"] = root->span_id;
        row["name"] = root->name;
        row["intent"] = root->attributes.value("intent", nlohmann::json());
        row["started_at"] = started_at;
        row["duration_ms"] = root->duration_ms;
        row["span_count"] = static_cast<int>(trace_spans.size());
        row["ok"] = all_ok;
        row["error"] = first_error;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per crash-report file for the system.crashes table (#640) —
// what a dying node wrote on its way down (crash.hpp: fatal signal, stack,
// recent spans, version/platform). Read from <data_dir>/crashes/ at query
// time (the checkpoints precedent): ephemeral, LOCAL, NOT replicated. A file
// that fails schema validation still gets a row (valid=false + the reason) —
// a corrupt crash report is itself something to observe.
std::vector<nlohmann::json> crash_rows(const Node& node) {
    const std::string crashes_dir =
        (std::filesystem::path(node.config().data_dir) / "crashes").string();
    std::vector<nlohmann::json> rows;
    for (const zetta::observability::LoadedCrashReport& loaded :
         zetta::observability::load_crash_reports(crashes_dir)) {
        nlohmann::json row;
        row["path"] = loaded.path;
        row["valid"] = loaded.report.has_value();
        row["error"] = loaded.error;
        if (loaded.report) {
            const zetta::observability::CrashReport& report = *loaded.report;
            row["id"] = report.id;
            row["signal_number"] = report.signal_number;
            row["signal_name"] = report.signal_name;
            row["crashed_at"] = report.crashed_at;
            row["version"] = report.version;
            row["platform"] = report.platform;
            row["frame_count"] = static_cast<int>(report.stacktrace.size());
            row["stacktrace"] = report.stacktrace;
            row["recent_spans"] = report.recent_spans;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// Apply a SELECT's WHERE + projection to in-memory rows (used by the system
// table; regular tables stream straight from the storage engine).
QueryResult select_over_rows(const SelectStmt& statement,
                             const std::vector<nlohmann::json>& source) {
    QueryResult result;
    if (!statement.all) result.columns = statement.columns;
    for (const nlohmann::json& row : source) {
        if (!row_matches(row, statement.where)) continue;
        if (statement.all) {
            result.rows.push_back(row);
        } else {
            nlohmann::json projected = nlohmann::json::object();
            for (const std::string& column : statement.columns) {
                projected[column] =
                    row.contains(column) ? row.at(column) : nlohmann::json();
            }
            result.rows.push_back(std::move(projected));
        }
    }
    result.message = "SELECT " + std::to_string(result.rows.size());
    return result;
}

// --- GROUP BY via the DDF engine --------------------------------------------
//
// An aggregating SELECT runs through the DDF DataFrame engine: the referenced
// numeric columns are loaded into a DataFrame (scan), WHERE is applied via
// ddf::filter, ddf::group_by computes the per-group aggregates, and the columnar
// result is materialized back into query rows. The DataFrame is numeric only, so a
// grouped query over a non-numeric column returns a clear error. This builds the
// already-validated DDF spec types directly (GroupBySpec / FilterPredicate); it
// introduces no new JSON boundary — the SQL is validated by the parser and the DDF
// types carry their own boundaries.

// Whether a DDF column dtype is an integer type (so a whole-number value is emitted
// as a JSON integer rather than a float).
bool is_integer_dtype(compute::DataType dtype) {
    switch (dtype) {
        case compute::DataType::Int32:
        case compute::DataType::Int64:
        case compute::DataType::UInt32:
        case compute::DataType::UInt64:
        case compute::DataType::UInt8:
            return true;
        case compute::DataType::Float32:
        case compute::DataType::Float64:
            return false;
    }
    return false;
}

// A numeric result as JSON: a whole-number value of an integer column comes back as
// a JSON integer; everything else (means, floats, non-integral values) as a double
// — so result rows read naturally (`segment: 1`, not `segment: 1.0`).
nlohmann::json number_json(double value, bool integral) {
    if (integral && std::isfinite(value) && std::floor(value) == value &&
        value >= -9.2e18 && value <= 9.2e18) {
        return nlohmann::json(static_cast<std::int64_t>(value));
    }
    return nlohmann::json(value);
}

// Map the AST aggregate function to the DDF engine's op (Avg -> Mean).
ddf::AggregateOp ddf_op_of(AggregateFunc func) {
    switch (func) {
        case AggregateFunc::Count: return ddf::AggregateOp::Count;
        case AggregateFunc::Sum:   return ddf::AggregateOp::Sum;
        case AggregateFunc::Min:   return ddf::AggregateOp::Min;
        case AggregateFunc::Max:   return ddf::AggregateOp::Max;
        case AggregateFunc::Avg:   return ddf::AggregateOp::Mean;
    }
    return ddf::AggregateOp::Count;
}

// Map the AST comparison to the DDF filter comparison (the two sets line up 1:1).
ddf::Comparison ddf_comparison_of(CompareOp op) {
    switch (op) {
        case CompareOp::Equal:        return ddf::Comparison::Equal;
        case CompareOp::NotEqual:     return ddf::Comparison::NotEqual;
        case CompareOp::Less:         return ddf::Comparison::Less;
        case CompareOp::LessEqual:    return ddf::Comparison::LessEqual;
        case CompareOp::Greater:      return ddf::Comparison::Greater;
        case CompareOp::GreaterEqual: return ddf::Comparison::GreaterEqual;
    }
    return ddf::Comparison::Equal;
}

// One materialized output column of a grouped SELECT, in select order.
struct OutputColumn {
    std::string name;                       // result column name
    std::optional<std::size_t> aggregate;   // index into the result's aggregates;
                                            //   nullopt => the grouping key
    AggregateFunc func = AggregateFunc::Count;  // decides integrality, for aggregates
    std::string column;                     // the aggregated source column
};

QueryResult execute_grouped(const SelectStmt& s, zetta::data::Table& table) {
    // The DDF engine groups by exactly one numeric key column.
    if (s.group_by.size() != 1) {
        return error("GROUP BY supports exactly one key column");
    }
    const std::string& key = s.group_by.front();

    // Validate the WHERE: the DDF filter is numeric, so the predicate value must be
    // a number (string/bool/null predicates are not supported in the grouped path).
    if (s.where && !s.where->value.is_number()) {
        return error("GROUP BY queries support only numeric WHERE predicates");
    }

    // Build the DDF group-by spec and the ordered output plan from the select list.
    // A non-aggregated select column must be the grouping key (standard SQL); a
    // duplicate output name is rejected (use AS to disambiguate).
    ddf::GroupBySpec spec;
    spec.key = key;
    std::vector<OutputColumn> plan;
    std::unordered_set<std::string> output_names;
    std::unordered_set<std::string> referenced{key};
    for (const GroupedItem& item : s.items) {
        if (item.aggregate) {
            ddf::Aggregate aggregate;
            aggregate.op = ddf_op_of(item.aggregate->func);
            aggregate.column = item.aggregate->column;
            aggregate.output = item.alias;  // empty => the DDF default name
            const std::string name = aggregate.output_name();
            if (!output_names.insert(name).second) {
                return error("duplicate output column \"" + name +
                             "\" (use AS to rename)");
            }
            if (!aggregate.column.empty()) referenced.insert(aggregate.column);
            plan.push_back({name, spec.aggregates.size(), item.aggregate->func,
                            aggregate.column});
            spec.aggregates.push_back(std::move(aggregate));
        } else {
            if (item.column != key) {
                return error("column \"" + item.column +
                             "\" must appear in GROUP BY or be aggregated");
            }
            const std::string name = item.alias.empty() ? item.column : item.alias;
            if (!output_names.insert(name).second) {
                return error("duplicate output column \"" + name + "\"");
            }
            plan.push_back({name, std::nullopt, AggregateFunc::Count, item.column});
        }
    }
    if (s.where) referenced.insert(s.where->column);

    QueryResult result;
    for (const OutputColumn& column : plan) result.columns.push_back(column.name);

    // An empty table aggregates to no rows (Postgres-compatible) — short-circuit
    // before touching the DDF engine.
    const zetta::storage::StorageEngine& store = table.store();
    if (store.size() == 0) {
        result.message = "SELECT 0";
        return result;
    }

    // Resolve each referenced column's dtype from the table's dense numeric fields.
    // A column that is missing or not dense-numeric can't be a DDF column.
    const ddf::DataFrameSchema inferred = ddf::infer_schema(store);
    ddf::DataFrameSchema schema;
    std::unordered_map<std::string, compute::DataType> dtypes;
    for (const std::string& name : referenced) {
        const ddf::ColumnSchema* found = nullptr;
        for (const ddf::ColumnSchema& column : inferred.columns) {
            if (column.name == name) {
                found = &column;
                break;
            }
        }
        if (found == nullptr) {
            return error("column \"" + name +
                         "\" is not a dense numeric column — GROUP BY runs over the "
                         "DDF engine, which operates on numeric columns only");
        }
        schema.columns.push_back(*found);
        dtypes.emplace(name, found->dtype);
    }

    // Scan the table into a DataFrame on a CPU device, filter (WHERE), then group.
    compute::CpuDevice device;
    ddf::DataFrameResult loaded = ddf::load_data_frame(store, schema, device);
    if (!loaded.ok()) {
        return error("GROUP BY scan failed: " + loaded.status.message);
    }
    ddf::DataFrame frame = std::move(loaded.frame);

    if (s.where) {
        ddf::FilterPredicate predicate;
        predicate.column = s.where->column;
        predicate.comparison = ddf_comparison_of(s.where->op);
        predicate.value = s.where->value.get<double>();
        ddf::DataFrameResult filtered = ddf::filter(frame, predicate);
        if (!filtered.ok()) {
            return error("GROUP BY filter failed: " + filtered.status.message);
        }
        frame = std::move(filtered.frame);
    }

    // No surviving rows -> no groups (and avoids grouping an empty frame).
    if (frame.row_count() == 0) {
        result.message = "SELECT 0";
        return result;
    }

    const ddf::GroupByOutcome outcome = ddf::group_by(frame, spec);
    if (!outcome.ok()) {
        return error("GROUP BY failed: " + outcome.status.message);
    }
    const ddf::GroupByResult& grouped = outcome.result;

    // Materialize one row per group, columns in select order. Integer columns emit
    // whole numbers as JSON integers; means and float columns stay doubles.
    const bool key_integral = is_integer_dtype(dtypes.at(key));
    for (std::size_t group = 0; group < grouped.keys.size(); ++group) {
        nlohmann::json row = nlohmann::json::object();
        for (const OutputColumn& column : plan) {
            if (!column.aggregate) {
                row[column.name] = number_json(grouped.keys[group], key_integral);
                continue;
            }
            const double value = grouped.values[*column.aggregate][group];
            bool integral = false;
            switch (column.func) {
                case AggregateFunc::Count:
                    integral = true;
                    break;
                case AggregateFunc::Avg:
                    integral = false;
                    break;
                default:  // Sum / Min / Max inherit the source column's integrality
                    integral = is_integer_dtype(dtypes.at(column.column));
                    break;
            }
            row[column.name] = number_json(value, integral);
        }
        result.rows.push_back(std::move(row));
    }
    result.message = "SELECT " + std::to_string(result.rows.size());
    return result;
}

}  // namespace

QueryResult Executor::execute(const Statement& stmt,
                              const std::string& current_database) {
    // Resolve a TableRef's database to a handle, or produce a clear error.
    const auto resolve_db =
        [&](const TableRef& ref) -> std::shared_ptr<zetta::data::Database> {
        const std::string& name =
            ref.database.empty() ? current_database : ref.database;
        if (name.empty()) return nullptr;  // no qualifier and no current db
        return node_.find_database(name);
    };
    const auto db_name = [&](const TableRef& ref) -> std::string {
        return ref.database.empty() ? current_database : ref.database;
    };

    return std::visit(
        overloaded{
            [&](const CreateDatabaseStmt& s) -> QueryResult {
                if (s.name == kSystemDatabase) {
                    return error("\"system\" is a reserved (virtual) database");
                }
                if (!node_.create_database(s.name)) {
                    return error("database \"" + s.name +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE DATABASE"};
            },
            [&](const DropDatabaseStmt& s) -> QueryResult {
                if (s.name == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                if (!node_.drop_database(s.name)) {
                    return error("database \"" + s.name + "\" does not exist");
                }
                return {.message = "DROP DATABASE"};
            },
            [&](const CreateTableStmt& s) -> QueryResult {
                const std::string name = db_name(s.table);
                if (name == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                if (name.empty()) {
                    return error("no database selected — qualify as db.table or "
                                 "select a database");
                }
                auto db = node_.find_database(name);
                if (!db) return error("database \"" + name + "\" does not exist");
                // Column definitions are parsed but not yet stored (the engine
                // is schemaless); the table is created by name.
                if (!db->create_table(s.table.table)) {
                    return error("table \"" + s.table.table +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE TABLE"};
            },
            [&](const DropTableStmt& s) -> QueryResult {
                if (db_name(s.table) == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                if (!db->drop_table(s.table.table)) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                return {.message = "DROP TABLE"};
            },
            [&](const InsertStmt& s) -> QueryResult {
                if (db_name(s.table) == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                if (s.columns.empty()) {
                    return error("INSERT requires an explicit column list, e.g. "
                                 "INSERT INTO t (a, b) VALUES (...)");
                }
                std::size_t inserted = 0;
                for (const auto& tuple : s.rows) {
                    if (tuple.size() != s.columns.size()) {
                        return error("INSERT has " +
                                     std::to_string(s.columns.size()) +
                                     " columns but " +
                                     std::to_string(tuple.size()) + " values");
                    }
                    nlohmann::json row = nlohmann::json::object();
                    for (std::size_t i = 0; i < s.columns.size(); ++i) {
                        row[s.columns[i]] = tuple[i];
                    }
                    table->add_row(std::move(row));
                    ++inserted;
                }
                return {.message = "INSERT 0 " + std::to_string(inserted)};
            },
            [&](const UpdateStmt& s) -> QueryResult {
                if (db_name(s.table) == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                // Collect the matches (id + the updated row) BEFORE applying —
                // update_row rewrites the store, which would invalidate indices.
                std::vector<std::pair<std::string, nlohmann::json>> edits;
                const zetta::storage::StorageEngine& store = table->store();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    const nlohmann::json& row = store.at(i);
                    if (!row_matches(row, s.where)) continue;
                    nlohmann::json updated = row;
                    for (const Assignment& a : s.assignments) {
                        updated[a.column] = a.value;
                    }
                    edits.emplace_back(store.id_at(i), std::move(updated));
                }
                for (auto& [id, doc] : edits) {
                    table->update_row(id, std::move(doc));
                }
                return {.message = "UPDATE " + std::to_string(edits.size())};
            },
            [&](const DeleteStmt& s) -> QueryResult {
                if (db_name(s.table) == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                std::vector<std::string> ids;
                const zetta::storage::StorageEngine& store = table->store();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    if (row_matches(store.at(i), s.where)) {
                        ids.push_back(store.id_at(i));
                    }
                }
                for (const std::string& id : ids) table->delete_row(id);
                return {.message = "DELETE " + std::to_string(ids.size())};
            },
            [&](const TruncateStmt& s) -> QueryResult {
                if (db_name(s.table) == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                // Remove every row — each as a tombstone, so the truncation
                // replicates (same path as DELETE with no WHERE).
                std::vector<std::string> ids;
                const zetta::storage::StorageEngine& store = table->store();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    ids.push_back(store.id_at(i));
                }
                for (const std::string& id : ids) table->delete_row(id);
                return {.message = "TRUNCATE TABLE"};
            },
            [&](const SelectStmt& s) -> QueryResult {
                // Virtual `system` database (computed, read-only).
                if (const std::string sys = system_table_of(s.table);
                    !sys.empty()) {
                    if (s.grouped()) {
                        return error("GROUP BY is not supported over system tables");
                    }
                    if (sys == kTablesTable) {
                        return select_over_rows(s, system_table_rows());
                    }
                    if (sys == kDeploymentsTable) {
                        return select_over_rows(s, deployment_rows(node_));
                    }
                    if (sys == kModelsTable) {
                        return select_over_rows(s, model_rows(node_));
                    }
                    if (sys == kIndexesTable) {
                        return select_over_rows(s, index_rows(node_));
                    }
                    if (sys == kJobsTable) {
                        return select_over_rows(s, compute_job_rows(node_));
                    }
                    if (sys == kCloudProvidersTable) {
                        return select_over_rows(s, cloud_provider_rows());
                    }
                    if (sys == kCloudStatusTable) {
                        return select_over_rows(s, cloud_status_rows(node_));
                    }
                    if (sys == kProvisionersTable) {
                        return select_over_rows(s, provisioner_rows(node_));
                    }
                    if (sys == kConsentRequestsTable) {
                        return select_over_rows(s, consent_request_rows(node_));
                    }
                    if (sys == kRunsTable) {
                        return select_over_rows(s, run_rows(node_));
                    }
                    if (sys == kPrincipalsTable) {
                        return select_over_rows(s, principal_rows(node_));
                    }
                    if (sys == kKeysTable) {
                        return select_over_rows(s, access_key_rows(node_));
                    }
                    if (sys == kAuditTable) {
                        return select_over_rows(s, audit_rows(node_));
                    }
                    if (sys == kActivityTable) {
                        return select_over_rows(s, activity_rows(node_));
                    }
                    if (sys == kPartitionsTable) {
                        return select_over_rows(s, partition_rows(node_));
                    }
                    if (sys == kDatasetsTable) {
                        return select_over_rows(s, dataset_rows(node_));
                    }
                    if (sys == kCheckpointsTable) {
                        return select_over_rows(s, checkpoint_rows(node_));
                    }
                    if (sys == kNodeMetricsTable) {
                        return select_over_rows(s, node_metrics_rows(node_));
                    }
                    if (sys == kPipelineJobsTable) {
                        return select_over_rows(s, pipeline_job_rows(node_));
                    }
                    if (sys == kCollectiveRoundsTable) {
                        return select_over_rows(s, collective_round_rows(node_));
                    }
                    if (sys == kSpansTable) {
                        return select_over_rows(s, span_rows(node_));
                    }
                    if (sys == kTracesTable) {
                        return select_over_rows(s, trace_rows(node_));
                    }
                    if (sys == kCrashesTable) {
                        return select_over_rows(s, crash_rows(node_));
                    }
                    if (sys != kNodesTable) {
                        // Suggest the catalog itself (never drifts, unlike a
                        // hand-kept list — the pre-#684 one already had).
                        std::string available;
                        for (const std::string_view name : kSystemTableNames) {
                            if (!available.empty()) available += ", ";
                            available += "system.";
                            available += name;
                        }
                        return error("no such system table \"" + sys +
                                     "\" (see SELECT * FROM system.tables; "
                                     "available: " + available + ")");
                    }
                    if (!cluster_) {
                        return error("system.nodes is unavailable here "
                                     "(no cluster)");
                    }
                    return select_over_rows(s, cluster_node_rows(*cluster_));
                }
                auto db = resolve_db(s.table);
                if (!db) {
                    return error("database \"" + db_name(s.table) +
                                 "\" does not exist");
                }
                auto table = db->find_table(s.table.table);
                if (!table) {
                    return error("table \"" + s.table.table + "\" does not exist");
                }
                // An aggregating SELECT (GROUP BY) runs through the DDF engine.
                if (s.grouped()) return execute_grouped(s, *table);
                QueryResult r;
                if (!s.all) r.columns = s.columns;
                const zetta::storage::StorageEngine& store = table->store();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    const nlohmann::json& row = store.at(i);
                    if (!row_matches(row, s.where)) continue;
                    if (s.all) {
                        r.rows.push_back(row);
                    } else {
                        nlohmann::json projected = nlohmann::json::object();
                        for (const std::string& col : s.columns) {
                            projected[col] =
                                row.contains(col) ? row.at(col) : nlohmann::json();
                        }
                        r.rows.push_back(std::move(projected));
                    }
                }
                r.message = "SELECT " + std::to_string(r.rows.size());
                return r;
            },
        },
        stmt);
}

}  // namespace zetta::query
