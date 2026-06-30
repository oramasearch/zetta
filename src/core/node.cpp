#include "zetta/node.hpp"

#include "zetta/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/executor.hpp"
#include "zetta/gate.hpp"
#include "zetta/index/in_memory_index_backend.hpp"
#ifdef ZETTA_WITH_ORAMACORE
#include "zetta/index/oramacore_fulltext_index.hpp"
#endif
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/partition_rebalance.hpp"
#include "zetta/project_graph.hpp"
#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/timestamp.hpp"
#include "zetta/uuid.hpp"
#include "zetta/version.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {
namespace fs = std::filesystem;

namespace {

// The cluster membership encoded in the document: the id of every node:<id> key,
// sorted ascending so the set is order-independent and comparable across ticks.
// Membership is replicated state (each node seeds its own node:<id> entry, peers
// merge), so a join/leave shows up here — the signal the #517 rebalance keys on.
std::vector<std::string> membership_from_snapshot(const nlohmann::json& snapshot) {
    std::vector<std::string> members;
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kNodePrefix);
        if (!id.empty()) members.push_back(std::move(id));
    }
    std::sort(members.begin(), members.end());
    return members;
}

}  // namespace

Node::Node(NodeConfig config, ReplicationManager* replication)
    : config_(std::move(config)),
      replication_(replication),
      checkpoint_store_(std::filesystem::path(config_.data_dir) / "checkpoints") {
    // Register the always-available index backends so an `index` step resolves
    // out of the box (the per-node container the run path builds + queries
    // through, #528). InMemoryIndexBackend ("memory_search") is the pure-host
    // reference Search backend; OramaCoreFulltextIndex ("oramacore_fulltext") is
    // the native full-text (BM25F) Search backend, registered only when its
    // private submodule was built (ZETTA_WITH_ORAMACORE). A host may add more.
    index_registry_.add(std::make_shared<InMemoryIndexBackend>());
#ifdef ZETTA_WITH_ORAMACORE
    index_registry_.add(std::make_shared<OramaCoreFulltextIndex>());
#endif
}

// Out-of-line (see node.hpp): destroying coordinator_ here means the complete
// compute::JobCoordinator type is visible, and — for a node that reached stop()
// — coordinator_ is already null, so the unwind never touches a dead cluster.
Node::~Node() = default;

std::vector<compute::JobCheckpoint> Node::checkpoints() const {
    // The latest valid checkpoint per job — the read view behind system.checkpoints
    // (RULE-7). Sorted by job id for a stable order; a checkpoint that fails to load
    // (corrupt) is skipped, so the view shows only resumable jobs.
    std::vector<compute::JobCheckpoint> result;
    std::vector<std::string> ids = checkpoint_store_.job_ids();
    std::sort(ids.begin(), ids.end());
    for (const std::string& id : ids) {
        if (std::optional<compute::JobCheckpoint> latest =
                checkpoint_store_.latest(id)) {
            result.push_back(std::move(*latest));
        }
    }
    return result;
}

std::vector<DatasetSummary> Node::datasets() const {
    // One dataset per table across every database — computed live from the tree +
    // the partition catalog, so the system.datasets view never drifts from the
    // truth (RULE-7; #585). A dataset is a first-class primitive with no stored
    // entity: one dataset == one table. (The virtual `system` catalog has no real
    // Database here, so it is naturally absent.)
    std::vector<DatasetSummary> result;
    for (const auto& database : databases_) {
        for (const auto& table : database->tables()) {
            DatasetSummary dataset;
            dataset.name = table->name();
            dataset.database = database->name();
            dataset.table_id = table->id();
            const StorageEngine& store = table->store();
            dataset.format = store.name();
            dataset.replication =
                std::string(to_string(table->replication_strategy().kind()));
            dataset.rows = store.size();

            // Schema: the union of each row's object keys in first-seen order — the
            // engine is schemaless, so a dataset's schema is whatever its rows carry.
            std::unordered_set<std::string> seen;
            for (std::size_t i = 0; i < store.size(); ++i) {
                const nlohmann::json& row = store.at(i);
                if (!row.is_object()) continue;
                for (auto field = row.begin(); field != row.end(); ++field) {
                    if (seen.insert(field.key()).second) {
                        dataset.columns.push_back(field.key());
                    }
                }
            }

            // On-disk size: the JSONL row chunks under the table's folder (the row
            // data's storage footprint). A filesystem error leaves it at 0.
            std::error_code ec;
            for (const auto& entry : fs::directory_iterator(table->dir(), ec)) {
                if (entry.path().extension() != ".jsonl") continue;
                const std::uintmax_t bytes = entry.file_size(ec);
                if (!ec) dataset.size_bytes += bytes;
            }

            // Partition placement across the cluster: the catalog entries whose
            // table id matches. The catalog is replicate-to-all, so this is the
            // global placement (which node owns which partition), identical on every
            // node — empty for a non-partitioned (replicate-to-all) dataset.
            for (const auto& partition : dataset_partitions_) {
                if (partition->table() != table->id()) continue;
                const PartitionRowRange& range = partition->row_range();
                dataset.partitions.push_back(
                    DatasetPlacement{partition->id(), partition->blob_ref(),
                                     range.start, range.end,
                                     partition->owner_nodes()});
            }

            result.push_back(std::move(dataset));
        }
    }
    return result;
}

bool Node::start() {
    if (running_) return false;
    running_ = true;
    started_at_ = Clock::now();
    ticks_ = 0;
    node_id_ = resolve_node_id();
    load_databases();
    load_projects();
    load_deployments();
    load_runs();
    load_models();
    load_indexes();
    load_jobs();
    load_dataset_partitions();
    load_principals();
    load_audit_events();
    if (replication_) {
        load_replication();         // bring in the persisted CRDT document
        migrate_to_replication();   // capture any local-only data into it
        const nlohmann::json snapshot = replication_->snapshot();
        reconcile_from_replication(snapshot);  // materialize document-only state
        // Establish the membership baseline so a later join/leave is a detectable
        // delta — but never rebalance on startup (the partitions just loaded
        // already reflect their recorded placement).
        last_membership_ = membership_from_snapshot(snapshot);
        persist_replication();
        last_seen_version_ = replication_->version();
    }
    std::printf(
        "zetta %s: node started (data_dir=%s, %zu databases, %zu projects, "
        "%zu deployments, %zu runs, %zu models, %zu indexes, %zu jobs, "
        "%zu partitions, %zu principals, %zu audit events)\n",
        kVersion, config_.data_dir.c_str(), databases_.size(),
        projects_.size(), deployments_.size(), runs_.size(), models_.size(),
        indexes_.size(), jobs_.size(), dataset_partitions_.size(),
        principals_.size(), audit_events_.size());
    if (config_.preflight) {
        integrity_ = check_integrity();
        std::printf(
            "zetta: preflight: %zu databases, %zu tables, %zu rows, %zu large "
            "objects (%zu present, %zu awaiting body), %zu models (%zu weights "
            "present, %zu awaiting body) — %s\n",
            integrity_.databases, integrity_.tables, integrity_.rows,
            integrity_.large_objects, integrity_.large_object_bodies_present,
            integrity_.large_object_bodies_missing, integrity_.models,
            integrity_.model_weights_present, integrity_.model_weights_missing,
            integrity_.ok() ? "OK" : "ISSUES FOUND");
        if (!integrity_.ok()) {
            std::printf(
                "zetta: preflight WARN: %zu corrupt large object(s), %zu "
                "corrupt model weight(s), %zu dangling table(s), %zu dangling "
                "row(s), %zu dangling large object(s), %zu dangling input(s), "
                "%zu dangling graph(s), %zu dangling access key(s)\n",
                integrity_.corrupt_large_objects,
                integrity_.corrupt_model_weights, integrity_.dangling_tables,
                integrity_.dangling_rows, integrity_.dangling_large_objects,
                integrity_.dangling_inputs, integrity_.dangling_graphs,
                integrity_.dangling_access_keys);
        }
    }
    // Re-establish the compute plane if a cluster was attached before this start
    // (a stop()/start() cycle). On first boot main attaches after start(), so this
    // is a no-op then and attach_cluster brings the coordinator up itself.
    start_coordinator();
    std::fflush(stdout);
    return true;
}

void Node::tick() {
    if (!running_) return;
    ++ticks_;
    // 1. Pull: the CRDT is the source of truth; when it has advanced (a local
    //    capture or a peer's gossip merged in on the network thread), reconcile
    //    the on-disk projection to match it. Reconcile only materializes (never
    //    captures), so it doesn't move the version — no feedback loop.
    if (replication_ && replication_->version() != last_seen_version_) {
        const nlohmann::json snapshot = replication_->snapshot();
        reconcile_from_replication(snapshot);
        // 1b. Between jobs: when the cluster membership (node:<id> keys) changed,
        //     rebalance partition placement (#517). This DOES capture updated
        //     part: owners; step 3 persists the moved document.
        rebalance_partitions_on_membership_change(snapshot);
    }
    // 2. Control plane: drive each local deployment desired → actual, starting
    //    or self-healing its run. May capture new runs into the document.
    controller_.reconcile();
    // 3. Persist if the document moved (a peer merge in step 1 or a run started
    //    in step 2), and remember the version we have now reflected on disk.
    if (replication_) {
        std::string version = replication_->version();
        if (version != last_seen_version_) {
            persist_replication();
            last_seen_version_ = std::move(version);
        }
    }
    // 4. Telemetry plane (#584): sample this node's live host utilization and
    //    broadcast it to peers as an ephemeral `node.metrics` signal-plane event,
    //    throttled to ~1 Hz (the CPU/network gauges are deltas over the interval).
    //    Every node caches what it receives (telemetry_->on_event), so any headed
    //    node's cluster view can animate the whole cluster's gauges. NOT routed
    //    through the CRDT — a measurement, not durable state.
    if (cluster_ && telemetry_) {
        using namespace std::chrono;
        constexpr duration<double> kTelemetryInterval{1.0};
        const steady_clock::time_point now = steady_clock::now();
        if (last_telemetry_sample_.time_since_epoch().count() == 0 ||
            now - last_telemetry_sample_ >= kTelemetryInterval) {
            last_telemetry_sample_ = now;
            const NodeMetrics metrics = sample_node_metrics();
            telemetry_->record_local(node_id(), metrics);
            cluster_->broadcast_event(
                std::string(NodeTelemetryExchange::kMethod),
                encode_node_metrics(metrics, node_id()));
        }
    }
}

void Node::stop() {
    if (!running_) return;
    // Tear the coordinator down first, while the attached cluster is still alive
    // (its signal-handler unwind is a cheap atomic store, and the steal loop joins
    // promptly) — so the cluster can be stopped/destroyed after us safely (#579).
    stop_coordinator();
    if (replication_) persist_replication();
    running_ = false;
    std::printf("zetta: node stopped\n");
    std::fflush(stdout);
}

void Node::attach_cluster(Cluster& cluster, unsigned worker_count) {
    cluster_ = &cluster;
    compute_workers_ = worker_count;
    // If the node is already running (the normal path — main attaches right after
    // cluster.start()), bring the coordinator up now; otherwise start() will.
    if (running_) {
        stop_coordinator();  // replace any prior attachment cleanly
        start_coordinator();
    }
}

void Node::start_coordinator() {
    if (!cluster_ || coordinator_) return;
    unsigned workers = compute_workers_;
    if (workers == 0) {
        // Auto: one worker per core, leaving one for the node loop + the cluster's
        // network thread. Always at least one (the SPMD parallel_map path needs a
        // worker; 0 would make this node a pure map source).
        const unsigned hardware = std::thread::hardware_concurrency();
        workers = hardware > 1 ? hardware - 1 : 1;
    }
    // The live-telemetry exchange (#584) rides the coordinator's signal-handler
    // chain as its FALLBACK — it receives every signal that is neither a steal nor
    // a collective, so it caches peers' `node.metrics` events without disturbing
    // the compute plane. Created once (the cache persists across a stop()/start()
    // cycle) and reused on each attach.
    if (!telemetry_) telemetry_ = std::make_unique<NodeTelemetryExchange>();
    coordinator_ = std::make_unique<compute::JobCoordinator>(*cluster_, workers,
                                                             telemetry_.get());
    // Register on the cluster's signal plane + start the cross-node steal loop, so
    // a job submitted through job_coordinator() runs SPMD across live membership.
    coordinator_->start();
}

void Node::stop_coordinator() {
    if (!coordinator_) return;
    coordinator_->stop();   // join the steal loop, restore the cluster's handler
    coordinator_.reset();
}

double Node::uptime_seconds() const {
    if (!running_) return 0.0;
    const std::chrono::duration<double> elapsed = Clock::now() - started_at_;
    return elapsed.count();
}

void Node::load_databases() {
    databases_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "database.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        if (meta.is_discarded() || !meta.contains("name")) continue;
        auto db = std::make_shared<Database>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(), entry.path(), replication_,
            replication_strategy_from_meta(meta));
        db->load();
        databases_.push_back(std::move(db));
    }
    // Stable order for the UI (directory iteration order is unspecified).
    std::sort(databases_.begin(), databases_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

void Node::load_projects() {
    projects_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "project.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: skip anything that doesn't match the project.json
        // shape (a name is required; the rest default if an older file omits it).
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("name") || !meta["name"].is_string()) {
            continue;
        }
        auto project = std::make_shared<Project>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(),
            meta.value("created", std::string{}),
            meta.value("updated", std::string{}),
            meta.value("owner", std::string{}), entry.path(), replication_,
            this);
        project->load();  // bring in this project's inputs + graphs from disk
        projects_.push_back(std::move(project));
    }
    // Stable order (directory iteration order is unspecified).
    std::sort(projects_.begin(), projects_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Project> Node::find_project_by_id(const std::string& id) const {
    for (const auto& project : projects_) {
        if (project->id() == id) return project;
    }
    return nullptr;
}

std::string Node::resolve_node_id() {
    // When replicated, the node id IS the replication actor id (the same UUID),
    // so the recorded placement node matches the document's actor.
    if (replication_) return replication_->node_id();
    // Standalone: the id is persisted in data_dir/node_id (created on first use),
    // so it's stable across restarts — mirroring how main.cpp seeds it.
    const fs::path path = fs::path(config_.data_dir) / "node_id";
    std::error_code ec;
    fs::create_directories(config_.data_dir, ec);
    if (std::ifstream in(path); in) {
        std::string id;
        std::getline(in, id);
        while (!id.empty() && (id.back() == '\n' || id.back() == '\r' ||
                               id.back() == ' ')) {
            id.pop_back();
        }
        if (!id.empty()) return id;
    }
    const std::string id = new_uuid();
    std::ofstream(path, std::ios::trunc) << id << '\n';
    return id;
}

std::vector<std::string> Node::cluster_members() const {
    // Membership is replicated state — the node:<id> keys in the document (the
    // same set the tick() rebalance keys on). Recompute from the live snapshot so
    // it reflects the current cluster, not a stale baseline.
    if (replication_) return membership_from_snapshot(replication_->snapshot());
    // A standalone node (no replication) is its own sole member.
    if (!node_id_.empty()) return {node_id_};
    return {};
}

void Node::load_deployments() {
    deployments_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "deployment.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a deployment needs a string graph_name and a
        // well-formed target (DeploymentTarget::from_json rejects an unknown
        // kind). Anything malformed is skipped, never trusted into the tree.
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("graph_name") || !meta["graph_name"].is_string()) {
            continue;
        }
        const std::optional<DeploymentTarget> target =
            DeploymentTarget::from_json(
                meta.value("target", nlohmann::json::object()));
        if (!target) continue;
        deployments_.push_back(std::make_shared<Deployment>(
            entry.path().filename().string(),
            meta["graph_name"].get<std::string>(), *target,
            meta.value("node", std::string{}),
            meta.value("node_count", std::size_t{0}),
            meta.value("edge_count", std::size_t{0}),
            meta.value("created", std::string{}), entry.path()));
    }
    // Stable, chronological order (id breaks ties on equal stamps).
    std::sort(deployments_.begin(), deployments_.end(),
              [](const auto& a, const auto& b) {
                  return std::tie(a->created(), a->id()) <
                         std::tie(b->created(), b->id());
              });
}

void Node::load_principals() {
    principals_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "principal.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: require an object with a known kind and a name;
        // skip anything that doesn't match the principal.json shape.
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("kind") || !meta["kind"].is_string() ||
            !meta.contains("name") || !meta["name"].is_string()) {
            continue;
        }
        // RULE-3: reject an unknown top-level field, matching the formal schema's
        // additionalProperties:false (schemas/principal.schema.json) — a record
        // carrying an unexpected key is malformed, so skip it rather than trust it.
        bool unknown_field = false;
        for (const auto& [field, ignored] : meta.items()) {
            if (field != "id" && field != "kind" && field != "name" &&
                field != "created") {
                unknown_field = true;
                break;
            }
        }
        if (unknown_field) continue;
        const auto kind = principal_kind_from(meta["kind"].get<std::string>());
        if (!kind) continue;
        auto principal = std::make_shared<Principal>(
            entry.path().filename().string(), *kind,
            meta["name"].get<std::string>(),
            meta.value("created", std::string{}), entry.path(), replication_);
        principal->load();  // bring in this principal's keys from disk
        principals_.push_back(std::move(principal));
    }
    // Stable order (directory iteration order is unspecified).
    std::sort(principals_.begin(), principals_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<Principal> Node::find_principal_by_id(
    const std::string& id) const {
    for (const auto& principal : principals_) {
        if (principal->id() == id) return principal;
    }
    return nullptr;
}

std::shared_ptr<Database> Node::create_database(
    std::string name, ReplicationStrategy replication_strategy) {
    // "system" is reserved for the virtual system database (computed system
    // tables like system.zetta_nodes); never a real on-disk database.
    if (name.empty() || name == "system" || find_database(name)) return nullptr;
    const std::string id = new_uuid();
    if (replication_) {
        // The default (ReplicateToAll) is omitted, so the db: value is unchanged
        // from before databases carried a strategy (ENT-35).
        nlohmann::json value{{"name", name}};
        if (!replication_strategy.is_default()) {
            value["replication"] = replication_strategy.to_json();
        }
        replication_->set(catalog::database_key(id), value);
    }
    return materialize_database(id, std::move(name),
                               std::move(replication_strategy));
}

std::shared_ptr<Database> Node::materialize_database(
    std::string id, std::string name, ReplicationStrategy strategy) {
    if (auto existing = find_database_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto db = std::make_shared<Database>(std::move(id), std::move(name), dir,
                                         replication_, std::move(strategy));
    db->persist_meta();
    databases_.push_back(db);
    return db;
}

std::shared_ptr<Database> Node::find_database(const std::string& name) const {
    for (const auto& db : databases_) {
        if (db->name() == name) return db;
    }
    return nullptr;
}

std::shared_ptr<Database> Node::find_database_by_id(
    const std::string& id) const {
    for (const auto& db : databases_) {
        if (db->id() == id) return db;
    }
    return nullptr;
}

bool Node::drop_database(const std::string& name) {
    auto db = find_database(name);
    if (!db) return false;
    if (replication_) replication_->remove(catalog::database_key(db->id()));
    return materialize_drop_database(db->id());
}

bool Node::materialize_drop_database(const std::string& id) {
    const auto it = std::find_if(
        databases_.begin(), databases_.end(),
        [&](const std::shared_ptr<Database>& d) { return d->id() == id; });
    if (it == databases_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    databases_.erase(it);
    return true;
}

bool Node::rename_database(const std::string& from, const std::string& to) {
    if (to.empty() || find_database(to)) return false;  // empty or clashes
    auto db = find_database(from);
    if (!db) return false;
    if (replication_) {
        replication_->set(catalog::database_key(db->id()),
                          nlohmann::json{{"name", to}});
    }
    db->set_name(to);
    return true;
}

std::vector<std::string> Node::database_names() const {
    std::vector<std::string> names;
    names.reserve(databases_.size());
    for (const auto& db : databases_) names.push_back(db->name());
    return names;
}

// ----- projects (mirror the database CRUD; replicated as project:<uuid>) -----

std::shared_ptr<Project> Node::create_project(std::string name,
                                              std::string owner) {
    if (name.empty() || find_project(name)) return nullptr;
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    if (replication_) {
        replication_->set(catalog::project_key(id),
                          nlohmann::json{{"name", name},
                                         {"created", now},
                                         {"updated", now},
                                         {"owner", owner}});
    }
    return materialize_project(id, std::move(name), now, now, std::move(owner));
}

std::shared_ptr<Project> Node::materialize_project(std::string id,
                                                   std::string name,
                                                   std::string created,
                                                   std::string updated,
                                                   std::string owner) {
    if (auto existing = find_project_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto project = std::make_shared<Project>(std::move(id), std::move(name),
                                             std::move(created),
                                             std::move(updated),
                                             std::move(owner), dir,
                                             replication_, this);
    project->persist_meta();
    projects_.push_back(project);
    return project;
}

std::shared_ptr<Project> Node::find_project(const std::string& name) const {
    for (const auto& project : projects_) {
        if (project->name() == name) return project;
    }
    return nullptr;
}

bool Node::drop_project(const std::string& name) {
    auto project = find_project(name);
    if (!project) return false;
    if (replication_) {
        // Drop the project's graphs from the document too, so they don't linger
        // as dangling `graph:` keys once their parent is gone.
        for (const auto& graph : project->graphs()) {
            replication_->remove(catalog::graph_key(graph->id()));
        }
        replication_->remove(catalog::project_key(project->id()));
    }
    return materialize_drop_project(project->id());
}

bool Node::materialize_drop_project(const std::string& id) {
    const auto it = std::find_if(
        projects_.begin(), projects_.end(),
        [&](const std::shared_ptr<Project>& p) { return p->id() == id; });
    if (it == projects_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    projects_.erase(it);
    return true;
}

bool Node::rename_project(const std::string& from, const std::string& to) {
    if (to.empty() || find_project(to)) return false;  // empty or clashes
    auto project = find_project(from);
    if (!project) return false;
    project->set_metadata(to, project->created(), now_iso8601(),
                          project->owner());
    if (replication_) {
        replication_->set(catalog::project_key(project->id()),
                          project->metadata());
    }
    return true;
}

std::vector<std::string> Node::project_names() const {
    std::vector<std::string> names;
    names.reserve(projects_.size());
    for (const auto& project : projects_) names.push_back(project->name());
    return names;
}

// ----- deployments (place a graph onto a target; replicated as deployment:<id>)

std::shared_ptr<Deployment> Node::deploy_graph(const ir::ExecutionGraph& graph,
                                               DeploymentTarget target,
                                               std::string graph_name) {
    // R1 places onto the local node only; k8s/cloud targets are deferred.
    if (!target.is_local()) return nullptr;
    // A deployable graph must have something to place.
    if (graph.empty()) return nullptr;

    // RULE-1: no deploy without a passing correctness gate. The gate runs at the
    // deploy seam itself — so a *programmatic* deploy clears the same gate the GUI
    // Deploy panel runs, and an ungated/failing graph can never be placed. A gate
    // failure is a governed decision: record a blocked (allowed=false) deploy event
    // and place nothing.
    const ValidationReport report = default_gate().run(graph);
    if (!report.passed()) {
        std::string reason = "correctness gate failed";
        for (const CheckResult& check : report.checks) {
            if (check.status == CheckStatus::Fail) {
                reason = "gate check '" + check.name + "' failed: " + check.reason;
                break;
            }
        }
        record_audit_event(
            {AuditAction::GraphDeployed, {}, graph_name, false, reason, {}});
        return nullptr;
    }

    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    const std::size_t node_count = graph.node_count();
    const std::size_t edge_count = graph.edge_count();
    if (replication_) {
        replication_->set(catalog::deployment_key(id),
                          nlohmann::json{{"graph_name", graph_name},
                                         {"target", target.to_json()},
                                         {"node", node_id_},
                                         {"node_count", node_count},
                                         {"edge_count", edge_count},
                                         {"created", now}});
    }
    auto deployment = materialize_deployment(id, graph_name, std::move(target),
                                             node_id_, node_count, edge_count,
                                             now);
    // Audit the deploy (FEAT-9): placing a graph onto a target is a governed
    // action. Record it once the placement is materialized.
    if (deployment) {
        record_audit_event(
            {AuditAction::GraphDeployed, {}, graph_name, true,
             "graph '" + graph_name + "' deployed (" +
                 std::to_string(node_count) + " nodes, " +
                 std::to_string(edge_count) + " edges) on node " + node_id_,
             {}});
    }
    return deployment;
}

std::shared_ptr<Deployment> Node::materialize_deployment(
    std::string id, std::string graph_name, DeploymentTarget target,
    std::string node_id, std::size_t node_count, std::size_t edge_count,
    std::string created) {
    if (auto existing = find_deployment(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto deployment = std::make_shared<Deployment>(
        std::move(id), std::move(graph_name), std::move(target),
        std::move(node_id), node_count, edge_count, std::move(created), dir);
    deployment->persist_meta();
    deployments_.push_back(deployment);
    return deployment;
}

std::shared_ptr<Deployment> Node::find_deployment(const std::string& id) const {
    for (const auto& deployment : deployments_) {
        if (deployment->id() == id) return deployment;
    }
    return nullptr;
}

// ----- principals (mirror the project CRUD; replicated as principal:<uuid>) ---

std::shared_ptr<Principal> Node::create_principal(std::string name,
                                                  PrincipalKind kind) {
    if (name.empty() || find_principal(name)) return nullptr;
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    if (replication_) {
        replication_->set(catalog::principal_key(id),
                          nlohmann::json{{"kind", to_string(kind)},
                                         {"name", name},
                                         {"created", now}});
    }
    return materialize_principal(id, kind, std::move(name), now);
}

std::shared_ptr<Principal> Node::materialize_principal(std::string id,
                                                       PrincipalKind kind,
                                                       std::string name,
                                                       std::string created) {
    if (auto existing = find_principal_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto principal = std::make_shared<Principal>(
        std::move(id), kind, std::move(name), std::move(created), dir,
        replication_);
    principal->persist_meta();
    principals_.push_back(principal);
    return principal;
}

std::shared_ptr<Principal> Node::find_principal(const std::string& name) const {
    for (const auto& principal : principals_) {
        if (principal->name() == name) return principal;
    }
    return nullptr;
}

bool Node::materialize_drop_deployment(const std::string& id) {
    const auto it = std::find_if(
        deployments_.begin(), deployments_.end(),
        [&](const std::shared_ptr<Deployment>& d) { return d->id() == id; });
    if (it == deployments_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    deployments_.erase(it);
    return true;
}

// ----- runs (one execution of a deployed graph; replicated as run:<uuid>) ----

void Node::load_runs() {
    runs_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "run.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a run.json that fails the Run JSON Schema is
        // rejected here (with a logged reason) rather than allowed to corrupt
        // the in-memory tree — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<RunValue> value =
            meta.is_discarded()
                ? std::nullopt
                : parse_run_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid run %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        runs_.push_back(std::make_shared<Run>(entry.path().filename().string(),
                                              *value, entry.path(),
                                              replication_));
    }
    // Stable order (directory iteration order is unspecified): chronological by
    // creation stamp (fixed-width ISO-8601 sorts as text), then id to break ties.
    std::sort(runs_.begin(), runs_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<Run> Node::create_run(std::string project, std::string graph,
                                      std::string deployment) {
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    RunValue value;
    value.project = std::move(project);
    value.graph = std::move(graph);
    value.deployment = std::move(deployment);
    value.status = RunStatus::Pending;
    value.created = now;
    value.updated = now;
    if (replication_) replication_->set(catalog::run_key(id), to_json(value));
    return materialize_run(id, std::move(value));
}

std::shared_ptr<Run> Node::materialize_run(std::string id, RunValue value) {
    if (auto existing = find_run(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto run = std::make_shared<Run>(std::move(id), std::move(value), dir,
                                     replication_);
    run->persist_meta();
    runs_.push_back(run);
    return run;
}

std::shared_ptr<Run> Node::find_run(const std::string& id) const {
    for (const auto& run : runs_) {
        if (run->id() == id) return run;
    }
    return nullptr;
}

// ----- models (a registered trained model; replicated as model:<uuid>) -------

void Node::load_models() {
    models_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "model.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a model.json that fails the Model JSON Schema is
        // rejected here (with a logged reason) rather than allowed to corrupt the
        // in-memory tree — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<ModelValue> value =
            meta.is_discarded() ? std::nullopt : parse_model_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid model %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        models_.push_back(std::make_shared<Model>(
            entry.path().filename().string(), *value, entry.path(),
            replication_));
    }
    // Stable order (directory iteration order is unspecified): chronological by
    // creation stamp (fixed-width ISO-8601 sorts as text), then id to break ties.
    std::sort(models_.begin(), models_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<Model> Node::create_model(ModelValue value) {
    if (value.kind.empty()) return nullptr;  // basic guard (kind is required)
    const std::string id = new_uuid();
    value.created = now_iso8601();
    value.weights.reset();  // weights are attached separately via set_weights
    if (replication_) replication_->set(catalog::model_key(id), to_json(value));
    return materialize_model(id, std::move(value));
}

std::shared_ptr<Model> Node::materialize_model(std::string id, ModelValue value) {
    if (auto existing = find_model(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto model = std::make_shared<Model>(std::move(id), std::move(value), dir,
                                         replication_);
    model->persist_meta();
    models_.push_back(model);
    return model;
}

std::shared_ptr<Model> Node::find_model(const std::string& id) const {
    for (const auto& model : models_) {
        if (model->id() == id) return model;
    }
    return nullptr;
}

bool Node::drop_model(const std::string& id) {
    if (!find_model(id)) return false;
    if (replication_) replication_->remove(catalog::model_key(id));
    return materialize_drop_model(id);
}

bool Node::materialize_drop_model(const std::string& id) {
    const auto it = std::find_if(
        models_.begin(), models_.end(),
        [&](const std::shared_ptr<Model>& m) { return m->id() == id; });
    if (it == models_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    models_.erase(it);
    return true;
}

// ----- indexes (a registered index over a table; replicated as index:<uuid>) --

void Node::load_indexes() {
    indexes_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "index.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: an index.json that fails the Index JSON Schema is
        // rejected here (with a logged reason) rather than allowed to corrupt the
        // in-memory tree — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<IndexValue> value =
            meta.is_discarded() ? std::nullopt : parse_index_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid index %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        indexes_.push_back(std::make_shared<Index>(
            entry.path().filename().string(), *value, entry.path(),
            replication_));
    }
    // Stable order (directory iteration order is unspecified): chronological by
    // creation stamp (fixed-width ISO-8601 sorts as text), then id to break ties.
    std::sort(indexes_.begin(), indexes_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<Index> Node::create_index(IndexValue value) {
    // Basic guards (the schema enforces the rest at persist/reconcile): a usable
    // index needs a source table, a backend to build it, and at least one field.
    if (value.table.empty() || value.backend.empty() || value.fields.empty()) {
        return nullptr;
    }
    const std::string id = new_uuid();
    value.created = now_iso8601();
    if (replication_) replication_->set(catalog::index_key(id), to_json(value));
    return materialize_index(id, std::move(value));
}

std::shared_ptr<Index> Node::materialize_index(std::string id, IndexValue value) {
    if (auto existing = find_index(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto index = std::make_shared<Index>(std::move(id), std::move(value), dir,
                                         replication_);
    index->persist_meta();
    indexes_.push_back(index);
    return index;
}

std::shared_ptr<Index> Node::find_index(const std::string& id) const {
    for (const auto& index : indexes_) {
        if (index->id() == id) return index;
    }
    return nullptr;
}

bool Node::drop_index(const std::string& id) {
    if (!find_index(id)) return false;
    if (replication_) replication_->remove(catalog::index_key(id));
    return materialize_drop_index(id);
}

bool Node::materialize_drop_index(const std::string& id) {
    const auto it = std::find_if(
        indexes_.begin(), indexes_.end(),
        [&](const std::shared_ptr<Index>& i) { return i->id() == id; });
    if (it == indexes_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    indexes_.erase(it);
    return true;
}

// ----- compute jobs (one unit of accelerated compute; replicated job:<uuid>) --

void Node::load_jobs() {
    jobs_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "job.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a job.json that fails the ComputeJob JSON Schema is
        // rejected here (with a logged reason) rather than allowed to corrupt the
        // in-memory tree — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<ComputeJobValue> value =
            meta.is_discarded() ? std::nullopt
                                : parse_compute_job_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid job %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        jobs_.push_back(std::make_shared<ComputeJob>(
            entry.path().filename().string(), *value, entry.path(),
            replication_, /*audit=*/this));
    }
    // Stable order (directory iteration order is unspecified): chronological by
    // creation stamp (fixed-width ISO-8601 sorts as text), then id to break ties.
    std::sort(jobs_.begin(), jobs_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<ComputeJob> Node::create_job(JobKind kind, std::string project,
                                             std::string graph, std::string run,
                                             std::string scaling_policy) {
    const std::string id = new_uuid();
    const std::string now = now_iso8601();
    ComputeJobValue value;
    value.kind = kind;
    value.status = JobStatus::Queued;
    value.project = std::move(project);
    value.graph = std::move(graph);
    value.run = std::move(run);
    value.scaling_policy = std::move(scaling_policy);
    value.created = now;
    value.updated = now;
    if (replication_) replication_->set(catalog::job_key(id), to_json(value));
    auto job = materialize_job(id, std::move(value));
    // Audit the create (FEAT-9): queueing a compute job is a governed action.
    // Record it once the job is materialized; a peer's create arrives via
    // reconcile (materialize_job) already carrying its own replicated audit event.
    if (job) {
        record_audit_event(
            {AuditAction::JobCreated, {}, job->id(), true,
             "compute job " + std::string(to_string(job->kind())) + " queued",
             {}});
    }
    return job;
}

std::shared_ptr<ComputeJob> Node::materialize_job(std::string id,
                                                  ComputeJobValue value) {
    if (auto existing = find_job(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto job = std::make_shared<ComputeJob>(std::move(id), std::move(value), dir,
                                            replication_, /*audit=*/this);
    job->persist_meta();
    jobs_.push_back(job);
    return job;
}

std::shared_ptr<ComputeJob> Node::find_job(const std::string& id) const {
    for (const auto& job : jobs_) {
        if (job->id() == id) return job;
    }
    return nullptr;
}

bool Node::drop_job(const std::string& id) {
    if (!find_job(id)) return false;
    if (replication_) replication_->remove(catalog::job_key(id));
    return materialize_drop_job(id);
}

bool Node::materialize_drop_job(const std::string& id) {
    const auto it = std::find_if(
        jobs_.begin(), jobs_.end(),
        [&](const std::shared_ptr<ComputeJob>& j) { return j->id() == id; });
    if (it == jobs_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    jobs_.erase(it);
    return true;
}

// ----- dataset partitions (the partition catalog; replicated part:<uuid>) -----

void Node::load_dataset_partitions() {
    dataset_partitions_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "partition.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: a partition.json that fails the DatasetPartition
        // JSON Schema is rejected here (with a logged reason) rather than allowed
        // to corrupt the in-memory tree — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<DatasetPartitionValue> value =
            meta.is_discarded() ? std::nullopt
                                : parse_dataset_partition_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid partition %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        dataset_partitions_.push_back(std::make_shared<DatasetPartition>(
            entry.path().filename().string(), *value, entry.path(),
            replication_));
    }
    // Stable order (directory iteration order is unspecified): chronological by
    // creation stamp (fixed-width ISO-8601 sorts as text), then id to break ties.
    std::sort(dataset_partitions_.begin(), dataset_partitions_.end(),
              [](const auto& a, const auto& b) {
                  if (a->created() != b->created())
                      return a->created() < b->created();
                  return a->id() < b->id();
              });
}

std::shared_ptr<DatasetPartition> Node::create_dataset_partition(
    DatasetPartitionValue value) {
    // A partition must reference a table — that ref is the catalog's anchor.
    if (value.table.empty()) return nullptr;
    const std::string id = new_uuid();
    if (value.created.empty()) value.created = now_iso8601();
    if (replication_) {
        replication_->set(catalog::partition_key(id), to_json(value));
    }
    return materialize_dataset_partition(id, std::move(value));
}

std::shared_ptr<DatasetPartition> Node::materialize_dataset_partition(
    std::string id, DatasetPartitionValue value) {
    if (auto existing = find_dataset_partition(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto partition = std::make_shared<DatasetPartition>(
        std::move(id), std::move(value), dir, replication_);
    partition->persist_meta();
    dataset_partitions_.push_back(partition);
    std::sort(dataset_partitions_.begin(), dataset_partitions_.end(),
              [](const auto& a, const auto& b) {
                  if (a->created() != b->created())
                      return a->created() < b->created();
                  return a->id() < b->id();
              });
    return partition;
}

std::shared_ptr<DatasetPartition> Node::find_dataset_partition(
    const std::string& id) const {
    for (const auto& partition : dataset_partitions_) {
        if (partition->id() == id) return partition;
    }
    return nullptr;
}

bool Node::drop_dataset_partition(const std::string& id) {
    if (!find_dataset_partition(id)) return false;
    if (replication_) replication_->remove(catalog::partition_key(id));
    return materialize_drop_dataset_partition(id);
}

bool Node::materialize_drop_dataset_partition(const std::string& id) {
    const auto it = std::find_if(
        dataset_partitions_.begin(), dataset_partitions_.end(),
        [&](const std::shared_ptr<DatasetPartition>& p) {
            return p->id() == id;
        });
    if (it == dataset_partitions_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    dataset_partitions_.erase(it);
    return true;
}

// ----- cloud inspections (ephemeral; not replicated, not persisted) ----------

void Node::record_cloud_inspection(CloudInspectReport report) {
    // Latest report per provider wins, so the cache holds one current row per
    // provider for system.zetta_cloud_status. No CRDT capture, no disk write —
    // this is session-local observed state (RULE-7).
    const auto it = std::find_if(
        cloud_inspections_.begin(), cloud_inspections_.end(),
        [&](const CloudInspectReport& existing) {
            return existing.provider_id == report.provider_id;
        });
    if (it == cloud_inspections_.end()) {
        cloud_inspections_.push_back(std::move(report));
    } else {
        *it = std::move(report);
    }
}

// ----- principals (mirror the project CRUD; replicated as principal:<uuid>) ---

bool Node::drop_principal(const std::string& name) {
    auto principal = find_principal(name);
    if (!principal) return false;
    // Drop the principal's keys from the document too (their folders go with the
    // principal folder below). Snapshot the ids first — drop_key mutates keys().
    if (replication_) {
        std::vector<std::string> key_ids;
        for (const auto& key : principal->keys()) key_ids.push_back(key->id());
        for (const std::string& key_id : key_ids) {
            replication_->remove(catalog::access_key_key(key_id));
        }
        replication_->remove(catalog::principal_key(principal->id()));
    }
    return materialize_drop_principal(principal->id());
}

bool Node::materialize_drop_principal(const std::string& id) {
    const auto it = std::find_if(
        principals_.begin(), principals_.end(),
        [&](const std::shared_ptr<Principal>& p) { return p->id() == id; });
    if (it == principals_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    principals_.erase(it);
    return true;
}

bool Node::rename_principal(const std::string& from, const std::string& to) {
    if (to.empty() || find_principal(to)) return false;  // empty or clashes
    auto principal = find_principal(from);
    if (!principal) return false;
    principal->set_metadata(principal->kind(), to, principal->created());
    if (replication_) {
        replication_->set(catalog::principal_key(principal->id()),
                          principal->metadata());
    }
    return true;
}

std::vector<std::string> Node::principal_names() const {
    std::vector<std::string> names;
    names.reserve(principals_.size());
    for (const auto& principal : principals_) names.push_back(principal->name());
    return names;
}

std::shared_ptr<AccessKey> Node::find_access_key(
    const std::string& key_id) const {
    for (const auto& principal : principals_) {
        if (auto key = principal->find_key(key_id)) return key;
    }
    return nullptr;
}

bool Node::drop_run(const std::string& id) {
    if (!find_run(id)) return false;
    if (replication_) replication_->remove(catalog::run_key(id));
    return materialize_drop_run(id);
}

bool Node::materialize_drop_run(const std::string& id) {
    const auto it = std::find_if(
        runs_.begin(), runs_.end(),
        [&](const std::shared_ptr<Run>& r) { return r->id() == id; });
    if (it == runs_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    runs_.erase(it);
    return true;
}

AccessDecision Node::authorize(const std::string& key_id,
                               const Resource& resource) {
    auto key = find_access_key(key_id);
    const AccessDecision decision =
        key ? zetta::authorize(*key, resource)
            : AccessDecision{false, "unknown access key — denied"};
    // Audit the data access (FEAT-9): every authorization decision — allow or
    // deny — is an immutable audit event. The target is the resource the request
    // tried to reach; the detail is the decision's human-readable reason.
    record_audit_event(
        {AuditAction::DataAccess, key_id,
         std::string(to_string(resource.kind)) + ":" + resource.name,
         decision.allowed, decision.reason, {}});
    return decision;
}

// ----- audit trail (immutable, append-only; replicated as audit:<uuid>) ------

fs::path Node::audit_dir() const {
    return fs::path(config_.data_dir) / "audit";
}

void Node::load_audit_events() {
    audit_events_.clear();
    const fs::path root = audit_dir();
    std::error_code ec;
    if (!fs::exists(root, ec)) return;
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_regular_file() ||
            entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream in(entry.path());
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        // Boundary validation: an audit file that fails the AuditEvent JSON
        // Schema is rejected here (with a logged reason) rather than allowed to
        // corrupt the in-memory trail — fail fast, don't propagate bad state.
        std::string error;
        const std::optional<AuditEventValue> value =
            meta.is_discarded() ? std::nullopt
                                : parse_audit_event_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid audit event %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        audit_events_.push_back(std::make_shared<AuditEvent>(
            entry.path().stem().string(), *value, root));
    }
    // Stable, chronological order (fixed-width ISO-8601 sorts as text); id breaks
    // ties on identical stamps.
    std::sort(audit_events_.begin(), audit_events_.end(),
              [](const auto& a, const auto& b) {
                  if (a->time() != b->time()) return a->time() < b->time();
                  return a->id() < b->id();
              });
}

void Node::record_audit_event(AuditEventValue value) {
    // The sink owns identity + time: stamp a fresh id and the current time so the
    // append-only contract holds regardless of what the caller passed.
    const std::string id = new_uuid();
    value.time = now_iso8601();
    // CRDT first (the source of truth), then materialize the on-disk projection.
    if (replication_) {
        replication_->set(catalog::audit_key(id), to_json(value));
    }
    materialize_audit_event(id, std::move(value));
}

std::shared_ptr<AuditEvent> Node::materialize_audit_event(
    std::string id, AuditEventValue value) {
    for (const auto& event : audit_events_) {
        if (event->id() == id) return event;  // already present (append-only)
    }
    const fs::path dir = audit_dir();
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto event = std::make_shared<AuditEvent>(std::move(id), std::move(value),
                                              dir);
    event->persist_meta();
    audit_events_.push_back(event);
    return event;
}

AccessDecision Node::authorize(const std::string& key_id,
                               const Operation& operation) const {
    auto key = find_access_key(key_id);
    if (!key) {
        return AccessDecision{false, "unknown access key — denied"};
    }
    return zetta::authorize(*key, operation, now_iso8601());
}

ExecutionResult Node::run_graph(const ir::ExecutionGraph& graph,
                                std::string project, std::string graph_id) {
    // Delegate to the standalone orchestrator with the default per-step executor.
    // The Run it creates is owned by this node (replicated + persisted as usual).
    return GraphExecutor{}.execute(*this, graph, std::move(project),
                                   std::move(graph_id));
}

// ----- replication: persistence ---------------------------------------------

fs::path Node::replication_path() const {
    return fs::path(config_.data_dir) / "replication.automerge";
}

void Node::load_replication() {
    std::error_code ec;
    const fs::path path = replication_path();
    if (!fs::exists(path, ec)) return;
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    if (raw.empty()) return;
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    replication_->merge(bytes);
}

void Node::persist_replication() const {
    const std::vector<std::byte> bytes = replication_->save();
    const fs::path path = replication_path();
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);  // atomic replace
}

// ----- replication: capture local-only data ---------------------------------

void Node::migrate_to_replication() {
    const nlohmann::json snapshot = replication_->snapshot();
    for (const auto& db : databases_) {
        if (!snapshot.contains(catalog::database_key(db->id()))) {
            // Carry the strategy (ENT-35); the default is omitted, so a
            // ReplicateToAll database migrates to the same db: value as before.
            nlohmann::json value{{"name", db->name()}};
            if (!db->replication_strategy().is_default()) {
                value["replication"] = db->replication_strategy().to_json();
            }
            replication_->set(catalog::database_key(db->id()), value);
        }
        for (const auto& table : db->tables()) {
            if (!snapshot.contains(catalog::table_key(table->id()))) {
                nlohmann::json value{{"db", db->id()}, {"name", table->name()}};
                if (!table->replication_strategy().is_default()) {
                    value["replication"] =
                        table->replication_strategy().to_json();
                }
                replication_->set(catalog::table_key(table->id()), value);
            }
            const StorageEngine& store = table->store();
            for (std::size_t i = 0; i < store.size(); ++i) {
                const std::string& row_id = store.id_at(i);
                if (!snapshot.contains(catalog::row_key(row_id))) {
                    replication_->set(
                        catalog::row_key(row_id),
                        nlohmann::json{{"tbl", table->id()},
                                       {"data", store.at(i)}});
                }
            }
        }
        for (const LargeObject& lo : db->large_objects().list()) {
            if (!snapshot.contains(catalog::large_object_key(lo.id))) {
                replication_->set(catalog::large_object_key(lo.id),
                                  nlohmann::json{{"db", db->id()},
                                                 {"name", lo.name},
                                                 {"size", lo.size},
                                                 {"sha256", lo.sha256}});
            }
        }
    }
    for (const auto& project : projects_) {
        if (!snapshot.contains(catalog::project_key(project->id()))) {
            replication_->set(catalog::project_key(project->id()),
                              project->metadata());
        }
        for (const auto& input : project->inputs()) {
            if (!snapshot.contains(catalog::input_key(input->id()))) {
                replication_->set(catalog::input_key(input->id()),
                                  input->metadata());
            }
        }
        for (const auto& graph : project->graphs()) {
            if (!snapshot.contains(catalog::graph_key(graph->id()))) {
                replication_->set(catalog::graph_key(graph->id()),
                                  graph->record());
            }
        }
    }
    for (const auto& deployment : deployments_) {
        if (!snapshot.contains(catalog::deployment_key(deployment->id()))) {
            replication_->set(catalog::deployment_key(deployment->id()),
                              deployment->metadata());
        }
    }
    for (const auto& run : runs_) {
        if (!snapshot.contains(catalog::run_key(run->id()))) {
            replication_->set(catalog::run_key(run->id()), run->metadata());
        }
    }
    for (const auto& model : models_) {
        if (!snapshot.contains(catalog::model_key(model->id()))) {
            replication_->set(catalog::model_key(model->id()),
                              model->metadata());
        }
    }
    for (const auto& index : indexes_) {
        if (!snapshot.contains(catalog::index_key(index->id()))) {
            replication_->set(catalog::index_key(index->id()),
                              index->metadata());
        }
    }
    for (const auto& job : jobs_) {
        if (!snapshot.contains(catalog::job_key(job->id()))) {
            replication_->set(catalog::job_key(job->id()), job->metadata());
        }
    }
    for (const auto& partition : dataset_partitions_) {
        if (!snapshot.contains(catalog::partition_key(partition->id()))) {
            replication_->set(catalog::partition_key(partition->id()),
                              partition->metadata());
        }
    }
    for (const auto& principal : principals_) {
        if (!snapshot.contains(catalog::principal_key(principal->id()))) {
            replication_->set(catalog::principal_key(principal->id()),
                              principal->metadata());
        }
        for (const auto& key : principal->keys()) {
            if (!snapshot.contains(catalog::access_key_key(key->id()))) {
                replication_->set(catalog::access_key_key(key->id()),
                                  key->metadata());
            }
        }
    }
    for (const auto& event : audit_events_) {
        if (!snapshot.contains(catalog::audit_key(event->id()))) {
            replication_->set(catalog::audit_key(event->id()),
                              event->metadata());
        }
    }
}

// ----- replication: apply the document to the local projection --------------

void Node::reconcile_from_replication(const nlohmann::json& snapshot) {
    // Reconcile each replicated entity type from the same snapshot, parents
    // before children (databases before tables/rows/large objects; projects
    // before inputs/graphs; principals before access keys). Order matters.
    reconcile_databases(snapshot);
    reconcile_projects(snapshot);
    reconcile_inputs(snapshot);
    reconcile_graphs(snapshot);
    reconcile_deployments(snapshot);
    reconcile_runs(snapshot);
    reconcile_models(snapshot);
    reconcile_indexes(snapshot);
    reconcile_jobs(snapshot);
    reconcile_partitions(snapshot);
    reconcile_principals(snapshot);
    reconcile_access_keys(snapshot);
    reconcile_audit_events(snapshot);
}

void Node::rebalance_partitions_on_membership_change(
    const nlohmann::json& snapshot) {
    std::vector<std::string> members = membership_from_snapshot(snapshot);
    if (members == last_membership_) return;  // no membership delta — nothing to do
    // The previous membership distinguishes replicate-to-all partitions (owned by
    // exactly the old member set) from sharded ones, so rebalance leaves the
    // former untouched. Move it out before overwriting the baseline.
    std::vector<std::string> previous = std::move(last_membership_);
    last_membership_ = members;
    // First observation (baseline established in start(), but be defensive) — and
    // a now-empty membership (nowhere to place) — never trigger a rebalance.
    if (previous.empty() || members.empty()) return;
    rebalance_partitions(*this, members, previous);
}

// Databases, tables, rows, and large objects reconcile together: they decode in
// one shared pass over the snapshot, and table/large-object removals nest under
// each kept database, so they don't split into independent helpers cleanly.
void Node::reconcile_databases(const nlohmann::json& snapshot) {
    // Decode the flat catalog keys into desired databases / tables / rows.
    struct DatabaseEntry { std::string name; ReplicationStrategy strategy; };
    std::map<std::string, DatabaseEntry> databases;           // id -> entry
    struct TableEntry {
        std::string database_id;
        std::string name;
        ReplicationStrategy strategy;
    };
    std::map<std::string, TableEntry> tables;                 // id -> entry
    struct RowEntry { std::string table_id; nlohmann::json data; };
    std::map<std::string, RowEntry> rows;                     // id -> entry
    struct LargeObjectEntry {
        std::string database_id;
        std::string name;
        std::uintmax_t size;
        std::string sha256;
    };
    std::map<std::string, LargeObjectEntry> large_objects;    // id -> entry

    for (const auto& [key, value] : snapshot.items()) {
        if (std::string id = catalog::id_after(key, catalog::kDatabasePrefix);
            !id.empty() && value.is_object() && value.contains("name")) {
            databases.emplace(std::move(id),
                              DatabaseEntry{value["name"].get<std::string>(),
                                            replication_strategy_from_meta(value)});
        } else if (std::string table_id =
                       catalog::id_after(key, catalog::kTablePrefix);
                   !table_id.empty() && value.is_object() &&
                   value.contains("db") && value.contains("name")) {
            tables.emplace(std::move(table_id),
                           TableEntry{value["db"].get<std::string>(),
                                      value["name"].get<std::string>(),
                                      replication_strategy_from_meta(value)});
        } else if (std::string row_id =
                       catalog::id_after(key, catalog::kRowPrefix);
                   !row_id.empty() && value.is_object() &&
                   value.contains("tbl") && value.contains("data")) {
            rows.emplace(std::move(row_id),
                         RowEntry{value["tbl"].get<std::string>(),
                                  value["data"]});
        } else if (std::string lo_id =
                       catalog::id_after(key, catalog::kLargeObjectPrefix);
                   !lo_id.empty() && value.is_object() && value.contains("db")) {
            large_objects.emplace(
                std::move(lo_id),
                LargeObjectEntry{value.value("db", std::string{}),
                                 value.value("name", std::string{}),
                                 value.value("size", std::uintmax_t{0}),
                                 value.value("sha256", std::string{})});
        }
    }

    // Removals first (a peer dropped a database/table). Snapshot ids up front so
    // we don't mutate while iterating.
    std::vector<std::string> local_db_ids;
    for (const auto& db : databases_) local_db_ids.push_back(db->id());
    for (const std::string& db_id : local_db_ids) {
        if (!databases.contains(db_id)) {
            materialize_drop_database(db_id);
            continue;
        }
        auto db = find_database_by_id(db_id);
        std::vector<std::string> local_table_ids;
        for (const auto& table : db->tables()) {
            local_table_ids.push_back(table->id());
        }
        for (const std::string& table_id : local_table_ids) {
            const auto it = tables.find(table_id);
            if (it == tables.end() || it->second.database_id != db_id) {
                db->materialize_drop_table(table_id);
            }
        }
        std::vector<std::string> local_lo_ids;
        for (const LargeObject& lo : db->large_objects().list()) {
            local_lo_ids.push_back(lo.id);
        }
        for (const std::string& lo_id : local_lo_ids) {
            const auto it = large_objects.find(lo_id);
            if (it == large_objects.end() || it->second.database_id != db_id) {
                db->large_objects().materialize_unlink(lo_id);
            }
        }
    }

    // Additions / renames. The strategy (ENT-35) is applied when a database/table
    // is first materialized from a peer; a peer rename only updates the name (the
    // strategy is fixed at create time in this slice — there is no update path).
    for (const auto& [id, entry] : databases) {
        auto db = find_database_by_id(id);
        if (!db) {
            materialize_database(id, entry.name, entry.strategy);
        } else if (db->name() != entry.name) {
            db->set_name(entry.name);  // a peer renamed it (no re-capture)
        }
    }
    for (const auto& [id, entry] : tables) {
        auto db = find_database_by_id(entry.database_id);
        if (db && !db->find_table_by_id(id)) {
            db->materialize_table(id, entry.name, entry.strategy);
        }
    }
    for (auto& [id, entry] : rows) {
        auto db_table = [&]() -> std::shared_ptr<Table> {
            for (const auto& db : databases_) {
                if (auto table = db->find_table_by_id(entry.table_id)) {
                    return table;
                }
            }
            return nullptr;
        }();
        if (db_table) db_table->materialize_row(id, std::move(entry.data));
    }
    for (const auto& [id, entry] : large_objects) {
        auto db = find_database_by_id(entry.database_id);
        if (!db) continue;
        LargeObject object;
        object.id = id;
        object.name = entry.name;
        object.size = entry.size;
        object.sha256 = entry.sha256;
        db->large_objects().materialize_object(std::move(object));
    }
}

void Node::reconcile_projects(const nlohmann::json& snapshot) {
    // Projects (top-level, no parent): project:<id> -> { name, created,
    // updated, owner }. Decode the desired set, then drop / add / update the
    // local projection to match the document — no re-capture.
    struct ProjectEntry {
        std::string name;
        std::string created;
        std::string updated;
        std::string owner;
    };
    std::map<std::string, ProjectEntry> projects;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kProjectPrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) {
            continue;
        }
        projects.emplace(std::move(id),
                         ProjectEntry{value.value("name", std::string{}),
                                      value.value("created", std::string{}),
                                      value.value("updated", std::string{}),
                                      value.value("owner", std::string{})});
    }
    std::vector<std::string> local_project_ids;
    for (const auto& project : projects_) {
        local_project_ids.push_back(project->id());
    }
    for (const std::string& project_id : local_project_ids) {
        if (!projects.contains(project_id)) materialize_drop_project(project_id);
    }
    for (const auto& [id, entry] : projects) {
        auto project = find_project_by_id(id);
        if (!project) {
            materialize_project(id, entry.name, entry.created, entry.updated,
                                entry.owner);
        } else if (project->name() != entry.name ||
                   project->updated() != entry.updated ||
                   project->owner() != entry.owner) {
            project->set_metadata(entry.name, entry.created, entry.updated,
                                  entry.owner);  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_inputs(const nlohmann::json& snapshot) {
    // Project inputs (owned by a project): input:<id> -> { project, kind,
    // source, filename, content_hash, size, created_by, created_at }. Inputs are
    // immutable provenance records (never renamed/edited), so reconcile only adds
    // them under their owning project and drops the ones a peer removed. The
    // payload body is not in the document and does not transfer yet.
    struct InputEntry {
        std::string project_id;
        ProjectInputKind kind;
        ProjectInputSource source;
        std::string filename;
        std::string content_hash;
        std::uintmax_t size;
        std::string created_by;
        std::string created_at;
    };
    std::map<std::string, InputEntry> inputs;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kInputPrefix);
        if (id.empty() || !value.is_object() || !value.contains("project")) {
            continue;
        }
        const auto kind = project_input_kind_from(
            value.value("kind", std::string{}));
        const auto source = project_input_source_from(
            value.value("source", std::string{}));
        if (!kind || !source) continue;  // boundary validation
        inputs.emplace(std::move(id),
                       InputEntry{value.value("project", std::string{}), *kind,
                                  *source, value.value("filename", std::string{}),
                                  value.value("content_hash", std::string{}),
                                  value.value("size", std::uintmax_t{0}),
                                  value.value("created_by", std::string{}),
                                  value.value("created_at", std::string{})});
    }
    // Removals first (a peer dropped an input, or its project is gone).
    for (const auto& project : projects_) {
        std::vector<std::string> local_input_ids;
        for (const auto& input : project->inputs()) {
            local_input_ids.push_back(input->id());
        }
        for (const std::string& input_id : local_input_ids) {
            const auto it = inputs.find(input_id);
            if (it == inputs.end() || it->second.project_id != project->id()) {
                project->materialize_drop_input(input_id);
            }
        }
    }
    // Additions (under the owning project, if it exists locally).
    for (const auto& [id, entry] : inputs) {
        auto project = find_project_by_id(entry.project_id);
        if (project && !project->find_input(id)) {
            project->materialize_input(id, entry.kind, entry.source,
                                       entry.filename, entry.content_hash,
                                       entry.size, entry.created_by,
                                       entry.created_at);
        }
    }
}

void Node::reconcile_graphs(const nlohmann::json& snapshot) {
    // Execution graphs (under a project): graph:<id> -> { project, name,
    // graph: {nodes, edges} }. Decode the desired set, parsing each graph
    // against the IR schema — a graph that doesn't parse is skipped, so a
    // malformed peer payload can't corrupt the projection — then drop / add /
    // update the local projection to match. Runs after projects so a graph's
    // parent already exists locally.
    struct GraphEntry {
        std::string project_id;
        std::string name;
        ir::ExecutionGraph graph;
    };
    std::map<std::string, GraphEntry> desired_graphs;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kGraphPrefix);
        if (id.empty() || !value.is_object() || !value.contains("project") ||
            !value.contains("graph")) {
            continue;
        }
        std::optional<ir::ExecutionGraph> graph =
            ir::from_json(value["graph"]);
        if (!graph) continue;  // boundary validation: drop a malformed graph
        desired_graphs.emplace(
            std::move(id),
            GraphEntry{value.value("project", std::string{}),
                       value.value("name", std::string{}), std::move(*graph)});
    }
    for (const auto& project : projects_) {
        std::vector<std::string> local_graph_ids;
        for (const auto& graph : project->graphs()) {
            local_graph_ids.push_back(graph->id());
        }
        for (const std::string& graph_id : local_graph_ids) {
            const auto it = desired_graphs.find(graph_id);
            if (it == desired_graphs.end() ||
                it->second.project_id != project->id()) {
                project->materialize_drop_graph(graph_id);
            }
        }
    }
    for (auto& [id, entry] : desired_graphs) {
        auto project = find_project_by_id(entry.project_id);
        if (!project) continue;  // dangling graph (project gone) — ignore
        auto existing = project->find_graph_by_id(id);
        if (!existing) {
            project->materialize_graph(id, entry.name, std::move(entry.graph));
        } else if (existing->name() != entry.name ||
                   ir::to_json(existing->graph()) != ir::to_json(entry.graph)) {
            existing->set_contents(entry.name,
                                   std::move(entry.graph));  // a peer changed it
        }
    }
}

void Node::reconcile_deployments(const nlohmann::json& snapshot) {
    // Deployments (top-level, no parent): deployment:<id> -> { graph_name,
    // target, node, node_count, edge_count, created }. Decode the desired set,
    // then drop / add the local projection to match the document. Deployments
    // are immutable once recorded in this slice (no update branch); reconcile /
    // self-heal that mutates them is a later task.
    struct DeploymentEntry {
        std::string graph_name;
        DeploymentTarget target;
        std::string node_id;
        std::size_t node_count;
        std::size_t edge_count;
        std::string created;
    };
    std::map<std::string, DeploymentEntry> deployments;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kDeploymentPrefix);
        if (id.empty() || !value.is_object() || !value.contains("graph_name")) {
            continue;
        }
        const std::optional<DeploymentTarget> target =
            DeploymentTarget::from_json(
                value.value("target", nlohmann::json::object()));
        if (!target) continue;
        deployments.emplace(
            std::move(id),
            DeploymentEntry{value.value("graph_name", std::string{}), *target,
                            value.value("node", std::string{}),
                            value.value("node_count", std::size_t{0}),
                            value.value("edge_count", std::size_t{0}),
                            value.value("created", std::string{})});
    }
    std::vector<std::string> local_deployment_ids;
    for (const auto& deployment : deployments_) {
        local_deployment_ids.push_back(deployment->id());
    }
    for (const std::string& deployment_id : local_deployment_ids) {
        if (!deployments.contains(deployment_id)) {
            materialize_drop_deployment(deployment_id);
        }
    }
    for (const auto& [id, entry] : deployments) {
        if (!find_deployment(id)) {
            materialize_deployment(id, entry.graph_name, entry.target,
                                   entry.node_id, entry.node_count,
                                   entry.edge_count, entry.created);
        }
    }
}

void Node::reconcile_runs(const nlohmann::json& snapshot) {
    // Runs (top-level, no parent): run:<id> -> the run value. Decode + validate
    // the desired set at the boundary (a malformed CRDT value is ignored, not
    // trusted), then drop / add / update the local projection to match — with no
    // re-capture (apply_value / materialize_run do not write back to the CRDT).
    std::map<std::string, RunValue> runs;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kRunPrefix);
        if (id.empty()) continue;
        if (std::optional<RunValue> parsed = parse_run_value(value)) {
            runs.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_run_ids;
    for (const auto& run : runs_) local_run_ids.push_back(run->id());
    for (const std::string& run_id : local_run_ids) {
        if (!runs.contains(run_id)) materialize_drop_run(run_id);
    }
    for (auto& [id, value] : runs) {
        auto run = find_run(id);
        if (!run) {
            materialize_run(id, std::move(value));
        } else if (run->metadata() != to_json(value)) {
            run->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_models(const nlohmann::json& snapshot) {
    // Models (top-level, no parent): model:<id> -> the model value. Decode +
    // validate the desired set at the boundary (a malformed CRDT value is
    // ignored, not trusted), then drop / add / update the local projection to
    // match — with no re-capture. apply_value updates only the metadata; the
    // serialized weights body is NOT in the document and arrives separately over
    // the large-object body seam (the blob store requests it when missing).
    std::map<std::string, ModelValue> models;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kModelPrefix);
        if (id.empty()) continue;
        if (std::optional<ModelValue> parsed = parse_model_value(value)) {
            models.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_model_ids;
    for (const auto& model : models_) local_model_ids.push_back(model->id());
    for (const std::string& model_id : local_model_ids) {
        if (!models.contains(model_id)) materialize_drop_model(model_id);
    }
    for (auto& [id, value] : models) {
        auto model = find_model(id);
        if (!model) {
            materialize_model(id, std::move(value));
        } else if (model->metadata() != to_json(value)) {
            model->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_indexes(const nlohmann::json& snapshot) {
    // Indexes (top-level, no parent): index:<id> -> the index value. Decode +
    // validate the desired set at the boundary (a malformed CRDT value is ignored,
    // not trusted), then drop / add / update the local projection to match — with
    // no re-capture. An index is metadata only (no body), so apply_value rewrites
    // index.json and nothing more.
    std::map<std::string, IndexValue> indexes;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kIndexPrefix);
        if (id.empty()) continue;
        if (std::optional<IndexValue> parsed = parse_index_value(value)) {
            indexes.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_index_ids;
    for (const auto& index : indexes_) local_index_ids.push_back(index->id());
    for (const std::string& index_id : local_index_ids) {
        if (!indexes.contains(index_id)) materialize_drop_index(index_id);
    }
    for (auto& [id, value] : indexes) {
        auto index = find_index(id);
        if (!index) {
            materialize_index(id, std::move(value));
        } else if (index->metadata() != to_json(value)) {
            index->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_jobs(const nlohmann::json& snapshot) {
    // Compute jobs (top-level, no parent): job:<id> -> the job value. Decode +
    // validate the desired set at the boundary (a malformed CRDT value is ignored,
    // not trusted), then drop / add / update the local projection to match — with
    // no re-capture and no audit (apply_value / materialize_job do not write back
    // to the CRDT, and a peer's create/finish arrives already-audited as its own
    // replicated `audit:` event).
    std::map<std::string, ComputeJobValue> compute_jobs;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kJobPrefix);
        if (id.empty()) continue;
        if (std::optional<ComputeJobValue> parsed =
                parse_compute_job_value(value)) {
            compute_jobs.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_job_ids;
    for (const auto& job : jobs_) local_job_ids.push_back(job->id());
    for (const std::string& job_id : local_job_ids) {
        if (!compute_jobs.contains(job_id)) materialize_drop_job(job_id);
    }
    for (auto& [id, value] : compute_jobs) {
        auto job = find_job(id);
        if (!job) {
            materialize_job(id, std::move(value));
        } else if (job->metadata() != to_json(value)) {
            job->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_partitions(const nlohmann::json& snapshot) {
    // Dataset partitions (top-level, no parent): part:<id> -> the partition value.
    // The catalog is replicate-to-all, so each node materializes the same set.
    // Decode + validate the desired set at the boundary (a malformed CRDT value is
    // ignored, not trusted), then drop / add / update the local projection to match
    // — with no re-capture (apply_value / materialize_dataset_partition do not write
    // back to the CRDT).
    std::map<std::string, DatasetPartitionValue> partitions;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPartitionPrefix);
        if (id.empty()) continue;
        if (std::optional<DatasetPartitionValue> parsed =
                parse_dataset_partition_value(value)) {
            partitions.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_partition_ids;
    for (const auto& partition : dataset_partitions_) {
        local_partition_ids.push_back(partition->id());
    }
    for (const std::string& partition_id : local_partition_ids) {
        if (!partitions.contains(partition_id)) {
            materialize_drop_dataset_partition(partition_id);
        }
    }
    for (auto& [id, value] : partitions) {
        auto partition = find_dataset_partition(id);
        if (!partition) {
            materialize_dataset_partition(id, std::move(value));
        } else if (partition->metadata() != to_json(value)) {
            partition->apply_value(
                std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_principals(const nlohmann::json& snapshot) {
    // Principals (top-level, no parent): principal:<id> -> { kind, name,
    // created }. Decode the desired set, then drop / add / update the local
    // projection to match the document — no re-capture.
    struct PrincipalEntry {
        PrincipalKind kind;
        std::string name;
        std::string created;
    };
    std::map<std::string, PrincipalEntry> principals;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPrincipalPrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) {
            continue;
        }
        const auto kind = principal_kind_from(value.value("kind", std::string{}));
        if (!kind) continue;  // boundary validation
        principals.emplace(std::move(id),
                           PrincipalEntry{*kind,
                                          value.value("name", std::string{}),
                                          value.value("created", std::string{})});
    }
    std::vector<std::string> local_principal_ids;
    for (const auto& principal : principals_) {
        local_principal_ids.push_back(principal->id());
    }
    for (const std::string& principal_id : local_principal_ids) {
        if (!principals.contains(principal_id)) {
            materialize_drop_principal(principal_id);
        }
    }
    for (const auto& [id, entry] : principals) {
        auto principal = find_principal_by_id(id);
        if (!principal) {
            materialize_principal(id, entry.kind, entry.name, entry.created);
        } else if (principal->name() != entry.name ||
                   principal->kind() != entry.kind) {
            principal->set_metadata(entry.kind, entry.name,
                                    entry.created);  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_access_keys(const nlohmann::json& snapshot) {
    // Access keys (owned by a principal): key:<id> -> { principal, scope,
    // created }. Keys are immutable credentials (never edited), so reconcile only
    // adds them under their owning principal and drops the ones a peer removed.
    struct AccessKeyEntry {
        std::string principal_id;
        Scope scope;
        CapabilitySet capabilities;
        std::string created;
    };
    std::map<std::string, AccessKeyEntry> access_keys;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kAccessKeyPrefix);
        if (id.empty() || !value.is_object() || !value.contains("principal") ||
            !value.contains("scope")) {
            continue;
        }
        const auto scope = scope_from_json(value["scope"]);
        if (!scope) continue;  // boundary
        // Capabilities are optional (a peer on an older shape omits them => no
        // rights); a present-yet-malformed set is rejected rather than applied.
        CapabilitySet capabilities;
        if (value.contains("capabilities")) {
            const auto parsed = capability_set_from_json(value["capabilities"]);
            if (!parsed) continue;
            capabilities = *parsed;
        }
        access_keys.emplace(
            std::move(id),
            AccessKeyEntry{value.value("principal", std::string{}), *scope,
                           std::move(capabilities),
                           value.value("created", std::string{})});
    }
    // Removals first (a peer dropped a key, or its principal is gone).
    for (const auto& principal : principals_) {
        std::vector<std::string> local_key_ids;
        for (const auto& key : principal->keys()) {
            local_key_ids.push_back(key->id());
        }
        for (const std::string& key_id : local_key_ids) {
            const auto it = access_keys.find(key_id);
            if (it == access_keys.end() ||
                it->second.principal_id != principal->id()) {
                principal->materialize_drop_key(key_id);
            }
        }
    }
    // Additions (under the owning principal, if it exists locally).
    for (auto& [id, entry] : access_keys) {
        auto principal = find_principal_by_id(entry.principal_id);
        if (principal && !principal->find_key(id)) {
            principal->materialize_key(id, std::move(entry.scope),
                                       std::move(entry.capabilities),
                                       entry.created);
        }
    }
}

void Node::reconcile_audit_events(const nlohmann::json& snapshot) {
    // Audit events (top-level, no parent): audit:<id> -> the event value. The
    // trail is immutable + append-only, so reconcile only *adds* events the
    // document has that we don't (a peer's, merged in via gossip) and never
    // removes or edits one. Each value is validated at the boundary — a
    // malformed CRDT value is ignored, not trusted into the trail.
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kAuditPrefix);
        if (id.empty()) continue;
        if (std::optional<AuditEventValue> parsed =
                parse_audit_event_value(value)) {
            materialize_audit_event(std::move(id), std::move(*parsed));
        }
    }
    // Keep the trail in chronological order after merging peer events in.
    std::sort(audit_events_.begin(), audit_events_.end(),
              [](const auto& a, const auto& b) {
                  if (a->time() != b->time()) return a->time() < b->time();
                  return a->id() < b->id();
              });
}

IntegrityReport Node::check_integrity() const {
    IntegrityReport report;
    for (const auto& db : databases_) {
        ++report.databases;
        for (const auto& table : db->tables()) {
            ++report.tables;
            report.rows += table->row_count();
        }
        for (const LargeObject& object : db->large_objects().list()) {
            ++report.large_objects;
            if (!object.body_present) {
                ++report.large_object_bodies_missing;
                continue;
            }
            ++report.large_object_bodies_present;
            // Re-hash the on-disk body and compare to the recorded digest.
            const std::optional<std::string> body =
                db->large_objects().read(object.id);
            if (!body || (!object.sha256.empty() &&
                          sha256_hex(*body) != object.sha256)) {
                ++report.corrupt_large_objects;
            }
        }
    }

    // Model weights ride the same large-object body contract: the metadata
    // (size + sha256) replicates, the body lives on disk and transfers
    // peer-to-peer. A model that has weights metadata but no body yet is awaiting
    // transfer (informational); a present body that no longer hashes to the
    // recorded digest is corruption.
    for (const auto& model : models_) {
        ++report.models;
        if (!model->has_weights()) continue;
        if (!model->has_weights_body()) {
            ++report.model_weights_missing;
            continue;
        }
        ++report.model_weights_present;
        const std::optional<std::string> body = model->read_weights();
        const ModelWeights& weights = *model->weights();
        if (!body || (!weights.sha256.empty() &&
                      sha256_hex(*body) != weights.sha256)) {
            ++report.corrupt_model_weights;
        }
    }

    // Dangling references in the replicated catalog: a table/large object whose
    // database is gone, or a row whose table is gone (orphaned keys the
    // reconciler ignores but that shouldn't accumulate).
    if (replication_) {
        const nlohmann::json snapshot = replication_->snapshot();
        std::unordered_set<std::string> database_ids;
        std::unordered_set<std::string> table_ids;
        std::unordered_set<std::string> project_ids;
        std::unordered_set<std::string> principal_ids;
        for (const auto& [key, value] : snapshot.items()) {
            if (std::string id = catalog::id_after(key, catalog::kDatabasePrefix);
                !id.empty()) {
                database_ids.insert(std::move(id));
            } else if (std::string table_id =
                           catalog::id_after(key, catalog::kTablePrefix);
                       !table_id.empty()) {
                table_ids.insert(std::move(table_id));
            } else if (std::string project_id =
                           catalog::id_after(key, catalog::kProjectPrefix);
                       !project_id.empty()) {
                project_ids.insert(std::move(project_id));
            } else if (std::string principal_id =
                           catalog::id_after(key, catalog::kPrincipalPrefix);
                       !principal_id.empty()) {
                principal_ids.insert(std::move(principal_id));
            }
        }
        const auto parent_missing = [](const nlohmann::json& value,
                                       const char* field,
                                       const std::unordered_set<std::string>& set) {
            return value.is_object() && value.contains(field) &&
                   !set.contains(value[field].get<std::string>());
        };
        for (const auto& [key, value] : snapshot.items()) {
            if (!catalog::id_after(key, catalog::kTablePrefix).empty()) {
                if (parent_missing(value, "db", database_ids)) ++report.dangling_tables;
            } else if (!catalog::id_after(key, catalog::kRowPrefix).empty()) {
                if (parent_missing(value, "tbl", table_ids)) ++report.dangling_rows;
            } else if (!catalog::id_after(key, catalog::kLargeObjectPrefix).empty()) {
                if (parent_missing(value, "db", database_ids)) {
                    ++report.dangling_large_objects;
                }
            } else if (!catalog::id_after(key, catalog::kInputPrefix).empty()) {
                if (parent_missing(value, "project", project_ids)) {
                    ++report.dangling_inputs;
                }
            } else if (!catalog::id_after(key, catalog::kGraphPrefix).empty()) {
                if (parent_missing(value, "project", project_ids)) {
                    ++report.dangling_graphs;
                }
            } else if (!catalog::id_after(key, catalog::kAccessKeyPrefix)
                            .empty()) {
                if (parent_missing(value, "principal", principal_ids)) {
                    ++report.dangling_access_keys;
                }
            }
        }
    }
    return report;
}

std::string Node::status_line() const {
    if (!running_) return "stopped";
    char buf[320];
    std::snprintf(
        buf, sizeof(buf),
        "running — up %.0fs, %llu ticks, %zu databases, %zu projects, "
        "%zu deployments, %zu runs, %zu models, %zu principals, %zu audit "
        "events",
        uptime_seconds(), static_cast<unsigned long long>(ticks_),
        databases_.size(), projects_.size(), deployments_.size(),
        runs_.size(), models_.size(), principals_.size(),
        audit_events_.size());
    return buf;
}

}  // namespace zetta
