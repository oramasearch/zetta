#include "zetta/node.hpp"

#include <fcntl.h>
#include <unistd.h>

#include "zetta/connect/connection_store.hpp"

#include "zetta/pubsub/node_integration.hpp"
#include "zetta/pubsub/pubsub.hpp"

#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/ml/synthetic_private_job.hpp"
#include "zetta/ml/entity_resolution_job.hpp"
#include "zetta/index/distributed_fulltext.hpp"
#include "zetta/index/in_memory_index_backend.hpp"
#include "zetta/ml/segmentation_methodology.hpp"
#include "zetta/ml/anomaly_methodology.hpp"
#include "zetta/ml/forecast_methodology.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/ml/next_best_action_methodology.hpp"
#include "zetta/network/records.hpp"
#ifdef ZETTA_WITH_ORAMACORE
#include "zetta/index/oramacore_fulltext_index.hpp"
#endif
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/entity_key_table.hpp"
#include "zetta/data/partition_rebalance.hpp"
#include "zetta/data/table.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/safe_mode.hpp"
#include "zetta/governance/permission.hpp"
#include "zetta/governance/token_secret.hpp"
#include "zetta/runtime.hpp"
#include "zetta/agent/model_cache.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/version.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
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

bool reconciliation_includes_key(
    const std::vector<std::string>* changed_keys,
    const std::string& key) {
    return !changed_keys ||
           std::ranges::find(*changed_keys, key) != changed_keys->end();
}

struct ReconcileBatch {
    nlohmann::json values = nlohmann::json::object();
    std::vector<std::string> keys;
};

struct ReplicationReconcileBatches {
    ReconcileBatch data;
    ReconcileBatch projects;
    ReconcileBatch inputs;
    ReconcileBatch graphs;
    ReconcileBatch deployments;
    ReconcileBatch runs;
    ReconcileBatch studies;
    ReconcileBatch models;
    ReconcileBatch monitors;
    ReconcileBatch experiments;
    ReconcileBatch profiles;
    ReconcileBatch plans;
    ReconcileBatch indexes;
    ReconcileBatch jobs;
    ReconcileBatch partitions;
    ReconcileBatch principals;
    ReconcileBatch access_keys;
    ReconcileBatch users;
    ReconcileBatch roles;
    ReconcileBatch tokens;
    ReconcileBatch user_roles;
    ReconcileBatch policies;
    ReconcileBatch ledger_entries;
    ReconcileBatch nodes;
};

ReconcileBatch* batch_for_replication_key(
    ReplicationReconcileBatches& batches,
    const std::string& key) {
    if (key.starts_with(catalog::kDatabasePrefix) ||
        key.starts_with(catalog::kTablePrefix) ||
        key.starts_with(catalog::kRowPrefix) ||
        key.starts_with(catalog::kLargeObjectPrefix) ||
        key.starts_with(catalog::kManifestPrefix) ||
        key.starts_with(catalog::kColumnOrderPrefix)) {
        return &batches.data;
    }
    if (key.starts_with(catalog::kProjectPrefix)) {
        return &batches.projects;
    }
    if (key.starts_with(catalog::kInputPrefix)) {
        return &batches.inputs;
    }
    if (key.starts_with(catalog::kGraphPrefix)) {
        return &batches.graphs;
    }
    if (key.starts_with(catalog::kDeploymentPrefix)) {
        return &batches.deployments;
    }
    if (key.starts_with(catalog::kRunPrefix)) {
        return &batches.runs;
    }
    if (key.starts_with(catalog::kStudyPrefix)) {
        return &batches.studies;
    }
    if (key.starts_with(catalog::kModelPrefix)) {
        return &batches.models;
    }
    if (key.starts_with(catalog::kMonitorPrefix)) {
        return &batches.monitors;
    }
    if (key.starts_with(catalog::kExperimentPrefix) ||
        key.starts_with(catalog::kExperimentAssignmentPrefix)) {
        return &batches.experiments;
    }
    if (key.starts_with(catalog::kWorkspaceProfilePrefix)) {
        return &batches.profiles;
    }
    if (key.starts_with(catalog::kPlanPrefix)) {
        return &batches.plans;
    }
    if (key.starts_with(catalog::kIndexPrefix)) {
        return &batches.indexes;
    }
    if (key.starts_with(catalog::kJobPrefix)) {
        return &batches.jobs;
    }
    if (key.starts_with(catalog::kPartitionPrefix)) {
        return &batches.partitions;
    }
    if (key.starts_with(catalog::kPrincipalPrefix)) {
        return &batches.principals;
    }
    if (key.starts_with(catalog::kAccessKeyPrefix)) {
        return &batches.access_keys;
    }
    if (key.starts_with(catalog::kUserPrefix)) {
        return &batches.users;
    }
    if (key.starts_with(catalog::kRolePrefix)) {
        return &batches.roles;
    }
    if (key.starts_with(catalog::kTokenPrefix)) {
        return &batches.tokens;
    }
    if (key.starts_with(catalog::kUserRolePrefix)) {
        return &batches.user_roles;
    }
    if (key.starts_with(catalog::kPolicyPrefix)) {
        return &batches.policies;
    }
    if (key.starts_with(catalog::kLedgerPrefix)) {
        return &batches.ledger_entries;
    }
    if (key.starts_with(catalog::kNodePrefix)) {
        return &batches.nodes;
    }
    return nullptr;
}

void add_current_value(ReconcileBatch& batch,
                       const nlohmann::json& snapshot,
                       const std::string& key) {
    batch.keys.push_back(key);
    if (const auto value = snapshot.find(key); value != snapshot.end()) {
        batch.values[key] = *value;
    }
}

void include_experiment_assignments(ReconcileBatch& batch,
                                    const nlohmann::json& snapshot) {
    std::set<std::string> parent_ids;
    for (const std::string& key : batch.keys) {
        const std::string id = catalog::id_after(key, catalog::kExperimentPrefix);
        if (!id.empty()) {
            parent_ids.insert(id);
        }
    }
    if (parent_ids.empty()) {
        return;
    }
    // A parent may arrive after an assignment, or be removed while its child
    // keys remain in the document. Revisit those children in the same tick.
    for (const auto& [key, value] : snapshot.items()) {
        const std::string compound_id =
            catalog::id_after(key, catalog::kExperimentAssignmentPrefix);
        if (parent_ids.contains(compound_id.substr(0, compound_id.find(':'))) &&
            !batch.values.contains(key)) {
            add_current_value(batch, snapshot, key);
        }
    }
}

ReplicationReconcileBatches make_reconcile_batches(
    const nlohmann::json& snapshot,
    const std::vector<std::string>& changed_keys) {
    ReplicationReconcileBatches batches;
    for (const std::string& key : changed_keys) {
        if (ReconcileBatch* batch = batch_for_replication_key(batches, key)) {
            add_current_value(*batch, snapshot, key);
        }
    }
    include_experiment_assignments(batches.experiments, snapshot);
    // A table's `tbl:`, `mfst:` and `cols:` keys reconcile together: a changed
    // one brings the other two, so the table is in the batch and its manifest
    // and column order are applied (or cleared) from the current document.
    for (const std::string& key : batches.data.keys) {
        std::string table_id = catalog::id_after(key, catalog::kTablePrefix);
        if (table_id.empty()) table_id = catalog::id_after(key, catalog::kManifestPrefix);
        if (table_id.empty()) {
            table_id = catalog::id_after(key, catalog::kColumnOrderPrefix);
        }
        if (table_id.empty()) continue;
        for (const std::string& related_key :
             {catalog::table_key(table_id), catalog::manifest_key(table_id),
              catalog::column_order_key(table_id)}) {
            if (related_key == key || batches.data.values.contains(related_key)) {
                continue;
            }
            if (const auto value = snapshot.find(related_key);
                value != snapshot.end()) {
                batches.data.values[related_key] = *value;
            }
        }
    }
    return batches;
}

}  // namespace

Node::Node(NodeConfig config, zetta::cluster::ReplicationManager* replication)
    : config_(std::move(config)),
      replication_(replication),
      experiment_manager_(config_.data_dir, replication),
      checkpoint_store_(std::filesystem::path(config_.data_dir) / "checkpoints") {
    register_memory_probes();
    // The owner thread is adopted LAZILY, at the first tick() (#624) — never
    // here. A constructed-but-never-ticked node has no owner loop to drain
    // the gate, so run_on_owner treats it as ungated (inline); adopting the
    // constructing thread here would instead deadlock any cross-thread caller
    // against an owner that never drains (a harness that builds nodes and
    // drives them from its own threads without ticking — the testkit's
    // per-node SPMD threads).
    // Register the always-available index backends so an `index` step resolves
    // out of the box (the per-node container the run path builds + queries
    // through, #528). InMemoryIndexBackend ("memory_search") is the pure-host
    // reference Search backend; OramaCoreFulltextIndex ("oramacore_fulltext") is
    // the native full-text (BM25F) Search backend, registered only when its
    // private submodule was built (ZETTA_WITH_ORAMACORE). A host may add more.
    index_registry_.add(std::make_shared<zetta::index::InMemoryIndexBackend>());
#ifdef ZETTA_WITH_ORAMACORE
    index_registry_.add(std::make_shared<zetta::index::OramaCoreFulltextIndex>());
#endif
}

// Out-of-line (see node.hpp): destroying coordinator_ here means the complete
// compute::JobCoordinator type is visible, and — for a node that reached stop()
// — coordinator_ is already null, so the unwind never touches a dead cluster.
Node::~Node() = default;

void Node::register_memory_probes() {
    memory_accounting_.register_probe("audit", [this] { return audit_memory_usage(); });
    memory_accounting_.register_probe("pubsub", [this] { return pubsub_memory_usage(); });
    memory_accounting_.register_probe("replication", [this] {
        return replication_memory_usage();
    });
    memory_accounting_.register_probe("datasets_tables", [this] {
        return dataset_memory_usage();
    });
    memory_accounting_.register_probe("registry_sessions_connections", [this] {
        return registry_memory_usage();
    });
    memory_accounting_.register_probe("runtime", [this] { return runtime_memory_usage(); });
    memory_accounting_.register_probe("models", [this] { return model_memory_usage(); });
    memory_accounting_.register_probe("runs_jobs", [this] {
        return run_job_memory_usage();
    });
}

zetta::memory::ProbeSample Node::audit_memory_usage() const {
    zetta::memory::ProbeSample sample;
    if (audit_trail_) {
        const auto usage = audit_trail_->memory_usage();
        sample.bytes += usage.bytes;
        sample.items += usage.items;
    }
    for (const auto& [key, window] : audit_summary_windows_) {
        sample.bytes += sizeof(window) + key.capacity() + window.actor.capacity() +
                        window.capability.capacity() + window.scope.capacity() +
                        window.started.capacity();
    }
    return sample;
}

zetta::memory::ProbeSample Node::pubsub_memory_usage() const {
    zetta::memory::ProbeSample sample;
    if (!pubsub_) {
        return sample;
    }
    const auto topics = pubsub_->topic_stats();
    sample.items = topics.size();
    for (const auto& topic : topics) {
        sample.bytes += topic.memory_bytes;
        sample.items += topic.held_events;
    }
    return sample;
}

zetta::memory::ProbeSample Node::replication_memory_usage() const {
    if (!replication_) {
        return {};
    }
    const auto usage = replication_->memory_usage();
    return {usage.bytes, usage.items + usage.pending_changes, 0};
}

zetta::memory::ProbeSample Node::dataset_memory_usage() const {
    zetta::memory::ProbeSample sample;
    for (const auto& database : databases_) {
        sample.bytes += sizeof(zetta::data::Database) + database->id().capacity() +
                        database->name().capacity();
        ++sample.items;
        for (const auto& table : database->tables()) {
            const auto usage = table->store().memory_usage();
            sample.bytes += sizeof(zetta::data::Table) + usage.bytes;
            sample.bytes += table->id().capacity() + table->name().capacity();
            sample.items += usage.items + 1;
        }
    }
    sample.bytes += dataset_partitions_.size() * 512;
    sample.items += dataset_partitions_.size();
    return sample;
}

zetta::memory::ProbeSample Node::registry_memory_usage() const {
    zetta::memory::ProbeSample sample;
    const auto registry_items = projects_.size() + deployments_.size() +
                                studies_.size() + monitors_.size() +
                                workspace_profiles_.size() + plans_.size() +
                                indexes_.size() + principals_.size() + users_.size() +
                                roles_.size() + tokens_.size() + user_roles_.size() +
                                policies_.size() + ledger_entries_.size();
    sample.items = registry_items;
    sample.bytes = registry_items * 512;
    {
        std::lock_guard lock{owner_work_mutex_};
        sample.items += owner_work_queue_.size();
        sample.bytes += owner_work_queue_.size() * sizeof(OwnerWork);
    }
    return sample;
}

zetta::memory::ProbeSample Node::runtime_memory_usage() const {
    const auto usage = zetta::runtime::memory_usage();
    return {usage.bytes,
            usage.coroutine_frames + usage.blocking_queue_depth + usage.thread_count,
            usage.thread_stack_bytes};
}

zetta::memory::ProbeSample Node::model_memory_usage() const {
    const auto loaded = zetta::agent::ModelCache::instance().memory_usage();
    zetta::memory::ProbeSample sample{loaded.bytes, loaded.models, 0};
    sample.bytes += models_.size() * (sizeof(zetta::ml::Model) + 1024);
    sample.items += models_.size();
    return sample;
}

zetta::memory::ProbeSample Node::run_job_memory_usage() const {
    zetta::memory::ProbeSample sample;
    sample.bytes += runs_.size() * (sizeof(zetta::pipeline::Run) + 1024);
    sample.items += runs_.size();
    sample.bytes += jobs_.size() * (sizeof(zetta::compute::ComputeJob) + 1024);
    sample.items += jobs_.size();
    {
        std::lock_guard lock{pipeline_jobs_mutex_};
        sample.bytes += pipeline_jobs_.capacity() *
                        sizeof(zetta::compute::PipelineJobInstrumentation);
        sample.items += pipeline_jobs_.size();
    }
    return sample;
}

zetta::cluster::NodeMetrics Node::sample_node_metrics() {
    auto metrics = node_metrics_sampler_.sample(config_.data_dir);
    const auto memory = memory_snapshot();
    metrics.process_memory_available = memory.allocator.rss_available;
    metrics.process_rss_bytes = memory.allocator.rss_bytes;
    metrics.process_peak_rss_bytes = memory.allocator.peak_rss_bytes;
    metrics.allocator_memory_available = memory.allocator.allocator_available;
    metrics.allocator_in_use_bytes = memory.allocator.in_use_bytes;
    metrics.allocator_free_held_bytes = memory.allocator.free_held_bytes;
    metrics.attributed_memory_bytes = memory.attributed_bytes;
    metrics.unattributed_memory_bytes = memory.unattributed_bytes;
    if (memory.allocator.in_use_bytes != 0) {
        metrics.memory_explained_percent =
            100.0 * static_cast<double>(memory.attributed_bytes) /
            static_cast<double>(memory.allocator.in_use_bytes);
    }
    return metrics;
}

void Node::report_memory(bool force) {
    constexpr auto kCheckInterval = std::chrono::seconds(1);
    constexpr auto kLogInterval = std::chrono::minutes(5);
    constexpr std::uint64_t kRssStepBytes = 256ULL * 1024 * 1024;
    const auto now = Clock::now();
    if (!force && last_memory_check_.time_since_epoch().count() != 0 &&
        now - last_memory_check_ < kCheckInterval) {
        return;
    }
    last_memory_check_ = now;
    const auto snapshot = memory_snapshot();
    const auto rss_step = snapshot.allocator.rss_bytes / kRssStepBytes;
    const bool crossed_step = last_memory_log_.time_since_epoch().count() != 0 &&
                              rss_step != last_rss_step_;
    const bool periodic = last_memory_log_.time_since_epoch().count() == 0 ||
                          now - last_memory_log_ >= kLogInterval;
    if (force || crossed_step || periodic) {
        std::printf("zetta: INFO %s\n",
                    zetta::memory::format_log_line(snapshot).c_str());
        last_memory_log_ = now;
        last_rss_step_ = rss_step;
    }
    if (config_.malloc_trim_threshold_bytes != 0 &&
        snapshot.allocator.free_held_bytes >= config_.malloc_trim_threshold_bytes &&
        (last_malloc_trim_.time_since_epoch().count() == 0 ||
         now - last_malloc_trim_ >= kLogInterval)) {
        const auto trim = zetta::memory::trim_allocator();
        last_malloc_trim_ = now;
        std::printf("zetta: INFO memory: malloc_trim supported=%s released=%s\n",
                    trim.supported ? "true" : "false",
                    trim.released ? "true" : "false");
    }
}

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

void Node::record_pipeline_job(zetta::compute::PipelineJobInstrumentation instrumentation) {
    // Append the recorded near-data job, bounding the ring to the most-recent
    // kMaxPipelineJobs (drop oldest) so this ephemeral, observed registry can't grow
    // without limit over a long-lived node. The source for system.pipeline_jobs.
    std::lock_guard<std::mutex> lock(pipeline_jobs_mutex_);
    pipeline_jobs_.push_back(std::move(instrumentation));
    if (pipeline_jobs_.size() > kMaxPipelineJobs) {
        const std::size_t excess = pipeline_jobs_.size() - kMaxPipelineJobs;
        pipeline_jobs_.erase(pipeline_jobs_.begin(),
                             pipeline_jobs_.begin() +
                                 static_cast<std::ptrdiff_t>(excess));
    }
}

std::vector<zetta::compute::PipelineJobInstrumentation> Node::pipeline_jobs() const {
    std::lock_guard<std::mutex> lock(pipeline_jobs_mutex_);
    return pipeline_jobs_;
}

std::vector<zetta::data::DatasetSummary> Node::datasets() const {
    // One dataset per table across every database — computed live from the tree +
    // the partition catalog, so the system.datasets view never drifts from the
    // truth (RULE-7; #585). A dataset is a first-class primitive with no stored
    // entity: one dataset == one table. (The virtual `system` catalog has no real
    // Database here, so it is naturally absent.)
    std::vector<zetta::data::DatasetSummary> result;
    for (const auto& database : databases_) {
        for (const auto& table : database->tables()) {
            zetta::data::DatasetSummary dataset;
            dataset.name = table->name();
            dataset.database = database->name();
            dataset.table_id = table->id();
            const zetta::storage::StorageEngine& store = table->store();
            dataset.format = store.name();
            dataset.source_format = table->source_format();
            dataset.replication =
                std::string(to_string(table->replication_strategy().kind()));

            // Schema: the union of each row's object keys in first-seen order — the
            // engine is schemaless, so a dataset's schema is whatever its rows carry
            // — typed by the values they hold (#1021).
            std::unordered_set<std::string> seen;
            const auto note_column = [&](const std::string& column) {
                if (seen.insert(column).second) dataset.columns.push_back(column);
            };
            zetta::data::ColumnProfile profile;

            // Row count + schema.
            //
            // A **partition-only** table (#833) does not hold the dataset in its
            // local store: each node has only the slice it produced, and on a node
            // that produced none the store is empty. Its row count and schema come
            // from the partition catalog instead — replicate-to-all, so every node
            // reports the same dataset, which is the whole point of the view.
            if (table->partition_only()) {
                for (const auto& partition : dataset_partitions_) {
                    if (partition->table() != table->id()) continue;
                    const zetta::data::PartitionRowRange& range =
                        partition->row_range();
                    const std::uint64_t partition_rows =
                        range.end > range.start ? range.end - range.start : 0;
                    dataset.rows += partition_rows;
                    const nlohmann::json& stats = partition->stats();
                    profile.add_partition_stats(stats, partition_rows);
                    if (!stats.is_object()) continue;
                    const auto columns = stats.find("columns");
                    if (columns == stats.end() || !columns->is_array()) continue;
                    for (const nlohmann::json& column : *columns) {
                        if (column.is_string()) note_column(column.get<std::string>());
                    }
                }
            } else {
                dataset.rows = store.size();
                for (std::size_t i = 0; i < store.size(); ++i) {
                    const zetta::storage::ParquetLazyPtr row = store.at(i);
                    if (!row) continue;
                    profile.add_row(*row);
                    if (!row->is_object()) continue;
                    for (auto field = row->begin(); field != row->end(); ++field) {
                        note_column(field.key());
                    }
                }
            }
            // The source's column order first, when the load recorded one
            // (#1021); the keys the rows carry otherwise read back sorted.
            dataset.column_order_recorded = !table->column_order().empty();
            dataset.columns =
                zetta::data::order_columns(table->column_order(), dataset.columns);
            dataset.typed_columns = profile.columns(dataset.columns);

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
                const zetta::data::PartitionRowRange& range = partition->row_range();
                dataset.partitions.push_back(
                    zetta::data::DatasetPlacement{partition->id(), partition->blob_ref(),
                                     range.start, range.end,
                                     partition->owner_nodes()});
            }

            dataset.partition_count = dataset.partitions.size();

            // Degraded (#837): the manifest says how many catalog entries the
            // finished dataset has. When fewer are visible here, `rows` above is
            // a fraction of the truth — say so rather than under-report it. The
            // planner fails the same query for the same reason, from the same
            // rule (zetta::data::catalog_shortfall).
            if (const auto& manifest = table->partition_manifest(); manifest) {
                dataset.expected_partitions = manifest->partitions;
                dataset.expected_rows = manifest->rows;
                dataset.degraded_reason = zetta::data::catalog_shortfall(
                    *manifest,
                    static_cast<std::uint64_t>(dataset.partitions.size()));
                dataset.degraded = !dataset.degraded_reason.empty();
            }

            result.push_back(std::move(dataset));
        }
    }
    return result;
}

std::vector<zetta::data::DatasetSummary> Node::dataset_list_summaries() const {
    // The list fields of datasets(), from counters only (#1195). One pass over
    // the partition catalog first: per table, how many entries and how many
    // rows their ranges cover — the same sums datasets() makes per table.
    struct CatalogCounts {
        std::uint64_t partitions = 0;
        std::uint64_t rows = 0;
    };
    std::unordered_map<std::string, CatalogCounts> catalog;
    for (const auto& partition : dataset_partitions_) {
        const zetta::data::PartitionRowRange& range = partition->row_range();
        CatalogCounts& counts = catalog[partition->table()];
        ++counts.partitions;
        counts.rows += range.end > range.start ? range.end - range.start : 0;
    }

    std::vector<zetta::data::DatasetSummary> result;
    for (const auto& database : databases_) {
        for (const auto& table : database->tables()) {
            zetta::data::DatasetSummary dataset;
            dataset.name = table->name();
            dataset.database = database->name();
            dataset.table_id = table->id();
            const zetta::storage::StorageEngine& store = table->store();
            dataset.format = store.name();
            dataset.source_format = table->source_format();
            dataset.replication =
                std::string(to_string(table->replication_strategy().kind()));
            dataset.column_order_recorded = !table->column_order().empty();

            const auto counted = catalog.find(table->id());
            const CatalogCounts counts =
                counted == catalog.end() ? CatalogCounts{} : counted->second;
            dataset.partition_count = counts.partitions;
            // A partition-only table (#833) counts its rows from the catalog,
            // as datasets() does; any other table's local store is its rows.
            dataset.rows = table->partition_only() ? counts.rows : store.size();
            // The engine's own count of its chunk files' bytes. Every table's
            // engine keeps one (JsonlStore); one that does not reports 0 here
            // rather than listing files.
            dataset.size_bytes = store.stored_bytes().value_or(0);

            if (const auto& manifest = table->partition_manifest(); manifest) {
                dataset.expected_partitions = manifest->partitions;
                dataset.expected_rows = manifest->rows;
                dataset.degraded_reason =
                    zetta::data::catalog_shortfall(*manifest, counts.partitions);
                dataset.degraded = !dataset.degraded_reason.empty();
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
    load_studies();
    load_models();
    load_monitors();
    load_workspace_profiles();
    load_plans();
    experiment_manager_.load();
    load_indexes();
    load_jobs();
    load_dataset_partitions();
    load_principals();
    load_users();
    load_roles();
    load_tokens();
    load_user_roles();
    load_policies();
    if (!load_audit_events()) {
        running_ = false;
        return false;
    }
    load_ledger_entries();
    if (replication_) {
        if (!load_replication()) {
            running_ = false;
            return false;
        }
        std::string migration_error;
        if (!replication_->retire_audit(
                [&](const std::string& id, const nlohmann::json& value) {
                    const auto parsed = zetta::governance::parse_audit_event_value(value, &migration_error);
                    return parsed && audit_trail_->append(id, *parsed, &migration_error);
                }, [&] {
                    return audit_trail_->flush();
                })) {
            std::fprintf(stderr, "zetta: legacy audit migration failed: %s\n", migration_error.c_str());
            running_ = false;
            return false;
        }
        // The record log (#1231): node-local, beside the catalogue, never in it.
        metric_store_.reset();
        record_store_ = std::make_unique<zetta::data::RecordStore>(
            *replication_, replication_->node_id(),
            std::filesystem::path(config_.data_dir) / "records");
        std::string record_log_error;
        if (!record_store_->open(&record_log_error)) {
            std::fprintf(stderr, "zetta: cannot open the record log: %s\n",
                         record_log_error.c_str());
            running_ = false;
            return false;
        }
        std::string network_record_error;
        if (!zetta::network::initialize_network_record_collections(
                *record_store_, &network_record_error)) {
            std::fprintf(stderr,
                         "zetta: cannot initialize network record collections: %s\n",
                         network_record_error.c_str());
            running_ = false;
            return false;
        }
        metric_store_ = std::make_unique<zetta::data::MetricStore>(
            *replication_, *record_store_, replication_->node_id());
        // The object logs (#1005): node-local, one per bucket, never in the catalogue.
        object_store_ = std::make_unique<zetta::data::ObjectStore>(
            *replication_, replication_->node_id(),
            std::filesystem::path(config_.data_dir) / "objects");
        object_store_->set_default_quota_bytes(config_.object_default_quota_bytes);
        std::string object_log_error;
        if (!object_store_->open(&object_log_error)) {
            std::fprintf(stderr, "zetta: cannot open the object logs: %s\n",
                         object_log_error.c_str());
            running_ = false;
            return false;
        }
        // The newest sealed identity records this node accepted before it
        // stopped, so a replay in the persisted document is refused (#1341).
        if (!load_identity_held()) {
            running_ = false;
            return false;
        }
        seal_preexisting_identity();  // the first start under a root (#764)
        migrate_to_replication();   // capture any local-only data into it
        const zetta::cluster::ReplicationChanges changes =
            replication_->changes_since(0);
        reconcile_from_replication(*changes.snapshot);
        backfill_denormalized_names();
        const zetta::cluster::ReplicationChanges settled =
            replication_->changes_since(0);
        // Establish the membership baseline so a later join/leave is a detectable
        // delta — but never rebalance on startup (the partitions just loaded
        // already reflect their recorded placement).
        last_membership_ = membership_from_snapshot(*settled.snapshot);
        last_replication_change_token_ = settled.next_token;
        persist_replication();
        last_seen_version_ = replication_->version();
    } else {
        backfill_denormalized_names();
    }
    // A dataset load this node was running when it last stopped can never
    // finish: fail its run and drop its partial table (#1302).
    if (const std::size_t settled = zetta::data::recover_object_dataset_loads(*this);
        settled > 0) {
        std::printf("zetta: %zu dataset load(s) left running by the last stop marked failed\n",
                    settled);
    }
    zetta::governance::AuditTrail::release_unused_memory();
    // Opening the sidecar and bounded recent/tail frames is sufficient. The
    // periodic retention pass runs later, outside the startup memory proof.
    last_audit_retention_ = Clock::now();
    std::printf(
        "zetta %s: node started (data_dir=%s, %zu databases, %zu projects, "
        "%zu deployments, %zu runs, %zu studies, %zu models, %zu experiments, %zu monitors, "
        "%zu indexes, %zu jobs, "
        "%zu partitions, %zu principals, %zu users, %zu roles, %zu tokens, "
        "%zu audit events, %zu ledger entries)\n",
        kVersion, config_.data_dir.c_str(), databases_.size(),
        projects_.size(), deployments_.size(), runs_.size(), studies_.size(), models_.size(),
        experiment_manager_.experiments().size(), monitors_.size(), indexes_.size(),
        jobs_.size(), dataset_partitions_.size(),
        principals_.size(), users_.size(), roles_.size(), tokens_.size(),
        audit_event_count(), ledger_entries_.size());
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
    // Saved connector credentials (#1230): seal any legacy plaintext entry and
    // re-seal any sealed under a retired root secret, so a rotated join token
    // takes effect on the stored credentials the first time a node starts with
    // it.
    {
        const zetta::connect::ResealReport resealed =
            zetta::connect::reseal_connections(*this);
        if (resealed.resealed > 0 || !resealed.unreadable.empty() ||
            !resealed.error.empty()) {
            std::printf(
                "zetta: connection credentials: %zu re-sealed, %zu not openable "
                "on this node%s%s\n",
                resealed.resealed, resealed.unreadable.size(),
                resealed.error.empty() ? "" : " — ", resealed.error.c_str());
        }
    }
    // Pub/sub (#1024): governed by this node's users and roles, tables reachable
    // as table:<database>.<table>; reloads what the last run flushed or spilled.
    if (config_.pubsub) {
        zetta::pubsub::PubSubConfig pubsub_config;
        pubsub_config.data_directory = config_.data_dir;
        pubsub_config.auto_create_prefixes = config_.pubsub_auto_create_prefixes;
        pubsub_config.dispatch_mode = config_.pubsub_dispatch_mode;
        pubsub_config.limit_overrides.topics_per_thread =
            config_.pubsub_topics_per_thread;
        pubsub_config.limit_overrides.dispatch_min_threads =
            config_.pubsub_dispatch_min_threads;
        pubsub_config.limit_overrides.dispatch_max_threads =
            config_.pubsub_dispatch_max_threads;
        // Drains and authorization marshal onto the owner loop; while stop()
        // waits for them on this thread, it keeps that queue moving.
        pubsub_config.idle_pump = [this] { if (on_owner_thread()) drain_owner_work(); };
        pubsub_config.spans = &span_recorder();
        pubsub_ = std::make_unique<zetta::pubsub::PubSub>(
            std::move(pubsub_config), zetta::pubsub::make_node_authorizer(*this));
        pubsub_->register_drain("table", zetta::pubsub::make_table_drain(*this));
        zetta::pubsub::connect_audit_trail(*pubsub_, *this);
        pubsub_->start();
    }
    // Re-establish the compute plane if a cluster was attached before this start
    // (a stop()/start() cycle). On first boot main attaches after start(), so this
    // is a no-op then and attach_cluster brings the coordinator up itself.
    start_coordinator();
    study_driver_ = std::make_unique<zetta::ml::StudyDriver>(*this);
    report_memory(true);
    std::fflush(stdout);
    return true;
}

// Flush every table's storage engine. The engines defer file writes so a
// reconcile pass costs one rewrite per touched chunk instead of one per row; this
// is the point at which that deferral is settled, on the thread that owns the
// tree.
void Node::flush_stores() {
    for (const std::shared_ptr<zetta::data::Database>& database : databases_) {
        if (!database) continue;
        for (const std::shared_ptr<zetta::data::Table>& table : database->tables()) {
            if (table) table->store().flush();
        }
    }
}

namespace {

// One record anti-entropy round, off the owner loop: it waits on the signal plane.
zetta::runtime::task<> run_record_anti_entropy(zetta::data::RecordStore* store) {
    [[maybe_unused]] const std::size_t pulled =
        co_await zetta::runtime::offload([store] { return store->anti_entropy(); });
}

}  // namespace

struct Node::RecordAntiEntropyRound {
    zetta::runtime::async_event done;
    std::atomic<bool> finished{false};
};

struct Node::ObjectUpkeepRound {
    zetta::runtime::async_event done;
    std::atomic<bool> finished{false};
};

namespace {

// One object-store upkeep round, off the owner loop: maintenance does disk I/O
// and anti-entropy waits on the signal plane (#1005).
zetta::runtime::task<> run_object_upkeep(zetta::data::ObjectStore* store, bool anti_entropy) {
    [[maybe_unused]] const std::size_t moved = co_await zetta::runtime::offload([store, anti_entropy] {
        store->maintain(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count());
        return anti_entropy ? store->anti_entropy() : std::size_t{0};
    });
}

}  // namespace

struct Node::StorageCensusRound {
    zetta::runtime::async_event done;
    std::atomic<bool> finished{false};
    zetta::data::StorageCensus census;  // written by the pool before `finished`
    std::string measured_at;
    std::chrono::steady_clock::time_point taken{};
};

namespace {

// One storage census, off the owner loop: it walks the data directory (zetta#1259).
// The round that owns the outputs stays alive until this finishes: the completion holds it.
zetta::runtime::task<> run_storage_census(zetta::data::StorageCensus* census,
                                          std::string* measured_at,
                                          std::chrono::steady_clock::time_point* taken,
                                          std::filesystem::path data_dir) {
    [[maybe_unused]] const bool measured =
        co_await zetta::runtime::offload([census, measured_at, taken, data_dir] {
            *census = zetta::data::measure_storage(data_dir);
            *measured_at = zetta::util::now_iso8601();
            *taken = std::chrono::steady_clock::now();
            return true;
        });
}

}  // namespace

std::vector<std::string> Node::cluster_member_ids() const {
    std::vector<std::string> ids;
    if (cluster_) {
        for (const zetta::cluster::ClusterMember& member : cluster_->membership()) {
            ids.push_back(member.node_id);
        }
    }
    if (std::ranges::find(ids, node_id()) == ids.end()) ids.push_back(node_id());
    return ids;
}

void Node::tick() {
    // 0. The owner-thread gate (#624): the loop that ticks IS the owner —
    //    adopt this thread, then execute any node-tree work queued by
    //    background-action workers (run_on_owner). Runs even before start()
    //    so a gated worker never waits on a loop that is ticking.
    owner_thread_.store(std::this_thread::get_id(), std::memory_order_release);
    drain_owner_work();
    if (!running_) return;
    ++ticks_;
    // 1. Pull: the CRDT is the source of truth; when it has advanced (a local
    //    capture or a peer's gossip merged in on the network thread), reconcile
    //    the on-disk projection to match it. Reconcile only materializes (never
    //    captures), so it doesn't move the version — no feedback loop.
    //
    //    **The version is read BEFORE the snapshot, and that is the watermark
    //    step 3 records** (#837). A peer's gossip merges on the *network*
    //    thread, so a merge can land at any point during this tick. Reading the
    //    version at the END (as this did) and recording it as reflected claims
    //    credit for a merge this tick never saw: the projection skips it, and —
    //    with no further write to move the version — never revisits it. The
    //    partition catalog then stalls permanently short of the document, which
    //    is exactly the "a peer sees 8 of 12 partitions forever" failure behind
    //    #837. Recording the EARLIER version is the conservative direction: at
    //    worst the next tick reconciles a snapshot it has already seen, which is
    //    idempotent (reconcile only materializes).
    const std::string version_at_pull =
        replication_ ? replication_->version() : std::string{};
    if (replication_ && version_at_pull != last_seen_version_) {
        const zetta::cluster::ReplicationChanges changes =
            replication_->changes_since(last_replication_change_token_);
        const std::size_t full_reconcile_threshold =
            std::max<std::size_t>(1, changes.snapshot->size() / 4);
        const bool full_reconcile =
            changes.full_resync ||
            changes.keys.size() > full_reconcile_threshold;
        if (full_reconcile) {
            reconcile_from_replication(*changes.snapshot);
            rebalance_partitions_on_membership_change(*changes.snapshot);
        } else if (!changes.keys.empty()) {
            reconcile_replication_changes(*changes.snapshot, changes.keys);
        }
        last_replication_change_token_ = changes.next_token;
        // 1c. Make the reconciled rows durable, ONCE. Reconcile issues an update
        //     per row and the storage engine defers the file write (#952), so the
        //     whole pass costs one rewrite per chunk it actually touched rather
        //     than one per row. Reads never saw the difference — the engine
        //     answers from memory — but the projection must not lag past the tick
        //     that produced it.
        flush_stores();
    }
    // 2. Control plane: drive each local deployment desired → actual, starting
    //    or self-healing its run. May capture new runs into the document.
    controller_.reconcile();
    if (study_driver_) study_driver_->reconcile();
    // 2a. Write coalesced trainer captures whose interval has passed (#1217).
    flush_pending_captures(false);
    // 2b. Search plane (#641): keep this node's per-partition search sub-indexes
    //     current with the (replicated) Search index entities + the placed
    //     partitions this node owns — so any node can answer a peer's
    //     `fulltext.query` scatter. Fingerprint-guarded (cheap when unchanged);
    //     local machinery only, nothing is captured.
    reconcile_search_indexes();
    // 2c. SELECT plane (#833): refresh the network-thread-readable snapshot of
    //     the partition-only partitions this node owns, so a peer's `sql.scan`
    //     is answered from the current catalog without touching the Node tree
    //     off the owner thread. Fingerprint-guarded; nothing is captured.
    if (distributed_select_) distributed_select_->refresh();
    // 3. Persist if the document moved (a peer merge in step 1 or a run started
    //    in step 2), and remember the version we have now reflected on disk —
    //    the one read at the TOP of the tick, never the current one (see step 1).
    if (replication_) {
        if (replication_->version() != last_seen_version_) {
            replication_persist_pending_ = true;
            last_seen_version_ = version_at_pull;
        }
        constexpr auto kPersistInterval = std::chrono::seconds(1);
        if (replication_persist_pending_ &&
            Clock::now() - last_replication_persist_ >= kPersistInterval) {
            persist_replication();
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
        // The storage census (zetta#1259): take a finished round, start the next when due.
        if (storage_census_round_ &&
            storage_census_round_->finished.load(std::memory_order_acquire)) {
            storage_census_ = std::move(storage_census_round_->census);
            storage_census_measured_at_ = std::move(storage_census_round_->measured_at);
            storage_census_taken_ = storage_census_round_->taken;
            storage_census_measured_ = true;
            storage_census_round_.reset();
        }
        if (!storage_census_round_ &&
            (last_storage_census_.time_since_epoch().count() == 0 ||
             now - last_storage_census_ >= kStorageCensusInterval)) {
            last_storage_census_ = now;
            storage_census_round_ = std::make_shared<StorageCensusRound>();
            zetta::runtime::spawn(run_storage_census(&storage_census_round_->census,
                                                     &storage_census_round_->measured_at,
                                                     &storage_census_round_->taken,
                                                     config_.data_dir),
                                  [round = storage_census_round_] {
                                      round->finished.store(true, std::memory_order_release);
                                      round->done.set();
                                  });
        }
        if (last_telemetry_sample_.time_since_epoch().count() == 0 ||
            now - last_telemetry_sample_ >= kTelemetryInterval) {
            last_telemetry_sample_ = now;
            zetta::cluster::NodeMetrics metrics = sample_node_metrics();
            if (storage_census_measured_) {
                metrics.storage_census_available = true;
                metrics.storage_census = storage_census_;
                metrics.storage_census_measured_at = storage_census_measured_at_;
                metrics.storage_census_age_seconds =
                    std::max(0.0, duration<double>(now - storage_census_taken_).count());
            }
            telemetry_->record_local(node_id(), metrics);
            cluster_->broadcast_event(
                std::string(zetta::cluster::NodeTelemetryExchange::kMethod),
                encode_node_metrics(metrics, node_id()));
        }
    }
    // The record log (#1231): retention every second, a flush and a bounded
    // compaction step every tick, and anti-entropy with the live peers every five
    // seconds on the blocking pool (it waits on the signal plane).
    if (record_store_) {
        constexpr auto kRecordSweepInterval = std::chrono::seconds(1);
        if (Clock::now() - last_record_sweep_ >= kRecordSweepInterval) {
            last_record_sweep_ = Clock::now();
            record_store_->sweep(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::system_clock::now().time_since_epoch())
                                     .count());
        }
        record_store_->maintain();
        constexpr auto kAntiEntropyInterval = std::chrono::seconds(5);
        if (cluster_ && !record_anti_entropy_ &&
            Clock::now() - last_record_anti_entropy_ >= kAntiEntropyInterval) {
            last_record_anti_entropy_ = Clock::now();
            record_anti_entropy_ = std::make_shared<RecordAntiEntropyRound>();
            zetta::runtime::spawn(run_record_anti_entropy(record_store_.get()),
                                  [round = record_anti_entropy_] {
                                      round->finished.store(true, std::memory_order_release);
                                      round->done.set();
                                  });
        }
        if (record_anti_entropy_ &&
            record_anti_entropy_->finished.load(std::memory_order_acquire)) {
            record_anti_entropy_.reset();
        }
    }
    // The object logs (#1005): a maintenance step every 100 ms (tombstone grace,
    // idle uploads, flushing rolled segments, a bounded compaction step) and
    // anti-entropy every five seconds, on the pool, one round at a time.
    if (object_store_) {
        if (object_upkeep_ && object_upkeep_->finished.load(std::memory_order_acquire)) {
            object_upkeep_.reset();
        }
        constexpr auto kObjectMaintenanceInterval = std::chrono::milliseconds(100);
        constexpr auto kObjectAntiEntropyInterval = std::chrono::seconds(5);
        if (!object_upkeep_ && Clock::now() - last_object_maintenance_ >= kObjectMaintenanceInterval) {
            last_object_maintenance_ = Clock::now();
            const bool anti_entropy =
                cluster_ && Clock::now() - last_object_anti_entropy_ >= kObjectAntiEntropyInterval;
            if (anti_entropy) last_object_anti_entropy_ = Clock::now();
            object_upkeep_ = std::make_shared<ObjectUpkeepRound>();
            zetta::runtime::spawn(run_object_upkeep(object_store_.get(), anti_entropy),
                                  [round = object_upkeep_] {
                                      round->finished.store(true, std::memory_order_release);
                                      round->done.set();
                                  });
        }
    }
    flush_due_audit_summaries();
    sync_audit_log();
    constexpr auto kAuditRetentionCheckInterval = std::chrono::hours(1);
    if (last_audit_retention_.time_since_epoch().count() == 0 ||
        Clock::now() - last_audit_retention_ >=
            kAuditRetentionCheckInterval) {
        apply_audit_retention();
    }
    // 5. Coordinated co-launch (#601): execute any `run.launch` requests peers
    //    fanned out, HERE on the owner thread — the launched SPMD body blocks on
    //    collective rounds, which must never run on the network thread that
    //    delivered the signal (it services those very rounds). This is the
    //    same "synchronous on the thread that owns the Node" pattern every
    //    interactive submitter uses; the tick resumes when the co-run finishes.
    if (launch_listener_) launch_listener_->drain();
    report_memory(false);
}

void Node::flush_pending_captures(bool force) {
    std::erase_if(pending_run_captures_, [&](const std::string& run_id) {
        const std::shared_ptr<zetta::pipeline::Run> run = find_run(run_id);
        return !run || run->flush_capture(force);
    });
    for (const std::shared_ptr<zetta::ml::Study>& study : studies_) {
        if (study) study->flush_capture(force);
    }
}

void Node::run_on_owner(const std::function<void()>& work) {
    if (on_owner_thread()) {
        work();
        return;
    }
    // A node NO loop has ever ticked has no owner to drain the queue — run
    // inline, the pre-gate contract: a harness driving nodes from its own
    // threads (the testkit's per-node SPMD threads, the acceptance tests)
    // arranges exclusive access itself, and queueing here would deadlock
    // against an owner that will never exist. Both apps tick from their first
    // loop iteration, so a real owner is always adopted before any background
    // action can launch.
    if (owner_thread_.load(std::memory_order_acquire) == std::thread::id{}) {
        work();
        return;
    }
    auto completed = std::make_shared<zetta::runtime::async_event>();
    // The work runs on another thread, so it carries the caller's acting
    // identity with it (#847): an audit event the work records is attributed
    // to whoever handed it over, not to whatever the owner thread was doing.
    std::function<void()> attributed =
        [work, actor = zetta::governance::current_audit_actor()] {
            const zetta::governance::AuditActorScope scope{actor};
            work();
        };
    {
        std::lock_guard<std::mutex> lock(owner_work_mutex_);
        owner_work_queue_.push_back(
            OwnerWork{std::move(attributed), [completed] { completed->set(); }});
        if (owner_wakeup_) owner_wakeup_();
    }
    // Block this worker until the owner loop has executed the work. The wait
    // goes through the runtime's async_event, awaited to completion — the same
    // completion signaling as the API MainThreadBridge, entered synchronously
    // because a background-action worker is not a coroutine. The wait's
    // io_result is bound rather than discarded (the runtime::offload() idiom).
    zetta::runtime::block_on(
        [](std::shared_ptr<zetta::runtime::async_event> event)
            -> zetta::runtime::task<> {
            [[maybe_unused]] auto ready = co_await event->wait();
        }(completed));
}

void Node::set_owner_wakeup(std::function<void()> wakeup) {
    std::lock_guard<std::mutex> lock(owner_work_mutex_);
    owner_wakeup_ = std::move(wakeup);
}

bool Node::on_owner_thread() const {
    return owner_thread_.load(std::memory_order_acquire) ==
           std::this_thread::get_id();
}

void Node::drain_owner_work() {
    std::vector<OwnerWork> batch;
    {
        std::lock_guard<std::mutex> lock(owner_work_mutex_);
        if (owner_work_queue_.empty()) return;
        batch.assign(std::make_move_iterator(owner_work_queue_.begin()),
                     std::make_move_iterator(owner_work_queue_.end()));
        owner_work_queue_.clear();
    }
    for (OwnerWork& pending : batch) {
        pending.work();
        pending.signal_completed();
    }
}

void Node::stop() {
    if (!running_) return;
    // A record anti-entropy round runs on the pool against the record store and
    // the cluster; let it finish before either goes away (#1231).
    if (record_anti_entropy_) {
        zetta::runtime::block_on(
            [](std::shared_ptr<RecordAntiEntropyRound> round) -> zetta::runtime::task<> {
                [[maybe_unused]] auto finished = co_await round->done.wait();
            }(record_anti_entropy_));
        record_anti_entropy_.reset();
    }
    // The storage census round walks the data directory (zetta#1259).
    if (storage_census_round_) {
        zetta::runtime::block_on(
            [](std::shared_ptr<StorageCensusRound> round) -> zetta::runtime::task<> {
                [[maybe_unused]] auto finished = co_await round->done.wait();
            }(storage_census_round_));
        storage_census_round_.reset();
    }
    // The object store's upkeep round likewise (#1005).
    if (object_upkeep_) {
        zetta::runtime::block_on(
            [](std::shared_ptr<ObjectUpkeepRound> round) -> zetta::runtime::task<> {
                [[maybe_unused]] auto finished = co_await round->done.wait();
            }(object_upkeep_));
        object_upkeep_.reset();
    }
    if (study_driver_) {
        study_driver_->stop();
        study_driver_.reset();
    }
    // Tear the coordinator down first, while the attached cluster is still alive
    // (its signal-handler unwind is a cheap atomic store, and the steal loop joins
    // promptly) — so the cluster can be stopped/destroyed after us safely (#579).
    stop_coordinator();
    // Pub/sub flushes its default topics within its budget (#1031, D20), before
    // the replicated document is persisted.
    if (pubsub_) {
        pubsub_->shutdown();
        pubsub_.reset();
    }
    flush_audit_summaries();
    if (audit_trail_) {
        audit_trail_->flush();
    }
    flush_pending_captures(true);
    if (replication_ && identity_held_dirty_) persist_identity_held();
    if (replication_) persist_replication();
    running_ = false;
    std::printf("zetta: node stopped\n");
    std::fflush(stdout);
}

void Node::request_stop_study_work(const std::string& study_id,
                                   const std::string& run_id) {
    if (study_driver_) study_driver_->request_stop(study_id, run_id);
}

void Node::attach_cluster(zetta::cluster::Cluster& cluster, unsigned worker_count) {
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
    if (!telemetry_) telemetry_ = std::make_unique<zetta::cluster::NodeTelemetryExchange>();
    // The coordinated cross-process launch listener (#601) sits between the
    // compute plane and the telemetry exchange in the signal-handler chain:
    // the coordinator forwards every non-steal / non-collective signal to it,
    // it consumes `run.launch` tasks (validate + queue — tick() drains and
    // executes on this owner thread), and everything else flows on to the
    // telemetry fallback. The built-in launch kinds are registered here, on
    // EVERY clustered node — that is what lets any member co-run a peer's
    // interactive SPMD run.
    launch_listener_ = std::make_unique<compute::CoordinatedLaunchListener>(
        *cluster_, telemetry_.get());
    launch_listener_->set_span_recorder(&span_recorder_);
    zetta::usecases::churn::install_churn_launch_runner(*launch_listener_, *this);
    zetta::ml::install_entity_resolution_launch_runner(*launch_listener_, *this,
                                                       *cluster_);
    zetta::ml::install_segmentation_launch_runner(*launch_listener_, *this);
    zetta::ml::install_forecast_launch_runner(*launch_listener_, *this);
    zetta::ml::install_anomaly_launch_runner(*launch_listener_, *this);
    zetta::ml::install_next_best_action_launch_runner(*launch_listener_, *this);
    zetta::ml::install_synthetic_private_launch_runner(*launch_listener_, *this);
    zetta::connect::install_connector_fetch_launch_runner(*launch_listener_,
                                                          *this);
    coordinator_ = std::make_unique<compute::JobCoordinator>(*cluster_, workers,
                                                             launch_listener_.get());
    // Every job this coordinator runs (and every stolen task it executes for a
    // peer) records spans into this node's ring (#640).
    coordinator_->set_span_recorder(&span_recorder_);
    // Job entity writes route through the owner-thread gate (#624), so a run
    // driven from a background-action worker mutates node-owned records safely.
    coordinator_->set_owner_gate(
        [this](const std::function<void()>& work) { run_on_owner(work); });
    // Register on the cluster's signal plane + start the cross-node steal loop, so
    // a job submitted through job_coordinator() runs SPMD across live membership.
    coordinator_->start();

    // The distributed search plane (#641) chains over the compute plane's
    // handler: `fulltext.query` is served here; every other signal routes on to
    // the scheduler (steals / collectives) and its telemetry fallback. Wired on
    // EVERY clustered node — that is what lets any node answer a coordinator's
    // scatter — with the sub-indexes built by reconcile_search_indexes() (tick).
    std::shared_ptr<zetta::index::IndexBackend> search_backend = search_backend_;
    if (!search_backend) {
#ifdef ZETTA_WITH_ORAMACORE
        search_backend = std::make_shared<zetta::index::OramaCoreFulltextIndex>();
#else
        // Degraded default without the private BM25F engine: exact-match only,
        // no ranking signal (docs/indexing/node-search.md); inject a ranked
        // backend with set_search_backend to override.
        search_backend = std::make_shared<zetta::index::InMemoryIndexBackend>();
#endif
    }
    distributed_fulltext_ = std::make_unique<zetta::index::DistributedFulltextIndex>(
        *cluster_, std::move(search_backend), cluster_->signal_handler());
    search_build_fingerprints_.clear();  // a fresh plane holds no sub-indexes yet

    // The scatter-gather SELECT plane (#833) chains over the search plane:
    // `sql.scan` is served here, everything else routes on. Wired on EVERY
    // clustered node — that is what lets any node answer for the partitions it
    // owns — with its snapshot of those partitions refreshed by tick().
    distributed_select_ = std::make_unique<zetta::query::DistributedSelect>(
        *this, *cluster_, cluster_->signal_handler());
    distributed_select_->refresh();

    // The record store (#1231) chains over the SELECT plane: `record.put` and
    // `record.apply` are served here, everything else routes on.
    if (record_store_) record_store_->attach(*cluster_);
    // The object store (#1005) chains over the record store: `object.*` is served
    // here, everything else routes on.
    if (object_store_) object_store_->attach(*cluster_);
}

void Node::stop_coordinator() {
    // Unwind in reverse wiring order: the object store restores the record
    // store's handler, the record store the SELECT plane's, the SELECT plane the
    // search plane's, the search plane the compute plane's, then the coordinator
    // restores its own fallback.
    if (object_store_) object_store_->detach();
    if (record_store_) record_store_->detach();
    distributed_select_.reset();
    distributed_fulltext_.reset();
    search_build_fingerprints_.clear();
    if (!coordinator_) return;
    coordinator_->stop();   // join the steal loop, restore the cluster's handler
    coordinator_.reset();
    // After the coordinator (which held it as the handler fallback): drop the
    // launch listener — pending, undrained launches are ephemeral signal-plane
    // messages and die with the attachment (the initiator's collective rounds
    // surface the absence with their bounded timeout).
    // The coordinator's stop restored the cluster's handler to its fallback,
    // which is this listener; hand the chain on to the listener's own fallback
    // (the telemetry exchange) before destroying it. set_signal_handler waits
    // out any dispatch still inside the listener, so the reset below cannot
    // race the network thread (#1131: a heap use-after-free under repeat).
    if (cluster_ && cluster_->signal_handler() == launch_listener_.get()) {
        cluster_->set_signal_handler(telemetry_.get());
    }
    launch_listener_.reset();
}

zetta::index::SearchResult Node::search(
    const zetta::index::SearchRequest& request,
    zetta::observability::SpanRecorder* recorder,
    const zetta::observability::TraceContext* parent) {
    return zetta::index::search(*this, request, recorder, parent);
}

void Node::reconcile_search_indexes() {
    if (!distributed_fulltext_) return;
    const std::string& self = node_id();

    // The desired local builds: one per distinct Search-kind spec key, each with
    // a fingerprint of THIS node's build inputs — the backend, source database
    // and placed partitions this node owns. A build is (re)run only when this
    // fingerprint changes, so the steady-state tick cost is string compares.
    std::map<std::string, std::pair<zetta::index::IndexSpec, std::string>> desired;
    for (const std::shared_ptr<zetta::index::Index>& index : indexes_) {
        if (!index || index->kind() != zetta::index::IndexKind::Search) continue;
        // The index's own source (#1428): its recorded database, or the one
        // database holding the name for an older record. An index whose source
        // does not resolve (missing, or ambiguous) builds nothing; search
        // reports why.
        const zetta::index::IndexSource source =
            zetta::index::resolve_index_source(*this, index->spec());
        if (!source.ok()) continue;
        zetta::index::IndexSpec spec = index->spec();
        const std::string key = spec.key();
        if (desired.contains(key)) continue;

        // Partitions record the table's ID; the spec names it by NAME.
        const std::string table_id = source.table->id();
        const std::string& source_database = source.database;
        std::string fingerprint = spec.backend + '\n' + source_database + '\n' +
                                  table_id + '\n';
        for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
             dataset_partitions()) {
            if (!partition || partition->table() != table_id) continue;
            if (partition->blob_ref().empty()) continue;  // unplaced
            const std::vector<std::string>& owners = partition->owner_nodes();
            if (std::find(owners.begin(), owners.end(), self) == owners.end()) {
                continue;
            }
            fingerprint += partition->id() + '=' + partition->blob_ref() + '\n';
        }
        desired.emplace(key, std::pair{std::move(spec), std::move(fingerprint)});
    }

    for (auto& [key, spec_and_fingerprint] : desired) {
        auto& [spec, fingerprint] = spec_and_fingerprint;
        const auto last = search_build_fingerprints_.find(key);
        if (last != search_build_fingerprints_.end() &&
            last->second == fingerprint) {
            continue;  // already built over exactly these inputs
        }
        const zetta::index::DistributedFulltextIndex::BuildResult build =
            distributed_fulltext_->build_local(*this, spec);
        if (!build.ok()) {
            std::printf("zetta: search sub-index build over '%s' failed: %s\n",
                        spec.table.c_str(), build.status.message.c_str());
        }
        // Record the fingerprint either way: the build is deterministic in its
        // inputs, so a failure would otherwise re-fail on every tick. A future
        // partition / index change moves the fingerprint and retries.
        search_build_fingerprints_[key] = std::move(fingerprint);
    }

    // Forget builds whose index entity is gone (a re-created index rebuilds).
    std::erase_if(search_build_fingerprints_, [&desired](const auto& entry) {
        return !desired.contains(entry.first);
    });
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
        auto db = std::make_shared<zetta::data::Database>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(), entry.path(), replication_,
            zetta::data::replication_strategy_from_meta(meta));
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
        auto project = std::make_shared<zetta::pipeline::Project>(
            entry.path().filename().string(),
            meta["name"].get<std::string>(),
            meta.value("created", std::string{}),
            meta.value("updated", std::string{}),
            meta.value("owner", std::string{}), entry.path(), replication_,
            this, meta.value("description", std::string{}),
            [this](const std::string& project_id,
                   const std::string& pipeline_id,
                   const std::string& old_name,
                   const std::string& new_name) {
                update_pipeline_display_name(project_id, pipeline_id,
                                             old_name, new_name);
            });
        if (const auto internal = meta.find("internal");
            internal != meta.end() && internal->is_boolean() && internal->get<bool>()) {
            project->set_internal(true);
        }
        project->load();  // bring in this project's inputs + graphs from disk
        projects_.push_back(std::move(project));
    }
    // Stable order (directory iteration order is unspecified).
    std::sort(projects_.begin(), projects_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<zetta::pipeline::Project> Node::find_project_by_id(const std::string& id) const {
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
    const std::string id = zetta::util::new_uuid();
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
        const std::optional<zetta::pipeline::DeploymentTarget> target =
            zetta::pipeline::DeploymentTarget::from_json(
                meta.value("target", nlohmann::json::object()));
        if (!target) continue;
        deployments_.push_back(std::make_shared<zetta::pipeline::Deployment>(
            entry.path().filename().string(),
            meta["graph_name"].get<std::string>(), *target,
            meta.value("node", std::string{}),
            meta.value("node_count", std::size_t{0}),
            meta.value("edge_count", std::size_t{0}),
            meta.value("created", std::string{}), entry.path(),
            zetta::pipeline::table_reads_from_json(
                meta.value("reads", nlohmann::json::array()))));
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
        const auto kind = zetta::governance::principal_kind_from(meta["kind"].get<std::string>());
        if (!kind) continue;
        auto principal = std::make_shared<zetta::governance::Principal>(
            entry.path().filename().string(), *kind,
            meta["name"].get<std::string>(),
            meta.value("created", std::string{}), entry.path(), replication_,
            identity_authority());
        principal->load();  // bring in this principal's keys from disk
        principals_.push_back(std::move(principal));
    }
    // Stable order (directory iteration order is unspecified).
    std::sort(principals_.begin(), principals_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<zetta::governance::Principal> Node::find_principal_by_id(
    const std::string& id) const {
    for (const auto& principal : principals_) {
        if (principal->id() == id) return principal;
    }
    return nullptr;
}

namespace {

// A change to the catalogue made through the Node's own API (#847): a project,
// database or model created, dropped or renamed, or a graph run. These are the
// calls the headed app makes directly (not through a capability), so the Node
// records them itself; the actor is whoever the entry point declared
// (AuditActorScope), filled in by record_audit_event. A peer's change arrives
// through reconcile (materialize_*) and is not re-recorded here.
void record_catalog_change(Node& node, std::string target, bool ok,
                           std::string detail) {
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DataChanged;
    event.target = std::move(target);
    event.allowed = ok;
    event.detail = std::move(detail);
    node.record_audit_event(std::move(event));
}

}  // namespace

std::shared_ptr<zetta::data::Database> Node::create_database(
    std::string name, zetta::data::ReplicationStrategy replication_strategy) {
    // "system" is reserved for the virtual system database (computed system
    // tables like system.nodes); never a real on-disk database.
    if (name.empty() || name == "system" || find_database(name)) return nullptr;
    const std::string id = zetta::util::new_uuid();
    if (replication_) {
        // The default (ReplicateToAll) is omitted, so the db: value is unchanged
        // from before databases carried a strategy (ENT-35).
        nlohmann::json value{{"name", name}};
        if (!replication_strategy.is_default()) {
            value["replication"] = replication_strategy.to_json();
        }
        replication_->set(catalog::database_key(id), value);
    }
    const std::string target = "database:" + name;
    auto database = materialize_database(id, std::move(name),
                                         std::move(replication_strategy));
    if (database) record_catalog_change(*this, target, true, "database created");
    return database;
}

std::shared_ptr<zetta::data::Database> Node::materialize_database(
    std::string id, std::string name, zetta::data::ReplicationStrategy strategy) {
    if (auto existing = find_database_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto db = std::make_shared<zetta::data::Database>(std::move(id), std::move(name), dir,
                                         replication_, std::move(strategy));
    db->persist_meta();
    databases_.push_back(db);
    return db;
}

std::shared_ptr<zetta::data::Table> Node::adopt_replicated_table(
    const std::string& database_id, const std::string& database_name,
    const std::string& table_id, const std::string& table_name,
    bool partition_only, const std::string& source_format) {
    // The keys are **captured**, not merely materialized — under the initiator's
    // ids, with the value `create_database`/`create_table` would have written.
    //
    // Materializing alone deletes itself: a launch is a signal and arrives at
    // once, while the `db:`/`tbl:` keys merge on the next gossip round up to
    // half a second later, and `reconcile_databases` prunes a local database or
    // table the document does not have — taking the directory, the rows and the
    // partition bodies underneath it with it. Writing the identical value under
    // the identical key is idempotent: when the initiator's copy merges there is
    // nothing to reconcile. Only written when this node's document does not
    // already carry the key, so an existing entry (with, say, a non-default
    // replication strategy) is never overwritten with a narrower one.
    if (replication_) {
        // A tombstoned key means the table (or its database) was dropped. A
        // causally-later `set` re-creates a removed key, so adopting it here
        // would bring a dropped table back on this node alone (#844).
        if (replicated_table_dropped(database_id, table_id)) return nullptr;
        const nlohmann::json snapshot = replication_->snapshot();
        const std::string database_key = catalog::database_key(database_id);
        if (!snapshot.contains(database_key)) {
            replication_->set(database_key, nlohmann::json{{"name", database_name}});
        }
        const std::string table_key = catalog::table_key(table_id);
        if (!snapshot.contains(table_key)) {
            nlohmann::json value{{"db", database_id}, {"name", table_name}};
            if (partition_only) value["partition_only"] = true;
            if (!source_format.empty()) value["source_format"] = source_format;
            replication_->set(table_key, value);
        }
    }
    std::shared_ptr<zetta::data::Database> database =
        find_database_by_id(database_id);
    if (!database) database = materialize_database(database_id, database_name);
    if (!database) return nullptr;
    if (auto existing = database->find_table_by_id(table_id)) return existing;
    return database->materialize_table(table_id, table_name,
                                       database->replication_strategy(),
                                       partition_only, source_format);
}

bool Node::replicated_table_dropped(const std::string& database_id,
                                    const std::string& table_id) const {
    if (!replication_) return false;
    // `knows_key` FIRST, then a fresh snapshot: a key that merges in between
    // then reads as live rather than as dropped. The other order would mistake
    // a table whose `tbl:` key is just arriving for one that was removed.
    const auto removed = [&](const std::string& key) {
        if (!replication_->knows_key(key)) return false;
        const std::shared_ptr<const nlohmann::json> snapshot =
            replication_->shared_snapshot();
        return !(snapshot && snapshot->contains(key));
    };
    return removed(catalog::table_key(table_id)) ||
           removed(catalog::database_key(database_id));
}

std::shared_ptr<zetta::data::Database> Node::find_database(const std::string& name) const {
    for (const auto& db : databases_) {
        if (db->name() == name) return db;
    }
    return nullptr;
}

std::shared_ptr<zetta::data::Database> Node::find_database_by_id(
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
    const bool dropped = materialize_drop_database(db->id());
    if (dropped) record_catalog_change(*this, "database:" + name, true, "database dropped");
    // Its safe mode level goes with it (#1373): a database created later under
    // the same name starts at the default. The row delete replicates like any
    // other, so peers forget the level too.
    if (dropped && name != zetta::governance::kSafeModeDatabase) {
        zetta::governance::clear_safe_mode(
            *this, zetta::governance::SafeModeTarget::Database, name);
    }
    return dropped;
}

bool Node::materialize_drop_database(const std::string& id) {
    const auto it = std::find_if(
        databases_.begin(), databases_.end(),
        [&](const std::shared_ptr<zetta::data::Database>& d) { return d->id() == id; });
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
    record_catalog_change(*this, "database:" + to, true,
                          "database renamed from '" + from + "'");
    return true;
}

std::vector<std::string> Node::database_names() const {
    std::vector<std::string> names;
    names.reserve(databases_.size());
    for (const auto& db : databases_) names.push_back(db->name());
    return names;
}

// ----- projects (mirror the database CRUD; replicated as project:<uuid>) -----

std::shared_ptr<zetta::pipeline::Project> Node::create_project(std::string name,
                                              std::string owner,
                                              std::string description,
                                              bool internal) {
    if (name.empty() || find_project(name)) return nullptr;
    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    if (replication_) {
        nlohmann::json value{{"name", name},
                             {"created", now},
                             {"updated", now},
                             {"owner", owner},
                             {"description", description}};
        if (internal) value["internal"] = true;
        replication_->set(catalog::project_key(id), std::move(value));
    }
    const std::string target = "project:" + name;
    auto project = materialize_project(id, std::move(name), now, now, std::move(owner),
                                       std::move(description), internal);
    if (project) {
        record_catalog_change(*this, target, true,
                              internal ? "internal project created" : "project created");
    }
    return project;
}

bool Node::reveal_project(const std::string& id, const std::string& name) {
    auto project = find_project_by_id(id);
    if (!project || !project->internal() || name.empty()) return false;
    const std::string from = project->name();
    if (name != from && find_project(name)) return false;
    project->set_metadata(name, project->created(), zetta::util::now_iso8601(),
                          project->owner(), project->description());
    project->set_internal(false);
    if (replication_) {
        replication_->set(catalog::project_key(project->id()), project->metadata());
    }
    if (name != from) update_project_display_name(project->id(), from, name);
    record_catalog_change(*this, "project:" + name, true,
                          "internal project '" + from + "' revealed");
    return true;
}

std::shared_ptr<zetta::pipeline::Project> Node::materialize_project(std::string id,
                                                   std::string name,
                                                   std::string created,
                                                   std::string updated,
                                                   std::string owner,
                                                   std::string description,
                                                   bool internal) {
    if (auto existing = find_project_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto project = std::make_shared<zetta::pipeline::Project>(std::move(id), std::move(name),
                                             std::move(created),
                                             std::move(updated),
                                             std::move(owner), dir,
                                             replication_, this,
                                             std::move(description),
                                             [this](const std::string& project_id,
                                                    const std::string& pipeline_id,
                                                    const std::string& old_name,
                                                    const std::string& new_name) {
                                                 update_pipeline_display_name(
                                                     project_id, pipeline_id,
                                                     old_name, new_name);
                                             });
    if (internal) project->set_internal(true);  // persists project.json
    else project->persist_meta();
    projects_.push_back(project);
    return project;
}

std::shared_ptr<zetta::pipeline::Project> Node::find_project(const std::string& name) const {
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
        // ...and its inputs (#1009). They were left behind as `input:` keys naming
        // a project that no longer exists — the integrity check counts exactly
        // these as dangling_inputs.
        for (const auto& input : project->inputs()) {
            replication_->remove(catalog::input_key(input->id()));
        }
        replication_->remove(catalog::project_key(project->id()));
    }
    const bool dropped = materialize_drop_project(project->id());
    if (dropped) record_catalog_change(*this, "project:" + name, true, "project dropped");
    return dropped;
}

bool Node::materialize_drop_project(const std::string& id) {
    const auto it = std::find_if(
        projects_.begin(), projects_.end(),
        [&](const std::shared_ptr<zetta::pipeline::Project>& p) { return p->id() == id; });
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
    project->set_metadata(to, project->created(), zetta::util::now_iso8601(),
                          project->owner(), project->description());
    if (replication_) {
        replication_->set(catalog::project_key(project->id()),
                          project->metadata());
    }
    update_project_display_name(project->id(), from, to);
    record_catalog_change(*this, "project:" + to, true,
                          "project renamed from '" + from + "'");
    return true;
}

std::vector<std::string> Node::project_names() const {
    std::vector<std::string> names;
    names.reserve(projects_.size());
    for (const auto& project : projects_) {
        if (!project->internal()) names.push_back(project->name());
    }
    return names;
}

// ----- deployments (place a graph onto a target; replicated as deployment:<id>)

std::shared_ptr<zetta::pipeline::Deployment> Node::deploy_graph(const ir::ExecutionGraph& graph,
                                               zetta::pipeline::DeploymentTarget target,
                                               std::string graph_name) {
    // R1 places onto the local node only; k8s/cloud targets are deferred.
    if (!target.is_local()) return nullptr;
    // A deployable graph must have something to place.
    if (graph.empty()) return nullptr;

    // RULE-1: no deploy without a passing correctness gate. The gate runs at the
    // deploy boundary itself — so a *programmatic* deploy clears the same gate the GUI
    // Deploy panel runs, and an ungated/failing graph can never be placed. A gate
    // failure is a governed decision: record a blocked (allowed=false) deploy event
    // and place nothing.
    const zetta::pipeline::ValidationReport report = zetta::pipeline::default_gate().run(graph);
    if (!report.passed()) {
        std::string reason = "correctness gate failed";
        for (const zetta::pipeline::CheckResult& check : report.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) {
                reason = "gate check '" + check.name + "' failed: " + check.reason;
                break;
            }
        }
        record_audit_event(
            {zetta::governance::AuditAction::GraphDeployed, {}, graph_name, false, reason, {}});
        return nullptr;
    }

    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    const std::size_t node_count = graph.node_count();
    const std::size_t edge_count = graph.edge_count();
    // The tables the graph reads (zetta#1272): every run the deployment
    // controller starts for it records them, so delete_dataset refuses them.
    std::vector<zetta::pipeline::TableRead> reads = tables_read(graph);
    if (replication_) {
        nlohmann::json value{{"graph_name", graph_name},
                             {"target", target.to_json()},
                             {"node", node_id_},
                             {"node_count", node_count},
                             {"edge_count", edge_count},
                             {"created", now}};
        if (!reads.empty()) value["reads"] = zetta::pipeline::to_json(reads);
        replication_->set(catalog::deployment_key(id), std::move(value));
    }
    auto deployment = materialize_deployment(id, graph_name, std::move(target),
                                             node_id_, node_count, edge_count,
                                             now, std::move(reads));
    // Audit the deploy (FEAT-9): placing a graph onto a target is a governed
    // action. Record it once the placement is materialized.
    if (deployment) {
        record_audit_event(
            {zetta::governance::AuditAction::GraphDeployed, {}, graph_name, true,
             "graph '" + graph_name + "' deployed (" +
                 std::to_string(node_count) + " nodes, " +
                 std::to_string(edge_count) + " edges) on node " + node_id_,
             {}});
    }
    return deployment;
}

std::shared_ptr<zetta::pipeline::Deployment> Node::materialize_deployment(
    std::string id, std::string graph_name, zetta::pipeline::DeploymentTarget target,
    std::string node_id, std::size_t node_count, std::size_t edge_count,
    std::string created, std::vector<zetta::pipeline::TableRead> reads) {
    if (auto existing = find_deployment(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto deployment = std::make_shared<zetta::pipeline::Deployment>(
        std::move(id), std::move(graph_name), std::move(target),
        std::move(node_id), node_count, edge_count, std::move(created), dir,
        std::move(reads));
    deployment->persist_meta();
    deployments_.push_back(deployment);
    return deployment;
}

std::shared_ptr<zetta::pipeline::Deployment> Node::find_deployment(const std::string& id) const {
    for (const auto& deployment : deployments_) {
        if (deployment->id() == id) return deployment;
    }
    return nullptr;
}

// ----- principals (mirror the project CRUD; replicated as principal:<uuid>) ---

std::shared_ptr<zetta::governance::Principal> Node::create_principal(std::string name,
                                                  zetta::governance::PrincipalKind kind) {
    if (!identity_writable()) return nullptr;
    if (name.empty() || find_principal(name)) return nullptr;
    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    if (replication_) {
        replicate_identity(catalog::principal_key(id),
                          nlohmann::json{{"kind", to_string(kind)},
                                         {"name", name},
                                         {"created", now}});
    }
    return materialize_principal(id, kind, std::move(name), now);
}

std::shared_ptr<zetta::governance::Principal> Node::materialize_principal(std::string id,
                                                       zetta::governance::PrincipalKind kind,
                                                       std::string name,
                                                       std::string created) {
    if (auto existing = find_principal_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto principal = std::make_shared<zetta::governance::Principal>(
        std::move(id), kind, std::move(name), std::move(created), dir,
        replication_, identity_authority());
    principal->persist_meta();
    principals_.push_back(principal);
    return principal;
}

std::shared_ptr<zetta::governance::Principal> Node::find_principal(const std::string& name) const {
    for (const auto& principal : principals_) {
        if (principal->name() == name) return principal;
    }
    return nullptr;
}

bool Node::materialize_drop_deployment(const std::string& id) {
    const auto it = std::find_if(
        deployments_.begin(), deployments_.end(),
        [&](const std::shared_ptr<zetta::pipeline::Deployment>& d) { return d->id() == id; });
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
        const std::optional<zetta::pipeline::RunValue> value =
            meta.is_discarded()
                ? std::nullopt
                : zetta::pipeline::parse_run_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid run %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        runs_.push_back(std::make_shared<zetta::pipeline::Run>(entry.path().filename().string(),
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

std::vector<zetta::pipeline::TableRead> Node::tables_read(
    const std::string& project, const std::string& graph,
    const std::string& dataset_path) const {
    std::vector<zetta::pipeline::TableRead> reads;
    const auto add = [&](const std::string& database, const std::string& table) {
        if (database.empty() || table.empty()) return;
        const std::shared_ptr<zetta::data::Database> owner = find_database(database);
        const std::shared_ptr<zetta::data::Table> found =
            owner ? owner->find_table(table) : nullptr;
        if (!found) return;
        zetta::pipeline::merge_table_reads(reads, {{database, table, found->id()}});
    };
    if (!dataset_path.empty()) {
        const zetta::data::DatasetPath path = zetta::data::parse_dataset_path(dataset_path);
        if (path.kind == zetta::data::DatasetPathKind::Table) add(path.database, path.table);
    }
    if (project.empty() || graph.empty()) return reads;
    std::shared_ptr<zetta::pipeline::Project> stored = find_project_by_id(project);
    if (!stored) stored = find_project(project);
    if (!stored) return reads;
    std::shared_ptr<zetta::pipeline::ProjectGraph> stored_graph = stored->find_graph_by_id(graph);
    if (!stored_graph) stored_graph = stored->find_graph(graph);
    if (!stored_graph) return reads;
    zetta::pipeline::merge_table_reads(reads, tables_read(stored_graph->graph()));
    return reads;
}

std::vector<zetta::pipeline::TableRead> Node::tables_read(
    const zetta::ir::ExecutionGraph& graph) const {
    std::vector<zetta::pipeline::TableRead> reads;
    for (const auto& step : graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!step || !step->attributes.is_object()) continue;
        const std::string database = step->attributes.value("source", std::string{});
        const std::string table = step->attributes.value("table", std::string{});
        if (database.empty() || table.empty()) continue;
        const std::shared_ptr<zetta::data::Database> owner = find_database(database);
        const std::shared_ptr<zetta::data::Table> found =
            owner ? owner->find_table(table) : nullptr;
        if (!found) continue;
        zetta::pipeline::merge_table_reads(reads, {{database, table, found->id()}});
    }
    return reads;
}

std::shared_ptr<zetta::pipeline::Run> Node::create_run(std::string project, std::string graph,
                                      std::string deployment) {
    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    zetta::pipeline::RunValue value;
    value.project = project;
    value.graph = graph;
    std::shared_ptr<zetta::pipeline::Project> stored_project =
        find_project_by_id(project);
    if (!stored_project) {
        stored_project = find_project(project);
    }
    if (stored_project) {
        value.project = stored_project->id();
        value.project_name = stored_project->name();
        std::shared_ptr<zetta::pipeline::ProjectGraph> stored_graph =
            stored_project->find_graph_by_id(graph);
        if (!stored_graph) {
            stored_graph = stored_project->find_graph(graph);
        }
        if (stored_graph) {
            value.graph = stored_graph->id();
            value.pipeline_name = stored_graph->name();
        }
    }
    value.deployment = std::move(deployment);
    // What it reads, recorded before it reads anything (zetta#1258).
    value.reads = tables_read(value.project, value.graph);
    value.status = zetta::pipeline::RunStatus::Pending;
    value.created = now;
    value.updated = now;
    if (replication_) replication_->set(catalog::run_key(id), to_json(value));
    return materialize_run(id, std::move(value));
}

std::shared_ptr<zetta::pipeline::Run> Node::materialize_run(std::string id, zetta::pipeline::RunValue value) {
    if (auto existing = find_run(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto run = std::make_shared<zetta::pipeline::Run>(std::move(id), std::move(value), dir,
                                     replication_);
    // The run stores a text entity-key mapping as a table reference (#1255).
    run->set_entity_key_store([this](std::string_view owner, nlohmann::json mapping) {
        return entity_key_reference(owner, std::move(mapping));
    });
    run->persist_meta();
    runs_.push_back(run);
    return run;
}

std::shared_ptr<zetta::pipeline::Run> Node::find_run(const std::string& id) const {
    for (const auto& run : runs_) {
        if (run->id() == id) return run;
    }
    return nullptr;
}

namespace {

std::map<std::string, double> run_system_sample(
    const zetta::cluster::NodeMetrics& host,
    const std::map<std::string, double>& values) {
    std::map<std::string, double> system_values;
    if (host.cpu_percent_available) {
        system_values["system.cpu.percent"] = host.cpu_percent;
    }
    if (host.memory_available) {
        system_values["system.memory.used_bytes"] =
            static_cast<double>(host.memory_used_bytes);
        system_values["system.memory.percent"] = host.memory_percent;
    }
    if (host.gpu_available) {
        system_values["system.gpu.percent"] = host.gpu_percent;
        system_values["system.gpu.vram_used_bytes"] =
            static_cast<double>(host.vram_used_bytes);
    }
    if (const auto tokens = values.find("train.tokens_per_second");
        tokens != values.end()) {
        system_values["system.throughput.per_second"] = tokens->second;
    } else if (const auto rows = values.find("train.rows_per_second");
               rows != values.end()) {
        system_values["system.throughput.per_second"] = rows->second;
    }
    std::erase_if(system_values, [&](const auto& metric) {
        return values.contains(metric.first);
    });
    return system_values;
}

}  // namespace

bool Node::log_run_metrics(const std::string& run_id, std::uint64_t step,
                           std::map<std::string, double> values,
                           std::string time, std::string* error,
                           bool coalesce) {
    const std::shared_ptr<zetta::pipeline::Run> run = find_run(run_id);
    if (!run) {
        if (error != nullptr) {
            *error = "/run: no run with id '" + run_id + "'";
        }
        return false;
    }
    std::map<std::string, double> system_values;
    if (run->value().metrics_suppressed.empty()) {
        system_values = run_system_sample(sample_node_metrics(), values);
    }
    if (time.empty()) {
        time = zetta::util::now_iso8601();
    }
    if (!run->log_metrics_with_system_sample(step, values, system_values,
                                            time, error, coalesce)) {
        return false;
    }
    if (run->capture_pending()) pending_run_captures_.insert(run_id);
    if (pubsub_ == nullptr) {
        return true;
    }

    const zetta::pubsub::Identity engine{
        zetta::pubsub::Address{"service", "run-metrics"}, {}};
    const zetta::pubsub::Address topic{"topic", "run-metrics." + run_id};
    if (!pubsub_->topic_settings(topic)) {
        zetta::pubsub::TopicSettings settings;
        settings.exposed = true;
        settings.lifecycle = zetta::pubsub::TopicLifecycle::Ephemeral;
        std::string reason;
        if (!pubsub_->create_topic(engine, topic, settings, &reason) &&
            !pubsub_->topic_settings(topic)) {
            if (error != nullptr) {
                *error = "/run: metric batch was stored but its live topic could not be created: " +
                         reason;
            }
            return false;
        }
    }
    const std::optional<zetta::pubsub::TopicSettings> settings =
        pubsub_->topic_settings(topic);
    if (!settings || !settings->exposed) {
        if (error != nullptr) {
            *error = "/run: metric batch was stored but topic " +
                     topic.to_string() + " is not exposed";
        }
        return false;
    }
    std::map<std::uint64_t, std::map<std::string, double>> batches;
    if (!values.empty()) {
        batches[step] = std::move(values);
    }
    for (const auto& [name, value] : system_values) {
        const auto stored_step = run->value().metrics.at("series")
                                     .at(name).at("steps").back()
                                     .get<std::uint64_t>();
        batches[stored_step][name] = value;
    }
    for (const auto& [stored_step, batch] : batches) {
        zetta::pubsub::PublishRequest request;
        request.to = {topic};
        request.encoding = zetta::pubsub::Encoding::Json;
        request.mime_type = "application/json";
        request.correlation_id = run_id;
        request.payload = nlohmann::json{{"run", run_id},
                                        {"step", stored_step},
                                        {"values", batch},
                                        {"time", time}}
                              .dump();
        const zetta::pubsub::PublishResult published =
            pubsub_->publish(engine, std::move(request));
        if (!published.accepted) {
            if (error != nullptr) {
                *error = "/run: metric batch was stored but live publish failed: " +
                         published.error;
            }
            return false;
        }
    }
    return true;
}

bool Node::log_run_params(const std::string& run_id, nlohmann::json params,
                          std::string* error) {
    const std::shared_ptr<zetta::pipeline::Run> run = find_run(run_id);
    if (!run) {
        if (error != nullptr) {
            *error = "/run: no run with id '" + run_id + "'";
        }
        return false;
    }
    return run->log_params(std::move(params), error);
}

bool Node::log_run_artifact(const std::string& run_id,
                            nlohmann::json artifact, std::string* error) {
    const std::shared_ptr<zetta::pipeline::Run> run = find_run(run_id);
    if (!run) {
        if (error != nullptr) {
            *error = "/run: no run with id '" + run_id + "'";
        }
        return false;
    }
    return run->log_artifact(std::move(artifact), error);
}

// ----- studies (replicated AutoML searches; each trial is a Run) ------------

void Node::load_studies() {
    studies_.clear();
    std::error_code error_code;
    fs::create_directories(config_.data_dir, error_code);
    for (const auto& entry : fs::directory_iterator(config_.data_dir, error_code)) {
        if (!entry.is_directory()) continue;
        const fs::path path = entry.path() / "study.json";
        if (!fs::exists(path)) continue;
        std::ifstream input(path);
        const nlohmann::json document = nlohmann::json::parse(
            input, nullptr, /*allow_exceptions=*/false);
        std::string reason;
        const std::optional<zetta::ml::StudyValue> value =
            document.is_discarded()
                ? std::nullopt
                : zetta::ml::parse_study_value(document, &reason);
        if (!value) {
            std::printf("zetta: skipping invalid study %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        document.is_discarded() ? "unparseable JSON"
                                                : reason.c_str());
            continue;
        }
        studies_.push_back(std::make_shared<zetta::ml::Study>(
            entry.path().filename().string(), *value, entry.path(), replication_));
    }
    std::ranges::sort(studies_, [](const auto& left, const auto& right) {
        if (left->value().created != right->value().created) {
            return left->value().created < right->value().created;
        }
        return left->id() < right->id();
    });
}

std::shared_ptr<zetta::ml::Study> Node::create_study(
    zetta::ml::StudySpec spec, std::string* error) {
    std::shared_ptr<zetta::pipeline::Project> project =
        find_project_by_id(spec.project);
    if (!project) {
        project = find_project(spec.project);
    }
    if (project) {
        spec.project = project->name();
        std::shared_ptr<zetta::pipeline::ProjectGraph> pipeline =
            project->find_graph_by_id(spec.pipeline);
        if (!pipeline) {
            pipeline = project->find_graph(spec.pipeline);
        }
        if (pipeline) {
            spec.pipeline = pipeline->name();
        }
    }
    if (!zetta::ml::validate_study_spec(spec, error)) return nullptr;
    if (find_study(spec.name)) {
        if (error != nullptr) *error = "/name: a study with this name already exists";
        return nullptr;
    }
    zetta::ml::StudyValue value;
    // The tables its trials read (zetta#1258): its dataset, or its pipeline's.
    value.reads = tables_read(spec.project, spec.pipeline, spec.dataset);
    value.spec = std::move(spec);
    value.created = zetta::util::now_iso8601();
    value.updated = value.created;
    const std::string id = zetta::util::new_uuid();
    if (replication_ != nullptr) {
        replication_->set(catalog::study_key(id), zetta::ml::to_json(value));
    }
    return materialize_study(id, std::move(value));
}

std::shared_ptr<zetta::ml::Study> Node::materialize_study(
    std::string id, zetta::ml::StudyValue value) {
    if (const auto existing = find_study(id)) return existing;
    const fs::path directory = fs::path(config_.data_dir) / id;
    std::error_code error_code;
    fs::create_directories(directory, error_code);
    if (error_code) return nullptr;
    auto study = std::make_shared<zetta::ml::Study>(
        std::move(id), std::move(value), directory, replication_);
    study->persist_meta();
    studies_.push_back(study);
    return study;
}

std::shared_ptr<zetta::ml::Study> Node::find_study(
    const std::string& id_or_name) const {
    const auto found = std::ranges::find_if(
        studies_, [&](const std::shared_ptr<zetta::ml::Study>& study) {
            return study->id() == id_or_name ||
                   study->spec().name == id_or_name;
        });
    return found == studies_.end() ? nullptr : *found;
}

std::optional<zetta::ml::StudyDeleteResult> Node::delete_study(
    const std::string& id_or_name, std::string* error) {
    const std::shared_ptr<zetta::ml::Study> study = find_study(id_or_name);
    if (!study) {
        if (error != nullptr) {
            *error = "/study: no study named '" + id_or_name + "'";
        }
        return std::nullopt;
    }
    if (study->status() == zetta::ml::StudyStatus::Running) {
        if (error != nullptr) {
            *error = "/study: running studies must be stopped before deletion";
        }
        return std::nullopt;
    }

    zetta::ml::StudyDeleteResult result;
    result.id = study->id();
    result.name = study->spec().name;
    result.kept_model = study->value().promoted_model;
    if (!result.kept_model.empty()) {
        if (const std::shared_ptr<zetta::ml::Model> model =
                find_model(result.kept_model)) {
            result.kept_run = model->run_id();
        }
        if (result.kept_run.empty()) {
            const auto promoted_trial = std::ranges::find(
                study->value().trials, result.kept_model,
                &zetta::ml::StudyTrial::model);
            if (promoted_trial != study->value().trials.end()) {
                result.kept_run = promoted_trial->run;
            }
        }
    }

    std::vector<std::string> removable_runs;
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        if (trial.run != result.kept_run) {
            removable_runs.push_back(trial.run);
        }
    }
    for (const std::string& run_id : removable_runs) {
        if (drop_run(run_id)) {
            result.removed_runs.push_back(run_id);
        }
    }
    if (!drop_study(result.id)) {
        if (error != nullptr) {
            *error = "/study: could not delete study '" + result.name + "'";
        }
        return std::nullopt;
    }
    return result;
}

bool Node::drop_study(const std::string& id) {
    if (!find_study(id)) return false;
    if (replication_ != nullptr) replication_->remove(catalog::study_key(id));
    return materialize_drop_study(id);
}

bool Node::materialize_drop_study(const std::string& id) {
    const auto found = std::ranges::find(studies_, id,
                                         &zetta::ml::Study::id);
    if (found == studies_.end()) return false;
    std::error_code error_code;
    fs::remove_all((*found)->dir(), error_code);
    studies_.erase(found);
    return true;
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
        const std::optional<zetta::ml::ModelValue> value =
            meta.is_discarded() ? std::nullopt : zetta::ml::parse_model_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid model %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        models_.push_back(std::make_shared<zetta::ml::Model>(
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

std::shared_ptr<zetta::ml::Model> Node::create_model(zetta::ml::ModelValue value) {
    if (value.kind.empty()) return nullptr;  // basic guard (kind is required)
    if (value.run_id.empty()) {
        value.run_id = value.run;
    } else {
        value.run = value.run_id;
    }
    if (const std::shared_ptr<zetta::pipeline::Run> run =
            find_run(value.run_id)) {
        value.project_name = run->project_name();
        value.pipeline_name = run->pipeline_name();
        if (!run->project().empty()) {
            value.project = run->project();
        }
    }
    if (value.project_name.empty()) {
        std::shared_ptr<zetta::pipeline::Project> project =
            find_project_by_id(value.project);
        if (!project) {
            project = find_project(value.project);
        }
        if (project) {
            value.project = project->id();
            value.project_name = project->name();
        }
    }
    const std::string id = zetta::util::new_uuid();
    value.created = zetta::util::now_iso8601();
    value.weights.reset();  // weights are attached separately via set_weights
    // The record references its text entity-key mapping instead of carrying
    // every id (#1255); every model writer sets the mapping before this point.
    if (value.feature_schema.is_object()) {
        if (const auto mapping = value.feature_schema.find("entity_key_mapping");
            mapping != value.feature_schema.end() && mapping->is_object()) {
            *mapping = entity_key_reference(zetta::data::entity_key_owner("model", id),
                                            std::move(*mapping));
        }
    }
    if (replication_) replication_->set(catalog::model_key(id), to_json(value));
    const std::string detail = "model '" + value.kind + "' registered" +
        (value.project_name.empty() ? std::string{} : " in project '" + value.project_name + "'");
    auto model = materialize_model(id, std::move(value));
    if (model) record_catalog_change(*this, "model:" + model->id(), true, detail);
    return model;
}

std::shared_ptr<zetta::ml::Model> Node::materialize_model(std::string id, zetta::ml::ModelValue value) {
    if (auto existing = find_model(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto model = std::make_shared<zetta::ml::Model>(std::move(id), std::move(value), dir,
                                         replication_);
    // The model stores a text entity-key mapping as a table reference (#1255).
    model->set_entity_key_store([this](std::string_view owner, nlohmann::json mapping) {
        return entity_key_reference(owner, std::move(mapping));
    });
    model->persist_meta();
    models_.push_back(model);
    return model;
}

std::shared_ptr<zetta::ml::Model> Node::find_model(const std::string& id) const {
    for (const auto& model : models_) {
        if (model->id() == id) return model;
    }
    return nullptr;
}

bool Node::drop_model(const std::string& id) {
    const std::shared_ptr<zetta::ml::Model> model = find_model(id);
    if (!model) return false;
    if (record_store_ && model->feature_schema().is_object()) {
        if (const auto mapping = model->feature_schema().find("entity_key_mapping");
            mapping != model->feature_schema().end()) {
            zetta::data::drop_entity_key_table(*record_store_, *mapping,
                                               zetta::data::entity_key_owner("model", id));
        }
    }
    if (replication_) replication_->remove(catalog::model_key(id));
    const bool dropped = materialize_drop_model(id);
    if (dropped) record_catalog_change(*this, "model:" + id, true, "model dropped");
    return dropped;
}

bool Node::materialize_drop_model(const std::string& id) {
    const auto it = std::find_if(
        models_.begin(), models_.end(),
        [&](const std::shared_ptr<zetta::ml::Model>& m) { return m->id() == id; });
    if (it == models_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    models_.erase(it);
    return true;
}

void Node::backfill_denormalized_names() {
    for (const std::shared_ptr<zetta::pipeline::Run>& run : runs_) {
        std::string project_name = run->project_name();
        std::string pipeline_name = run->pipeline_name();
        std::shared_ptr<zetta::pipeline::Project> project =
            find_project_by_id(run->project());
        if (!project) {
            project = find_project(run->project());
        }
        if (project) {
            project_name = project->name();
            std::shared_ptr<zetta::pipeline::ProjectGraph> pipeline =
                project->find_graph_by_id(run->graph());
            if (!pipeline) {
                pipeline = project->find_graph(run->graph());
            }
            if (pipeline) {
                pipeline_name = pipeline->name();
            }
        }
        run->set_display_names(std::move(project_name),
                               std::move(pipeline_name));
    }

    for (const std::shared_ptr<zetta::ml::Model>& model : models_) {
        const std::string run_id = model->run_id().empty()
                                       ? model->run()
                                       : model->run_id();
        std::string project_name = model->project_name();
        std::string pipeline_name = model->pipeline_name();
        if (const std::shared_ptr<zetta::pipeline::Run> run =
                find_run(run_id)) {
            project_name = run->project_name();
            pipeline_name = run->pipeline_name();
        } else {
            std::shared_ptr<zetta::pipeline::Project> project =
                find_project_by_id(model->project());
            if (!project) {
                project = find_project(model->project());
            }
            if (project) {
                project_name = project->name();
            }
        }
        model->set_display_lineage(std::move(project_name),
                                   std::move(pipeline_name), run_id);
    }

    for (const std::shared_ptr<zetta::ml::Study>& study : studies_) {
        std::string project_name = study->spec().project;
        std::string pipeline_name = study->spec().pipeline;
        std::shared_ptr<zetta::pipeline::Project> project =
            find_project_by_id(project_name);
        if (!project) {
            project = find_project(project_name);
        }
        if (project) {
            project_name = project->name();
            std::shared_ptr<zetta::pipeline::ProjectGraph> pipeline =
                project->find_graph_by_id(pipeline_name);
            if (!pipeline) {
                pipeline = project->find_graph(pipeline_name);
            }
            if (pipeline) {
                pipeline_name = pipeline->name();
            }
        }
        study->set_project_pipeline(std::move(project_name),
                                    std::move(pipeline_name));
    }
}

void Node::update_project_display_name(const std::string& project_id,
                                       const std::string& old_name,
                                       const std::string& new_name) {
    for (const std::shared_ptr<zetta::pipeline::Run>& run : runs_) {
        if (run->project() == project_id || run->project() == old_name ||
            run->project_name() == old_name) {
            run->set_display_names(new_name, run->pipeline_name());
        }
    }
    for (const std::shared_ptr<zetta::ml::Model>& model : models_) {
        bool matches = model->project() == project_id ||
                       model->project() == old_name ||
                       model->project_name() == old_name;
        if (const std::shared_ptr<zetta::pipeline::Run> run =
                find_run(model->run_id())) {
            matches = matches || run->project() == project_id;
        }
        if (matches) {
            model->set_display_lineage(new_name, model->pipeline_name(),
                                       model->run_id());
        }
    }
    for (const std::shared_ptr<zetta::ml::Study>& study : studies_) {
        if (study->spec().project == project_id ||
            study->spec().project == old_name) {
            study->set_project_pipeline(new_name, study->spec().pipeline);
        }
    }
}

void Node::update_pipeline_display_name(const std::string& project_id,
                                        const std::string& pipeline_id,
                                        const std::string& old_name,
                                        const std::string& new_name) {
    const std::shared_ptr<zetta::pipeline::Project> project =
        find_project_by_id(project_id);
    const std::string project_name = project ? project->name() : std::string{};
    std::vector<std::string> matching_runs;
    for (const std::shared_ptr<zetta::pipeline::Run>& run : runs_) {
        const bool project_matches =
            run->project() == project_id ||
            (!project_name.empty() &&
             (run->project() == project_name ||
              run->project_name() == project_name));
        const bool pipeline_matches = run->graph() == pipeline_id ||
                                      run->graph() == old_name ||
                                      run->pipeline_name() == old_name;
        if (project_matches && pipeline_matches) {
            run->set_display_names(run->project_name(), new_name);
            matching_runs.push_back(run->id());
        }
    }
    for (const std::shared_ptr<zetta::ml::Model>& model : models_) {
        const bool run_matches = std::ranges::contains(
            matching_runs, model->run_id());
        const bool direct_match = model->pipeline_name() == old_name &&
                                  (model->project() == project_id ||
                                   (!project_name.empty() &&
                                    model->project_name() == project_name));
        if (run_matches || direct_match) {
            model->set_display_lineage(model->project_name(), new_name,
                                       model->run_id());
        }
    }
    for (const std::shared_ptr<zetta::ml::Study>& study : studies_) {
        const bool project_matches =
            study->spec().project == project_id ||
            (project && study->spec().project == project->name());
        const bool pipeline_matches = study->spec().pipeline == pipeline_id ||
                                      study->spec().pipeline == old_name;
        if (project_matches && pipeline_matches) {
            study->set_project_pipeline(study->spec().project, new_name);
        }
    }
}

// ----- solution monitors (#1106) -------------------------------------------

void Node::load_monitors() {
    monitors_.clear();
    const fs::path root = config_.data_dir;
    std::error_code filesystem_error;
    fs::create_directories(root, filesystem_error);
    for (const auto& entry : fs::directory_iterator(root, filesystem_error)) {
        if (!entry.is_directory()) {
            continue;
        }
        const fs::path metadata_path = entry.path() / "monitor.json";
        if (!fs::exists(metadata_path)) {
            continue;
        }
        std::ifstream input(metadata_path);
        const nlohmann::json metadata = nlohmann::json::parse(
            input,
            nullptr,
            /*allow_exceptions=*/false);
        std::string error;
        const std::optional<zetta::solutions::MonitorValue> value =
            metadata.is_discarded()
                ? std::nullopt
                : zetta::solutions::parse_monitor_value(metadata, &error);
        if (!value) {
            std::printf(
                "zetta: skipping invalid solution monitor %s (%s)\n",
                entry.path().filename().string().c_str(),
                metadata.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        monitors_.push_back(std::make_shared<zetta::solutions::Monitor>(
            entry.path().filename().string(),
            *value,
            entry.path(),
            replication_,
            [this](const zetta::solutions::MonitorBreach& breach) {
                publish_monitor_breach(breach);
            }));
    }
    std::sort(
        monitors_.begin(),
        monitors_.end(),
        [](const auto& left, const auto& right) {
            if (left->created() != right->created()) {
                return left->created() < right->created();
            }
            return left->id() < right->id();
        });
}

std::shared_ptr<zetta::solutions::Monitor> Node::create_monitor(
    zetta::solutions::MonitorSpec spec,
    std::string* error) {
    if (find_monitor(spec.solution)) {
        if (error != nullptr) {
            *error = "a monitor already exists for solution '" +
                     spec.solution + "'";
        }
        return nullptr;
    }
    const std::string id = zetta::util::new_uuid();
    std::optional<zetta::solutions::MonitorValue> value =
        zetta::solutions::make_monitor_value(
            std::move(spec),
            zetta::util::now_iso8601(),
            error);
    if (!value) {
        return nullptr;
    }
    if (replication_ != nullptr) {
        replication_->set(
            catalog::monitor_key(id),
            zetta::solutions::to_json(*value));
    }
    return materialize_monitor(id, std::move(*value));
}

std::shared_ptr<zetta::solutions::Monitor> Node::materialize_monitor(
    std::string id,
    zetta::solutions::MonitorValue value) {
    if (auto existing = find_monitor(id)) {
        return existing;
    }
    const fs::path directory = fs::path(config_.data_dir) / id;
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) {
        return nullptr;
    }
    auto monitor = std::make_shared<zetta::solutions::Monitor>(
        std::move(id),
        std::move(value),
        directory,
        replication_,
        [this](const zetta::solutions::MonitorBreach& breach) {
            publish_monitor_breach(breach);
        });
    monitor->persist_meta();
    monitors_.push_back(monitor);
    std::sort(
        monitors_.begin(),
        monitors_.end(),
        [](const auto& left, const auto& right) {
            if (left->created() != right->created()) {
                return left->created() < right->created();
            }
            return left->id() < right->id();
        });
    return monitor;
}

std::shared_ptr<zetta::solutions::Monitor> Node::find_monitor(
    const std::string& id_or_solution) const {
    for (const auto& monitor : monitors_) {
        if (monitor->id() == id_or_solution ||
            monitor->solution() == id_or_solution) {
            return monitor;
        }
    }
    return nullptr;
}

bool Node::drop_monitor(const std::string& id) {
    if (!find_monitor(id)) {
        return false;
    }
    if (replication_ != nullptr) {
        replication_->remove(catalog::monitor_key(id));
    }
    return materialize_drop_monitor(id);
}

bool Node::materialize_drop_monitor(const std::string& id) {
    const auto found = std::find_if(
        monitors_.begin(),
        monitors_.end(),
        [&](const std::shared_ptr<zetta::solutions::Monitor>& monitor) {
            return monitor->id() == id;
        });
    if (found == monitors_.end()) {
        return false;
    }
    std::error_code error;
    fs::remove_all((*found)->dir(), error);
    monitors_.erase(found);
    return true;
}

void Node::publish_monitor_breach(
    const zetta::solutions::MonitorBreach& breach) {
    if (pubsub_ == nullptr) {
        return;
    }
    const zetta::pubsub::Identity engine{
        zetta::pubsub::Address{"service", "solution-monitor"},
        {},
    };
    const zetta::pubsub::Address topic{"topic", "solution-events"};
    if (!pubsub_->topic_settings(topic)) {
        zetta::pubsub::TopicSettings settings;
        settings.exposed = false;
        std::string reason;
        if (!pubsub_->create_topic(engine, topic, settings, &reason) &&
            !pubsub_->topic_settings(topic)) {
            return;
        }
    }
    nlohmann::json payload = zetta::solutions::to_json(breach);
    payload["type"] = "monitor_breach";
    zetta::pubsub::PublishRequest request;
    request.to = {topic};
    request.encoding = zetta::pubsub::Encoding::Json;
    request.mime_type = "application/json";
    request.payload = payload.dump();
    request.correlation_id = breach.monitor_id;
    pubsub_->publish(engine, std::move(request));
}

// ----- workspace profiles (#886) --------------------------------------------
// A top-level replicated entity, identical in shape to models: a `profile:<uuid>`
// CRDT key + a `<uuid>/workspace_profile.json` projection, boundary-validated on
// load and reconcile.

void Node::load_workspace_profiles() {
    workspace_profiles_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "workspace_profile.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        std::string error;
        const std::optional<zetta::workspace::WorkspaceProfileValue> value =
            meta.is_discarded()
                ? std::nullopt
                : zetta::workspace::parse_workspace_profile_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid workspace profile %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        workspace_profiles_.push_back(
            std::make_shared<zetta::workspace::WorkspaceProfile>(
                entry.path().filename().string(), *value, entry.path(),
                replication_));
    }
    std::sort(workspace_profiles_.begin(), workspace_profiles_.end(),
              [](const auto& a, const auto& b) {
                  if (a->created() != b->created()) return a->created() < b->created();
                  return a->id() < b->id();
              });
}

std::shared_ptr<zetta::workspace::WorkspaceProfile>
Node::create_workspace_profile(zetta::workspace::WorkspaceProfileValue value) {
    if (value.owner.empty()) return nullptr;  // owner is required
    const std::string id = zetta::util::new_uuid();
    value.created = zetta::util::now_iso8601();
    if (value.updated.empty()) value.updated = value.created;
    if (replication_) {
        replication_->set(catalog::workspace_profile_key(id), to_json(value));
    }
    return materialize_workspace_profile(id, std::move(value));
}

std::shared_ptr<zetta::workspace::WorkspaceProfile>
Node::materialize_workspace_profile(std::string id,
                                    zetta::workspace::WorkspaceProfileValue value) {
    if (auto existing = find_workspace_profile(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto profile = std::make_shared<zetta::workspace::WorkspaceProfile>(
        std::move(id), std::move(value), dir, replication_);
    profile->persist_meta();
    workspace_profiles_.push_back(profile);
    return profile;
}

std::shared_ptr<zetta::workspace::WorkspaceProfile>
Node::find_workspace_profile(const std::string& id) const {
    for (const auto& profile : workspace_profiles_) {
        if (profile->id() == id) return profile;
    }
    return nullptr;
}

bool Node::drop_workspace_profile(const std::string& id) {
    if (!find_workspace_profile(id)) return false;
    if (replication_) replication_->remove(catalog::workspace_profile_key(id));
    return materialize_drop_workspace_profile(id);
}

bool Node::materialize_drop_workspace_profile(const std::string& id) {
    const auto it = std::find_if(
        workspace_profiles_.begin(), workspace_profiles_.end(),
        [&](const std::shared_ptr<zetta::workspace::WorkspaceProfile>& p) {
            return p->id() == id;
        });
    if (it == workspace_profiles_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    workspace_profiles_.erase(it);
    return true;
}

// ----- plans (Conflict D: metered usage + plan) -----------------------------
// A top-level replicated entity, identical in shape to workspace profiles: a
// `plan:<uuid>` CRDT key + a `<uuid>/plan.json` projection, boundary-validated.

void Node::load_plans() {
    plans_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "plan.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr,
                                                /*allow_exceptions=*/false);
        std::string error;
        const std::optional<zetta::billing::PlanValue> value =
            meta.is_discarded() ? std::nullopt
                                : zetta::billing::parse_plan_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid plan %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        plans_.push_back(std::make_shared<zetta::billing::Plan>(
            entry.path().filename().string(), *value, entry.path(),
            replication_));
    }
    std::sort(plans_.begin(), plans_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<zetta::billing::Plan> Node::create_plan(
    zetta::billing::PlanValue value) {
    if (value.name.empty()) return nullptr;  // name is required
    const std::string id = zetta::util::new_uuid();
    value.created = zetta::util::now_iso8601();
    if (value.updated.empty()) value.updated = value.created;
    if (replication_) replication_->set(catalog::plan_key(id), to_json(value));
    return materialize_plan(id, std::move(value));
}

std::shared_ptr<zetta::billing::Plan> Node::materialize_plan(
    std::string id, zetta::billing::PlanValue value) {
    if (auto existing = find_plan(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto plan = std::make_shared<zetta::billing::Plan>(
        std::move(id), std::move(value), dir, replication_);
    plan->persist_meta();
    plans_.push_back(plan);
    return plan;
}

std::shared_ptr<zetta::billing::Plan> Node::find_plan(const std::string& id) const {
    for (const auto& plan : plans_) {
        if (plan->id() == id) return plan;
    }
    return nullptr;
}

bool Node::drop_plan(const std::string& id) {
    if (!find_plan(id)) return false;
    if (replication_) replication_->remove(catalog::plan_key(id));
    return materialize_drop_plan(id);
}

bool Node::materialize_drop_plan(const std::string& id) {
    const auto it = std::find_if(
        plans_.begin(), plans_.end(),
        [&](const std::shared_ptr<zetta::billing::Plan>& p) { return p->id() == id; });
    if (it == plans_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    plans_.erase(it);
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
        const std::optional<zetta::index::IndexValue> value =
            meta.is_discarded() ? std::nullopt : zetta::index::parse_index_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid index %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON" : error.c_str());
            continue;
        }
        indexes_.push_back(std::make_shared<zetta::index::Index>(
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

std::shared_ptr<zetta::index::Index> Node::create_index(zetta::index::IndexValue value) {
    // Basic guards (the schema enforces the rest at persist/reconcile): a usable
    // index needs a source table, a backend to build it, and at least one field.
    if (value.table.empty() || value.backend.empty() || value.fields.empty()) {
        return nullptr;
    }
    const std::string id = zetta::util::new_uuid();
    value.created = zetta::util::now_iso8601();
    if (replication_) replication_->set(catalog::index_key(id), to_json(value));
    return materialize_index(id, std::move(value));
}

std::shared_ptr<zetta::index::Index> Node::materialize_index(std::string id, zetta::index::IndexValue value) {
    if (auto existing = find_index(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto index = std::make_shared<zetta::index::Index>(std::move(id), std::move(value), dir,
                                         replication_);
    index->persist_meta();
    indexes_.push_back(index);
    return index;
}

std::shared_ptr<zetta::index::Index> Node::find_index(const std::string& id) const {
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
        [&](const std::shared_ptr<zetta::index::Index>& i) { return i->id() == id; });
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
        const std::optional<zetta::compute::ComputeJobValue> value =
            meta.is_discarded() ? std::nullopt
                                : zetta::compute::parse_compute_job_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid job %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        jobs_.push_back(std::make_shared<zetta::compute::ComputeJob>(
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

std::shared_ptr<zetta::compute::ComputeJob> Node::create_job(zetta::compute::JobKind kind, std::string project,
                                             std::string graph, std::string run,
                                             std::string scaling_policy) {
    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    zetta::compute::ComputeJobValue value;
    value.kind = kind;
    value.status = zetta::compute::JobStatus::Queued;
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
            {zetta::governance::AuditAction::JobCreated, {}, job->id(), true,
             "compute job " + std::string(to_string(job->kind())) + " queued",
             {}});
    }
    return job;
}

std::shared_ptr<zetta::compute::ComputeJob> Node::materialize_job(std::string id,
                                                  zetta::compute::ComputeJobValue value) {
    if (auto existing = find_job(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto job = std::make_shared<zetta::compute::ComputeJob>(std::move(id), std::move(value), dir,
                                            replication_, /*audit=*/this);
    job->persist_meta();
    jobs_.push_back(job);
    return job;
}

std::shared_ptr<zetta::compute::ComputeJob> Node::find_job(const std::string& id) const {
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
        [&](const std::shared_ptr<zetta::compute::ComputeJob>& j) { return j->id() == id; });
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
        const std::optional<zetta::data::DatasetPartitionValue> value =
            meta.is_discarded() ? std::nullopt
                                : zetta::data::parse_dataset_partition_value(meta, &error);
        if (!value) {
            std::printf("zetta: skipping invalid partition %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        meta.is_discarded() ? "unparseable JSON"
                                            : error.c_str());
            continue;
        }
        dataset_partitions_.push_back(std::make_shared<zetta::data::DatasetPartition>(
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

std::shared_ptr<zetta::data::DatasetPartition> Node::create_dataset_partition(
    zetta::data::DatasetPartitionValue value) {
    // A partition must reference a table — that ref is the catalog's anchor.
    if (value.table.empty()) return nullptr;
    const std::string id = zetta::util::new_uuid();
    if (value.created.empty()) value.created = zetta::util::now_iso8601();
    if (replication_) {
        replication_->set(catalog::partition_key(id), to_json(value));
    }
    return materialize_dataset_partition(id, std::move(value));
}

std::shared_ptr<zetta::data::DatasetPartition> Node::materialize_dataset_partition(
    std::string id, zetta::data::DatasetPartitionValue value) {
    if (auto existing = find_dataset_partition(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto partition = std::make_shared<zetta::data::DatasetPartition>(
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

std::shared_ptr<zetta::data::DatasetPartition> Node::find_dataset_partition(
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
        [&](const std::shared_ptr<zetta::data::DatasetPartition>& p) {
            return p->id() == id;
        });
    if (it == dataset_partitions_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    dataset_partitions_.erase(it);
    return true;
}

// ----- cloud inspections (ephemeral; not replicated, not persisted) ----------

void Node::record_cloud_inspection(zetta::cloud::CloudInspectReport report) {
    // A consent-approved cloud read is a governed action: record it on the
    // audit trail (#624), so it surfaces in the activity feed like every other
    // completed action. `allowed` carries whether the inspect actually reached
    // the provider; `detail` is the inspector's overall note verbatim.
    {
        zetta::governance::AuditEventValue event;
        event.action = zetta::governance::AuditAction::CloudInspected;
        event.target = report.provider_id;
        event.allowed = report.ran && report.reachable;
        event.detail = !report.detail.empty()
                           ? report.detail
                           : (report.authenticated
                                  ? "authenticated as " + report.identity
                                  : "not authenticated");
        record_audit_event(std::move(event));
    }

    // Latest report per provider wins, so the cache holds one current row per
    // provider for system.cloud_status. No CRDT capture, no disk write —
    // this is session-local observed state (RULE-7).
    const auto it = std::find_if(
        cloud_inspections_.begin(), cloud_inspections_.end(),
        [&](const zetta::cloud::CloudInspectReport& existing) {
            return existing.provider_id == report.provider_id;
        });
    if (it == cloud_inspections_.end()) {
        cloud_inspections_.push_back(std::move(report));
    } else {
        *it = std::move(report);
    }
}

// ----- provisioner configs (ephemeral; CLI-detected, not replicated) ----------

void Node::record_provisioner_config(zetta::cloud::ProvisionerConfig config) {
    // Latest config per target wins, so the cache holds one current row per target
    // for system.provisioners — like record_cloud_inspection, this is
    // session-local observed state from the local CLI (RULE-7): no CRDT capture,
    // no disk write, no credentials.
    const auto it = std::find_if(
        provisioner_configs_.begin(), provisioner_configs_.end(),
        [&](const zetta::cloud::ProvisionerConfig& existing) {
            return existing.target == config.target;
        });
    if (it == provisioner_configs_.end()) {
        provisioner_configs_.push_back(std::move(config));
    } else {
        *it = std::move(config);
    }
}

// ----- principals (mirror the project CRUD; replicated as principal:<uuid>) ---

bool Node::drop_principal(const std::string& name) {
    if (!identity_writable()) return false;
    auto principal = find_principal(name);
    if (!principal) return false;
    // Drop the principal's keys from the document too (their folders go with the
    // principal folder below). Snapshot the ids first — drop_key mutates keys().
    if (replication_) {
        std::vector<std::string> key_ids;
        for (const auto& key : principal->keys()) key_ids.push_back(key->id());
        for (const std::string& key_id : key_ids) {
            remove_identity(catalog::access_key_key(key_id));
        }
        remove_identity(catalog::principal_key(principal->id()));
    }
    return materialize_drop_principal(principal->id());
}

bool Node::materialize_drop_principal(const std::string& id) {
    const auto it = std::find_if(
        principals_.begin(), principals_.end(),
        [&](const std::shared_ptr<zetta::governance::Principal>& p) { return p->id() == id; });
    if (it == principals_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    principals_.erase(it);
    return true;
}

bool Node::rename_principal(const std::string& from, const std::string& to) {
    if (!identity_writable()) return false;
    if (to.empty() || find_principal(to)) return false;  // empty or clashes
    auto principal = find_principal(from);
    if (!principal) return false;
    principal->set_metadata(principal->kind(), to, principal->created());
    if (replication_) {
        replicate_identity(catalog::principal_key(principal->id()),
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

std::shared_ptr<zetta::governance::AccessKey> Node::find_access_key(
    const std::string& key_id) const {
    for (const auto& principal : principals_) {
        if (auto key = principal->find_key(key_id)) return key;
    }
    return nullptr;
}

// ===== Users / roles / tokens: the credential store (FEAT-9 / #744) ==========
// Each entity is a top-level replicated record living in its own UUID-named
// folder under the data dir (like principals), distinguished by its marker file
// (user.json / role.json / token.json / user_role.json). Local mutations capture
// into the CRDT (`user:` / `role:` / `token:` / `user_role:`); reconcile
// materializes the projection back from the document (never re-captures). The
// load_* helpers rebuild the projection from disk on start().

// ----- users (user:<uuid>) ---------------------------------------------------

void Node::load_users() {
    users_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "user.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr, false);
        if (meta.is_discarded() || !meta.is_object() || !meta.contains("name") ||
            !meta["name"].is_string()) {
            continue;
        }
        const auto status = governance::user_status_from(
            meta.value("status", std::string{}));
        if (!status) continue;  // boundary validation
        users_.push_back(std::make_shared<governance::User>(
            entry.path().filename().string(), meta["name"].get<std::string>(),
            *status, meta.value("created", std::string{}), entry.path()));
    }
    std::sort(users_.begin(), users_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<zetta::governance::User> Node::find_user(
    const std::string& name) const {
    for (const auto& user : users_) {
        if (user->name() == name) return user;
    }
    return nullptr;
}

std::shared_ptr<zetta::governance::User> Node::find_user_by_id(
    const std::string& id) const {
    for (const auto& user : users_) {
        if (user->id() == id) return user;
    }
    return nullptr;
}

std::shared_ptr<zetta::governance::User> Node::create_user(std::string name) {
    if (!identity_writable()) return nullptr;
    if (name.empty() || find_user(name)) return nullptr;
    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    if (replication_) {
        replicate_identity(catalog::user_key(id),
                          nlohmann::json{{"name", name},
                                         {"status", to_string(
                                              governance::UserStatus::Active)},
                                         {"created", now}});
    }
    return materialize_user(id, std::move(name), governance::UserStatus::Active,
                            now);
}

std::shared_ptr<zetta::governance::User> Node::materialize_user(
    std::string id, std::string name, governance::UserStatus status,
    std::string created) {
    if (auto existing = find_user_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto user = std::make_shared<governance::User>(
        std::move(id), std::move(name), status, std::move(created), dir);
    user->persist_meta();
    users_.push_back(user);
    return user;
}

bool Node::set_user_status(const std::string& name,
                           governance::UserStatus status) {
    if (!identity_writable()) return false;
    auto user = find_user(name);
    if (!user) return false;
    user->set_metadata(user->name(), status, user->created());
    if (replication_) {
        replicate_identity(catalog::user_key(user->id()), user->metadata());
    }
    return true;
}

bool Node::drop_user(const std::string& name) {
    if (!identity_writable()) return false;
    auto user = find_user(name);
    if (!user) return false;
    const std::string user_id = user->id();
    if (replication_) {
        // Drop the user's tokens and role assignments too (their bodies go with
        // the folders removed below). Snapshot ids first — the removes mutate the
        // vectors as reconcile/materialize run.
        std::vector<std::string> token_ids;
        for (const auto& token : tokens_) {
            if (token->user_id() == user_id) token_ids.push_back(token->id());
        }
        for (const std::string& id : token_ids) {
            remove_identity(catalog::token_key(id));
        }
        std::vector<std::string> assignment_ids;
        for (const auto& assignment : user_roles_) {
            if (assignment->user_id() == user_id) {
                assignment_ids.push_back(assignment->id());
            }
        }
        for (const std::string& id : assignment_ids) {
            remove_identity(catalog::user_role_key(id));
        }
        remove_identity(catalog::user_key(user_id));
    }
    // Also drop the local projections (standalone has no CRDT to reconcile from).
    std::vector<std::string> token_ids;
    for (const auto& token : tokens_) {
        if (token->user_id() == user_id) token_ids.push_back(token->id());
    }
    for (const std::string& id : token_ids) materialize_drop_token(id);
    std::vector<std::string> assignment_ids;
    for (const auto& assignment : user_roles_) {
        if (assignment->user_id() == user_id) {
            assignment_ids.push_back(assignment->id());
        }
    }
    for (const std::string& id : assignment_ids) materialize_drop_user_role(id);
    return materialize_drop_user(user_id);
}

bool Node::materialize_drop_user(const std::string& id) {
    const auto it = std::find_if(
        users_.begin(), users_.end(),
        [&](const std::shared_ptr<governance::User>& u) { return u->id() == id; });
    if (it == users_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    users_.erase(it);
    return true;
}

void Node::reconcile_users(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    struct UserEntry {
        std::string name;
        governance::UserStatus status;
        std::string created;
    };
    std::map<std::string, UserEntry> desired;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kUserPrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) continue;
        const auto status =
            governance::user_status_from(value.value("status", std::string{}));
        if (!status) continue;  // boundary
        desired.emplace(std::move(id),
                        UserEntry{value.value("name", std::string{}), *status,
                                  value.value("created", std::string{})});
    }
    std::vector<std::string> local_ids;
    for (const auto& user : users_) local_ids.push_back(user->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::user_key(id)) &&
            !desired.contains(id)) {
            materialize_drop_user(id);
        }
    }
    for (const auto& [id, entry] : desired) {
        auto user = find_user_by_id(id);
        if (!user) {
            materialize_user(id, entry.name, entry.status, entry.created);
        } else if (user->name() != entry.name || user->status() != entry.status) {
            user->set_metadata(entry.name, entry.status, entry.created);
        }
    }
}

// ----- roles (role:<uuid>) ---------------------------------------------------

void Node::load_roles() {
    roles_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "role.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr, false);
        if (meta.is_discarded() || !meta.is_object() || !meta.contains("name") ||
            !meta["name"].is_string()) {
            continue;
        }
        const auto permissions = governance::role_permissions_from_json(meta);
        if (!permissions) continue;  // boundary validation
        roles_.push_back(std::make_shared<governance::Role>(
            entry.path().filename().string(), meta["name"].get<std::string>(),
            *permissions, meta.value("created", std::string{}), entry.path()));
    }
    std::sort(roles_.begin(), roles_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<zetta::governance::Role> Node::find_role(
    const std::string& name) const {
    for (const auto& role : roles_) {
        if (role->name() == name) return role;
    }
    return nullptr;
}

std::shared_ptr<zetta::governance::Role> Node::find_role_by_id(
    const std::string& id) const {
    for (const auto& role : roles_) {
        if (role->id() == id) return role;
    }
    return nullptr;
}

std::shared_ptr<zetta::governance::Role> Node::create_builtin_role(
    const std::string& name) {
    if (!identity_writable()) return nullptr;
    if (find_role(name)) return nullptr;  // clash
    const auto permissions = governance::builtin_role_permissions(name);
    if (!permissions) return nullptr;  // not a known built-in
    // Seeded under a root: every node that creates this built-in writes the same
    // key, so nodes that seed before they sync converge to one role (#763).
    const std::string id = seeded_identity_id("builtin-role:" + name);
    const std::string now = zetta::util::now_iso8601();
    if (replication_) {
        replicate_identity(catalog::role_key(id),
                          nlohmann::json{{"name", name},
                                         {"scope", permissions->scope},
                                         {"capabilities", permissions->capabilities},
                                         {"created", now}});
    }
    return materialize_role(id, name, *permissions, now);
}

std::size_t Node::ensure_builtin_roles() {
    if (!identity_writable()) return 0;
    std::size_t created = 0;
    for (const std::string& name : governance::builtin_role_names()) {
        const auto canonical = governance::builtin_role_permissions(name);
        if (!canonical) continue;  // not a known built-in
        // Reconcile drift: a built-in must always mean its canonical permission
        // set. Repair EVERY role carrying the name, not just the first — CRDT
        // multi-leader seeding can leave duplicates (one per node that bootstrapped
        // before syncing), and a user may be assigned any of them, so all must
        // converge to canonical. Idempotent: keeps each id, writes only on drift,
        // re-captures into the CRDT. This runs on every startup (main.cpp), so a
        // binary upgrade heals roles frozen at an older vocabulary.
        bool found = false;
        for (const auto& role : roles_) {
            if (role->name() != name) continue;
            found = true;
            if (role->permissions() != *canonical) {
                role->set_metadata(role->name(), *canonical, role->created());
                if (replication_) {
                    replicate_identity(catalog::role_key(role->id()),
                                      role->metadata());
                }
            }
        }
        // A fresh store has none yet — create the one canonical instance.
        if (!found && create_builtin_role(name)) ++created;
    }
    return created;
}

std::shared_ptr<zetta::governance::Role> Node::create_role(
    const std::string& name) {
    if (!identity_writable()) return nullptr;
    if (name.empty() || find_role(name)) return nullptr;  // empty or clash
    // A custom role starts with NO rights (deny-by-default) and an unrestricted
    // scope — scope granularity is a later slice (epic #743 #4); today
    // authorize_user decides on capabilities alone, so an empty capability set
    // grants nothing until GRANT adds permissions.
    governance::RolePermissions permissions;
    permissions.scope = governance::unrestricted_scope();
    const std::string id = zetta::util::new_uuid();
    const std::string now = zetta::util::now_iso8601();
    if (replication_) {
        replicate_identity(catalog::role_key(id),
                          nlohmann::json{{"name", name},
                                         {"scope", permissions.scope},
                                         {"capabilities", permissions.capabilities},
                                         {"created", now}});
    }
    return materialize_role(id, name, std::move(permissions), now);
}

bool Node::drop_role(const std::string& name) {
    if (!identity_writable()) return false;
    auto role = find_role(name);
    if (!role) return false;
    const std::string role_id = role->id();
    // Snapshot the assignments referencing this role first (the removes below
    // mutate user_roles_ as materialize/reconcile run).
    std::vector<std::string> assignment_ids;
    for (const auto& assignment : user_roles_) {
        if (assignment->role_id() == role_id) {
            assignment_ids.push_back(assignment->id());
        }
    }
    if (replication_) {
        for (const std::string& id : assignment_ids) {
            remove_identity(catalog::user_role_key(id));
        }
        remove_identity(catalog::role_key(role_id));
    }
    for (const std::string& id : assignment_ids) materialize_drop_user_role(id);
    return materialize_drop_role(role_id);
}

bool Node::grant_permissions(const std::string& role_name,
                             const std::vector<std::string>& permissions) {
    if (!identity_writable()) return false;
    auto role = find_role(role_name);
    if (!role) return false;
    governance::RolePermissions updated = role->permissions();
    for (const std::string& name : permissions) {
        for (const governance::Operation& op :
             governance::permission_operations(name)) {
            bool present = false;
            for (const governance::Capability& existing :
                 updated.capabilities.grants) {
                if (existing.right() == op) {
                    present = true;
                    break;
                }
            }
            if (!present) updated.capabilities.grant(op.domain, op.action);
        }
    }
    role->set_metadata(role->name(), std::move(updated), role->created());
    if (replication_) {
        replicate_identity(catalog::role_key(role->id()), role->metadata());
    }
    return true;
}

bool Node::revoke_permissions(const std::string& role_name,
                              const std::vector<std::string>& permissions) {
    if (!identity_writable()) return false;
    auto role = find_role(role_name);
    if (!role) return false;
    // Collect the operations to remove (the union across the named permissions).
    std::vector<governance::Operation> to_remove;
    for (const std::string& name : permissions) {
        for (const governance::Operation& op :
             governance::permission_operations(name)) {
            to_remove.push_back(op);
        }
    }
    governance::RolePermissions updated = role->permissions();
    std::vector<governance::Capability> kept;
    for (const governance::Capability& capability : updated.capabilities.grants) {
        bool remove = false;
        for (const governance::Operation& op : to_remove) {
            if (capability.right() == op) {
                remove = true;
                break;
            }
        }
        if (!remove) kept.push_back(capability);
    }
    updated.capabilities.grants = std::move(kept);
    role->set_metadata(role->name(), std::move(updated), role->created());
    if (replication_) {
        replicate_identity(catalog::role_key(role->id()), role->metadata());
    }
    return true;
}

bool Node::grant_scoped_permissions(
    const std::string& role_name, const std::vector<std::string>& permissions,
    const governance::Resource& object) {
    if (!identity_writable()) return false;
    auto role = find_role(role_name);
    if (!role) return false;
    governance::RolePermissions updated = role->permissions();
    for (const std::string& name : permissions) {
        for (const governance::Operation& op :
             governance::permission_operations(name)) {
            const governance::ScopedGrant grant{op, object};
            bool present = false;
            for (const governance::ScopedGrant& existing : updated.scoped_grants) {
                if (existing == grant) {
                    present = true;
                    break;
                }
            }
            if (!present) updated.scoped_grants.push_back(grant);
        }
    }
    role->set_metadata(role->name(), std::move(updated), role->created());
    if (replication_) {
        replicate_identity(catalog::role_key(role->id()), role->metadata());
    }
    return true;
}

bool Node::revoke_scoped_permissions(
    const std::string& role_name, const std::vector<std::string>& permissions,
    const governance::Resource& object) {
    if (!identity_writable()) return false;
    auto role = find_role(role_name);
    if (!role) return false;
    // The (operation, object) pairs to remove — the named permissions' operations
    // each bound to `object`.
    std::vector<governance::ScopedGrant> to_remove;
    for (const std::string& name : permissions) {
        for (const governance::Operation& op :
             governance::permission_operations(name)) {
            to_remove.push_back(governance::ScopedGrant{op, object});
        }
    }
    governance::RolePermissions updated = role->permissions();
    std::vector<governance::ScopedGrant> kept;
    for (const governance::ScopedGrant& grant : updated.scoped_grants) {
        bool remove = false;
        for (const governance::ScopedGrant& target : to_remove) {
            if (grant == target) {
                remove = true;
                break;
            }
        }
        if (!remove) kept.push_back(grant);
    }
    updated.scoped_grants = std::move(kept);
    role->set_metadata(role->name(), std::move(updated), role->created());
    if (replication_) {
        replicate_identity(catalog::role_key(role->id()), role->metadata());
    }
    return true;
}

std::shared_ptr<zetta::governance::Role> Node::materialize_role(
    std::string id, std::string name, governance::RolePermissions permissions,
    std::string created) {
    if (auto existing = find_role_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto role = std::make_shared<governance::Role>(
        std::move(id), std::move(name), std::move(permissions),
        std::move(created), dir);
    role->persist_meta();
    roles_.push_back(role);
    return role;
}

bool Node::materialize_drop_role(const std::string& id) {
    const auto it = std::find_if(
        roles_.begin(), roles_.end(),
        [&](const std::shared_ptr<governance::Role>& r) { return r->id() == id; });
    if (it == roles_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    roles_.erase(it);
    return true;
}

void Node::reconcile_roles(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    struct RoleEntry {
        std::string name;
        governance::RolePermissions permissions;
        std::string created;
    };
    std::map<std::string, RoleEntry> desired;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kRolePrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) continue;
        const auto permissions = governance::role_permissions_from_json(value);
        if (!permissions) continue;  // boundary
        desired.emplace(std::move(id),
                        RoleEntry{value.value("name", std::string{}),
                                  *permissions,
                                  value.value("created", std::string{})});
    }
    std::vector<std::string> local_ids;
    for (const auto& role : roles_) local_ids.push_back(role->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::role_key(id)) &&
            !desired.contains(id)) {
            materialize_drop_role(id);
        }
    }
    for (auto& [id, entry] : desired) {
        auto role = find_role_by_id(id);
        if (!role) {
            materialize_role(id, entry.name, entry.permissions, entry.created);
        } else if (role->name() != entry.name ||
                   role->permissions() != entry.permissions) {
            role->set_metadata(entry.name, entry.permissions, entry.created);
        }
    }
}

// ----- tokens (token:<uuid>) -------------------------------------------------

void Node::load_tokens() {
    tokens_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "token.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr, false);
        const auto value = governance::token_value_from_json(meta);
        if (!value) continue;  // boundary validation — skip a malformed token
        tokens_.push_back(std::make_shared<governance::Token>(
            entry.path().filename().string(), *value, entry.path()));
    }
    std::sort(tokens_.begin(), tokens_.end(), [](const auto& a, const auto& b) {
        if (a->created() != b->created()) return a->created() < b->created();
        return a->id() < b->id();
    });
}

std::shared_ptr<zetta::governance::Token> Node::find_token_by_id(
    const std::string& id) const {
    for (const auto& token : tokens_) {
        if (token->id() == id) return token;
    }
    return nullptr;
}

Node::TokenIssue Node::create_token(const std::string& user_id,
                                    governance::TokenType type,
                                    std::string expires_at) {
    if (!identity_writable()) return {};
    if (!find_user_by_id(user_id)) return {};  // unknown user
    // The bearer names its token (#1417): `zt1_<id>_<random>`, hashed whole,
    // so authentication verifies only the token it names.
    const std::string id = zetta::util::new_uuid();
    const std::string random = governance::generate_token_secret();
    if (random.empty()) return {};  // RNG failure
    const std::string secret = governance::format_token_bearer(id, random);
    const std::string salt = governance::generate_token_salt();
    const std::string hash = governance::hash_token_secret(salt, secret);
    if (salt.empty() || hash.empty()) return {};  // RNG failure
    governance::TokenValue value;
    value.user_id = user_id;
    value.type = type;
    value.hash = hash;
    value.salt = salt;
    value.created = zetta::util::now_iso8601();
    value.expires_at = std::move(expires_at);
    value.revoked = false;
    if (replication_) {
        nlohmann::json crdt = value.expires_at.empty()
            ? nlohmann::json{{"user", value.user_id},
                             {"type", to_string(value.type)},
                             {"hash", value.hash},
                             {"salt", value.salt},
                             {"created", value.created},
                             {"revoked", value.revoked}}
            : nlohmann::json{{"user", value.user_id},
                             {"type", to_string(value.type)},
                             {"hash", value.hash},
                             {"salt", value.salt},
                             {"created", value.created},
                             {"expires_at", value.expires_at},
                             {"revoked", value.revoked}};
        replicate_identity(catalog::token_key(id), std::move(crdt));
    }
    TokenIssue issue;
    issue.token = materialize_token(id, std::move(value));
    if (!issue.token) return {};
    issue.secret = secret;  // the ONLY time the plaintext leaves this function
    return issue;
}

std::shared_ptr<zetta::governance::Token> Node::materialize_token(
    std::string id, governance::TokenValue value) {
    if (auto existing = find_token_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto token = std::make_shared<governance::Token>(std::move(id),
                                                     std::move(value), dir);
    token->persist_meta();
    tokens_.push_back(token);
    return token;
}

bool Node::revoke_token(const std::string& id) {
    if (!identity_writable()) return false;
    auto token = find_token_by_id(id);
    if (!token) return false;
    if (token->revoked()) return true;  // already revoked — idempotent
    governance::TokenValue value = token->value();
    value.revoked = true;
    token->set_value(value);
    if (replication_) {
        nlohmann::json crdt = token->metadata();
        replicate_identity(catalog::token_key(id), std::move(crdt));
    }
    return true;
}

bool Node::materialize_drop_token(const std::string& id) {
    const auto it = std::find_if(
        tokens_.begin(), tokens_.end(),
        [&](const std::shared_ptr<governance::Token>& t) { return t->id() == id; });
    if (it == tokens_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    tokens_.erase(it);
    return true;
}

void Node::reconcile_tokens(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    std::map<std::string, governance::TokenValue> desired;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kTokenPrefix);
        if (id.empty()) continue;
        const auto parsed = governance::token_value_from_json(value);
        if (!parsed) continue;  // boundary
        desired.emplace(std::move(id), *parsed);
    }
    std::vector<std::string> local_ids;
    for (const auto& token : tokens_) local_ids.push_back(token->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::token_key(id)) &&
            !desired.contains(id)) {
            materialize_drop_token(id);
        }
    }
    for (auto& [id, value] : desired) {
        auto token = find_token_by_id(id);
        if (!token) {
            materialize_token(id, value);
        } else if (token->value() != value) {
            token->set_value(value);  // a peer revoked / changed it (no re-capture)
        }
    }
}

std::shared_ptr<zetta::governance::User> Node::authenticate_token(
    std::string_view secret, bool allow_scan) const {
    if (secret.empty()) return nullptr;
    const std::string now = zetta::util::now_iso8601();
    // A bearer in the issued format names its token (#1417): verify that one
    // token and stop. A wrong or unknown one costs one digest, never a scan.
    if (const std::optional<std::string_view> named = governance::token_lookup_id(secret)) {
        const auto token = find_token_by_id(std::string(*named));
        if (!token || !token->validates(secret, now, /*allow_kdf=*/false)) return nullptr;
        auto user = find_user_by_id(token->user_id());
        return user && user->active() ? user : nullptr;
    }
    // A bearer without the prefix: an operator-chosen admin token, or a token
    // issued before the format. One this process already verified is found by
    // its digest; anything else scans, only when `allow_scan` (the HTTP gate
    // withholds it from a source past its failed-authentication budget).
    const std::string digest = governance::hash_token_secret(kLegacyTokenLookupSalt, secret);
    if (const auto cached = legacy_token_lookup_.find(digest);
        cached != legacy_token_lookup_.end()) {
        const auto token = find_token_by_id(cached->second);
        if (token && token->validates(secret, now, /*allow_kdf=*/false)) {
            auto user = find_user_by_id(token->user_id());
            if (user && user->active()) return user;
        }
        legacy_token_lookup_.erase(cached);
    }
    if (!allow_scan) return nullptr;
    const auto remember = [&](const governance::Token& token) {
        if (digest.empty()) return;
        if (legacy_token_lookup_.size() >= kLegacyTokenLookupCapacity) {
            legacy_token_lookup_.clear();
        }
        legacy_token_lookup_.emplace(digest, token.id());
    };
    bool unverified_kdf_token = false;
    for (const auto& token : tokens_) {
        // validates() is constant-time on the hash compare and rejects revoked /
        // expired tokens; then the owning user must exist and be active. The
        // first pass never derives: a KDF token not yet verified is left for the
        // second pass.
        if (token->uses_kdf() && !token->kdf_verified()) unverified_kdf_token = true;
        if (!token->validates(secret, now, /*allow_kdf=*/false)) continue;
        auto user = find_user_by_id(token->user_id());
        if (user && user->active()) {
            remember(*token);
            return user;
        }
    }
    if (!unverified_kdf_token) return nullptr;
    // The slow pass (#769), budgeted: refill one check per kColdKdfRefill up to
    // the burst, and spend one per bearer that reaches here.
    const auto clock_now = std::chrono::steady_clock::now();
    if (cold_kdf_refilled_ == std::chrono::steady_clock::time_point{}) {
        cold_kdf_refilled_ = clock_now;
    }
    const auto refills = (clock_now - cold_kdf_refilled_) / kColdKdfRefill;
    if (refills > 0) {
        cold_kdf_budget_ = static_cast<int>(std::min<std::int64_t>(
            kColdKdfBurst, cold_kdf_budget_ + static_cast<std::int64_t>(refills)));
        cold_kdf_refilled_ += refills * kColdKdfRefill;
    }
    if (cold_kdf_budget_ <= 0) return nullptr;
    --cold_kdf_budget_;
    for (const auto& token : tokens_) {
        if (!token->uses_kdf() || token->kdf_verified()) continue;
        if (!token->validates(secret, now)) continue;
        auto user = find_user_by_id(token->user_id());
        if (user && user->active()) {
            remember(*token);
            return user;
        }
    }
    return nullptr;
}

// ----- user_roles (user_role:<uuid>) -----------------------------------------

void Node::load_user_roles() {
    user_roles_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "user_role.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr, false);
        if (meta.is_discarded() || !meta.is_object() ||
            !meta.contains("user") || !meta["user"].is_string() ||
            !meta.contains("role") || !meta["role"].is_string()) {
            continue;
        }
        user_roles_.push_back(std::make_shared<governance::UserRole>(
            entry.path().filename().string(), meta["user"].get<std::string>(),
            meta["role"].get<std::string>(), entry.path()));
    }
    std::sort(user_roles_.begin(), user_roles_.end(),
              [](const auto& a, const auto& b) { return a->id() < b->id(); });
}

std::shared_ptr<zetta::governance::UserRole> Node::assign_role(
    const std::string& user_id, const std::string& role_id) {
    if (!identity_writable()) return nullptr;
    if (!find_user_by_id(user_id) || !find_role_by_id(role_id)) return nullptr;
    // Idempotent: return the existing assignment if the pair is already assigned.
    for (const auto& assignment : user_roles_) {
        if (assignment->user_id() == user_id &&
            assignment->role_id() == role_id) {
            return assignment;
        }
    }
    const std::string id = zetta::util::new_uuid();
    if (replication_) {
        replicate_identity(catalog::user_role_key(id),
                          nlohmann::json{{"user", user_id}, {"role", role_id}});
    }
    return materialize_user_role(id, user_id, role_id);
}

bool Node::unassign_role(const std::string& user_id,
                         const std::string& role_id) {
    if (!identity_writable()) return false;
    std::string assignment_id;
    for (const auto& assignment : user_roles_) {
        if (assignment->user_id() == user_id &&
            assignment->role_id() == role_id) {
            assignment_id = assignment->id();
            break;
        }
    }
    if (assignment_id.empty()) return false;  // not assigned
    if (replication_) {
        remove_identity(catalog::user_role_key(assignment_id));
    }
    return materialize_drop_user_role(assignment_id);
}

std::vector<std::string> Node::roles_for_user(const std::string& user_id) const {
    std::vector<std::string> role_ids;
    for (const auto& assignment : user_roles_) {
        if (assignment->user_id() == user_id) {
            role_ids.push_back(assignment->role_id());
        }
    }
    return role_ids;
}

std::shared_ptr<zetta::governance::UserRole> Node::materialize_user_role(
    std::string id, std::string user_id, std::string role_id) {
    for (const auto& assignment : user_roles_) {
        if (assignment->id() == id) return assignment;
    }
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto assignment = std::make_shared<governance::UserRole>(
        std::move(id), std::move(user_id), std::move(role_id), dir);
    assignment->persist_meta();
    user_roles_.push_back(assignment);
    return assignment;
}

bool Node::materialize_drop_user_role(const std::string& id) {
    const auto it = std::find_if(
        user_roles_.begin(), user_roles_.end(),
        [&](const std::shared_ptr<governance::UserRole>& a) {
            return a->id() == id;
        });
    if (it == user_roles_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    user_roles_.erase(it);
    return true;
}

void Node::reconcile_user_roles(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Assignments are immutable (like access keys): reconcile only adds new ones
    // and drops the ones a peer removed.
    struct AssignmentEntry {
        std::string user_id;
        std::string role_id;
    };
    std::map<std::string, AssignmentEntry> desired;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kUserRolePrefix);
        if (id.empty() || !value.is_object() || !value.contains("user") ||
            !value.contains("role") || !value["user"].is_string() ||
            !value["role"].is_string()) {
            continue;
        }
        desired.emplace(std::move(id),
                        AssignmentEntry{value["user"].get<std::string>(),
                                        value["role"].get<std::string>()});
    }
    std::vector<std::string> local_ids;
    for (const auto& assignment : user_roles_) {
        local_ids.push_back(assignment->id());
    }
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::user_role_key(id)) &&
            !desired.contains(id)) {
            materialize_drop_user_role(id);
        }
    }
    for (auto& [id, entry] : desired) {
        bool present = false;
        for (const auto& assignment : user_roles_) {
            if (assignment->id() == id) {
                present = true;
                break;
            }
        }
        if (!present) materialize_user_role(id, entry.user_id, entry.role_id);
    }
}

// ----- row-level security policies (policy:<uuid>, #748) ---------------------

void Node::load_policies() {
    policies_.clear();
    const fs::path root = config_.data_dir;
    std::error_code ec;
    fs::create_directories(root, ec);
    for (const auto& entry : fs::directory_iterator(root, ec)) {
        if (!entry.is_directory()) continue;
        const fs::path meta_path = entry.path() / "policy.json";
        if (!fs::exists(meta_path)) continue;
        std::ifstream in(meta_path);
        const auto meta = nlohmann::json::parse(in, nullptr, false);
        const auto value = governance::policy_value_from_json(meta);
        if (!value) continue;  // boundary validation — skip a malformed policy
        policies_.push_back(std::make_shared<governance::Policy>(
            entry.path().filename().string(), *value, entry.path()));
    }
    std::sort(policies_.begin(), policies_.end(),
              [](const auto& a, const auto& b) { return a->name() < b->name(); });
}

std::shared_ptr<zetta::governance::Policy> Node::find_policy(
    const std::string& name) const {
    for (const auto& policy : policies_) {
        if (policy->name() == name) return policy;
    }
    return nullptr;
}

std::shared_ptr<zetta::governance::Policy> Node::find_policy_by_id(
    const std::string& id) const {
    for (const auto& policy : policies_) {
        if (policy->id() == id) return policy;
    }
    return nullptr;
}

std::shared_ptr<zetta::governance::Policy> Node::create_policy(
    governance::PolicyValue value) {
    if (!identity_writable()) return nullptr;
    if (value.name.empty() || find_policy(value.name)) return nullptr;  // empty/clash
    if (value.created.empty()) value.created = zetta::util::now_iso8601();
    const std::string id = zetta::util::new_uuid();
    if (replication_) {
        replicate_identity(catalog::policy_key(id),
                          governance::policy_value_to_json(value));
    }
    return materialize_policy(id, std::move(value));
}

bool Node::drop_policy(const std::string& name) {
    if (!identity_writable()) return false;
    auto policy = find_policy(name);
    if (!policy) return false;
    const std::string id = policy->id();
    if (replication_) {
        remove_identity(catalog::policy_key(id));
    }
    return materialize_drop_policy(id);
}

std::shared_ptr<zetta::governance::Policy> Node::materialize_policy(
    std::string id, governance::PolicyValue value) {
    if (auto existing = find_policy_by_id(id)) return existing;
    const fs::path dir = fs::path(config_.data_dir) / id;
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) return nullptr;
    auto policy = std::make_shared<governance::Policy>(std::move(id),
                                                       std::move(value), dir);
    policy->persist_meta();
    policies_.push_back(policy);
    return policy;
}

bool Node::materialize_drop_policy(const std::string& id) {
    const auto it = std::find_if(
        policies_.begin(), policies_.end(),
        [&](const std::shared_ptr<governance::Policy>& p) {
            return p->id() == id;
        });
    if (it == policies_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    policies_.erase(it);
    return true;
}

void Node::reconcile_policies(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    std::map<std::string, governance::PolicyValue> desired;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPolicyPrefix);
        if (id.empty()) continue;
        const auto parsed = governance::policy_value_from_json(value);
        if (!parsed) continue;  // boundary
        desired.emplace(std::move(id), *parsed);
    }
    std::vector<std::string> local_ids;
    for (const auto& policy : policies_) local_ids.push_back(policy->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::policy_key(id)) &&
            !desired.contains(id)) {
            materialize_drop_policy(id);
        }
    }
    for (auto& [id, value] : desired) {
        auto policy = find_policy_by_id(id);
        if (!policy) {
            materialize_policy(id, value);
        } else if (policy->value() != value) {
            policy->set_value(value);
        }
    }
}

// ----- virtual tables (vtable:<name>, #809 slice 3) -------------------------
//
// A virtual table is durable, replicated catalog metadata read live from the
// CRDT document — there is no in-memory projection to keep in sync (the document
// IS the source of truth, exactly like system.nodes reads the node-list). So
// these go straight through the ReplicationManager: create/drop capture the
// `vtable:<name>` key; find/list/rows read the current snapshot. Durability +
// convergence + restart-survival come free from the CRDT (persist/load/gossip).
// A standalone node has no CRDT to store in or project over, so create/drop
// return false and find/list/rows are empty there.

bool Node::create_virtual_table(const zetta::data::VirtualTable& definition) {
    if (!replication_) return false;         // no CRDT to store/project over
    if (definition.name.empty()) return false;
    const nlohmann::json snapshot = replication_->snapshot();
    if (snapshot.contains(catalog::virtual_table_key(definition.name))) {
        return false;                        // name clash
    }
    replication_->set(catalog::virtual_table_key(definition.name),
                      zetta::data::virtual_table_to_json(definition));
    return true;
}

bool Node::drop_virtual_table(const std::string& name) {
    if (!replication_) return false;
    const nlohmann::json snapshot = replication_->snapshot();
    if (!snapshot.contains(catalog::virtual_table_key(name))) return false;
    replication_->remove(catalog::virtual_table_key(name));
    return true;
}

std::optional<zetta::data::VirtualTable> Node::find_virtual_table(
    const std::string& name) const {
    if (!replication_) return std::nullopt;
    const nlohmann::json snapshot = replication_->snapshot();
    const auto it = snapshot.find(catalog::virtual_table_key(name));
    if (it == snapshot.end()) return std::nullopt;
    return zetta::data::virtual_table_from_json(*it);
}

std::vector<zetta::data::VirtualTable> Node::virtual_tables() const {
    std::vector<zetta::data::VirtualTable> tables;
    if (!replication_) return tables;
    const nlohmann::json snapshot = replication_->snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (catalog::id_after(key, catalog::kVirtualTablePrefix).empty()) continue;
        if (auto table = zetta::data::virtual_table_from_json(value)) {
            tables.push_back(std::move(*table));
        }
    }
    std::sort(tables.begin(), tables.end(),
              [](const auto& a, const auto& b) { return a.name < b.name; });
    return tables;
}

std::vector<nlohmann::json> Node::virtual_table_rows(
    const zetta::data::VirtualTable& definition) const {
    std::vector<nlohmann::json> rows;
    if (!replication_) return rows;
    const nlohmann::json snapshot = replication_->snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        // A row per CRDT entry under the definition's prefix. `id_after` yields
        // the id (the part after the prefix), empty when the key is not under it.
        const std::string id = catalog::id_after(key, definition.crdt_prefix);
        if (id.empty()) continue;
        if (!value.is_object()) continue;  // a durable tier is a JSON entry
        nlohmann::json row = nlohmann::json::object();
        for (const zetta::data::VirtualColumn& column : definition.columns) {
            if (column.is_key) {
                row[column.name] = id;  // the CRDT key (the id after the prefix)
            } else if (column.tier == zetta::data::VirtualColumnTier::Durable) {
                row[column.name] = value.contains(column.name)
                                       ? value.at(column.name)
                                       : nlohmann::json();
            } else {
                // Ephemeral tiers are null until a resolver is bound (follow-up).
                row[column.name] = nlohmann::json();
            }
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

const zetta::connect::CredentialKeyring* Node::connection_keyring(
    std::string* reason) const {
    if (!connection_keyring_ && connection_keyring_error_.empty()) {
        std::string why;
        std::optional<zetta::connect::CredentialKeyring> keyring =
            config_.connection_root_secret.empty()
                ? zetta::connect::load_or_create_local_keyring(config_.data_dir, &why)
                : zetta::connect::CredentialKeyring::from_root_secrets(
                      config_.connection_root_secret,
                      config_.retired_connection_root_secrets, &why);
        if (keyring) {
            connection_keyring_ = std::move(*keyring);
        } else {
            connection_keyring_error_ =
                why.empty() ? std::string("no sealing key") : why;
        }
    }
    if (!connection_keyring_) {
        if (reason) *reason = connection_keyring_error_;
        return nullptr;
    }
    return &*connection_keyring_;
}

bool Node::bypasses_row_security(const std::string& acting_id,
                                 bool acting_is_user) const {
    if (acting_id.empty()) return true;   // trusted unauthenticated-local session
    const governance::Operation admin_marker{governance::CapabilityDomain::Api,
                                             "cluster_admin"};
    // A legacy AccessKey bypasses only when it holds cluster:admin itself (#762):
    // otherwise row security binds it like any non-admin (it has no user name or
    // roles, so only a policy that names no roles can admit a row).
    if (!acting_is_user) return static_cast<bool>(authorize(acting_id, admin_marker));
    auto user = find_user_by_id(acting_id);
    if (!user || !user->active()) return false;  // unknown/disabled — no bypass
    // Admin bypasses RLS: only the built-in admin role grants cluster:admin
    // (api:cluster_admin), so it distinguishes an admin from writer/reader/custom
    // users. Deny-by-default otherwise — an ordinary user is subject to policy.
    const std::string now = zetta::util::now_iso8601();
    for (const std::string& role_id : roles_for_user(acting_id)) {
        auto role = find_role_by_id(role_id);
        if (role && role->capabilities().permits(admin_marker, now)) return true;
    }
    return false;
}

// ----- the identity root (#763, #764, docs/governance/identity-seal.md) -------

bool Node::identity_writable() const {
    return !replication_ || can_issue_identity();
}

namespace {

// The record the document holds for `key` now, if any (a copy, so the caller
// may write the document afterwards).
std::optional<nlohmann::json> current_identity_value(
    const zetta::cluster::ReplicationManager& replication, const std::string& key) {
    const auto view = replication.shared_snapshot();
    if (!view) return std::nullopt;
    const auto found = view->find(key);
    if (found == view->end()) return std::nullopt;
    return std::optional<nlohmann::json>(std::in_place, *found);
}

}  // namespace

void Node::replicate_identity(const std::string& key, nlohmann::json value) {
    if (!replication_) return;
    if (const governance::IdentityAuthority* root = identity_authority()) {
        // Newer than the record the document holds and than the one this node
        // holds, so every node accepts it over both (#1341).
        const std::optional<nlohmann::json> current =
            current_identity_value(*replication_, key);
        const auto held = identity_held_.find(key);
        const std::uint64_t version = governance::next_identity_version(
            *root, key, current ? &*current : nullptr,
            held == identity_held_.end() ? 0 : held->second.opened.version);
        std::optional<nlohmann::json> sealed = root->seal(key, std::move(value), version);
        std::optional<governance::OpenedIdentityRecord> opened;
        if (sealed) opened = root->open(key, *sealed);
        if (!sealed || !opened) {
            // Every caller checks identity_writable() first, so this is a value
            // that is not a JSON object: write nothing a peer would refuse.
            std::fprintf(stderr, "zetta: identity record %s not sealed — not replicated\n",
                         key.c_str());
            return;
        }
        replication_->set(key, *sealed);
        hold_identity(key, *sealed, *opened);
        return;
    }
    replication_->set(key, value);
}

void Node::remove_identity(const std::string& key) {
    if (!replication_) return;
    const governance::IdentityAuthority* root = identity_authority();
    if (!root) {
        replication_->remove(key);  // unsealed mode: the CRDT's own tombstone
        return;
    }
    // A removal is a sealed record of its own, newer than the one it removes
    // (#1341). The CRDT's tombstone carries no seal, so any member could write
    // one; no node honors it.
    const std::optional<nlohmann::json> current = current_identity_value(*replication_, key);
    const auto held = identity_held_.find(key);
    const std::uint64_t version = governance::next_identity_version(
        *root, key, current ? &*current : nullptr,
        held == identity_held_.end() ? 0 : held->second.opened.version);
    std::optional<nlohmann::json> tombstone = root->seal_removal(key, version);
    std::optional<governance::OpenedIdentityRecord> opened;
    if (tombstone) opened = root->open(key, *tombstone);
    if (!tombstone || !opened) {
        std::fprintf(stderr, "zetta: removal of %s not sealed — not replicated\n",
                     key.c_str());
        return;
    }
    replication_->set(key, *tombstone);
    hold_identity(key, *tombstone, *opened);
}

void Node::hold_identity(const std::string& key, const nlohmann::json& sealed,
                         const governance::OpenedIdentityRecord& opened) {
    const auto held = identity_held_.find(key);
    if (held != identity_held_.end() &&
        !governance::identity_record_supersedes(opened, held->second.opened)) {
        return;
    }
    identity_held_[key] = HeldIdentity{sealed, opened};
    identity_held_dirty_ = true;
}

nlohmann::json Node::trusted_identity(const nlohmann::json& values,
                                      const std::vector<std::string>* keys) {
    nlohmann::json trusted = nlohmann::json::object();
    const governance::IdentityAuthority* root = identity_authority();
    if (!root) {
        for (const auto& [key, value] : values.items()) {
            if (governance::is_identity_key(key)) trusted[key] = value;  // unsealed mode
        }
        return trusted;
    }
    // Every key to decide: the identity keys in `values`, and the keys that
    // changed (one that left `values` was removed) or, on a full reconcile,
    // every key this node holds (one missing from the document was removed).
    std::set<std::string> candidates;
    for (const auto& [key, value] : values.items()) {
        if (governance::is_identity_key(key)) candidates.insert(key);
    }
    if (keys) {
        for (const std::string& key : *keys) {
            if (governance::is_identity_key(key)) candidates.insert(key);
        }
    } else {
        for (const auto& [key, held] : identity_held_) candidates.insert(key);
    }
    std::vector<std::string> write_back;
    for (const std::string& key : candidates) {
        const auto found = values.find(key);
        std::optional<governance::OpenedIdentityRecord> opened;
        if (found != values.end()) opened = root->open(key, *found);
        auto held = identity_held_.find(key);
        if (opened && (held == identity_held_.end() ||
                       !governance::identity_record_supersedes(held->second.opened,
                                                               *opened))) {
            // The document's record opens and is not older than the one held.
            hold_identity(key, *found, *opened);
            held = identity_held_.find(key);
        } else if (held != identity_held_.end()) {
            // The document holds an older sealed record (a replay), a record that
            // does not open (unsealed, another root's, changed), or nothing (an
            // unsealed removal). Keep the newer record, and put it back so a node
            // that never saw it gets it too (#1341). Only an identical sealed
            // value is ever written, so this settles in one round.
            write_back.push_back(key);
        } else {
            continue;  // nothing trusted for this key: absent
        }
        if (!held->second.opened.removed) trusted[key] = held->second.opened.value;
    }
    for (const std::string& key : write_back) {
        replication_->set(key, identity_held_.at(key).sealed);
    }
    if (identity_held_dirty_) persist_identity_held();
    return trusted;
}

namespace {

constexpr int kIdentityHeldFormat = 1;

fs::path identity_held_path(const std::string& data_dir) {
    return fs::path(data_dir) / "security" / "identity-held.json";
}

}  // namespace

bool Node::load_identity_held() {
    identity_held_.clear();
    identity_held_dirty_ = false;
    const governance::IdentityAuthority* root = identity_authority();
    if (!replication_ || !root) return true;
    const fs::path path = identity_held_path(config_.data_dir);
    std::error_code ec;
    if (!fs::exists(path, ec)) return !ec;
    std::ifstream in(path, std::ios::binary);
    const nlohmann::json document = nlohmann::json::parse(in, nullptr, false);
    if (document.is_discarded() || !document.is_object() ||
        document.value("version", 0) != kIdentityHeldFormat ||
        !document.contains("records") || !document["records"].is_object()) {
        std::fprintf(stderr,
                     "zetta: %s is malformed; refusing to start without the identity "
                     "records this node held (docs/governance/identity-seal.md)\n",
                     path.c_str());
        return false;
    }
    // Each record is verified again: the file is only as trusted as its seals.
    std::size_t dropped = 0;
    for (const auto& [key, sealed] : document["records"].items()) {
        if (!governance::is_identity_key(key)) {
            ++dropped;
            continue;
        }
        if (auto opened = root->open(key, sealed)) {
            identity_held_[key] = HeldIdentity{sealed, std::move(*opened)};
        } else {
            ++dropped;
        }
    }
    if (dropped > 0) {
        std::fprintf(stderr,
                     "zetta: %zu record(s) in %s do not open under this node's identity "
                     "root and were ignored\n",
                     dropped, path.c_str());
        identity_held_dirty_ = true;
    }
    return true;
}

void Node::persist_identity_held() {
    identity_held_dirty_ = false;
    if (!replication_ || !identity_authority()) return;
    nlohmann::json records = nlohmann::json::object();
    for (const auto& [key, held] : identity_held_) records[key] = held.sealed;
    const nlohmann::json document{{"version", kIdentityHeldFormat},
                                  {"records", std::move(records)}};
    const fs::path path = identity_held_path(config_.data_dir);
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << document.dump() << "\n";
        if (!out) {
            std::fprintf(stderr, "zetta: cannot write %s\n", tmp.c_str());
            identity_held_dirty_ = true;  // try again on the next change
            return;
        }
    }
    fs::rename(tmp, path, ec);  // atomic replace
    if (ec) {
        std::fprintf(stderr, "zetta: cannot write %s: %s\n", path.c_str(),
                     ec.message().c_str());
        identity_held_dirty_ = true;
    }
}

void Node::seal_preexisting_identity() {
    const governance::IdentityAuthority* root = identity_authority();
    if (!replication_ || !root) return;
    const fs::path security = fs::path(config_.data_dir) / "security";
    const fs::path marker = security / "identity-sealed";
    std::error_code ec;
    if (fs::exists(marker, ec)) return;  // done once; never bless a later arrival
    std::size_t sealed = 0;
    if (root->can_seal()) {
        // Only this node's own persisted document is here: start() runs before
        // the cluster connects, so nothing a peer sends now is blessed.
        const nlohmann::json snapshot = replication_->snapshot();
        for (const auto& [key, value] : snapshot.items()) {
            if (!governance::is_identity_key(key) || !value.is_object()) continue;
            // A record already carrying a seal is left alone: this root's opens
            // as it is, and another root's is not this node's to re-issue.
            if (value.contains(std::string(governance::kIdentitySealField))) continue;
            replicate_identity(key, value);
            ++sealed;
        }
    }
    fs::create_directories(security, ec);
    std::ofstream out(marker, std::ios::trunc);
    out << "identity root " << root->public_key_hex() << "\n";
    if (sealed > 0) {
        std::printf("zetta: sealed %zu identity record(s) this node held before "
                    "the identity root (#764)\n",
                    sealed);
    }
}

std::string Node::seeded_identity_id(std::string_view purpose) const {
    if (const governance::IdentityAuthority* root = identity_authority()) {
        return root->derived_id(purpose);
    }
    return zetta::util::new_uuid();
}

// ----- bootstrap owner (#733, #763) -------------------------------------------

Node::BootstrapResult Node::bootstrap_owner(
    const std::string& admin_token_secret) {
    BootstrapResult result;
    if (admin_token_secret.empty()) {
        result.reason = "no admin token secret supplied";
        return result;  // nothing to seed the owner's credential with
    }
    if (!identity_writable()) {
        // A verify-only node is a member, not an authority: it seeds no owner,
        // whatever --admin-token it was started with (#763).
        result.reason = "this node does not hold the cluster's identity root";
        return result;
    }
    // A seed cut short (#769): the only user is "owner" and it holds no token.
    // Resume it instead of skipping, or the node would have no usable owner.
    std::shared_ptr<governance::User> resumed;
    if (!users_.empty()) {
        if (users_.size() == 1 && users_.front()->name() == "owner") {
            const std::string owner_id = users_.front()->id();
            const bool has_token = std::any_of(
                tokens_.begin(), tokens_.end(),
                [&](const auto& token) { return token->user_id() == owner_id; });
            if (!has_token) resumed = users_.front();
        }
        if (!resumed) {
            // Already seeded: never re-mint. Check the operator's secret once
            // so the owner's token is verified and its later checks are cheap.
            (void)authenticate_token(admin_token_secret);
            result.reason = "user store is not empty";
            return result;
        }
    }
    // Derive the token hash first, so nothing is written when it fails. The
    // PBKDF2 form (#769): the operator chose this secret, so it may not carry
    // the entropy of a generated one, and a stolen store must not be guessed at
    // SHA-256 speed. create_token generates its own secret, which we don't want
    // for the seeded owner credential, so the token is written here.
    const std::string salt = governance::generate_token_salt();
    const std::string hash =
        salt.empty() ? std::string{}
                     : governance::kdf_hash_token_secret(salt, admin_token_secret);
    if (hash.empty()) {
        result.reason = "failed to hash the admin token secret";
        return result;
    }
    // Mint (or reuse) the owner user, ensure the built-in roles exist, grant it
    // admin, and issue its access token from the operator's secret. Under a root
    // each id is fixed by the root, so every node that seeds the same root writes
    // the same records and the cluster holds one owner (#763). Any failure after
    // a user was created here drops that user again, leaving the store empty so
    // the next start retries the whole seed.
    const std::string now = zetta::util::now_iso8601();
    std::shared_ptr<governance::User> owner = resumed;
    if (!owner) {
        const std::string owner_id = seeded_identity_id("owner-user");
        replicate_identity(catalog::user_key(owner_id),
                           nlohmann::json{{"name", "owner"},
                                          {"status", to_string(
                                               governance::UserStatus::Active)},
                                          {"created", now}});
        owner = materialize_user(owner_id, "owner",
                                 governance::UserStatus::Active, now);
    }
    if (!owner) {
        result.reason = "failed to create owner user";
        return result;
    }
    const auto roll_back = [&](std::string reason) {
        if (!resumed) drop_user(owner->name());
        result.reason = std::move(reason);
        return result;
    };
    ensure_builtin_roles();
    auto admin = find_role("admin");
    if (!admin) {
        return roll_back("failed to assign the admin role to the owner");
    }
    const std::string assignment_id = seeded_identity_id("owner-admin-assignment");
    replicate_identity(catalog::user_role_key(assignment_id),
                       nlohmann::json{{"user", owner->id()}, {"role", admin->id()}});
    if (!materialize_user_role(assignment_id, owner->id(), admin->id())) {
        return roll_back("failed to assign the admin role to the owner");
    }
    governance::TokenValue value;
    value.user_id = owner->id();
    value.type = governance::TokenType::Access;
    value.hash = hash;
    value.salt = salt;
    value.created = now;
    value.revoked = false;
    const std::string token_id = seeded_identity_id("owner-token");
    replicate_identity(catalog::token_key(token_id),
                       nlohmann::json{{"user", value.user_id},
                                      {"type", to_string(value.type)},
                                      {"hash", value.hash},
                                      {"salt", value.salt},
                                      {"created", value.created},
                                      {"revoked", value.revoked}});
    auto token = materialize_token(token_id, std::move(value));
    if (!token) {
        return roll_back("failed to persist the owner token");
    }
    // Verify once now, while the secret is in hand, so the owner's requests take
    // the cheap path from the start.
    (void)token->validates(admin_token_secret, now);
    result.performed = true;
    result.user_id = owner->id();
    result.user_name = owner->name();
    result.token_id = token->id();
    return result;
}

bool Node::drop_run(const std::string& id) {
    const std::shared_ptr<zetta::pipeline::Run> run = find_run(id);
    if (!run) return false;
    if (record_store_) {
        zetta::data::drop_entity_key_table(*record_store_, run->entity_key_mapping(),
                                           zetta::data::entity_key_owner("run", id));
    }
    if (replication_) replication_->remove(catalog::run_key(id));
    return materialize_drop_run(id);
}

bool Node::materialize_drop_run(const std::string& id) {
    const auto it = std::find_if(
        runs_.begin(), runs_.end(),
        [&](const std::shared_ptr<zetta::pipeline::Run>& r) { return r->id() == id; });
    if (it == runs_.end()) return false;
    std::error_code ec;
    fs::remove_all((*it)->dir(), ec);
    runs_.erase(it);
    return true;
}

zetta::governance::AccessDecision Node::authorize(const std::string& key_id,
                               const zetta::governance::Resource& resource) {
    auto key = find_access_key(key_id);
    const zetta::governance::AccessDecision decision =
        key ? zetta::governance::authorize(*key, resource)
            : zetta::governance::AccessDecision{false, "unknown access key — denied"};
    // Audit the data access (FEAT-9): every authorization decision — allow or
    // deny — is an immutable audit event. The target is the resource the request
    // tried to reach; the detail is the decision's human-readable reason.
    record_audit_event(
        {zetta::governance::AuditAction::DataAccess, key_id,
         std::string(to_string(resource.kind)) + ":" + resource.name,
         decision.allowed, decision.reason, {}});
    return decision;
}

// ----- audit trail (immutable records; replicated through segment ranges) ----

fs::path Node::audit_dir() const {
    return fs::path(config_.data_dir) / "audit";
}

bool Node::load_audit_events() {
    audit_trail_ = std::make_unique<zetta::governance::AuditTrail>(
        audit_dir(), config_.audit_recent_events);
    std::string error;
    if (!audit_trail_->open(&error)) {
        std::printf("zetta: audit store could not open (%s)\n", error.c_str());
        return false;
    }
    return true;
}

void Node::record_audit_event(zetta::governance::AuditEventValue value) {
    // The sink owns identity + time: stamp a fresh id and the current time so the
    // immutable record contract holds regardless of what the caller passed.
    const std::string id = zetta::util::new_uuid();
    value.time = zetta::util::now_iso8601();
    // An event recorded without an actor inside a call names the caller (#847):
    // the entry point declared who is acting (AuditActorScope), so a deploy, a
    // queued job or a created project is attributed instead of left blank.
    if (value.actor.empty()) {
        if (auto actor = zetta::governance::current_audit_actor()) {
            value.actor = std::move(*actor);
        }
    }
    std::string error;
    if (!audit_trail_ || !audit_trail_->append_local(node_id_, id, std::move(value), &error)) {
        std::fprintf(stderr, "zetta: audit append failed (%s)\n", error.c_str());
    }
}

std::optional<std::size_t> Node::purge_audit_before(
    const std::string& before, std::string actor, std::string* error) {
    const auto cutoff = zetta::util::parse_iso8601_seconds(before);
    const auto now = zetta::util::parse_iso8601_seconds(
        zetta::util::now_iso8601());
    if (!cutoff) {
        if (error) {
            *error = "before must be a valid UTC ISO-8601 timestamp";
        }
        return std::nullopt;
    }
    if (!now || *cutoff > *now) {
        if (error) {
            *error = "before must not be in the future";
        }
        return std::nullopt;
    }
    if (!audit_trail_) {
        if (error) {
            *error = "audit trail is unavailable";
        }
        return std::nullopt;
    }

    std::size_t removed = 0;
    if (!audit_trail_->retain_since(
            before, before,
            [&removed](const std::string&) {
                ++removed;
            },
            error)) {
        return std::nullopt;
    }

    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::AuditPurged;
    event.actor = actor.empty() ? "local" : std::move(actor);
    event.target = "audit";
    event.allowed = true;
    event.detail = "purged audit records before " + before + "; " +
                   std::to_string(removed) + " rows removed";
    event.time = zetta::util::now_iso8601();
    const std::string id = zetta::util::new_uuid();
    if (!audit_trail_->append_local(
            node_id_, id, std::move(event), error)) {
        return std::nullopt;
    }
    return removed;
}

std::vector<std::shared_ptr<zetta::governance::AuditEvent>>
Node::audit_events() const {
    return audit_trail_ ? audit_trail_->events()
                        : std::vector<std::shared_ptr<zetta::governance::AuditEvent>>{};
}

std::vector<std::shared_ptr<zetta::governance::AuditEvent>>
Node::recent_audit_events() const {
    return audit_trail_ ? audit_trail_->recent_events()
                        : std::vector<std::shared_ptr<zetta::governance::AuditEvent>>{};
}

std::size_t Node::audit_event_count() const {
    return audit_trail_ ? audit_trail_->size() : 0;
}

nlohmann::json Node::audit_watermarks() const {
    nlohmann::json result = nlohmann::json::object();
    if (audit_trail_) {
        for (const auto& [origin, watermark] : audit_trail_->watermarks()) {
            result[origin] = {{"high", watermark.high},
                              {"low", watermark.low},
                              {"summary_low", watermark.summary_low}};
        }
    }
    return result;
}

std::size_t Node::audit_memory_bytes() const {
    return audit_memory_usage().bytes;
}

void Node::sync_audit_log() {
    if (!cluster_ || !audit_trail_) {
        return;
    }
    const auto request_range = [&](const std::string& peer, const std::string& origin,
                                    std::uint64_t after) {
        const auto now = Clock::now();
        const auto pending = audit_requests_.find(origin);
        if (pending != audit_requests_.end() && now - pending->second < std::chrono::seconds(2)) {
            return;
        }
        zetta::cluster::AuditMessage request;
        request.kind = zetta::cluster::AuditMessage::Kind::Request;
        request.peer = peer;
        request.batch.origin = origin;
        request.batch.after = after;
        if (cluster_->send_audit(request)) {
            audit_requests_[origin] = now;
        }
    };
    bool caught_up = false;
    for (auto& message : cluster_->take_audit_messages()) {
        using Kind = zetta::cluster::AuditMessage::Kind;
        std::string error;
        if (message.kind == Kind::Status) {
            const auto local = audit_trail_->watermarks();
            for (const auto& [origin, watermark] : message.watermarks) {
                const auto found = local.find(origin);
                const auto high = found == local.end() ? 0 : found->second.high;
                if (high < watermark.high ||
                    (found != local.end() &&
                     (found->second.low < watermark.low ||
                      found->second.summary_low < watermark.summary_low))) {
                    request_range(message.peer, origin, high);
                }
            }
        } else if (message.kind == Kind::Request) {
            auto batch = audit_trail_->read_range(message.batch.origin, message.batch.after, &error);
            if (batch) {
                message.kind = Kind::Response;
                message.batch = std::move(*batch);
                cluster_->send_audit(message);
            }
        } else {
            audit_requests_.erase(message.batch.origin);
            if (audit_trail_->receive(message.batch, &error)) {
                audit_received_since_relief_ += message.batch.frames.size();
                if (message.batch.next < message.batch.watermark.high) {
                    request_range(message.peer, message.batch.origin, message.batch.next);
                } else {
                    caught_up = true;
                }
            }
        }
        if (!error.empty()) {
            std::fprintf(stderr, "zetta: audit exchange with %s failed: %s\n", message.peer.c_str(), error.c_str());
        }
    }
    // The message vector and decoded pages have been destroyed before asking
    // the allocator to return unused pages. Small incremental writes do not
    // trigger this process-wide operation.
    if (caught_up && audit_requests_.empty() && audit_received_since_relief_ >= 8 * 1024 * 1024) {
        zetta::governance::AuditTrail::release_unused_memory();
        audit_received_since_relief_ = 0;
    }
    cluster_->publish_audit_watermarks(audit_trail_->watermarks());
}

void Node::record_authorized_read(std::string actor, std::string capability,
                                  std::string scope,
                                  std::uint64_t events_delivered,
                                  bool returned_data) {
    if (actor.empty()) {
        actor = "local";
    }
    const std::string key = actor + '\x1f' + capability + '\x1f' + scope;
    const Clock::time_point now = Clock::now();
    auto found = audit_summary_windows_.find(key);
    if (found != audit_summary_windows_.end() &&
        config_.audit_summary_window.count() > 0 &&
        now - found->second.opened_at >= config_.audit_summary_window) {
        flush_audit_summary(key);
        found = audit_summary_windows_.find(key);
    }
    if (found == audit_summary_windows_.end()) {
        zetta::governance::AuditEventValue started;
        started.action = zetta::governance::AuditAction::DataAccess;
        started.actor = actor;
        started.target = "api:" + capability;
        started.allowed = true;
        started.detail = "authorised read session started for " + scope;
        record_audit_event(std::move(started));

        AuditSummaryWindow window;
        window.actor = std::move(actor);
        window.capability = std::move(capability);
        window.scope = std::move(scope);
        window.started = zetta::util::now_iso8601();
        window.opened_at = now;
        found = audit_summary_windows_.emplace(key, std::move(window)).first;
    }
    ++found->second.calls;
    found->second.events_delivered += events_delivered;
    found->second.returned_data = found->second.returned_data || returned_data;
}

void Node::flush_audit_summary(const std::string& key) {
    const auto found = audit_summary_windows_.find(key);
    if (found == audit_summary_windows_.end() || found->second.calls == 0) {
        return;
    }
    AuditSummaryWindow& window = found->second;
    const std::string ended = zetta::util::now_iso8601();
    if (window.events_delivered > 0 || window.returned_data) {
        zetta::governance::AuditEventValue summary;
        summary.action = zetta::governance::AuditAction::DataAccessSummary;
        summary.actor = window.actor;
        summary.target = "api:" + window.capability;
        summary.allowed = true;
        summary.detail = std::to_string(window.calls) +
                         " authorised reads; " +
                         std::to_string(window.events_delivered) +
                         " events delivered";
        summary.summary = zetta::governance::AuditReadSummary{
            window.calls, window.events_delivered, window.scope,
            window.started, ended};
        record_audit_event(std::move(summary));
    }
    window.calls = 0;
    window.events_delivered = 0;
    window.returned_data = false;
    window.started = ended;
    window.opened_at = Clock::now();
}

void Node::flush_due_audit_summaries() {
    if (config_.audit_summary_window.count() <= 0) {
        return;
    }
    const Clock::time_point now = Clock::now();
    std::vector<std::string> due;
    for (const auto& [key, window] : audit_summary_windows_) {
        if (window.calls > 0 &&
            now - window.opened_at >= config_.audit_summary_window) {
            due.push_back(key);
        }
    }
    for (const std::string& key : due) {
        flush_audit_summary(key);
    }
}

void Node::flush_audit_summaries() {
    std::vector<std::string> keys;
    keys.reserve(audit_summary_windows_.size());
    for (const auto& [key, window] : audit_summary_windows_) {
        keys.push_back(key);
    }
    for (const std::string& key : keys) {
        flush_audit_summary(key);
    }
    audit_summary_windows_.clear();
}

void Node::apply_audit_retention() {
    last_audit_retention_ = Clock::now();
    if (!audit_trail_ ||
        (config_.audit_retention.count() <= 0 &&
         config_.audit_summary_retention.count() <= 0)) {
        return;
    }
    const auto cutoff_for = [](std::chrono::hours retention) {
        if (retention.count() <= 0) {
            return std::string{};
        }
        const long long seconds =
            std::chrono::duration_cast<std::chrono::seconds>(retention).count();
        return zetta::util::iso8601_seconds_ago(seconds);
    };
    const std::string cutoff = cutoff_for(config_.audit_retention);
    const std::string summary_cutoff =
        cutoff_for(config_.audit_summary_retention);
    std::string error;
    const bool retained = audit_trail_->retain_since(
        cutoff, summary_cutoff,
        [](const std::string&) {},
        &error);
    if (!retained) {
        std::printf("zetta: audit retention failed (%s)\n", error.c_str());
    }
}

// ----- solutions reward ledger (immutable, replicated as ledger:<uuid>) -----

fs::path Node::ledger_dir() const {
    return fs::path(config_.data_dir) / "ledger";
}

void Node::load_ledger_entries() {
    ledger_entries_.clear();
    const fs::path root = ledger_dir();
    std::error_code error_code;
    if (!fs::exists(root, error_code)) {
        return;
    }
    for (const auto& entry : fs::directory_iterator(root, error_code)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".json") {
            continue;
        }
        std::ifstream input(entry.path());
        const nlohmann::json document = nlohmann::json::parse(
            input, nullptr, /*allow_exceptions=*/false);
        std::string error;
        const std::optional<zetta::solutions::LedgerEntryValue> value =
            document.is_discarded()
                ? std::nullopt
                : zetta::solutions::parse_audit_ledger_entry(document, &error);
        if (!value) {
            std::printf("zetta: skipping invalid ledger entry %s (%s)\n",
                        entry.path().filename().string().c_str(),
                        document.is_discarded() ? "unparseable JSON"
                                                : error.c_str());
            continue;
        }
        ledger_entries_.push_back(
            std::make_shared<zetta::solutions::LedgerEntry>(
                entry.path().stem().string(), *value, root));
    }
    std::sort(ledger_entries_.begin(), ledger_entries_.end(),
              [](const auto& left, const auto& right) {
                  if (left->recorded_at() != right->recorded_at()) {
                      return left->recorded_at() < right->recorded_at();
                  }
                  return left->id() < right->id();
              });
}

std::shared_ptr<zetta::solutions::LedgerEntry> Node::record_action(
    zetta::solutions::ActionLedgerValue value) {
    const std::string id = zetta::util::new_uuid();
    value.recorded_at = zetta::util::now_iso8601();
    zetta::solutions::LedgerEntryValue entry_value = std::move(value);
    if (!zetta::solutions::validate_audit_ledger_entry(
            zetta::solutions::to_json(entry_value))) {
        return nullptr;
    }
    if (replication_ != nullptr) {
        replication_->set(catalog::ledger_key(id),
                          zetta::solutions::to_json(entry_value));
    }
    return materialize_ledger_entry(id, std::move(entry_value));
}

std::shared_ptr<zetta::solutions::LedgerEntry> Node::record_outcome(
    zetta::solutions::OutcomeLedgerValue value) {
    const std::shared_ptr<zetta::solutions::LedgerEntry> action =
        find_ledger_entry(value.action_id);
    if (!action || action->kind() != zetta::solutions::LedgerEntryKind::Action) {
        return nullptr;
    }
    const std::string id = zetta::util::new_uuid();
    value.recorded_at = zetta::util::now_iso8601();
    zetta::solutions::LedgerEntryValue entry_value = std::move(value);
    if (!zetta::solutions::validate_audit_ledger_entry(
            zetta::solutions::to_json(entry_value))) {
        return nullptr;
    }
    if (replication_ != nullptr) {
        replication_->set(catalog::ledger_key(id),
                          zetta::solutions::to_json(entry_value));
    }
    return materialize_ledger_entry(id, std::move(entry_value));
}

std::shared_ptr<zetta::solutions::LedgerEntry> Node::find_ledger_entry(
    const std::string& id) const {
    for (const auto& entry : ledger_entries_) {
        if (entry->id() == id) {
            return entry;
        }
    }
    return nullptr;
}

std::shared_ptr<zetta::solutions::LedgerEntry> Node::materialize_ledger_entry(
    std::string id, zetta::solutions::LedgerEntryValue value) {
    if (const auto existing = find_ledger_entry(id)) {
        return existing;
    }
    const fs::path root = ledger_dir();
    std::error_code error_code;
    fs::create_directories(root, error_code);
    if (error_code) {
        return nullptr;
    }
    auto entry = std::make_shared<zetta::solutions::LedgerEntry>(
        std::move(id), std::move(value), root);
    entry->persist_meta();
    ledger_entries_.push_back(entry);
    std::sort(ledger_entries_.begin(), ledger_entries_.end(),
              [](const auto& left, const auto& right) {
                  if (left->recorded_at() != right->recorded_at()) {
                      return left->recorded_at() < right->recorded_at();
                  }
                  return left->id() < right->id();
              });
    return entry;
}

zetta::governance::AccessDecision Node::authorize(const std::string& key_id,
                               const zetta::governance::Operation& operation) const {
    auto key = find_access_key(key_id);
    if (!key) {
        return zetta::governance::AccessDecision{false, "unknown access key — denied"};
    }
    return zetta::governance::authorize(*key, operation, zetta::util::now_iso8601());
}

zetta::governance::AccessDecision Node::authorize_user(
    const std::string& user_id,
    const zetta::governance::Operation& operation) const {
    // Deny-by-default: an unknown or disabled user has no reach at all.
    auto user = find_user_by_id(user_id);
    if (!user) {
        return zetta::governance::AccessDecision{false, "unknown user — denied"};
    }
    if (!user->active()) {
        return zetta::governance::AccessDecision{false, "user is disabled — denied"};
    }
    // Effective permissions = the union of the user's roles' capability sets.
    // Allowed as soon as any assigned role grants a matching, unexpired right;
    // TTL is honored against the current time (same contract as an AccessKey).
    const std::string now = zetta::util::now_iso8601();
    for (const std::string& role_id : roles_for_user(user_id)) {
        auto role = find_role_by_id(role_id);
        if (role && role->capabilities().permits(operation, now)) {
            return zetta::governance::AccessDecision{
                true, "granted by role '" + role->name() + "'"};
        }
    }
    return zetta::governance::AccessDecision{
        false, "no role grants " + std::string(to_string(operation.domain)) +
                   ":" + operation.action};
}

namespace {

// Does any role assigned to `user_id` hold `operation` as a FLAT, unscoped
// capability (unexpired as of `now`)? A true here means the identity reaches the
// operation on EVERY object — the coarse behavior, and the object-scoping bypass.
bool user_holds_global(const Node& node, const std::string& user_id,
                       const zetta::governance::Operation& operation,
                       const std::string& now) {
    for (const std::string& role_id : node.roles_for_user(user_id)) {
        auto role = node.find_role_by_id(role_id);
        if (role && role->capabilities().permits(operation, now)) return true;
    }
    return false;
}

// The union of every OBJECT-SCOPED grant across a user's roles (#747).
std::vector<zetta::governance::ScopedGrant> user_scoped_grants(
    const Node& node, const std::string& user_id) {
    std::vector<zetta::governance::ScopedGrant> grants;
    for (const std::string& role_id : node.roles_for_user(user_id)) {
        auto role = node.find_role_by_id(role_id);
        if (!role) continue;
        for (const auto& grant : role->permissions().scoped_grants) {
            grants.push_back(grant);
        }
    }
    return grants;
}

}  // namespace

bool Node::authorize_object(const std::string& acting_id, bool acting_is_user,
                            const zetta::governance::Operation& operation,
                            const zetta::governance::Resource& object) const {
    if (acting_id.empty()) return true;    // trusted unauthenticated-local session
    // A legacy AccessKey has no object-scoped grants (#762): it reaches an object
    // only by holding the operation as a flat capability of its own.
    if (!acting_is_user) return static_cast<bool>(authorize(acting_id, operation));
    auto user = find_user_by_id(acting_id);
    if (!user || !user->active()) return false;  // unknown/disabled user — denied
    const std::string now = zetta::util::now_iso8601();
    const bool global = user_holds_global(*this, acting_id, operation, now);
    return zetta::governance::object_permitted(
        global, user_scoped_grants(*this, acting_id), operation, object);
}

zetta::governance::ColumnAccess Node::column_scope(
    const std::string& acting_id, bool acting_is_user,
    const zetta::governance::Operation& operation, const std::string& database,
    const std::string& table) const {
    zetta::governance::ColumnAccess bypass;
    bypass.table_allowed = true;
    bypass.all_columns = true;
    if (acting_id.empty()) return bypass;   // trusted local
    // A legacy AccessKey (#762): the whole table when it holds the operation as a
    // flat capability, nothing otherwise. It has no column grants to mask by.
    if (!acting_is_user) {
        return authorize(acting_id, operation) ? bypass
                                               : zetta::governance::ColumnAccess{};
    }
    auto user = find_user_by_id(acting_id);
    if (!user || !user->active()) return {};  // table_allowed=false — denied
    const std::string now = zetta::util::now_iso8601();
    const bool global = user_holds_global(*this, acting_id, operation, now);
    return zetta::governance::column_access_for(
        global, user_scoped_grants(*this, acting_id), operation, database, table);
}

bool Node::may_call_tool(const std::string& acting_id, bool acting_is_user,
                         const std::string& tool_name) const {
    return authorize_object(acting_id, acting_is_user,
                            {zetta::governance::CapabilityDomain::Api, "query"},
                            zetta::governance::Resource::tool(tool_name));
}

bool Node::authorize_blob(const std::string& acting_id, bool acting_is_user,
                          const std::string& blob_id) const {
    return authorize_object(
        acting_id, acting_is_user,
        {zetta::governance::CapabilityDomain::Data, "blob_read"},
        zetta::governance::Resource::blob(blob_id));
}

zetta::pipeline::ExecutionResult Node::run_graph(const ir::ExecutionGraph& graph,
                                std::string project, std::string graph_id) {
    // The trusted local session: an empty principal runs every step unchecked.
    return run_graph(graph, std::move(project), std::move(graph_id),
                     zetta::pipeline::RunPrincipal{});
}

zetta::pipeline::ExecutionResult Node::run_graph(
    const ir::ExecutionGraph& graph, std::string project, std::string graph_id,
    const zetta::pipeline::RunPrincipal& principal) {
    // Delegate to the standalone orchestrator with the default per-step executor,
    // each step run as `principal` (#843). The Run it creates is owned by this
    // node (replicated + persisted as usual).
    // The run's audit rows name who runs it (#847): the entry point's declared
    // actor when there is one, else the principal (`local` for the trusted
    // session), so a console or headed-app run is not recorded with no actor.
    std::optional<std::string> actor = zetta::governance::current_audit_actor();
    if (!actor) actor = principal.local() ? std::string("local") : principal.acting_id;
    const zetta::governance::AuditActorScope audit_actor{actor};
    zetta::pipeline::GraphExecutor executor;
    executor.run_as(principal);
    zetta::pipeline::ExecutionResult result =
        executor.execute(*this, graph, std::move(project), std::move(graph_id));
    // A run is a governed action (#847): record it with its outcome, so a run
    // started from the headed app or a console session is on the trail like one
    // started over the API, whichever overload started it.
    std::string target = "run:" + (result.run ? result.run->id() : std::string("(none)"));
    std::string detail = "graph run ";
    if (result.ok) {
        detail += "succeeded";
    } else {
        detail += "failed";
        if (!result.failed_step.empty()) detail += " at step '" + result.failed_step + "'";
        if (!result.error.empty()) detail += ": " + result.error;
    }
    record_catalog_change(*this, std::move(target), true, std::move(detail));
    return result;
}

// ----- text entity-key tables (#1255) ----------------------------------------

nlohmann::json Node::entity_key_reference(std::string_view owner, nlohmann::json mapping) {
    if (!record_store_ || !mapping.is_object()) return mapping;
    if (zetta::data::is_entity_key_reference(mapping) &&
        mapping.value("owner", std::string()) == owner) {
        return mapping;
    }
    std::string error;
    const std::shared_ptr<const zetta::data::EntityKeyMapping> entries =
        resolve_entity_key_mapping(mapping, &error);
    if (!entries) {
        // Kept as given: a malformed inline mapping is the validator's to refuse,
        // and another owner's rows that have not reached this node stay shared.
        return mapping;
    }
    std::optional<nlohmann::json> reference =
        zetta::data::store_entity_key_table(*record_store_, owner, *entries, &error);
    if (!reference) {
        std::fprintf(stderr, "zetta: %s; keeping %s's entity keys inline\n", error.c_str(),
                     std::string(owner).c_str());
        return mapping;
    }
    return *reference;
}

std::shared_ptr<const zetta::data::EntityKeyMapping> Node::resolve_entity_key_mapping(
    const nlohmann::json& mapping, std::string* error) const {
    return zetta::data::load_entity_key_mapping(record_store_.get(), mapping, error);
}

// ----- replication: persistence ---------------------------------------------

fs::path Node::replication_path() const {
    return fs::path(config_.data_dir) / "replication.automerge";
}

bool Node::load_replication() {
    std::error_code ec;
    const fs::path path = replication_path();
    if (!fs::exists(path, ec)) {
        return !ec;
    }
    std::ifstream in(path, std::ios::binary);
    const std::vector<char> raw((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
    if (raw.empty()) {
        std::fprintf(stderr, "zetta: empty or unreadable replicated catalog %s\n", path.c_str());
        return false;
    }
    std::vector<std::byte> bytes(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        bytes[i] = static_cast<std::byte>(raw[i]);
    }
    if (!replication_->restore(bytes)) {
        std::fprintf(stderr, "zetta: cannot restore replicated catalog %s\n", path.c_str());
        return false;
    }
    return true;
}

void Node::persist_replication() {
    persisting_.store(true, std::memory_order_release);
    const auto started = std::chrono::steady_clock::now();
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
    ++persist_stats_.saves;
    persist_stats_.last_bytes = bytes.size();
    persist_stats_.last_ms = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - started)
                                 .count();
    persisting_.store(false, std::memory_order_release);
    last_replication_persist_ = Clock::now();
    replication_persist_pending_ = false;
}

// ----- replication: capture local-only data ---------------------------------

void Node::migrate_to_replication() {
    const nlohmann::json snapshot = replication_->snapshot();
    // "Is this entity already in the document?" — and a REMOVED entity counts as
    // already there. The snapshot omits tombstoned keys (that is what makes a
    // removal look like absence to every reconcile pass), so asking the snapshot
    // alone would read a deliberate drop as local-only data that predates
    // replication and re-capture it on the next start — resurrecting, on disk,
    // exactly what an operator dropped (#838). knows_key sees through the
    // tombstone.
    const auto already_replicated = [&](const std::string& key) {
        return snapshot.contains(key) || replication_->knows_key(key);
    };
    for (const auto& db : databases_) {
        if (!already_replicated(catalog::database_key(db->id()))) {
            // Carry the strategy (ENT-35); the default is omitted, so a
            // ReplicateToAll database migrates to the same db: value as before.
            nlohmann::json value{{"name", db->name()}};
            if (!db->replication_strategy().is_default()) {
                value["replication"] = db->replication_strategy().to_json();
            }
            replication_->set(catalog::database_key(db->id()), value);
        }
        for (const auto& table : db->tables()) {
            if (!already_replicated(catalog::table_key(table->id()))) {
                nlohmann::json value{{"db", db->id()}, {"name", table->name()}};
                if (!table->replication_strategy().is_default()) {
                    value["replication"] =
                        table->replication_strategy().to_json();
                }
                if (table->partition_only()) value["partition_only"] = true;
                if (!table->source_format().empty()) {
                    value["source_format"] = table->source_format();
                }
                replication_->set(catalog::table_key(table->id()), value);
            }
            // The partition manifest (#837) migrates with the table. Without
            // this, a manifest loaded from table.json on a node that has just
            // gained replication would be pruned by the very next reconcile —
            // and every read of that dataset would then plan with no authority.
            if (table->partition_manifest() &&
                !already_replicated(catalog::manifest_key(table->id()))) {
                replication_->set(catalog::manifest_key(table->id()),
                                  to_json(*table->partition_manifest()));
            }
            // So does the source column order (#1021), for the same reason.
            if (!table->column_order().empty() &&
                !already_replicated(catalog::column_order_key(table->id()))) {
                replication_->set(catalog::column_order_key(table->id()),
                                  nlohmann::json{{"columns", table->column_order()}});
            }
            // A partition-only table's rows are deliberately absent from the
            // document (#833) — migrating them in would undo exactly what the
            // marker exists to prevent.
            if (table->partition_only()) continue;
            const zetta::storage::StorageEngine& store = table->store();
            for (std::size_t i = 0; i < store.size(); ++i) {
                const std::string& row_id = store.id_at(i);
                if (already_replicated(catalog::row_key(row_id))) continue;
                // A row that cannot be read cannot be captured — leaving it out
                // of the document is recoverable (the next capture retries it),
                // capturing an empty row would replicate the loss.
                const zetta::storage::ParquetLazyPtr row = store.at(i);
                if (!row) continue;
                replication_->set(catalog::row_key(row_id),
                                  nlohmann::json{{"tbl", table->id()},
                                                 {"data", *row}});
            }
        }
        for (const zetta::data::LargeObject& lo : db->large_objects().list()) {
            if (!already_replicated(catalog::large_object_key(lo.id))) {
                replication_->set(catalog::large_object_key(lo.id),
                                  nlohmann::json{{"db", db->id()},
                                                 {"name", lo.name},
                                                 {"size", lo.size},
                                                 {"sha256", lo.sha256}});
            }
        }
    }
    for (const auto& project : projects_) {
        if (!already_replicated(catalog::project_key(project->id()))) {
            replication_->set(catalog::project_key(project->id()),
                              project->metadata());
        }
        for (const auto& input : project->inputs()) {
            if (!already_replicated(catalog::input_key(input->id()))) {
                replication_->set(catalog::input_key(input->id()),
                                  input->metadata());
            }
        }
        for (const auto& graph : project->graphs()) {
            if (!already_replicated(catalog::graph_key(graph->id()))) {
                replication_->set(catalog::graph_key(graph->id()),
                                  graph->record());
            }
        }
    }
    for (const auto& deployment : deployments_) {
        if (!already_replicated(catalog::deployment_key(deployment->id()))) {
            replication_->set(catalog::deployment_key(deployment->id()),
                              deployment->metadata());
        }
    }
    for (const auto& run : runs_) {
        if (!already_replicated(catalog::run_key(run->id()))) {
            replication_->set(catalog::run_key(run->id()), run->metadata());
        }
    }
    for (const auto& study : studies_) {
        if (!already_replicated(catalog::study_key(study->id()))) {
            replication_->set(catalog::study_key(study->id()),
                              study->metadata());
        }
    }
    for (const auto& model : models_) {
        if (!already_replicated(catalog::model_key(model->id()))) {
            replication_->set(catalog::model_key(model->id()),
                              model->metadata());
        }
    }
    for (const auto& monitor : monitors_) {
        if (!already_replicated(catalog::monitor_key(monitor->id()))) {
            replication_->set(catalog::monitor_key(monitor->id()),
                              monitor->metadata());
        }
    }
    for (const auto& profile : workspace_profiles_) {
        if (!already_replicated(catalog::workspace_profile_key(profile->id()))) {
            replication_->set(catalog::workspace_profile_key(profile->id()),
                              profile->metadata());
        }
    }
    for (const auto& plan : plans_) {
        if (!already_replicated(catalog::plan_key(plan->id()))) {
            replication_->set(catalog::plan_key(plan->id()), plan->metadata());
        }
    }
    experiment_manager_.migrate_to_replication();
    for (const auto& index : indexes_) {
        if (!already_replicated(catalog::index_key(index->id()))) {
            replication_->set(catalog::index_key(index->id()),
                              index->metadata());
        }
    }
    for (const auto& job : jobs_) {
        if (!already_replicated(catalog::job_key(job->id()))) {
            replication_->set(catalog::job_key(job->id()), job->metadata());
        }
    }
    for (const auto& partition : dataset_partitions_) {
        if (!already_replicated(catalog::partition_key(partition->id()))) {
            replication_->set(catalog::partition_key(partition->id()),
                              partition->metadata());
        }
    }
    // A node that cannot seal captures no identity: anything it wrote would be
    // refused by every peer (#764).
    for (const auto& principal : principals_) {
        if (!identity_writable()) break;
        if (!already_replicated(catalog::principal_key(principal->id()))) {
            replicate_identity(catalog::principal_key(principal->id()),
                               principal->metadata());
        }
        for (const auto& key : principal->keys()) {
            if (!already_replicated(catalog::access_key_key(key->id()))) {
                replicate_identity(catalog::access_key_key(key->id()),
                                   key->metadata());
            }
        }
    }
    for (const auto& entry : ledger_entries_) {
        if (!already_replicated(catalog::ledger_key(entry->id()))) {
            replication_->set(catalog::ledger_key(entry->id()),
                              entry->metadata());
        }
    }
}

// ----- replication: apply the document to the local projection --------------

void Node::reconcile_from_replication(const nlohmann::json& snapshot) {
    // Reconcile each replicated entity type from the same snapshot, parents
    // before children (databases before tables/rows/large objects; projects
    // before inputs/graphs; principals before access keys; users + roles before
    // their tokens + assignments). Order matters.
    reconcile_databases(snapshot);
    reconcile_projects(snapshot);
    reconcile_inputs(snapshot);
    reconcile_graphs(snapshot);
    reconcile_deployments(snapshot);
    reconcile_runs(snapshot);
    reconcile_studies(snapshot);
    reconcile_models(snapshot);
    reconcile_monitors(snapshot);
    reconcile_workspace_profiles(snapshot);
    reconcile_plans(snapshot);
    experiment_manager_.reconcile(snapshot);
    reconcile_indexes(snapshot);
    reconcile_jobs(snapshot);
    reconcile_partitions(snapshot);
    // Identity is read only through trusted_identity: a record whose seal does
    // not verify is absent to every reconciler below (#764).
    const nlohmann::json identity = trusted_identity(snapshot);
    reconcile_principals(identity);
    reconcile_access_keys(identity);
    reconcile_users(identity);
    reconcile_roles(identity);
    reconcile_tokens(identity);
    reconcile_user_roles(identity);
    reconcile_policies(identity);
    reconcile_ledger_entries(snapshot);
}

void Node::reconcile_replication_changes(
    const nlohmann::json& snapshot,
    const std::vector<std::string>& changed_keys) {
    ReplicationReconcileBatches batches =
        make_reconcile_batches(snapshot, changed_keys);

    if (!batches.data.keys.empty()) {
        reconcile_databases(batches.data.values, &batches.data.keys);
    }
    if (!batches.projects.keys.empty()) {
        reconcile_projects(batches.projects.values, &batches.projects.keys);
    }
    if (!batches.inputs.keys.empty()) {
        reconcile_inputs(batches.inputs.values, &batches.inputs.keys);
    }
    if (!batches.graphs.keys.empty()) {
        reconcile_graphs(batches.graphs.values, &batches.graphs.keys);
    }
    if (!batches.deployments.keys.empty()) {
        reconcile_deployments(batches.deployments.values,
                              &batches.deployments.keys);
    }
    if (!batches.runs.keys.empty()) {
        reconcile_runs(batches.runs.values, &batches.runs.keys);
    }
    if (!batches.studies.keys.empty()) {
        reconcile_studies(batches.studies.values, &batches.studies.keys);
    }
    if (!batches.models.keys.empty()) {
        reconcile_models(batches.models.values, &batches.models.keys);
    }
    if (!batches.monitors.keys.empty()) {
        reconcile_monitors(batches.monitors.values, &batches.monitors.keys);
    }
    if (!batches.profiles.keys.empty()) {
        reconcile_workspace_profiles(batches.profiles.values,
                                     &batches.profiles.keys);
    }
    if (!batches.plans.keys.empty()) {
        reconcile_plans(batches.plans.values, &batches.plans.keys);
    }
    if (!batches.experiments.keys.empty()) {
        experiment_manager_.reconcile(batches.experiments.values,
                                      &batches.experiments.keys);
    }
    if (!batches.indexes.keys.empty()) {
        reconcile_indexes(batches.indexes.values, &batches.indexes.keys);
    }
    if (!batches.jobs.keys.empty()) {
        reconcile_jobs(batches.jobs.values, &batches.jobs.keys);
    }
    if (!batches.partitions.keys.empty()) {
        reconcile_partitions(batches.partitions.values,
                             &batches.partitions.keys);
    }
    if (!batches.principals.keys.empty()) {
        reconcile_principals(trusted_identity(batches.principals.values, &batches.principals.keys),
                             &batches.principals.keys);
    }
    if (!batches.access_keys.keys.empty()) {
        reconcile_access_keys(trusted_identity(batches.access_keys.values, &batches.access_keys.keys),
                              &batches.access_keys.keys);
    }
    if (!batches.users.keys.empty()) {
        reconcile_users(trusted_identity(batches.users.values, &batches.users.keys), &batches.users.keys);
    }
    if (!batches.roles.keys.empty()) {
        reconcile_roles(trusted_identity(batches.roles.values, &batches.roles.keys), &batches.roles.keys);
    }
    if (!batches.tokens.keys.empty()) {
        reconcile_tokens(trusted_identity(batches.tokens.values, &batches.tokens.keys), &batches.tokens.keys);
    }
    if (!batches.user_roles.keys.empty()) {
        reconcile_user_roles(trusted_identity(batches.user_roles.values, &batches.user_roles.keys),
                             &batches.user_roles.keys);
    }
    if (!batches.policies.keys.empty()) {
        reconcile_policies(trusted_identity(batches.policies.values, &batches.policies.keys), &batches.policies.keys);
    }
    if (!batches.ledger_entries.keys.empty()) {
        reconcile_ledger_entries(batches.ledger_entries.values,
                                 &batches.ledger_entries.keys);
    }
    if (!batches.nodes.keys.empty()) {
        rebalance_partitions_on_membership_change(snapshot,
                                                  &batches.nodes.keys);
    }
}

void Node::rebalance_partitions_on_membership_change(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    std::vector<std::string> members;
    if (!changed_keys) {
        members = membership_from_snapshot(snapshot);
    } else {
        members = last_membership_;
        for (const std::string& key : *changed_keys) {
            const std::string id = catalog::id_after(key, catalog::kNodePrefix);
            if (id.empty()) continue;
            const auto member = std::ranges::find(members, id);
            if (snapshot.contains(key)) {
                if (member == members.end()) members.push_back(id);
            } else if (member != members.end()) {
                members.erase(member);
            }
        }
        std::sort(members.begin(), members.end());
    }
    if (members == last_membership_) return;  // no membership delta — nothing to do
    // The previous membership distinguishes replicate-to-all partitions (owned by
    // exactly the old member set) from sharded ones, so rebalance leaves the
    // former untouched. Move it out before overwriting the baseline.
    std::vector<std::string> previous = std::move(last_membership_);
    last_membership_ = members;
    // First observation (baseline established in start(), but be defensive) — and
    // a now-empty membership (nowhere to place) — never trigger a rebalance.
    if (previous.empty() || members.empty()) return;
    zetta::data::rebalance_partitions(*this, members, previous);
}

// Databases, tables, rows, and large objects reconcile together: they decode in
// one shared pass over the snapshot, and table/large-object removals nest under
// each kept database, so they don't split into independent helpers cleanly.
void Node::reconcile_databases(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Decode the flat catalog keys into desired databases / tables / rows.
    struct DatabaseEntry { std::string name; zetta::data::ReplicationStrategy strategy; };
    std::map<std::string, DatabaseEntry> databases;           // id -> entry
    struct TableEntry {
        std::string database_id;
        std::string name;
        zetta::data::ReplicationStrategy strategy;
        bool partition_only = false;  // #833; rows are never captured
        std::string source_format;    // #1221; what the rows were loaded from
    };
    std::map<std::string, TableEntry> tables;                 // id -> entry
    struct RowEntry { std::string table_id; nlohmann::json data; bool tombstone = false; };
    std::map<std::string, RowEntry> rows;                     // id -> entry
    // Partition manifests (#837), keyed by the table they describe.
    std::map<std::string, zetta::data::DatasetManifestValue> manifests;
    // Source column orders (#1021), keyed by the table they describe.
    std::map<std::string, std::vector<std::string>> column_orders;
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
                                            zetta::data::replication_strategy_from_meta(value)});
        } else if (std::string table_id =
                       catalog::id_after(key, catalog::kTablePrefix);
                   !table_id.empty() && value.is_object() &&
                   value.contains("db") && value.contains("name")) {
            tables.emplace(std::move(table_id),
                           TableEntry{value["db"].get<std::string>(),
                                      value["name"].get<std::string>(),
                                      zetta::data::replication_strategy_from_meta(value),
                                      value.value("partition_only", false),
                                      value.value("source_format", std::string())});
        } else if (std::string row_id =
                       catalog::id_after(key, catalog::kRowPrefix);
                   !row_id.empty() && value.is_object() &&
                   value.contains("tbl")) {
            // A row entry is either data (`{tbl, data}`) or a delete TOMBSTONE
            // (`{tbl, deleted:true}`) — both replicate as value changes.
            const bool tombstone = value.value("deleted", false);
            rows.emplace(std::move(row_id),
                         RowEntry{value["tbl"].get<std::string>(),
                                  tombstone ? nlohmann::json()
                                            : value.value("data", nlohmann::json()),
                                  tombstone});
        } else if (std::string manifest_table =
                       catalog::id_after(key, catalog::kManifestPrefix);
                   !manifest_table.empty()) {
            // Validated at the boundary: a malformed manifest is ignored rather
            // than adopted as the authority reads are failed against.
            if (std::optional<zetta::data::DatasetManifestValue> parsed =
                    zetta::data::parse_dataset_manifest_value(value)) {
                manifests.emplace(std::move(manifest_table), std::move(*parsed));
            }
        } else if (std::string order_table =
                       catalog::id_after(key, catalog::kColumnOrderPrefix);
                   !order_table.empty()) {
            // `{columns: [name, …]}`; anything else is ignored at the boundary.
            if (value.is_object()) {
                const auto columns = value.find("columns");
                if (columns != value.end() && columns->is_array()) {
                    std::vector<std::string> names;
                    for (const nlohmann::json& column : *columns) {
                        if (column.is_string()) {
                            names.push_back(column.get<std::string>());
                        }
                    }
                    column_orders.emplace(std::move(order_table), std::move(names));
                }
            }
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
        const bool database_changed = reconciliation_includes_key(
            changed_keys, catalog::database_key(db_id));
        if (database_changed && !databases.contains(db_id)) {
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
            if (reconciliation_includes_key(
                    changed_keys, catalog::table_key(table_id)) &&
                (it == tables.end() || it->second.database_id != db_id)) {
                db->materialize_drop_table(table_id);
            }
        }
        std::vector<std::string> local_lo_ids;
        for (const zetta::data::LargeObject& lo : db->large_objects().list()) {
            local_lo_ids.push_back(lo.id);
        }
        for (const std::string& lo_id : local_lo_ids) {
            const auto it = large_objects.find(lo_id);
            if (reconciliation_includes_key(
                    changed_keys, catalog::large_object_key(lo_id)) &&
                (it == large_objects.end() ||
                 it->second.database_id != db_id)) {
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
            db->materialize_table(id, entry.name, entry.strategy,
                                  entry.partition_only, entry.source_format);
        }
    }
    // Partition manifests (#837): the document is the source of truth, so a table
    // whose `mfst:` key is absent has its local copy cleared — the same
    // materialize-only rule every other reconcile path follows.
    for (const auto& [table_id, entry] : tables) {
        auto db = find_database_by_id(entry.database_id);
        if (!db) continue;
        auto table = db->find_table_by_id(table_id);
        if (!table) continue;
        const auto manifest = manifests.find(table_id);
        table->apply_partition_manifest(
            manifest == manifests.end()
                ? std::nullopt
                : std::optional<zetta::data::DatasetManifestValue>(manifest->second));
        // The source column order (#1021), by the same rule.
        const auto order = column_orders.find(table_id);
        table->apply_column_order(order == column_orders.end()
                                      ? std::vector<std::string>{}
                                      : order->second);
    }
    for (auto& [id, entry] : rows) {
        auto db_table = [&]() -> std::shared_ptr<zetta::data::Table> {
            for (const auto& db : databases_) {
                if (auto table = db->find_table_by_id(entry.table_id)) {
                    return table;
                }
            }
            return nullptr;
        }();
        if (!db_table) continue;
        if (entry.tombstone) {
            db_table->materialize_row_delete(id);  // a peer deleted this row
        } else {
            db_table->materialize_row(id, std::move(entry.data));  // add or update
        }
    }
    for (const auto& [id, entry] : large_objects) {
        auto db = find_database_by_id(entry.database_id);
        if (!db) continue;
        zetta::data::LargeObject object;
        object.id = id;
        object.name = entry.name;
        object.size = entry.size;
        object.sha256 = entry.sha256;
        db->large_objects().materialize_object(std::move(object));
    }
}

void Node::reconcile_projects(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Projects (top-level, no parent): project:<id> -> { name, created,
    // updated, owner }. Decode the desired set, then drop / add / update the
    // local projection to match the document — no re-capture.
    struct ProjectEntry {
        std::string name;
        std::string created;
        std::string updated;
        std::string owner;
        std::string description;
        bool internal = false;
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
                                      value.value("owner", std::string{}),
                                      value.value("description", std::string{}),
                                      value.contains("internal") &&
                                          value["internal"].is_boolean() &&
                                          value["internal"].get<bool>()});
    }
    std::vector<std::string> local_project_ids;
    for (const auto& project : projects_) {
        local_project_ids.push_back(project->id());
    }
    for (const std::string& project_id : local_project_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::project_key(project_id)) &&
            !projects.contains(project_id)) {
            materialize_drop_project(project_id);
        }
    }
    for (const auto& [id, entry] : projects) {
        auto project = find_project_by_id(id);
        if (!project) {
            materialize_project(id, entry.name, entry.created, entry.updated,
                                entry.owner, entry.description, entry.internal);
        } else {
            if (project->name() != entry.name ||
                project->updated() != entry.updated ||
                project->owner() != entry.owner ||
                project->description() != entry.description) {
                project->set_metadata(entry.name, entry.created, entry.updated,
                                      entry.owner, entry.description);  // a peer changed it (no re-capture)
            }
            if (project->internal() != entry.internal) {
                project->set_internal(entry.internal);  // a peer revealed it (lake#654)
            }
        }
    }
}

void Node::reconcile_inputs(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Project inputs (owned by a project): input:<id> -> { project, kind,
    // source, filename, content_hash, size, created_by, created_at }. Inputs are
    // immutable provenance records (never renamed/edited), so reconcile only adds
    // them under their owning project and drops the ones a peer removed. The
    // payload body is not in the document and does not transfer yet.
    struct InputEntry {
        std::string project_id;
        zetta::pipeline::ProjectInputKind kind;
        zetta::pipeline::ProjectInputSource source;
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
        const auto kind = zetta::pipeline::project_input_kind_from(
            value.value("kind", std::string{}));
        const auto source = zetta::pipeline::project_input_source_from(
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
            if (reconciliation_includes_key(
                    changed_keys, catalog::input_key(input_id)) &&
                (it == inputs.end() ||
                 it->second.project_id != project->id())) {
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

void Node::reconcile_graphs(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
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
        bool approved = false;  // #885: the replicated plan-approval state
        std::string approved_by;
        std::string approved_at;
        zetta::pipeline::PipelineExposure exposure;  // zetta#1289
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
        // A malformed exposure is not served: the graph reconciles unexposed.
        zetta::pipeline::PipelineExposure exposure;
        if (const auto found = value.find("exposure"); found != value.end()) {
            if (auto parsed = zetta::pipeline::pipeline_exposure_from_json(*found)) {
                exposure = std::move(*parsed);
            }
        }
        desired_graphs.emplace(
            std::move(id),
            GraphEntry{value.value("project", std::string{}),
                       value.value("name", std::string{}), std::move(*graph),
                       value.value("approval", std::string{}) == "approved",
                       value.value("approved_by", std::string{}),
                       value.value("approved_at", std::string{}),
                       std::move(exposure)});
    }
    for (const auto& project : projects_) {
        std::vector<std::string> local_graph_ids;
        for (const auto& graph : project->graphs()) {
            local_graph_ids.push_back(graph->id());
        }
        for (const std::string& graph_id : local_graph_ids) {
            const auto it = desired_graphs.find(graph_id);
            if (reconciliation_includes_key(
                    changed_keys, catalog::graph_key(graph_id)) &&
                (it == desired_graphs.end() ||
                 it->second.project_id != project->id())) {
                project->materialize_drop_graph(graph_id);
            }
        }
    }
    for (auto& [id, entry] : desired_graphs) {
        auto project = find_project_by_id(entry.project_id);
        if (!project) continue;  // dangling graph (project gone) — ignore
        auto existing = project->find_graph_by_id(id);
        if (!existing) {
            auto created = project->materialize_graph(id, entry.name, std::move(entry.graph),
                                                      entry.approved, entry.approved_by,
                                                      entry.approved_at);
            if (created && entry.exposure.version > 0) {
                created->set_exposure(std::move(entry.exposure));
                created->persist();
            }
        } else if (existing->name() != entry.name ||
                   existing->approved() != entry.approved ||
                   !(existing->exposure() == entry.exposure) ||
                   ir::to_json(existing->graph()) != ir::to_json(entry.graph)) {
            // A peer changed the plan, its approval and/or its publication —
            // mirror them, then persist once (set_approval and set_exposure are
            // pure; set_contents writes the file).
            existing->set_approval(entry.approved, entry.approved_by,
                                   entry.approved_at);
            existing->set_exposure(std::move(entry.exposure));
            existing->set_contents(entry.name, std::move(entry.graph));
        }
    }
}

void Node::reconcile_deployments(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Deployments (top-level, no parent): deployment:<id> -> { graph_name,
    // target, node, node_count, edge_count, created }. Decode the desired set,
    // then drop / add the local projection to match the document. Deployments
    // are immutable once recorded in this slice (no update branch); reconcile /
    // self-heal that mutates them is a later task.
    struct DeploymentEntry {
        std::string graph_name;
        zetta::pipeline::DeploymentTarget target;
        std::string node_id;
        std::size_t node_count;
        std::size_t edge_count;
        std::string created;
        std::vector<zetta::pipeline::TableRead> reads;
    };
    std::map<std::string, DeploymentEntry> deployments;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kDeploymentPrefix);
        if (id.empty() || !value.is_object() || !value.contains("graph_name")) {
            continue;
        }
        const std::optional<zetta::pipeline::DeploymentTarget> target =
            zetta::pipeline::DeploymentTarget::from_json(
                value.value("target", nlohmann::json::object()));
        if (!target) continue;
        deployments.emplace(
            std::move(id),
            DeploymentEntry{value.value("graph_name", std::string{}), *target,
                            value.value("node", std::string{}),
                            value.value("node_count", std::size_t{0}),
                            value.value("edge_count", std::size_t{0}),
                            value.value("created", std::string{}),
                            zetta::pipeline::table_reads_from_json(
                                value.value("reads", nlohmann::json::array()))});
    }
    std::vector<std::string> local_deployment_ids;
    for (const auto& deployment : deployments_) {
        local_deployment_ids.push_back(deployment->id());
    }
    for (const std::string& deployment_id : local_deployment_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::deployment_key(deployment_id)) &&
            !deployments.contains(deployment_id)) {
            materialize_drop_deployment(deployment_id);
        }
    }
    for (const auto& [id, entry] : deployments) {
        if (!find_deployment(id)) {
            materialize_deployment(id, entry.graph_name, entry.target,
                                   entry.node_id, entry.node_count,
                                   entry.edge_count, entry.created, entry.reads);
        }
    }
}

void Node::reconcile_runs(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Runs (top-level, no parent): run:<id> -> the run value. Decode + validate
    // the desired set at the boundary (a malformed CRDT value is ignored, not
    // trusted), then drop / add / update the local projection to match — with no
    // re-capture (apply_value / materialize_run do not write back to the CRDT).
    std::map<std::string, zetta::pipeline::RunValue> runs;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kRunPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::pipeline::RunValue> parsed = zetta::pipeline::parse_run_value(value)) {
            runs.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_run_ids;
    for (const auto& run : runs_) local_run_ids.push_back(run->id());
    for (const std::string& run_id : local_run_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::run_key(run_id)) &&
            !runs.contains(run_id)) {
            materialize_drop_run(run_id);
        }
    }
    for (auto& [id, value] : runs) {
        auto run = find_run(id);
        if (!run) {
            materialize_run(id, std::move(value));
        } else if (!run->capture_pending() &&
                   run->metadata() != to_json(value)) {
            // A peer changed it (no re-capture). A Run holding a coalesced
            // local change is newer than any capture it made, so it is left
            // alone until that change is written (#1217).
            run->apply_value(std::move(value));
        }
    }
}

void Node::reconcile_studies(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    std::map<std::string, zetta::ml::StudyValue> desired;
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kStudyPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::ml::StudyValue> parsed =
                zetta::ml::parse_study_value(value)) {
            desired.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_ids;
    for (const auto& study : studies_) local_ids.push_back(study->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::study_key(id)) &&
            !desired.contains(id)) {
            materialize_drop_study(id);
        }
    }
    for (auto& [id, value] : desired) {
        const auto study = find_study(id);
        if (!study) {
            materialize_study(id, std::move(value));
        } else if (!study->capture_pending() &&
                   study->metadata() != zetta::ml::to_json(value)) {
            study->apply_value(std::move(value));
        }
    }
}

void Node::reconcile_models(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Models (top-level, no parent): model:<id> -> the model value. Decode +
    // validate the desired set at the boundary (a malformed CRDT value is
    // ignored, not trusted), then drop / add / update the local projection to
    // match — with no re-capture. apply_value updates only the metadata; the
    // serialized weights body is NOT in the document and arrives separately over
    // the large-object body transfer path (the blob store requests it when missing).
    std::map<std::string, zetta::ml::ModelValue> models;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kModelPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::ml::ModelValue> parsed = zetta::ml::parse_model_value(value)) {
            models.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_model_ids;
    for (const auto& model : models_) local_model_ids.push_back(model->id());
    for (const std::string& model_id : local_model_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::model_key(model_id)) &&
            !models.contains(model_id)) {
            materialize_drop_model(model_id);
        }
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

void Node::reconcile_monitors(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    std::map<std::string, zetta::solutions::MonitorValue> monitors;
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kMonitorPrefix);
        if (id.empty() || !reconciliation_includes_key(changed_keys, key)) {
            continue;
        }
        std::optional<zetta::solutions::MonitorValue> parsed =
            zetta::solutions::parse_monitor_value(value);
        if (parsed) {
            monitors.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_ids;
    for (const auto& monitor : monitors_) {
        local_ids.push_back(monitor->id());
    }
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::monitor_key(id)) &&
            !monitors.contains(id)) {
            materialize_drop_monitor(id);
        }
    }
    for (auto& [id, value] : monitors) {
        auto monitor = find_monitor(id);
        if (!monitor) {
            materialize_monitor(id, std::move(value));
        } else if (monitor->metadata() != zetta::solutions::to_json(value)) {
            monitor->apply_value(std::move(value));
        }
    }
}

void Node::reconcile_workspace_profiles(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Workspace profiles (top-level, no parent): profile:<id> -> the value.
    // Decode + validate the desired set at the boundary (a malformed CRDT value
    // is ignored, not trusted), then drop / add / update the local projection to
    // match — with no re-capture (apply_value rewrites the file only).
    std::map<std::string, zetta::workspace::WorkspaceProfileValue> profiles;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kWorkspaceProfilePrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::workspace::WorkspaceProfileValue> parsed =
                zetta::workspace::parse_workspace_profile_value(value)) {
            profiles.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_ids;
    for (const auto& profile : workspace_profiles_) local_ids.push_back(profile->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::workspace_profile_key(id)) &&
            !profiles.contains(id)) {
            materialize_drop_workspace_profile(id);
        }
    }
    for (auto& [id, value] : profiles) {
        auto profile = find_workspace_profile(id);
        if (!profile) {
            materialize_workspace_profile(id, std::move(value));
        } else if (profile->metadata() != to_json(value)) {
            profile->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_plans(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Plans (top-level, no parent): plan:<id> -> the value. Decode + validate at
    // the boundary (a malformed CRDT value is ignored), then drop / add / update
    // the local projection to match — with no re-capture.
    std::map<std::string, zetta::billing::PlanValue> plans;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPlanPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::billing::PlanValue> parsed =
                zetta::billing::parse_plan_value(value)) {
            plans.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_ids;
    for (const auto& plan : plans_) local_ids.push_back(plan->id());
    for (const std::string& id : local_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::plan_key(id)) &&
            !plans.contains(id)) {
            materialize_drop_plan(id);
        }
    }
    for (auto& [id, value] : plans) {
        auto plan = find_plan(id);
        if (!plan) {
            materialize_plan(id, std::move(value));
        } else if (plan->metadata() != to_json(value)) {
            plan->apply_value(std::move(value));  // a peer changed it (no re-capture)
        }
    }
}

void Node::reconcile_indexes(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Indexes (top-level, no parent): index:<id> -> the index value. Decode +
    // validate the desired set at the boundary (a malformed CRDT value is ignored,
    // not trusted), then drop / add / update the local projection to match — with
    // no re-capture. An index is metadata only (no body), so apply_value rewrites
    // index.json and nothing more.
    std::map<std::string, zetta::index::IndexValue> indexes;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kIndexPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::index::IndexValue> parsed = zetta::index::parse_index_value(value)) {
            indexes.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_index_ids;
    for (const auto& index : indexes_) local_index_ids.push_back(index->id());
    for (const std::string& index_id : local_index_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::index_key(index_id)) &&
            !indexes.contains(index_id)) {
            materialize_drop_index(index_id);
        }
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

void Node::reconcile_jobs(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Compute jobs (top-level, no parent): job:<id> -> the job value. Decode +
    // validate the desired set at the boundary (a malformed CRDT value is ignored,
    // not trusted), then drop / add / update the local projection to match — with
    // no re-capture and no audit (apply_value / materialize_job do not write back
    // to the CRDT, and a peer's create/finish arrives already-audited as its own
    // replicated `audit:` event).
    std::map<std::string, zetta::compute::ComputeJobValue> compute_jobs;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kJobPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::compute::ComputeJobValue> parsed =
                zetta::compute::parse_compute_job_value(value)) {
            compute_jobs.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_job_ids;
    for (const auto& job : jobs_) local_job_ids.push_back(job->id());
    for (const std::string& job_id : local_job_ids) {
        if (reconciliation_includes_key(changed_keys, catalog::job_key(job_id)) &&
            !compute_jobs.contains(job_id)) {
            materialize_drop_job(job_id);
        }
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

void Node::reconcile_partitions(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Dataset partitions (top-level, no parent): part:<id> -> the partition value.
    // The catalog is replicate-to-all, so each node materializes the same set.
    // Decode + validate the desired set at the boundary (a malformed CRDT value is
    // ignored, not trusted), then drop / add / update the local projection to match
    // — with no re-capture (apply_value / materialize_dataset_partition do not write
    // back to the CRDT).
    std::map<std::string, zetta::data::DatasetPartitionValue> partitions;  // id -> value
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPartitionPrefix);
        if (id.empty()) continue;
        if (std::optional<zetta::data::DatasetPartitionValue> parsed =
                zetta::data::parse_dataset_partition_value(value)) {
            partitions.emplace(std::move(id), std::move(*parsed));
        }
    }
    std::vector<std::string> local_partition_ids;
    for (const auto& partition : dataset_partitions_) {
        local_partition_ids.push_back(partition->id());
    }
    for (const std::string& partition_id : local_partition_ids) {
        if (reconciliation_includes_key(
                changed_keys, catalog::partition_key(partition_id)) &&
            !partitions.contains(partition_id)) {
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

void Node::reconcile_principals(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Principals (top-level, no parent): principal:<id> -> { kind, name,
    // created }. Decode the desired set, then drop / add / update the local
    // projection to match the document — no re-capture.
    struct PrincipalEntry {
        zetta::governance::PrincipalKind kind;
        std::string name;
        std::string created;
    };
    std::map<std::string, PrincipalEntry> principals;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kPrincipalPrefix);
        if (id.empty() || !value.is_object() || !value.contains("name")) {
            continue;
        }
        const auto kind = zetta::governance::principal_kind_from(value.value("kind", std::string{}));
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
        if (reconciliation_includes_key(
                changed_keys, catalog::principal_key(principal_id)) &&
            !principals.contains(principal_id)) {
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

void Node::reconcile_access_keys(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Access keys (owned by a principal): key:<id> -> { principal, scope,
    // created }. Keys are immutable credentials (never edited), so reconcile only
    // adds them under their owning principal and drops the ones a peer removed.
    struct AccessKeyEntry {
        std::string principal_id;
        zetta::governance::Scope scope;
        zetta::governance::CapabilitySet capabilities;
        std::string created;
    };
    std::map<std::string, AccessKeyEntry> access_keys;  // id -> entry
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kAccessKeyPrefix);
        if (id.empty() || !value.is_object() || !value.contains("principal") ||
            !value.contains("scope")) {
            continue;
        }
        const auto scope = zetta::governance::scope_from_json(value["scope"]);
        if (!scope) continue;  // boundary
        // Capabilities are optional (a peer on an older shape omits them => no
        // rights); a present-yet-malformed set is rejected rather than applied.
        zetta::governance::CapabilitySet capabilities;
        if (value.contains("capabilities")) {
            const auto parsed = zetta::governance::capability_set_from_json(value["capabilities"]);
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
            if (reconciliation_includes_key(
                    changed_keys, catalog::access_key_key(key_id)) &&
                (it == access_keys.end() ||
                 it->second.principal_id != principal->id())) {
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

void Node::reconcile_ledger_entries(
    const nlohmann::json& snapshot,
    const std::vector<std::string>* changed_keys) {
    // Like the audit trail, ledger records are immutable and append-only.
    // An incremental bucket may add records, never edit or remove history.
    for (const auto& [key, value] : snapshot.items()) {
        std::string id = catalog::id_after(key, catalog::kLedgerPrefix);
        if (id.empty() || !reconciliation_includes_key(changed_keys, key)) {
            continue;
        }
        const std::optional<zetta::solutions::LedgerEntryValue> parsed =
            zetta::solutions::parse_audit_ledger_entry(value);
        if (parsed) {
            materialize_ledger_entry(std::move(id), *parsed);
        }
    }
}

IntegrityReport Node::check_integrity() const {
    IntegrityReport report;
    for (const auto& db : databases_) {
        ++report.databases;
        for (const auto& table : db->tables()) {
            ++report.tables;
            report.rows += table->row_count();
        }
        for (const zetta::data::LargeObject& object : db->large_objects().list()) {
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
                          zetta::util::sha256_hex(*body) != object.sha256)) {
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
        const zetta::ml::ModelWeights& weights = *model->weights();
        if (!body || (!weights.sha256.empty() &&
                      zetta::util::sha256_hex(*body) != weights.sha256)) {
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
        audit_event_count());
    std::string result = buf;
    const auto memory = memory_snapshot();
    result += " | mem rss " + zetta::memory::format_bytes(memory.allocator.rss_bytes);
    if (!memory.subsystems.empty()) {
        result += " top " + memory.subsystems.front().name + " " +
                  zetta::memory::format_bytes(memory.subsystems.front().bytes);
    }
    return result;
}

}  // namespace zetta
