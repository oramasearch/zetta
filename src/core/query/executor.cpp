#include "zetta/query/executor.hpp"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <algorithm>
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
#include "zetta/connect/connection_store.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/virtual_table.hpp"
#include "zetta/governance/permission.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/observability/activity.hpp"
#include "zetta/observability/crash.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/node.hpp"
#include "zetta/query/distributed_select.hpp"
#include "zetta/query/predicate.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/data/table.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/pubsub/node_integration.hpp"
#include "zetta/pubsub/pubsub.hpp"

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
//  - `users` — the authenticatable identities (Node::users(); FEAT-9 / #744):
//    identity + status + assigned role names + token count.
//  - `roles` — the named permission sets (Node::roles()): each role's scope
//    (resources) + capabilities (rights), the same rendering as `keys`.
//  - `tokens` — the bearer credentials per user (Node::tokens()): id, user,
//    type, created, expiry, revoked — deliberately NO hash/salt/secret (the
//    plaintext is shown once at creation and never stored).
//  - `audit` — retained immutable audit records (Node::audit_events(); ENT-23 /
//    FEAT-9), including nullable polling-summary totals.
//  - `ledger` — the immutable, replicated score, decision, action, and outcome
//    reward stream (Node::ledger_entries(); #1107).
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
constexpr std::string_view kWorkspaceProfilesTable = "workspace_profiles";
constexpr std::string_view kUsageTable = "usage";
constexpr std::string_view kPlansTable = "plans";
constexpr std::string_view kIndexesTable = "indexes";
constexpr std::string_view kJobsTable = "jobs";
constexpr std::string_view kCloudProvidersTable = "cloud_providers";
constexpr std::string_view kCloudStatusTable = "cloud_status";
constexpr std::string_view kProvisionersTable = "provisioners";
constexpr std::string_view kPubSubTopicsTable = "pubsub_topics";
constexpr std::string_view kConsentRequestsTable = "consent_requests";
constexpr std::string_view kConnectionsTable = "connections";
constexpr std::string_view kRunsTable = "runs";
constexpr std::string_view kPrincipalsTable = "principals";
constexpr std::string_view kKeysTable = "keys";
constexpr std::string_view kUsersTable = "users";
constexpr std::string_view kRolesTable = "roles";
constexpr std::string_view kUserRolesTable = "user_roles";
constexpr std::string_view kPoliciesTable = "policies";
constexpr std::string_view kTokensTable = "tokens";
constexpr std::string_view kAuditTable = "audit";
constexpr std::string_view kLedgerTable = "ledger";
constexpr std::string_view kActivityTable = "activity";
constexpr std::string_view kPartitionsTable = "dataset_partitions";
constexpr std::string_view kDatasetsTable = "datasets";
constexpr std::string_view kLargeObjectsTable = "large_objects";
constexpr std::string_view kCheckpointsTable = "checkpoints";
constexpr std::string_view kNodeMetricsTable = "node_metrics";
constexpr std::string_view kMemoryTable = "memory";
constexpr std::string_view kPipelineJobsTable = "pipeline_jobs";
constexpr std::string_view kCollectiveRoundsTable = "collective_rounds";
constexpr std::string_view kSpansTable = "spans";
constexpr std::string_view kTracesTable = "traces";
constexpr std::string_view kCrashesTable = "crashes";
constexpr std::string_view kTablesTable = "tables";
constexpr std::string_view kTestCasesTable = "test_cases";

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
    SystemTableInfo{kAuditTable, "the immutable, retained audit trail"},
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
    SystemTableInfo{kConnectionsTable,
                    "the saved source connections held on every node — "
                    "settings only, credentials never exposed"},
    SystemTableInfo{kCrashesTable,
                    "crash reports found under this node's data_dir "
                    "(ephemeral, local)"},
    SystemTableInfo{kPartitionsTable,
                    "the partition catalog: where each slice of a table's "
                    "data lives"},
    SystemTableInfo{kDatasetsTable,
                    "datasets as a first-class primitive: schema, rows, size, "
                    "format, partition placement"},
    SystemTableInfo{kLargeObjectsTable,
                    "large objects (files / blobs) stored per database: name, "
                    "size, sha256, and whether the body has synced to this node"},
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
    SystemTableInfo{kLedgerTable,
                    "the immutable score, decision, action and outcome reward ledger"},
    SystemTableInfo{kMemoryTable,
                    "this process memory by owning subsystem plus allocator truth"},
    SystemTableInfo{kModelsTable,
                    "the model registry: trained models + their kind, "
                    "metrics, weights fingerprint"},
    SystemTableInfo{kWorkspaceProfilesTable,
                    "the workspace/environment profile registry: owner, role, "
                    "daily work, and approved source refs (#886)"},
    SystemTableInfo{kUsageTable,
                    "metered usage: a single-row snapshot of live consumption "
                    "(projects/runs/jobs/models/datasets+bytes/nodes) for "
                    "Billing/Usage"},
    SystemTableInfo{kPlansTable,
                    "the plan registry: name, tier, and quota limits the metered "
                    "usage is measured against (Conflict D)"},
    SystemTableInfo{kNodeMetricsTable,
                    "this node's live host resource utilization, sampled at "
                    "query time"},
    SystemTableInfo{kNodesTable,
                    "FULL cluster membership (virtual table over the node-list "
                    "CRDT, #809): every node — incl. those not directly linked — "
                    "with reachability, node_type, liveness + replication status "
                    "(needs a cluster)"},
    SystemTableInfo{kPipelineJobsTable,
                    "recorded near-data compute jobs (ephemeral, not "
                    "replicated)"},
    SystemTableInfo{kPoliciesTable,
                    "the row-level security policy registry: per-table USING "
                    "read filters + WITH CHECK write filters (RLS)"},
    SystemTableInfo{kPrincipalsTable, "the authenticated actors"},
    SystemTableInfo{kProvisionersTable,
                    "cloud-burst provisioners detected from this node's local "
                    "CLIs; carries no credentials"},
    SystemTableInfo{kPubSubTopicsTable,
                    "pub/sub topics and inboxes on this node: subscribers, held "
                    "events, memory and disk, spilling, the slowest subscriber "
                    "(#1034; ephemeral, not replicated)"},
    SystemTableInfo{kRolesTable,
                    "the role registry: named permission sets (scope + rights) "
                    "granted to users"},
    SystemTableInfo{kRunsTable,
                    "the run registry: one row per graph execution + its "
                    "lifecycle and lineage"},
    SystemTableInfo{kSpansTable,
                    "one row per span in this node's span ring: every traced "
                    "operation's intent, plan, timing"},
    SystemTableInfo{kTablesTable,
                    "this catalog: every system table + what it holds"},
    SystemTableInfo{kTokensTable,
                    "the token registry: bearer credentials per user — id, "
                    "type, created, revoked (NO secret material)"},
    SystemTableInfo{kTestCasesTable,
                    "the in-app test-suite catalog: one row per runnable case "
                    "(id, category, name, description, option count)"},
    SystemTableInfo{kTracesTable,
                    "one row per trace aggregated from the span ring: the "
                    "what-happened summary"},
    SystemTableInfo{kUserRolesTable,
                    "the user↔role assignments: one row per (user, role) grant — "
                    "the many-to-many feeding each user's effective permissions"},
    SystemTableInfo{kUsersTable,
                    "the user registry: authenticatable identities + status + "
                    "assigned roles"},
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

// The roles:manage operation the role-management DDL (CREATE/DROP ROLE,
// GRANT/REVOKE …) is gated on (#746). Maps onto the api:manage_roles capability
// — see the permission vocabulary (governance/permission.hpp).
const zetta::governance::Operation kManageRoles{
    zetta::governance::CapabilityDomain::Api, "manage_roles"};

// The data operations the executor enforces object scoping against (#747): a
// SELECT reads, an INSERT writes. These are the `sql:read` / `sql:write`
// operations from the permission vocabulary (governance/permission.hpp).
const zetta::governance::Operation kSqlRead{
    zetta::governance::CapabilityDomain::Data, "read"};
const zetta::governance::Operation kSqlWrite{
    zetta::governance::CapabilityDomain::Data, "write"};

// Translate the AST's dialect-neutral GrantObject into the governance Resource
// the scoped-grant store + enforcement use (#747). RoleWide has no object (the
// caller routes it to the flat grant path instead).
zetta::governance::Resource resource_of_object(const GrantObject& object) {
    using RK = zetta::governance::ResourceKind;
    switch (object.kind) {
        case GrantObjectKind::Database:
            return zetta::governance::Resource{RK::Database, object.database};
        case GrantObjectKind::Table:
            return zetta::governance::Resource::table(object.database,
                                                      object.table);
        case GrantObjectKind::Columns:
            return zetta::governance::Resource::column(object.database,
                                                       object.table,
                                                       object.columns);
        case GrantObjectKind::Blob:
            return zetta::governance::Resource::blob(object.name);
        case GrantObjectKind::Tool:
            return zetta::governance::Resource::tool(object.name);
        case GrantObjectKind::RoleWide:
            break;
    }
    return zetta::governance::Resource{};  // unreachable for a well-formed stmt
}

// The canonical stored spelling of a policy command (the FOR clause), matching
// governance::is_policy_command (#748).
std::string policy_command_string(PolicyCommand command) {
    switch (command) {
        case PolicyCommand::Select: return "select";
        case PolicyCommand::Insert: return "insert";
        case PolicyCommand::Update: return "update";
        case PolicyCommand::All:    return "all";
    }
    return "all";
}

// The first permission name in `names` outside the vocabulary, or nullopt when
// all are valid — the boundary check GRANT/REVOKE runs before touching a role, so
// a typo fails the whole statement rather than silently granting nothing.
std::optional<std::string> first_unknown_permission(
    const std::vector<std::string>& names) {
    for (const std::string& name : names) {
        if (!zetta::governance::is_permission(name)) return name;
    }
    return std::nullopt;
}

QueryResult error(std::string message) {
    QueryResult r;
    r.error = std::move(message);
    return r;
}

// One JSON row per node for the nodes system table. Unknown replication
// fields (e.g. a peer that hasn't reported yet, or self's sync state) are null.
// system.nodes (#809) — the unified virtual table over the node-list CRDT. Its rows
// are the FULL membership (every node:<id> the document knows — converging to all N
// from any node, not just direct peers), composed at query time from three tiers:
//   durable (CRDT node:<id>): node_id, address, reachability, node_type
//   ephemeral-local: is_self, directly_connected, status, last_seen_seconds
//   ephemeral-synced/local: replication status (self + direct peers, over STATUS
//     gossip) and liveness from the telemetry plane (cluster-wide, TTL'd).
// The ephemeral tiers are overlaid here and NEVER written to the CRDT, so a steady
// cluster produces zero CRDT writes. (The `gone` status + node-list GC is #809
// slice 2; user-facing CREATE VIRTUAL TABLE is slice 3.)
std::vector<nlohmann::json> cluster_node_rows(const zetta::cluster::Cluster& cluster,
                                              const Node& node) {
    // Direct-peer replication status, keyed by node_id (the live peer table).
    std::map<std::string, zetta::cluster::NodeReplication> replication_by_id;
    std::map<std::string, unsigned> audit_protocol_by_id;
    for (const zetta::cluster::NodeStatus& n : cluster.nodes()) {
        replication_by_id[n.node_id] = n.replication;
        audit_protocol_by_id[n.node_id] = n.audit_protocol_version;
    }
    // Live telemetry, cluster-wide + TTL'd — the ephemeral liveness signal
    // (node_id -> seconds since its last sample).
    std::map<std::string, double> telemetry_age_by_id;
    for (const zetta::cluster::NodeTelemetrySample& sample : node.telemetry_snapshot()) {
        telemetry_age_by_id[sample.node_id] = sample.age_seconds;
    }

    std::vector<nlohmann::json> rows;
    for (const zetta::cluster::ClusterMember& member : cluster.membership()) {
        nlohmann::json row;
        // --- durable (CRDT node:<id>) ---
        row["node_id"] = member.node_id;
        const auto audit_protocol = audit_protocol_by_id.find(member.node_id);
        row["audit_protocol_version"] = audit_protocol == audit_protocol_by_id.end()
            ? nlohmann::json() : nlohmann::json(audit_protocol->second);
        row["audit_compatible"] = audit_protocol != audit_protocol_by_id.end() &&
            audit_protocol->second == zetta::governance::kAuditProtocolVersion;
        row["address"] = member.address;
        row["reachability"] =
            member.dial_out_only ? "dial-out-only" : "wan-accessible";
        row["node_type"] =
            member.node_type.empty() ? "permanent" : member.node_type;
        // --- ephemeral-local ---
        row["is_self"] = member.is_self;
        row["directly_connected"] = member.directly_connected;
        // Liveness: live if we hold a direct link OR fresh telemetry reached us
        // within the TTL; else a declared member we can't currently see.
        const auto telemetry = telemetry_age_by_id.find(member.node_id);
        const bool has_telemetry = telemetry != telemetry_age_by_id.end();
        const bool live =
            member.is_self || member.directly_connected || has_telemetry;
        row["status"] = live ? "live" : "unreachable";
        if (member.is_self || member.directly_connected) {
            row["last_seen_seconds"] = 0.0;  // now
        } else if (has_telemetry) {
            row["last_seen_seconds"] = telemetry->second;
        } else {
            row["last_seen_seconds"] = nlohmann::json();  // never seen here
        }
        // --- ephemeral-synced/local: replication status (self + direct peers) ---
        const auto rep = replication_by_id.find(member.node_id);
        const bool rep_known = rep != replication_by_id.end() && rep->second.known;
        row["entries"] =
            rep_known ? nlohmann::json(rep->second.entries) : nlohmann::json();
        row["version"] =
            rep_known ? nlohmann::json(rep->second.version) : nlohmann::json();
        row["in_sync"] = (rep_known && !member.is_self)
                             ? nlohmann::json(rep->second.in_sync)
                             : nlohmann::json();
        row["last_sync_seconds"] =
            (rep_known && !member.is_self)
                ? nlohmann::json(rep->second.seconds_since_sync)
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
// not in the registry (it's a large binary that moves over the body path); its
// fingerprint (size + sha256) and presence are, so the registry shows what is
// trained and whether its weights have landed here.
// One JSON row per registered Workspace Profile (#886) — the input to the
// "Discover Opportunities" pathway. Sourced from Node::workspace_profiles()
// (every profile replicates, so each node reports the same set). `sources` rides
// as a JSON array (like model_rows' feature_schema), with `source_count` for a
// quick scalar.
// One JSON row per registered Plan (Conflict D — the quota/tier config the
// metered usage in system.usage is measured against). Sourced from Node::plans()
// (every plan replicates). `quotas` rides as a JSON object.
std::vector<nlohmann::json> plan_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& plan : node.plans()) {
        nlohmann::json row;
        row["plan_id"] = plan->id();
        row["name"] = plan->name();
        row["tier"] = plan->tier();
        row["quotas"] = plan->quotas();
        row["period_start"] = plan->period_start();
        row["created"] = plan->created();
        row["updated"] = plan->updated();
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<nlohmann::json> workspace_profile_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& profile : node.workspace_profiles()) {
        nlohmann::json row;
        row["workspace_profile_id"] = profile->id();
        row["owner"] = profile->owner();
        row["role"] = profile->role();
        row["daily_work"] = profile->daily_work();
        row["sources"] = profile->sources();
        row["source_count"] = profile->sources().is_array()
                                  ? static_cast<int>(profile->sources().size())
                                  : 0;
        row["created"] = profile->created();
        row["updated"] = profile->updated();
        rows.push_back(std::move(row));
    }
    return rows;
}

// A single-row **metered usage** snapshot for the Billing/Usage surface
// (Conflict D — metered usage + plan). Every field is computed at query time
// from live engine state (RULE-7): no stored counters, no billing subsystem —
// the real consumption a plan's quotas are measured against.
std::vector<nlohmann::json> usage_rows(const Node& node) {
    const auto datasets = node.datasets();
    std::uint64_t dataset_bytes = 0;
    for (const auto& dataset : datasets) dataset_bytes += dataset.size_bytes;
    nlohmann::json row;
    row["projects"] = static_cast<std::int64_t>(node.project_names().size());
    row["runs"] = static_cast<std::int64_t>(node.runs().size());
    row["jobs"] = static_cast<std::int64_t>(node.job_count());
    row["models"] = static_cast<std::int64_t>(node.models().size());
    row["datasets"] = static_cast<std::int64_t>(datasets.size());
    row["dataset_bytes"] = dataset_bytes;
    row["indexes"] = static_cast<std::int64_t>(node.index_count());
    row["deployments"] = static_cast<std::int64_t>(node.deployment_count());
    row["workspace_profiles"] =
        static_cast<std::int64_t>(node.workspace_profile_count());
    row["nodes"] = static_cast<std::int64_t>(node.cluster_members().size());
    std::vector<nlohmann::json> rows;
    rows.push_back(std::move(row));
    return rows;
}

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

// One JSON row per case for the test_cases system table — the in-app test-suite
// catalog (zetta::testkit::TestSuite::builtin()). Pure metadata, assembled once
// + cached: it declares each runnable case (nothing is executed at query time).
// The headed Test suite panel and the testsuite_list capability serve the same
// live source (RULE-7).
std::vector<nlohmann::json> test_case_rows() {
    std::vector<nlohmann::json> rows;
    for (const zetta::testkit::TestCase& test_case :
         zetta::testkit::TestSuite::builtin().cases()) {
        nlohmann::json row;
        row["id"] = test_case.id;
        row["category"] =
            std::string(zetta::testkit::to_string(test_case.category));
        row["name"] = test_case.name;
        row["description"] = test_case.description;
        row["options"] = static_cast<long>(test_case.options.size());
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

// Render a Scope + CapabilitySet as the two human-readable strings shared by the
// keys and roles views: `scope` ("*" for unrestricted, else "<kind>:<name>,…")
// and `capabilities` ("<domain>:<action>[@<ttl>],…"). No secret material.
void render_reach(const zetta::governance::Scope& scope,
                  const zetta::governance::CapabilitySet& capabilities,
                  nlohmann::json& row) {
    std::vector<std::string> scope_parts;
    if (scope.unrestricted) {
        scope_parts.push_back("*");
    } else {
        for (const zetta::governance::Resource& resource : scope.grants) {
            scope_parts.push_back(std::string(to_string(resource.kind)) + ":" +
                                  resource.name);
        }
    }
    std::vector<std::string> capability_parts;
    for (const zetta::governance::Capability& capability : capabilities.grants) {
        std::string text =
            std::string(to_string(capability.domain)) + ":" + capability.action;
        if (!capability.expires_at.empty()) text += "@" + capability.expires_at;
        capability_parts.push_back(std::move(text));
    }
    row["unrestricted"] = scope.unrestricted;
    row["scope"] = join_comma(scope_parts);
    row["capabilities"] = join_comma(capability_parts);
}

// One JSON row per user for the users system table — the authenticatable
// identities (FEAT-9 / #744). Sourced from Node::users(): identity + status +
// created + the names of its assigned roles + how many tokens it owns. No secret
// material (tokens are the tokens view; neither exposes hashes).
std::vector<nlohmann::json> user_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& user : node.users()) {
        std::vector<std::string> role_names;
        for (const std::string& role_id : node.roles_for_user(user->id())) {
            if (auto role = node.find_role_by_id(role_id)) {
                role_names.push_back(role->name());
            }
        }
        int token_count = 0;
        for (const auto& token : node.tokens()) {
            if (token->user_id() == user->id()) ++token_count;
        }
        nlohmann::json row;
        row["user_id"] = user->id();
        row["name"] = user->name();
        row["status"] = std::string(to_string(user->status()));
        row["created"] = user->created();
        row["roles"] = join_comma(role_names);
        row["token_count"] = token_count;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The vocabulary permission name a single operation stands for (e.g. {data,read}
// -> "sql:read"), or the raw "<domain>:<action>" spelling when no single-op
// permission matches — for rendering a scoped grant's right in the roles view.
std::string permission_name_of(const zetta::governance::Operation& op) {
    for (const std::string& name : zetta::governance::permission_vocabulary()) {
        const auto ops = zetta::governance::permission_operations(name);
        if (ops.size() == 1 && ops.front() == op) return name;
    }
    return std::string(to_string(op.domain)) + ":" + op.action;
}

// Render one scoped grant as "<perm> ON <object>" (#747) for the roles view.
std::string render_scoped_grant(const zetta::governance::ScopedGrant& grant) {
    using RK = zetta::governance::ResourceKind;
    const zetta::governance::Resource& r = grant.object;
    std::string object;
    switch (r.kind) {
        case RK::Database: object = r.name + ".*"; break;
        case RK::Project:  object = "project:" + r.name; break;
        case RK::Table:    object = r.database + "." + r.name; break;
        case RK::Column:
            object = r.database + "." + r.name + "(" + join_comma(r.columns) + ")";
            break;
        case RK::Blob: object = "blob:" + r.name; break;
        case RK::Tool: object = "tool:" + r.name; break;
        case RK::Topic: object = "topic:" + r.name; break;
    }
    return permission_name_of(grant.operation) + " ON " + object;
}

// One JSON row per role for the roles system table — the named permission sets
// (FEAT-9 / #744, #746). Sourced from Node::roles(): the role's scope (resources)
// + capabilities (rights, the same rendering as the keys view) + its permission
// SET rendered in the RBAC vocabulary (permission.hpp) — the referenceable atoms
// a custom role is built from — plus its object-scoped grants (#747). No secret
// material.
std::vector<nlohmann::json> role_rows(const Node& node) {
    const std::string now = zetta::util::now_iso8601();
    std::vector<nlohmann::json> rows;
    for (const auto& role : node.roles()) {
        nlohmann::json row;
        row["role_id"] = role->id();
        row["name"] = role->name();
        render_reach(role->scope(), role->capabilities(), row);
        row["permissions"] =
            join_comma(zetta::governance::permissions_of(role->capabilities(), now));
        std::vector<std::string> scoped;
        for (const auto& grant : role->permissions().scoped_grants) {
            scoped.push_back(render_scoped_grant(grant));
        }
        row["scoped_grants"] = join_comma(scoped);
        row["created"] = role->created();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per user↔role assignment for the user_roles system table (#746) —
// the many-to-many that feeds each user's effective permissions. Sourced from
// Node::user_roles(); the user/role names are resolved for legibility.
std::vector<nlohmann::json> user_role_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& assignment : node.user_roles()) {
        nlohmann::json row;
        row["user_id"] = assignment->user_id();
        row["user"] = node.find_user_by_id(assignment->user_id())
                          ? node.find_user_by_id(assignment->user_id())->name()
                          : std::string{};
        row["role_id"] = assignment->role_id();
        row["role"] = node.find_role_by_id(assignment->role_id())
                          ? node.find_role_by_id(assignment->role_id())->name()
                          : std::string{};
        rows.push_back(std::move(row));
    }
    return rows;
}

// ===== Row-level security (RLS, #748) — predicate (de)serialization + eval =====
//
// A policy's USING / WITH CHECK predicate is a query::PolicyPredicate (ast.hpp).
// The executor serializes it to JSON for the governance Policy store (opaque
// there), reads it back on enforcement, and evaluates it against a row + the
// acting identity — REUSING the WHERE comparison primitive (compare_cell), so
// there is no second predicate language. All helpers are file-local: only the
// executor parses, stores, renders, and evaluates policy predicates.

// The stable string spelling of a comparison, for the serialized predicate.
std::string_view compare_op_token(CompareOp op) {
    switch (op) {
        case CompareOp::Equal:        return "eq";
        case CompareOp::NotEqual:     return "ne";
        case CompareOp::Less:         return "lt";
        case CompareOp::LessEqual:    return "le";
        case CompareOp::Greater:      return "gt";
        case CompareOp::GreaterEqual: return "ge";
    }
    return "eq";
}
bool compare_op_from_token(std::string_view text, CompareOp& out) {
    if (text == "eq") { out = CompareOp::Equal; return true; }
    if (text == "ne") { out = CompareOp::NotEqual; return true; }
    if (text == "lt") { out = CompareOp::Less; return true; }
    if (text == "le") { out = CompareOp::LessEqual; return true; }
    if (text == "gt") { out = CompareOp::Greater; return true; }
    if (text == "ge") { out = CompareOp::GreaterEqual; return true; }
    return false;
}

std::string_view policy_term_kind_token(PolicyTerm::Kind kind) {
    switch (kind) {
        case PolicyTerm::Kind::Column:       return "column";
        case PolicyTerm::Kind::Literal:      return "literal";
        case PolicyTerm::Kind::CurrentUser:  return "current_user";
        case PolicyTerm::Kind::CurrentRoles: return "current_roles";
    }
    return "literal";
}

std::string_view literal_type_token(PolicyTerm::LiteralType type) {
    switch (type) {
        case PolicyTerm::LiteralType::String:  return "string";
        case PolicyTerm::LiteralType::Number:  return "number";
        case PolicyTerm::LiteralType::Boolean: return "boolean";
        case PolicyTerm::LiteralType::Null:    return "null";
    }
    return "string";
}
bool literal_type_from_token(std::string_view text, PolicyTerm::LiteralType& out) {
    if (text == "string")  { out = PolicyTerm::LiteralType::String;  return true; }
    if (text == "number")  { out = PolicyTerm::LiteralType::Number;  return true; }
    if (text == "boolean") { out = PolicyTerm::LiteralType::Boolean; return true; }
    if (text == "null")    { out = PolicyTerm::LiteralType::Null;    return true; }
    return false;
}

// Decode a literal term's raw text + type tag into its JSON value (the executor
// side of the parser's json-free predicate representation).
nlohmann::json decode_policy_literal(const PolicyTerm& term) {
    switch (term.literal_type) {
        case PolicyTerm::LiteralType::String:
            return nlohmann::json(term.literal);
        case PolicyTerm::LiteralType::Boolean:
            return nlohmann::json(term.literal == "true");
        case PolicyTerm::LiteralType::Null:
            return nlohmann::json();
        case PolicyTerm::LiteralType::Number: {
            auto value = nlohmann::json::parse(term.literal, nullptr,
                                               /*allow_exceptions=*/false);
            if (value.is_discarded() || !value.is_number()) return nlohmann::json();
            return value;
        }
    }
    return nlohmann::json();
}

nlohmann::json policy_term_to_json(const PolicyTerm& term) {
    nlohmann::json out;
    out["kind"] = std::string(policy_term_kind_token(term.kind));
    if (term.kind == PolicyTerm::Kind::Column) {
        out["column"] = term.column;
    } else if (term.kind == PolicyTerm::Kind::Literal) {
        out["literal"] = term.literal;  // raw text; decoded at evaluation
        out["literal_type"] = std::string(literal_type_token(term.literal_type));
    }
    return out;
}

std::optional<PolicyTerm> policy_term_from_json(const nlohmann::json& document) {
    if (!document.is_object() || !document.contains("kind") ||
        !document["kind"].is_string()) {
        return std::nullopt;
    }
    PolicyTerm term;
    const std::string kind = document["kind"].get<std::string>();
    if (kind == "column") {
        if (!document.contains("column") || !document["column"].is_string()) {
            return std::nullopt;
        }
        term.kind = PolicyTerm::Kind::Column;
        term.column = document["column"].get<std::string>();
    } else if (kind == "literal") {
        term.kind = PolicyTerm::Kind::Literal;
        term.literal = document.value("literal", std::string{});
        PolicyTerm::LiteralType type = PolicyTerm::LiteralType::String;
        if (document.contains("literal_type")) {
            if (!document["literal_type"].is_string() ||
                !literal_type_from_token(
                    document["literal_type"].get<std::string>(), type)) {
                return std::nullopt;
            }
        }
        term.literal_type = type;
    } else if (kind == "current_user") {
        term.kind = PolicyTerm::Kind::CurrentUser;
    } else if (kind == "current_roles") {
        term.kind = PolicyTerm::Kind::CurrentRoles;
    } else {
        return std::nullopt;
    }
    return term;
}

nlohmann::json policy_predicate_to_json(const PolicyPredicate& predicate) {
    nlohmann::json out;
    if (predicate.always_true) {
        out["always_true"] = true;
        return out;
    }
    out["lhs"] = policy_term_to_json(predicate.lhs);
    out["op"] = std::string(compare_op_token(predicate.op));
    out["rhs"] = policy_term_to_json(predicate.rhs);
    if (predicate.rhs_any) out["any"] = true;
    return out;
}

std::optional<PolicyPredicate> policy_predicate_from_json(
    const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    PolicyPredicate predicate;
    if (document.value("always_true", false)) {
        predicate.always_true = true;
        return predicate;
    }
    if (!document.contains("lhs") || !document.contains("op") ||
        !document.contains("rhs") || !document["op"].is_string()) {
        return std::nullopt;
    }
    const auto lhs = policy_term_from_json(document["lhs"]);
    const auto rhs = policy_term_from_json(document["rhs"]);
    if (!lhs || !rhs) return std::nullopt;
    if (!compare_op_from_token(document["op"].get<std::string>(), predicate.op)) {
        return std::nullopt;
    }
    predicate.lhs = *lhs;
    predicate.rhs = *rhs;
    predicate.rhs_any = document.value("any", false);
    return predicate;
}

// Render a term / predicate back to SQL-ish text for the system.policies view.
std::string policy_term_to_sql(const PolicyTerm& term) {
    switch (term.kind) {
        case PolicyTerm::Kind::Column:       return term.column;
        case PolicyTerm::Kind::CurrentUser:  return "current_user()";
        case PolicyTerm::Kind::CurrentRoles: return "current_roles()";
        case PolicyTerm::Kind::Literal:
            switch (term.literal_type) {
                case PolicyTerm::LiteralType::String:
                    return "'" + term.literal + "'";
                case PolicyTerm::LiteralType::Null:
                    return "null";
                case PolicyTerm::LiteralType::Number:
                case PolicyTerm::LiteralType::Boolean:
                    return term.literal;
            }
    }
    return "";
}
std::string policy_predicate_to_sql(const PolicyPredicate& predicate) {
    if (predicate.always_true) return "true";
    std::string sql = policy_term_to_sql(predicate.lhs) + " ";
    switch (predicate.op) {
        case CompareOp::Equal:        sql += "="; break;
        case CompareOp::NotEqual:     sql += "<>"; break;
        case CompareOp::Less:         sql += "<"; break;
        case CompareOp::LessEqual:    sql += "<="; break;
        case CompareOp::Greater:      sql += ">"; break;
        case CompareOp::GreaterEqual: sql += ">="; break;
    }
    sql += " ";
    if (predicate.rhs_any) {
        sql += "ANY(" + policy_term_to_sql(predicate.rhs) + ")";
    } else {
        sql += policy_term_to_sql(predicate.rhs);
    }
    return sql;
}

// Resolve a term to a scalar JSON value against a row + the acting identity, or
// nullopt when it cannot resolve (an absent row column, or a bare current_roles()
// used outside ANY()). current_user() -> the acting user's name.
std::optional<nlohmann::json> resolve_policy_term(
    const PolicyTerm& term, const nlohmann::json& row,
    const std::string& current_user) {
    // Explicit in_place construction: `std::optional<json>` from a bare `json`
    // is ambiguous (json's templated conversion operator competes with optional's
    // converting constructor), so build the contained value directly.
    switch (term.kind) {
        case PolicyTerm::Kind::Column:
            if (!row.contains(term.column)) return std::nullopt;
            return std::optional<nlohmann::json>(std::in_place, row.at(term.column));
        case PolicyTerm::Kind::Literal:
            return std::optional<nlohmann::json>(std::in_place,
                                                 decode_policy_literal(term));
        case PolicyTerm::Kind::CurrentUser:
            return std::optional<nlohmann::json>(std::in_place, current_user);
        case PolicyTerm::Kind::CurrentRoles:
            return std::nullopt;  // only meaningful as the ANY() array
    }
    return std::nullopt;
}

// Whether a row satisfies a policy predicate for the acting identity — the RLS
// counterpart of row_matches, reusing compare_cell for the comparison. A membership
// predicate (`x = ANY(current_roles())`) tests set membership against the caller's
// roles; every other form resolves both sides and compares. A predicate that
// cannot resolve fails closed (the row does not match) — never widens access.
bool policy_predicate_matches(const PolicyPredicate& predicate,
                              const nlohmann::json& row,
                              const std::string& current_user,
                              const std::vector<std::string>& current_roles) {
    if (predicate.always_true) return true;
    // `<lhs> = ANY(current_roles())` — a membership test over the caller's roles.
    if (predicate.rhs_any) {
        if (predicate.rhs.kind != PolicyTerm::Kind::CurrentRoles) return false;
        if (predicate.op != CompareOp::Equal &&
            predicate.op != CompareOp::NotEqual) {
            return false;  // only = / <> ANY(...) are supported
        }
        const auto lhs = resolve_policy_term(predicate.lhs, row, current_user);
        if (!lhs) return false;
        bool member = false;
        for (const std::string& role : current_roles) {
            if (nlohmann::json(role) == *lhs) { member = true; break; }
        }
        return predicate.op == CompareOp::Equal ? member : !member;
    }
    const auto lhs = resolve_policy_term(predicate.lhs, row, current_user);
    const auto rhs = resolve_policy_term(predicate.rhs, row, current_user);
    if (!lhs || !rhs) return false;
    return compare_cell(*lhs, predicate.op, *rhs);
}

// One JSON row per row-security policy for the system.policies view (#748).
// Sourced from Node::policies(); renders the USING / WITH CHECK predicates back to
// text for legibility (no secret material — a policy carries none).
std::vector<nlohmann::json> policy_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& policy : node.policies()) {
        const zetta::governance::PolicyValue& v = policy->value();
        nlohmann::json row;
        row["policy_id"] = policy->id();
        row["name"] = v.name;
        row["table"] = v.database + "." + v.table;
        row["command"] = v.command;
        std::string using_sql;
        if (!v.using_predicate.is_null()) {
            if (auto p = policy_predicate_from_json(v.using_predicate)) {
                using_sql = policy_predicate_to_sql(*p);
            }
        }
        std::string check_sql;
        if (!v.check_predicate.is_null()) {
            if (auto p = policy_predicate_from_json(v.check_predicate)) {
                check_sql = policy_predicate_to_sql(*p);
            }
        }
        row["using"] = using_sql;
        row["check"] = check_sql;
        row["roles"] = join_comma(v.roles);
        row["created"] = v.created;
        rows.push_back(std::move(row));
    }
    return rows;
}

// The acting identity's RLS context: whether it BYPASSES row security entirely
// (admin / unrestricted / trusted-local — see Node::bypasses_row_security), plus
// the current_user() name and current_roles() set the predicates resolve against.
// A bypassing identity carries no user/roles — it never evaluates a predicate.
struct RlsContext {
    bool bypass = false;
    std::string user;
    std::vector<std::string> roles;
};

RlsContext rls_context(const Node& node, const std::string& acting_id,
                       bool acting_is_user) {
    RlsContext ctx;
    ctx.bypass = node.bypasses_row_security(acting_id, acting_is_user);
    if (ctx.bypass) return ctx;
    if (acting_is_user) {  // only a user session has current_user/current_roles
        if (auto user = node.find_user_by_id(acting_id)) ctx.user = user->name();
        for (const std::string& role_id : node.roles_for_user(acting_id)) {
            if (auto role = node.find_role_by_id(role_id)) {
                ctx.roles.push_back(role->name());
            }
        }
    }
    return ctx;
}

bool policy_command_covers(const std::string& command, bool write) {
    if (command == "all") return true;
    return write ? (command == "insert" || command == "update")
                 : (command == "select");
}

// Whether a policy applies to the acting identity: its role set intersects the
// policy's role list, or the policy names no roles (applies to every role).
bool policy_applies_to_roles(const zetta::governance::PolicyValue& value,
                             const std::vector<std::string>& roles) {
    if (value.roles.empty()) return true;
    for (const std::string& want : value.roles) {
        for (const std::string& have : roles) {
            if (want == have) return true;
        }
    }
    return false;
}

// The row-security decision for one (db.table, access class) under an identity.
// `engaged` is true when the table has ANY policy covering this access class
// (regardless of caller roles — RLS is per-table opt-in by creating a policy);
// `predicates` are the applicable ones (roles match), OR-ed permissively.
struct RlsFilter {
    bool bypass = false;
    bool engaged = false;
    std::vector<PolicyPredicate> predicates;
};

RlsFilter build_rls_filter(const Node& node, const RlsContext& ctx,
                           const std::string& database, const std::string& table,
                           bool write) {
    RlsFilter filter;
    filter.bypass = ctx.bypass;
    if (ctx.bypass) return filter;
    for (const auto& policy : node.policies()) {
        const zetta::governance::PolicyValue& v = policy->value();
        if (v.database != database || v.table != table) continue;
        if (!policy_command_covers(v.command, write)) continue;
        filter.engaged = true;  // a policy governs this table+class -> RLS is on
        if (!policy_applies_to_roles(v, ctx.roles)) continue;  // not this caller
        // read -> USING; write -> WITH CHECK, falling back to USING when a write
        // policy carries no explicit WITH CHECK (Postgres-like).
        const nlohmann::json& source =
            write ? (!v.check_predicate.is_null() ? v.check_predicate
                                                  : v.using_predicate)
                  : v.using_predicate;
        if (source.is_null()) {
            PolicyPredicate always;  // an applicable, predicate-less policy admits its rows
            always.always_true = true;
            filter.predicates.push_back(always);
        } else if (auto parsed = policy_predicate_from_json(source)) {
            filter.predicates.push_back(*parsed);
        }
        // A malformed predicate contributes nothing (fail-closed).
    }
    return filter;
}

// Whether a row is permitted under the filter. Bypass or a table with no policy
// (not engaged) admits everything; an engaged table with no applicable predicate
// admits NOTHING (deny-by-default); otherwise the row must satisfy at least one
// applicable predicate (permissive OR of the policies).
bool rls_row_permitted(const RlsFilter& filter, const nlohmann::json& row,
                       const RlsContext& ctx) {
    if (filter.bypass || !filter.engaged) return true;
    for (const PolicyPredicate& predicate : filter.predicates) {
        if (policy_predicate_matches(predicate, row, ctx.user, ctx.roles)) {
            return true;
        }
    }
    return false;
}

// One JSON row per saved source connection for the connections system table
// (#827). Sourced from connect::list_connections(). Like system.tokens this view
// deliberately exposes NO credential: the token lives only in the replicated
// store row that lets every node present it, and every read surface renders it
// through SavedConnection::to_redacted_json(), which is built from
// ConnectorConfig::to_json() — a serialization that cannot emit the token.
std::vector<nlohmann::json> connection_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const zetta::connect::SavedConnection& saved :
         zetta::connect::list_connections(node)) {
        const nlohmann::json connector = saved.config.to_json();
        nlohmann::json row;
        row["name"] = saved.name;
        row["type"] = saved.config.type;
        row["account"] = saved.config.account;
        row["auth"] = saved.config.auth;
        row["database"] = saved.config.database;
        row["schema"] = saved.config.schema;
        row["warehouse"] = saved.config.warehouse;
        row["role"] = saved.config.role;
        row["created"] = saved.created;
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per token for the tokens system table — the bearer credentials
// (FEAT-9 / #744). Sourced from Node::tokens(). This view deliberately exposes
// NO secret material: only the token id, its owning user, type, created, optional
// expiry, and revoked flag — never the hash or salt (the plaintext is shown once
// at creation and never stored). This is the "system.tokens must not leak the
// secret" guarantee.
std::vector<nlohmann::json> token_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& token : node.tokens()) {
        nlohmann::json row;
        row["token_id"] = token->id();
        row["user_id"] = token->user_id();
        if (auto user = node.find_user_by_id(token->user_id())) {
            row["user"] = user->name();
        } else {
            row["user"] = "";
        }
        row["type"] = std::string(to_string(token->type()));
        row["created"] = token->created();
        row["expires_at"] = token->expires_at();
        row["revoked"] = token->revoked();
        rows.push_back(std::move(row));
    }
    return rows;
}

// One JSON row per retained audit event (ENT-23 / FEAT-9), including structured
// polling totals. Sourced from Node::audit_events() in chronological order.
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
        if (event->value().summary) {
            row["calls"] = event->value().summary->calls;
            row["events_delivered"] =
                event->value().summary->events_delivered;
            row["scope"] = event->value().summary->scope;
            row["window_started"] =
                event->value().summary->window_started;
            row["window_ended"] = event->value().summary->window_ended;
        } else {
            row["calls"] = nullptr;
            row["events_delivered"] = nullptr;
            row["scope"] = nullptr;
            row["window_started"] = nullptr;
            row["window_ended"] = nullptr;
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<nlohmann::json> ledger_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const auto& entry : node.ledger_entries()) {
        nlohmann::json row = entry->metadata();
        row["ledger_id"] = entry->id();
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
// One JSON row per stored large object (file / blob), across every database — computed
// live from each Database's LargeObjectStore (RULE-7; no CRDT-backed copy). `body_present`
// is false on a node that has the replicated metadata but not the transferred body yet.
std::vector<nlohmann::json> large_object_rows(const Node& node) {
    std::vector<nlohmann::json> rows;
    for (const std::string& database_name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database = node.find_database(database_name);
        if (!database) continue;
        for (const zetta::data::LargeObject& object : database->large_objects().list()) {
            nlohmann::json row;
            row["object_id"] = object.id;
            row["name"] = object.name;
            row["database"] = database_name;
            row["size_bytes"] = object.size;
            row["sha256"] = object.sha256;
            row["body_present"] = object.body_present;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

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
        // Whether this node can see the WHOLE dataset (#837). `expected_*` come
        // from the dataset's partition manifest (0 when it has none); `degraded`
        // says `rows` above is a fraction, which is the state a reader must see
        // instead of a plausible-looking undercount.
        row["degraded"] = dataset.degraded;
        row["degraded_reason"] = dataset.degraded_reason;
        row["expected_partition_count"] =
            static_cast<int>(dataset.expected_partitions);
        row["expected_rows"] = dataset.expected_rows;
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
    row["process_rss_bytes"] =
        number_or_null(metrics.process_memory_available, metrics.process_rss_bytes);
    row["process_peak_rss_bytes"] = number_or_null(
        metrics.process_memory_available, metrics.process_peak_rss_bytes);
    row["allocator_in_use_bytes"] = number_or_null(
        metrics.allocator_memory_available, metrics.allocator_in_use_bytes);
    row["allocator_free_held_bytes"] = number_or_null(
        metrics.allocator_memory_available, metrics.allocator_free_held_bytes);
    row["attributed_memory_bytes"] = number_or_null(
        metrics.allocator_memory_available, metrics.attributed_memory_bytes);
    row["unattributed_memory_bytes"] = number_or_null(
        metrics.allocator_memory_available, metrics.unattributed_memory_bytes);
    row["memory_explained_percent"] = number_or_null(
        metrics.allocator_memory_available, metrics.memory_explained_percent);
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
    row["audit_protocol_version"] = zetta::governance::kAuditProtocolVersion;
    row["audit_watermarks"] = node.audit_watermarks().dump();
    row["audit_memory_bytes"] = node.audit_memory_bytes();
    if (const auto* pubsub = node.pubsub()) {
        row["pubsub_dispatch_mode"] =
            std::string(zetta::pubsub::to_string(pubsub->dispatch_mode()));
        row["pubsub_dispatch_threads"] = pubsub->dispatch_threads();
        row["pubsub_dispatch_keys"] = pubsub->dispatch_keys();
        row["pubsub_topics_per_dispatch_thread"] =
            pubsub->limits().topics_per_thread;
        row["pubsub_dispatch_min_threads"] =
            pubsub->limits().dispatch_min_threads;
        row["pubsub_dispatch_max_threads"] =
            pubsub->limits().dispatch_max_threads;
    } else {
        row["pubsub_dispatch_mode"] = nlohmann::json();
        row["pubsub_dispatch_threads"] = nlohmann::json();
        row["pubsub_dispatch_keys"] = nlohmann::json();
        row["pubsub_topics_per_dispatch_thread"] = nlohmann::json();
        row["pubsub_dispatch_min_threads"] = nlohmann::json();
        row["pubsub_dispatch_max_threads"] = nlohmann::json();
    }

    return {std::move(row)};
}

std::vector<nlohmann::json> memory_rows(const Node& node) {
    const auto snapshot = node.memory_snapshot();
    std::vector<nlohmann::json> rows;
    const auto append = [&](std::string subsystem, std::uint64_t bytes,
                            std::uint64_t items, std::uint64_t reserved_bytes,
                            double share) {
        nlohmann::json row;
        row["subsystem"] = std::move(subsystem);
        row["bytes"] = bytes;
        row["items"] = items;
        row["reserved_bytes"] = reserved_bytes;
        row["share_of_in_use"] = share;
        row["allocator_in_use_bytes"] = snapshot.allocator.in_use_bytes;
        row["allocator_free_held_bytes"] = snapshot.allocator.free_held_bytes;
        row["rss_bytes"] = snapshot.allocator.rss_bytes;
        row["peak_rss_bytes"] = snapshot.allocator.peak_rss_bytes;
        row["anonymous_bytes"] = snapshot.allocator.anonymous_bytes;
        row["sampled_at_ms"] = snapshot.sampled_at_ms;
        rows.push_back(std::move(row));
    };
    for (const auto& subsystem : snapshot.subsystems) {
        append(subsystem.name, subsystem.bytes, subsystem.items,
               subsystem.reserved_bytes, subsystem.share_of_in_use);
    }
    const double unattributed_share = snapshot.allocator.in_use_bytes == 0
                                          ? 0.0
                                          : 100.0 * snapshot.unattributed_bytes /
                                                snapshot.allocator.in_use_bytes;
    append("unattributed", snapshot.unattributed_bytes, 0, 0,
           unattributed_share);
    return rows;
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
// Postgres' order for one ORDER BY key: numbers numerically, then booleans,
// then strings; NULL (or a missing field) greatest, so last ascending and first
// descending. Values of different kinds order by kind.
int order_rank(const nlohmann::json& value) {
    if (value.is_null()) return 4;
    if (value.is_number()) return 0;
    if (value.is_boolean()) return 1;
    if (value.is_string()) return 2;
    return 3;
}

int compare_for_order(const nlohmann::json& a, const nlohmann::json& b) {
    const int ra = order_rank(a), rb = order_rank(b);
    if (ra != rb) return ra < rb ? -1 : 1;
    if (a.is_number()) {
        const double x = a.get<double>(), y = b.get<double>();
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (a.is_boolean()) return a.get<bool>() == b.get<bool>() ? 0 : (a.get<bool>() ? 1 : -1);
    if (a.is_string()) {
        const int c = a.get_ref<const std::string&>().compare(b.get_ref<const std::string&>());
        return c < 0 ? -1 : (c > 0 ? 1 : 0);
    }
    const std::string x = a.dump(), y = b.dump();
    return x < y ? -1 : (x > y ? 1 : 0);
}

// ORDER BY, OFFSET and LIMIT over a finished SELECT result. Returns an error
// when a key names a column the result does not have.
std::optional<std::string> apply_order_and_limit(const SelectStmt& statement,
                                                 QueryResult& result) {
    if (!statement.order_by.empty()) {
        for (const OrderKey& key : statement.order_by) {
            const bool known =
                statement.all ||
                std::find(result.columns.begin(), result.columns.end(), key.column) !=
                    result.columns.end() ||
                (!result.rows.empty() && result.rows.front().contains(key.column));
            if (!known && !result.rows.empty()) {
                return "ORDER BY \"" + key.column +
                       "\": order by a column of the result (a selected column, a "
                       "GROUP BY key or an aggregate's alias)";
            }
        }
        static const nlohmann::json null_value;
        std::stable_sort(result.rows.begin(), result.rows.end(),
                         [&](const nlohmann::json& a, const nlohmann::json& b) {
                             for (const OrderKey& key : statement.order_by) {
                                 const auto fa = a.find(key.column);
                                 const auto fb = b.find(key.column);
                                 int c = compare_for_order(
                                     fa == a.end() ? null_value : *fa,
                                     fb == b.end() ? null_value : *fb);
                                 if (key.descending) c = -c;
                                 if (c != 0) return c < 0;
                             }
                             return false;
                         });
    }
    if (statement.offset > 0) {
        const std::size_t drop = std::min(statement.offset, result.rows.size());
        result.rows.erase(result.rows.begin(),
                          result.rows.begin() + static_cast<std::ptrdiff_t>(drop));
    }
    if (statement.limit && result.rows.size() > *statement.limit) {
        result.rows.resize(*statement.limit);
    }
    if (!statement.order_by.empty() || statement.offset > 0 || statement.limit) {
        result.message = "SELECT " + std::to_string(result.rows.size());
    }
    return std::nullopt;
}

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

    // A virtual table (#809 slice 3) is referenced by its unqualified, global
    // name; a real table in the current database takes precedence (a virtual
    // table never shadows one). Resolves to the definition when `ref` is an
    // unqualified name that is a virtual table and is NOT shadowed by a real
    // table — used by SELECT (to project) and the write paths (to reject).
    const auto resolve_virtual_table =
        [&](const TableRef& ref) -> std::optional<zetta::data::VirtualTable> {
        if (!ref.database.empty()) return std::nullopt;
        auto current = node_.find_database(current_database);
        if (current && current->find_table(ref.table)) return std::nullopt;
        return node_.find_virtual_table(ref.table);
    };

    QueryResult result = std::visit(
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
            [&](const CreateVirtualTableStmt& s) -> QueryResult {
                // OVER CRDT is required (the must-extend-a-CRDT invariant). The
                // grammar can't reach here without it, but a bare '' is possible.
                if (s.crdt_prefix.empty()) {
                    return error("CREATE VIRTUAL TABLE requires a non-empty OVER "
                                 "CRDT '<prefix>' (a virtual table must extend a "
                                 "CRDT key-family)");
                }
                // Exactly one DURABLE KEY column — the convergent row identity.
                int key_count = 0;
                std::unordered_set<std::string> seen;
                for (const VirtualColumnDef& column : s.columns) {
                    if (!seen.insert(column.name).second) {
                        return error("duplicate column \"" + column.name +
                                     "\" in CREATE VIRTUAL TABLE");
                    }
                    if (column.is_key) ++key_count;
                }
                if (key_count != 1) {
                    return error("CREATE VIRTUAL TABLE needs exactly one DURABLE "
                                 "KEY column (the CRDT key — the id after the "
                                 "prefix)");
                }
                // The name must not shadow a system table (or the bare zetta_nodes
                // alias), nor an existing table in the current database.
                for (const std::string_view sys : kSystemTableNames) {
                    if (s.name == sys) {
                        return error("\"" + s.name +
                                     "\" is a reserved system-table name");
                    }
                }
                if (s.name == "zetta_nodes") {
                    return error("\"zetta_nodes\" is a reserved system-table alias");
                }
                if (auto current = node_.find_database(current_database)) {
                    if (current->find_table(s.name)) {
                        return error("\"" + s.name +
                                     "\" already names a table in \"" +
                                     current_database + "\"");
                    }
                }
                // Translate the parsed statement into the durable definition.
                zetta::data::VirtualTable definition;
                definition.name = s.name;
                definition.crdt_prefix = s.crdt_prefix;
                for (const VirtualColumnDef& column : s.columns) {
                    zetta::data::VirtualColumn out;
                    out.name = column.name;
                    out.type = column.type;
                    out.is_key = column.is_key;
                    switch (column.tier) {
                        case VirtualTableTier::Durable:
                            out.tier = zetta::data::VirtualColumnTier::Durable;
                            break;
                        case VirtualTableTier::EphemeralLocal:
                            out.tier =
                                zetta::data::VirtualColumnTier::EphemeralLocal;
                            break;
                        case VirtualTableTier::EphemeralSynced:
                            out.tier =
                                zetta::data::VirtualColumnTier::EphemeralSynced;
                            break;
                    }
                    definition.columns.push_back(std::move(out));
                }
                if (!node_.create_virtual_table(definition)) {
                    return error("virtual table \"" + s.name +
                                 "\" already exists, or this node has no replicated "
                                 "CRDT to store the definition (needs a cluster / "
                                 "ReplicationManager)");
                }
                return {.message = "CREATE VIRTUAL TABLE"};
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
                if (resolve_virtual_table(s.table)) {
                    return error("virtual table \"" + s.table.table +
                                 "\" is read-only — durable writes go to the base "
                                 "entity's own path");
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
                // Object-scoped write enforcement (#747): the acting identity must
                // be able to write this table, and every target column.
                const zetta::governance::ColumnAccess write_access =
                    node_.column_scope(acting_key_, acting_is_user_, kSqlWrite,
                                       db_name(s.table), s.table.table);
                if (!write_access.table_allowed) {
                    return error("permission denied: no sql:write grant on \"" +
                                 db_name(s.table) + "." + s.table.table + "\"");
                }
                if (!write_access.all_columns) {
                    for (const std::string& col : s.columns) {
                        if (!write_access.allows(col)) {
                            return error("permission denied: no sql:write grant on "
                                         "column \"" + col + "\" of \"" +
                                         db_name(s.table) + "." + s.table.table +
                                         "\"");
                        }
                    }
                }
                // Row-level security (#748): a WITH CHECK write filter rejects a
                // row the caller may not write. Deny-by-default — a table with a
                // write policy admits a row only when it satisfies an applicable
                // WITH CHECK; admin / unrestricted / the local session bypass.
                const RlsContext rls = rls_context(node_, acting_key_,
                                                    acting_is_user_);
                const RlsFilter write_filter = build_rls_filter(
                    node_, rls, db_name(s.table), s.table.table, /*write=*/true);
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
                    if (!rls_row_permitted(write_filter, row, rls)) {
                        return error("permission denied: row violates the "
                                     "row-security WITH CHECK policy on \"" +
                                     db_name(s.table) + "." + s.table.table + "\"");
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
                if (resolve_virtual_table(s.table)) {
                    return error("virtual table \"" + s.table.table +
                                 "\" is read-only — durable writes go to the base "
                                 "entity's own path");
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
                    const zetta::storage::ParquetLazyPtr row = store.at(i);
                    if (!row) return error("row " + store.id_at(i) +
                                           " could not be read: " + row.error());
                    if (!row_matches(*row, s.where)) continue;
                    nlohmann::json updated = *row;
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
                if (resolve_virtual_table(s.table)) {
                    return error("virtual table \"" + s.table.table +
                                 "\" is read-only — durable writes go to the base "
                                 "entity's own path");
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
                    const zetta::storage::ParquetLazyPtr row = store.at(i);
                    if (!row) return error("row " + store.id_at(i) +
                                           " could not be read: " + row.error());
                    if (row_matches(*row, s.where)) ids.push_back(store.id_at(i));
                }
                for (const std::string& id : ids) table->delete_row(id);
                return {.message = "DELETE " + std::to_string(ids.size())};
            },
            [&](const TruncateStmt& s) -> QueryResult {
                if (db_name(s.table) == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                if (resolve_virtual_table(s.table)) {
                    return error("virtual table \"" + s.table.table +
                                 "\" is read-only — durable writes go to the base "
                                 "entity's own path");
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
                    if (sys == kTestCasesTable) {
                        return select_over_rows(s, test_case_rows());
                    }
                    if (sys == kDeploymentsTable) {
                        return select_over_rows(s, deployment_rows(node_));
                    }
                    if (sys == kModelsTable) {
                        return select_over_rows(s, model_rows(node_));
                    }
                    if (sys == kWorkspaceProfilesTable) {
                        return select_over_rows(s, workspace_profile_rows(node_));
                    }
                    if (sys == kUsageTable) {
                        return select_over_rows(s, usage_rows(node_));
                    }
                    if (sys == kPlansTable) {
                        return select_over_rows(s, plan_rows(node_));
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
                    if (sys == kPubSubTopicsTable) {
                        return select_over_rows(
                            s, node_.pubsub() != nullptr
                                   ? zetta::pubsub::pubsub_topic_rows(*node_.pubsub())
                                   : std::vector<nlohmann::json>{});
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
                    if (sys == kUsersTable) {
                        return select_over_rows(s, user_rows(node_));
                    }
                    if (sys == kRolesTable) {
                        return select_over_rows(s, role_rows(node_));
                    }
                    if (sys == kUserRolesTable) {
                        return select_over_rows(s, user_role_rows(node_));
                    }
                    if (sys == kPoliciesTable) {
                        return select_over_rows(s, policy_rows(node_));
                    }
                    if (sys == kTokensTable) {
                        return select_over_rows(s, token_rows(node_));
                    }
                    if (sys == kConnectionsTable) {
                        return select_over_rows(s, connection_rows(node_));
                    }
                    if (sys == kAuditTable) {
                        return select_over_rows(s, audit_rows(node_));
                    }
                    if (sys == kLedgerTable) {
                        return select_over_rows(s, ledger_rows(node_));
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
                    if (sys == kLargeObjectsTable) {
                        return select_over_rows(s, large_object_rows(node_));
                    }
                    if (sys == kCheckpointsTable) {
                        return select_over_rows(s, checkpoint_rows(node_));
                    }
                    if (sys == kNodeMetricsTable) {
                        return select_over_rows(s, node_metrics_rows(node_));
                    }
                    if (sys == kMemoryTable) {
                        return select_over_rows(s, memory_rows(node_));
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
                    return select_over_rows(s,
                                            cluster_node_rows(*cluster_, node_));
                }
                // Virtual table (#809 slice 3): a user-declared read-only table
                // over a CRDT key-family, referenced by its unqualified global
                // name (a real table takes precedence). Its durable rows are
                // projected from the CRDT; WHERE/projection reuse select_over_rows.
                if (auto vtable = resolve_virtual_table(s.table)) {
                    if (s.grouped()) {
                        return error(
                            "GROUP BY is not supported over virtual tables");
                    }
                    return select_over_rows(s, node_.virtual_table_rows(*vtable));
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
                // Object-scoped read enforcement (#747): the acting identity must
                // be able to read this table; columns it lacks sql:read on are
                // masked (nulled) in the output.
                const zetta::governance::ColumnAccess read_access =
                    node_.column_scope(acting_key_, acting_is_user_, kSqlRead,
                                       db_name(s.table), s.table.table);
                if (!read_access.table_allowed) {
                    return error("permission denied: no sql:read grant on \"" +
                                 db_name(s.table) + "." + s.table.table + "\"");
                }
                // Block filtering/grouping on a column the caller can't read — a
                // WHERE/GROUP BY over a masked column would otherwise leak it.
                if (!read_access.all_columns) {
                    if (s.where && !read_access.allows(s.where->column)) {
                        return error("permission denied: no sql:read grant on "
                                     "column \"" + s.where->column +
                                     "\" used in WHERE");
                    }
                    for (const std::string& key : s.group_by) {
                        if (!read_access.allows(key)) {
                            return error("permission denied: no sql:read grant on "
                                         "column \"" + key + "\" used in GROUP BY");
                        }
                    }
                    for (const GroupedItem& item : s.items) {
                        if (!item.column.empty() &&
                            !read_access.allows(item.column)) {
                            return error("permission denied: no sql:read grant on "
                                         "column \"" + item.column + "\"");
                        }
                    }
                }
                // Row-level security (#748): AND the applicable policies' USING
                // read filter into the row scan — the caller sees only rows it may.
                // Deny-by-default: a table with a read policy shows a non-bypass
                // caller only rows an applicable USING admits (none, if no policy
                // applies to its roles). admin / unrestricted / local bypass.
                const RlsContext rls = rls_context(node_, acting_key_,
                                                   acting_is_user_);
                const RlsFilter read_filter = build_rls_filter(
                    node_, rls, db_name(s.table), s.table.table, /*write=*/false);
                // An aggregating SELECT (GROUP BY) runs through the DDF engine,
                // which filters numerically and can't apply a row-security
                // predicate — refuse it on a policy-protected table (for a
                // non-bypass caller) rather than leak aggregates over hidden rows.
                // A partition-only table (#833) holds its rows in the partition
                // bodies its owners hold, not in this node's store — so the
                // answer is scattered to the owners and merged, never read
                // locally. A node-local scan would silently answer from
                // whatever slice happened to be here.
                if (table->partition_only()) {
                    if (!read_filter.bypass && read_filter.engaged) {
                        return error("row-level security is enabled on \"" +
                                     db_name(s.table) + "." + s.table.table +
                                     "\": it is a partition-only dataset, whose "
                                     "rows are filtered on their owner — query a "
                                     "replicated table instead");
                    }
                    const SelectOutcome scattered = select_over_partitions(
                        node_, node_.distributed_select(), s, table->id());
                    if (!scattered.ok()) return error(scattered.error);
                    QueryResult r;
                    r.columns = s.grouped() ? scattered.columns
                                            : (s.all ? std::vector<std::string>{}
                                                     : s.columns);
                    r.rows = scattered.rows;
                    // Column-level access stays with the coordinator: the owner
                    // filtered + projected, this node knows the caller.
                    if (!read_access.all_columns) {
                        for (nlohmann::json& row : r.rows) {
                            for (auto field = row.begin(); field != row.end();
                                 ++field) {
                                if (!read_access.allows(field.key())) {
                                    field.value() = nlohmann::json();
                                }
                            }
                        }
                    }
                    r.message = "SELECT " + std::to_string(r.rows.size());
                    return r;
                }
                if (s.grouped()) {
                    if (!read_filter.bypass && read_filter.engaged) {
                        return error("row-level security is enabled on \"" +
                                     db_name(s.table) + "." + s.table.table +
                                     "\": GROUP BY over a policy-protected table is "
                                     "not supported — query it without aggregation");
                    }
                    return execute_grouped(s, *table);
                }
                QueryResult r;
                if (!s.all) r.columns = s.columns;
                const zetta::storage::StorageEngine& store = table->store();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    const zetta::storage::ParquetLazyPtr handle = store.at(i);
                    if (!handle) return error("row " + store.id_at(i) +
                                              " could not be read: " + handle.error());
                    const nlohmann::json& row = *handle;
                    if (!row_matches(row, s.where)) continue;
                    if (!rls_row_permitted(read_filter, row, rls)) continue;
                    if (s.all) {
                        if (read_access.all_columns) {
                            r.rows.push_back(row);
                        } else {
                            // SELECT * — keep every column but redact the ones the
                            // caller can't read (masked to null, not dropped).
                            nlohmann::json masked = nlohmann::json::object();
                            for (auto it = row.begin(); it != row.end(); ++it) {
                                masked[it.key()] = read_access.allows(it.key())
                                                       ? it.value()
                                                       : nlohmann::json();
                            }
                            r.rows.push_back(std::move(masked));
                        }
                    } else {
                        nlohmann::json projected = nlohmann::json::object();
                        for (const std::string& col : s.columns) {
                            const bool visible =
                                read_access.allows(col) && row.contains(col);
                            projected[col] =
                                visible ? row.at(col) : nlohmann::json();
                        }
                        r.rows.push_back(std::move(projected));
                    }
                }
                r.message = "SELECT " + std::to_string(r.rows.size());
                return r;
            },
            [&](const CreateUserStmt& s) -> QueryResult {
                if (!node_.create_user(s.name)) {
                    return error("user \"" + s.name +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE USER"};
            },
            [&](const CreateTokenStmt& s) -> QueryResult {
                auto user = node_.find_user(s.user);
                if (!user) {
                    return error("user \"" + s.user + "\" does not exist");
                }
                const auto type = zetta::governance::token_type_from(s.type);
                if (!type) {
                    return error("unknown token type \"" + s.type +
                                 "\" (want 'access' or 'api')");
                }
                Node::TokenIssue issue = node_.create_token(user->id(), *type);
                if (!issue.token) {
                    return error("failed to issue token for user \"" + s.user +
                                 "\"");
                }
                // Return the plaintext secret ONCE — the only time it is ever
                // surfaced. It is not stored (only its salted hash is).
                QueryResult r;
                r.message = "CREATE TOKEN";
                r.columns = {"token_id", "user", "type", "secret"};
                nlohmann::json row;
                row["token_id"] = issue.token->id();
                row["user"] = s.user;
                row["type"] = std::string(to_string(*type));
                row["secret"] = issue.secret;
                r.rows.push_back(std::move(row));
                return r;
            },
            [&](const CreateRoleStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                if (!node_.create_role(s.name)) {
                    return error("role \"" + s.name +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE ROLE"};
            },
            [&](const DropRoleStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                if (!node_.drop_role(s.name)) {
                    return error("role \"" + s.name + "\" does not exist");
                }
                return {.message = "DROP ROLE"};
            },
            [&](const GrantPermissionStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                if (auto bad = first_unknown_permission(s.permissions)) {
                    return error("unknown permission \"" + *bad +
                                 "\" — not in the permission vocabulary");
                }
                // A bare GRANT adds flat rights; `ON <object>` (#747) adds
                // object-scoped grants instead.
                const bool ok =
                    s.object.kind == GrantObjectKind::RoleWide
                        ? node_.grant_permissions(s.role, s.permissions)
                        : node_.grant_scoped_permissions(
                              s.role, s.permissions, resource_of_object(s.object));
                if (!ok) {
                    return error("role \"" + s.role + "\" does not exist");
                }
                return {.message = "GRANT"};
            },
            [&](const RevokePermissionStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                if (auto bad = first_unknown_permission(s.permissions)) {
                    return error("unknown permission \"" + *bad +
                                 "\" — not in the permission vocabulary");
                }
                const bool ok =
                    s.object.kind == GrantObjectKind::RoleWide
                        ? node_.revoke_permissions(s.role, s.permissions)
                        : node_.revoke_scoped_permissions(
                              s.role, s.permissions, resource_of_object(s.object));
                if (!ok) {
                    return error("role \"" + s.role + "\" does not exist");
                }
                return {.message = "REVOKE"};
            },
            [&](const GrantRoleStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                auto user = node_.find_user(s.user);
                if (!user) {
                    return error("user \"" + s.user + "\" does not exist");
                }
                auto role = node_.find_role(s.role);
                if (!role) {
                    return error("role \"" + s.role + "\" does not exist");
                }
                if (!node_.assign_role(user->id(), role->id())) {
                    return error("failed to assign role \"" + s.role +
                                 "\" to user \"" + s.user + "\"");
                }
                return {.message = "GRANT ROLE"};
            },
            [&](const RevokeRoleStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                auto user = node_.find_user(s.user);
                if (!user) {
                    return error("user \"" + s.user + "\" does not exist");
                }
                auto role = node_.find_role(s.role);
                if (!role) {
                    return error("role \"" + s.role + "\" does not exist");
                }
                if (!node_.unassign_role(user->id(), role->id())) {
                    return error("role \"" + s.role +
                                 "\" is not assigned to user \"" + s.user + "\"");
                }
                return {.message = "REVOKE ROLE"};
            },
            [&](const CreatePolicyStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                const std::string database = db_name(s.table);
                if (database.empty()) {
                    return error("CREATE POLICY needs a database-qualified table "
                                 "(db.table) or a current database");
                }
                if (database == kSystemDatabase) {
                    return error("the \"system\" database is read-only");
                }
                auto policy_db = node_.find_database(database);
                if (!policy_db) {
                    return error("database \"" + database + "\" does not exist");
                }
                if (!policy_db->find_table(s.table.table)) {
                    return error("table \"" + database + "." + s.table.table +
                                 "\" does not exist");
                }
                // Require the predicate the command actually enforces, so a policy
                // can never silently fail open: a read policy needs USING, a write
                // policy needs WITH CHECK (USING is accepted as the check fallback).
                const bool reads = s.command == PolicyCommand::Select ||
                                   s.command == PolicyCommand::All;
                const bool writes = s.command == PolicyCommand::Insert ||
                                    s.command == PolicyCommand::Update ||
                                    s.command == PolicyCommand::All;
                if (reads && !s.using_predicate) {
                    return error("CREATE POLICY FOR SELECT/ALL requires a "
                                 "USING (<predicate>) clause");
                }
                if (writes && !s.using_predicate && !s.check_predicate) {
                    return error("CREATE POLICY FOR INSERT/UPDATE requires a "
                                 "WITH CHECK (<predicate>) clause");
                }
                // A policy scoped to a role that doesn't exist would never apply —
                // fail the statement rather than store a dead rule.
                for (const std::string& role : s.roles) {
                    if (!node_.find_role(role)) {
                        return error("role \"" + role + "\" does not exist");
                    }
                }
                zetta::governance::PolicyValue value;
                value.name = s.name;
                value.database = database;
                value.table = s.table.table;
                value.command = policy_command_string(s.command);
                value.roles = s.roles;
                if (s.using_predicate) {
                    value.using_predicate =
                        policy_predicate_to_json(*s.using_predicate);
                }
                if (s.check_predicate) {
                    value.check_predicate =
                        policy_predicate_to_json(*s.check_predicate);
                }
                if (!node_.create_policy(std::move(value))) {
                    return error("policy \"" + s.name +
                                 "\" already exists or the name is invalid");
                }
                return {.message = "CREATE POLICY"};
            },
            [&](const DropPolicyStmt& s) -> QueryResult {
                if (auto denied = require_permission(kManageRoles)) {
                    return error(*denied);
                }
                if (!node_.drop_policy(s.name)) {
                    return error("policy \"" + s.name + "\" does not exist");
                }
                return {.message = "DROP POLICY"};
            },
        },
        stmt);
    // ORDER BY / LIMIT / OFFSET apply to whichever path answered the SELECT.
    if (const auto* select = std::get_if<SelectStmt>(&stmt); select && result.ok()) {
        if (auto problem = apply_order_and_limit(*select, result)) return error(*problem);
    }
    return result;
}

std::optional<std::string> Executor::require_permission(
    const zetta::governance::Operation& operation) const {
    // Mirror the API layer's three-mode authorization (registry.cpp): a
    // token-resolved user is checked against its effective (role-union)
    // permissions; a legacy AccessKey against its own capabilities; an empty
    // acting key is the trusted unauthenticated-local session (the library / GUI
    // / test path) and is always allowed. Deny-by-default otherwise.
    if (acting_is_user_) {
        auto decision = node_.authorize_user(acting_key_, operation);
        if (!decision) return decision.reason;
    } else if (!acting_key_.empty()) {
        auto decision = node_.authorize(acting_key_, operation);
        if (!decision) return decision.reason;
    }
    return std::nullopt;
}

}  // namespace zetta::query
