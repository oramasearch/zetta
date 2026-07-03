#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/cloud/cloud_inspector.hpp"
#include "zetta/cloud/provisioner_config.hpp"
#include "zetta/compute/checkpoint.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_controller.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/cluster/node_metrics.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/governance/scope.hpp"

namespace zetta::cluster { class Cluster; }
namespace zetta::pipeline { struct ExecutionResult; }
namespace zetta::ir { class ExecutionGraph; }
namespace zetta::compute { class JobCoordinator; }
namespace zetta::compute { class CoordinatedLaunchListener; }

namespace zetta::cluster { class ReplicationManager; }



namespace zetta {



// Where a node's persistent state lives and how it presents itself. More
// fields (listen address, replication peers, cache sizes) land alongside the
// storage + query engine.
struct NodeConfig {
    std::string data_dir = "./zetta-data";  // database directory on disk
    bool preflight = true;  // run an integrity check on start() (--no-preflight)
};

// Result of the startup integrity ("preflight") check — a snapshot of how sound
// the on-disk + replicated state is. Counts plus the specific problems found.
struct IntegrityReport {
    std::size_t databases = 0;
    std::size_t tables = 0;
    std::size_t rows = 0;
    std::size_t large_objects = 0;
    std::size_t large_object_bodies_present = 0;
    std::size_t large_object_bodies_missing = 0;  // metadata here, body not (yet)
    std::size_t corrupt_large_objects = 0;         // on-disk body != recorded sha256
    std::size_t models = 0;
    std::size_t model_weights_present = 0;
    std::size_t model_weights_missing = 0;  // metadata here, weights body not (yet)
    std::size_t corrupt_model_weights = 0;  // on-disk weights != recorded sha256
    std::size_t dangling_tables = 0;               // table -> missing database
    std::size_t dangling_rows = 0;                 // row -> missing table
    std::size_t dangling_large_objects = 0;        // large object -> missing database
    std::size_t dangling_inputs = 0;               // project input -> missing project
    std::size_t dangling_graphs = 0;               // graph -> missing project
    std::size_t dangling_access_keys = 0;          // access key -> missing principal

    // A "good state": no corruption and no dangling references. (A missing body
    // is informational — it may simply be awaiting transfer from a peer.)
    bool ok() const {
        return corrupt_large_objects == 0 && corrupt_model_weights == 0 &&
               dangling_tables == 0 && dangling_rows == 0 &&
               dangling_large_objects == 0 && dangling_inputs == 0 &&
               dangling_graphs == 0 && dangling_access_keys == 0;
    }
};

// A single zetta database node — the headless core.
//
// A Node owns the database lifecycle and knows nothing about any UI: the GUI
// (zetta::gui::App) is a thin renderer driven by a Node, and headless mode
// (zetta::run_headless) drives the same Node with no renderer at all.
//
// Replication: when constructed with a ReplicationManager, the **CRDT document
// is the source of truth** and the on-disk store + this in-memory tree are a
// projection of it. Local DDL/row mutations are captured into the document; on
// each tick the node reconciles the projection to match the document (applying
// changes merged from peers) and persists the document to disk. With no
// ReplicationManager (the default), a Node is a standalone local store.
//
// A Node is also the **audit sink** (FEAT-9): it owns the immutable, append-only
// audit trail and records a governed action (graph change / deploy / data
// access) as an AuditEvent. It implements AuditSink so the entities it owns (a
// Project's graph CRUD) can append events without depending on Node directly.
class Node : public governance::AuditSink {
public:
    explicit Node(NodeConfig config, zetta::cluster::ReplicationManager* replication = nullptr);

    // Out-of-line so the unique_ptr<compute::JobCoordinator> member (held as an
    // incomplete type here, to keep the heavy compute/cluster headers out of this
    // widely-included header) is destroyed where the complete type is visible.
    ~Node();

    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // Bring the node up (open data_dir, load the replicated document, reconcile).
    // Returns false without changing state if already running.
    bool start();

    // Advance background work, including reconciling the local projection with
    // the replicated document when it has changed. Cheap and non-blocking.
    void tick();

    // --- The owner-thread gate (#624) -------------------------------------
    //
    // The Node tree is owned by ONE thread: the thread that constructed the
    // node and drives tick() (the headed frame loop, or the headless tick
    // loop). run_on_owner is how work running on ANOTHER thread — a
    // background-action worker driving a long engine run off the UI thread —
    // touches the tree safely: called on the owner thread it runs `work`
    // inline (zero cost for every existing synchronous caller); called
    // anywhere else it queues `work` and blocks the calling thread until the
    // owner loop has executed it (tick() drains the queue right where
    // reconcile already runs). The same decision as the API surface's
    // MainThreadBridge (docs/api/registry-design.md), moved down to the Node
    // so engine runners can be driven from a worker.
    //
    // `work` must be quick (it executes inside a tick) and must not throw.
    // Calling from a non-owner thread requires a live owner loop — EXCEPT on
    // a node no loop has ever ticked: with no owner to drain the queue the
    // work runs inline (the pre-gate contract — a harness driving nodes from
    // its own threads arranges exclusive access itself; both apps adopt an
    // owner on their first loop pass, before any background action exists).
    // The wait rides the runtime seam (async_event + block_on) — no
    // caller-facing condition variables.
    void run_on_owner(const std::function<void()>& work);

    // True when the calling thread is the node's owner thread (the thread
    // that constructed it, refreshed by tick() so a node handed to another
    // loop follows its ticker).
    bool on_owner_thread() const;

    // The owner loop's drain point: execute every queued run_on_owner work
    // item on the calling thread. tick() calls it every pass; call it directly
    // ONLY from the owner thread, and only where a loop must pump the gate
    // without a full tick (e.g. a teardown path joining a gated background
    // worker — see AsyncChurnRun's destructor).
    void drain_owner_work();

    // Bring the node down cleanly (flush, close). Safe to call when stopped.
    void stop();

    // CRUD for databases. create/rename yield an empty handle / false on an
    // empty name or a name clash; the UI surfaces that inline. An optional
    // replication strategy (ENT-35) is recorded + persisted + replicated; the
    // default ReplicateToAll preserves current behavior. Tables created in the
    // database inherit it unless they override (see Database::create_table).
    std::shared_ptr<zetta::data::Database> create_database(
        std::string name, zetta::data::ReplicationStrategy replication_strategy = {});
    std::shared_ptr<zetta::data::Database> find_database(const std::string& name) const;
    bool drop_database(const std::string& name);
    bool rename_database(const std::string& from, const std::string& to);
    std::vector<std::string> database_names() const;
    std::size_t database_count() const { return databases_.size(); }

    // CRUD for projects (the central, user-managed AI artifact — see
    // project.hpp / docs/pipeline/projects.md), mirroring the database CRUD shape.
    // create stamps created/updated and takes an optional owner key;
    // create/rename yield an empty handle / false on an empty name or a clash.
    std::shared_ptr<zetta::pipeline::Project> create_project(std::string name,
                                            std::string owner = {});
    std::shared_ptr<zetta::pipeline::Project> find_project(const std::string& name) const;
    bool drop_project(const std::string& name);
    bool rename_project(const std::string& from, const std::string& to);
    std::vector<std::string> project_names() const;
    std::size_t project_count() const { return projects_.size(); }

    // This node's id (its cluster id + replication actor id), stable across
    // restarts (persisted in data_dir/node_id). Set by start(); empty before it.
    const std::string& node_id() const { return node_id_; }

    // The cluster membership this node currently observes: the sorted, unique set
    // of node ids from the replicated node:<id> keys (the source of
    // system.nodes and the #517 rebalance baseline). A standalone node (no
    // replication) reports just itself; before start() it is empty. Used by the
    // add_nodes(Node&, …) overload to feed the previous membership into the
    // between-jobs rebalance (#519).
    std::vector<std::string> cluster_members() const;

    // Attach this running node's **compute plane** to its real Cluster: construct
    // and start a long-lived `compute::JobCoordinator` over it (ARC-11 / #579, the
    // SPMD job-submission "spine"), reachable via job_coordinator(). The
    // coordinator owns the collectives + the distributed work-stealing scheduler
    // bound to `cluster`'s signal plane, so a job submitted through it runs SPMD
    // across the live cluster membership.
    //
    // Called once after both the node and the cluster are up (main wires it right
    // after cluster.start()), because main constructs the Node before the Cluster
    // — so the cluster can't be a constructor argument. `worker_count` is the
    // coordinator's local pool size; 0 (the default) picks a sensible default from
    // the hardware concurrency (leaving a core for the loop + network thread). If
    // the node is already running the coordinator starts immediately; otherwise it
    // starts on the next start(). `cluster` must outlive the node (or stop()/a
    // detach must run first) — stop() tears the coordinator down. Calling it again
    // replaces the attachment.
    void attach_cluster(zetta::cluster::Cluster& cluster, unsigned worker_count = 0);

    // This node's long-lived JobCoordinator over its real cluster, or nullptr when
    // no cluster has been attached (a standalone node, or before attach_cluster).
    // The single entry the native distributed compute algorithms submit work
    // through; see compute/job_coordinator.hpp / docs/compute/job-coordinator.md.
    compute::JobCoordinator* job_coordinator() { return coordinator_.get(); }
    const compute::JobCoordinator* job_coordinator() const {
        return coordinator_.get();
    }

    // This node's `run.launch` listener (#601) — the coordinated cross-process
    // SPMD launch seam, wired by attach_cluster as the coordinator's signal
    // fallback and drained from tick(). An initiator fans a launch out through
    // it; each peer's copy co-runs the registered kind on its owner thread.
    // nullptr when no cluster is attached (like job_coordinator()). See
    // compute/coordinated_launch.hpp / docs/compute/coordinated-launch.md.
    compute::CoordinatedLaunchListener* coordinated_launch() {
        return launch_listener_.get();
    }

    // Deployment (ENT-6 / FEAT-4): place a validated execution graph onto a
    // target and record it. R1 supports a **Local** target only; deploy_graph
    // returns an empty handle for a non-local target or an empty graph. The
    // recorded deployment is replicated + on disk and queryable via
    // deployments() / find_deployment() (and the system.deployments SQL
    // table). The graph is recorded as a summary (name + node/edge counts) since
    // the IR has no serialized form yet. Reconcile/self-heal is a later slice.
    std::shared_ptr<zetta::pipeline::Deployment> deploy_graph(const ir::ExecutionGraph& graph,
                                             zetta::pipeline::DeploymentTarget target,
                                             std::string graph_name);
    std::shared_ptr<zetta::pipeline::Deployment> find_deployment(const std::string& id) const;
    std::vector<std::shared_ptr<zetta::pipeline::Deployment>> deployments() const {
        return deployments_;
    }
    std::size_t deployment_count() const { return deployments_.size(); }

    // CRUD for runs (one execution of a deployed graph — see run.hpp /
    // docs/pipeline/runs.md). Runs have no human name: they are keyed by id, so find /
    // drop take the id. create stamps created/updated, starts Pending, and takes
    // optional project + graph + deployment references (the deployment ref ties a
    // run to the placement the reconcile loop is keeping alive). The "update"
    // half of the slice is the mutators on the returned Run handle (set_status /
    // add_log / set_outputs / set_lineage), which capture into the CRDT and
    // persist. runs() is in a stable order (created, then id).
    std::shared_ptr<zetta::pipeline::Run> create_run(std::string project = {},
                                    std::string graph = {},
                                    std::string deployment = {});
    std::shared_ptr<zetta::pipeline::Run> find_run(const std::string& id) const;
    bool drop_run(const std::string& id);
    std::vector<std::shared_ptr<zetta::pipeline::Run>> runs() const { return runs_; }
    std::size_t run_count() const { return runs_.size(); }

    // CRUD for models (a trained model registered on the node — see model.hpp /
    // docs/ml/models.md; ENT-14 / FEAT-6). Models are keyed by id (like runs), so
    // find / drop take the id; the registry is models() + the
    // system.models SQL table. create stamps `created` and requires a
    // non-empty `kind` (empty handle otherwise); the caller fills the rest of the
    // ModelValue (name / feature_schema / metrics / project / run). Weights are
    // attached separately via the returned Model handle's set_weights (the body
    // is stored on disk + fingerprinted, then transfers peer-to-peer over the
    // large-object body seam). models() is in a stable order (created, then id).
    std::shared_ptr<zetta::ml::Model> create_model(zetta::ml::ModelValue value);
    std::shared_ptr<zetta::ml::Model> find_model(const std::string& id) const;
    bool drop_model(const std::string& id);
    std::vector<std::shared_ptr<zetta::ml::Model>> models() const { return models_; }
    std::size_t model_count() const { return models_.size(); }

    // CRUD for indexes (a registered index over a table — search / vector /
    // feature; ENT-39 / ENT-40, see index/index.hpp / docs/indexing/index-backend.md). Like
    // models, indexes are keyed by id (no human name), so find / drop take the id;
    // the registry is indexes() + the system.indexes SQL table. create stamps
    // `created` and requires a non-empty `table` / `backend` and at least one field
    // (empty handle otherwise — the IndexValue is validated at the schema boundary
    // on persist/reconcile). An index is metadata only: this records its
    // definition; building it is the IndexBackend seam's job (index_backend.hpp).
    // indexes() is in a stable order (created, then id).
    std::shared_ptr<zetta::index::Index> create_index(zetta::index::IndexValue value);
    std::shared_ptr<zetta::index::Index> find_index(const std::string& id) const;
    bool drop_index(const std::string& id);
    std::vector<std::shared_ptr<zetta::index::Index>> indexes() const { return indexes_; }
    std::size_t index_count() const { return indexes_.size(); }

    // The per-node registry of IndexBackends — the container + selector the run
    // path resolves an `index` step's IndexSpec to a concrete backend through
    // (#528). Where `indexes()` is the durable *records* of which indexes exist
    // (`index:<uuid>`), this is the live *machinery* that builds + serves them.
    // Constructed with the always-available reference backends registered
    // (InMemoryIndexBackend "memory_search", plus OramaCoreFulltextIndex
    // "oramacore_fulltext" when ZETTA_WITH_ORAMACORE), the way a device registry
    // enumerates the CPU device; a host may add more. The IndexStepExecutor
    // (index/index_step_executor.hpp) resolves + builds through it, and a built
    // index is queryable for the node's lifetime via
    // `index_registry().resolve(spec)->query(spec, q)`. See docs/indexing/index-step-executor.md.
    zetta::index::IndexRegistry& index_registry() { return index_registry_; }
    const zetta::index::IndexRegistry& index_registry() const { return index_registry_; }

    // THE canonical search entry point (#641) — what every surface (GUI, MCP,
    // tests) calls; there is no second query path. Resolves `request.target` to a
    // registered Search index (by id, else the newest over that table), routes it
    // through the per-partition distributed path when the table has placed
    // partitions on a clustered node (the distributed-first default; consolidated
    // registry backend otherwise — the degraded path), and returns the global
    // ranking with per-node provenance. With a recorder, the whole search is ONE
    // trace (a root `search` span + per-node children). Main-thread only. See
    // index/search.hpp / docs/indexing/node-search.md.
    zetta::index::SearchResult search(
        const zetta::index::SearchRequest& request,
        zetta::observability::SpanRecorder* recorder = nullptr,
        const zetta::observability::TraceContext* parent = nullptr);

    // The per-partition distributed full-text machinery over the attached
    // cluster (#529/#641), or nullptr when no cluster is attached (standalone /
    // before attach_cluster) — like job_coordinator(). Owned by the node: wired
    // in start_coordinator() chained over the compute plane's signal handler,
    // torn down in stop_coordinator(). Search goes through search(), not this.
    zetta::index::DistributedFulltextIndex* distributed_fulltext() {
        return distributed_fulltext_.get();
    }

    // The IndexBackend the distributed fulltext machinery builds its
    // per-partition sub-indexes into. Set BEFORE attach_cluster to override the
    // default (OramaCoreFulltextIndex when compiled in, else the exact-match
    // InMemoryIndexBackend) — the seam tests inject a reference scorer through.
    void set_search_backend(std::shared_ptr<zetta::index::IndexBackend> backend) {
        search_backend_ = std::move(backend);
    }

    // Keep this node's per-partition search sub-indexes current: for every
    // Search-kind Index entity, (re)build the local sub-indexes over the
    // partitions this node OWNS whenever that input set changed (a fingerprint
    // guards rebuilds, so the steady-state tick cost is a cheap comparison).
    // Runs from tick() on every node — which is what lets ANY node answer a
    // peer's scatter — and from search() before it queries. Main-thread only.
    // No-op until a cluster is attached.
    void reconcile_search_indexes();

    // CRUD for compute jobs (one unit of native distributed accelerated compute —
    // ARC-11 / ENT-31; see compute_job.hpp / docs/compute/compute-jobs.md). Like runs,
    // jobs have no human name: they are keyed by id, so find / drop take the id.
    // create stamps created/updated, starts Queued, takes the job `kind` and
    // optional project / graph / run / scaling_policy (reward) references, and
    // **records a `job_created` audit event**. The "update" half is the mutator on
    // the returned ComputeJob handle (set_status), which captures into the CRDT,
    // persists, and — on the first transition into a terminal state — records a
    // `job_finished` audit event (FEAT-9). jobs() is in a stable order (created,
    // then id).
    std::shared_ptr<zetta::compute::ComputeJob> create_job(zetta::compute::JobKind kind,
                                           std::string project = {},
                                           std::string graph = {},
                                           std::string run = {},
                                           std::string scaling_policy = {});
    std::shared_ptr<zetta::compute::ComputeJob> find_job(const std::string& id) const;
    bool drop_job(const std::string& id);
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs() const { return jobs_; }
    std::size_t job_count() const { return jobs_.size(); }

    // This node's durable **checkpoint store** for burst-job eviction tolerance
    // (RULE-5 / RSK-9; see compute/checkpoint.hpp / docs/compute/job-checkpoint.md), rooted
    // at `<data_dir>/checkpoints`. A long-running job body (e.g. the distributed
    // GBDT trainer) checkpoints its progress here and **resumes from the last
    // checkpoint** after the node's process restarts. Checkpoints are **local,
    // ephemeral** durability artifacts — NOT replicated state (a finished job's
    // outputs are durable elsewhere; a *replacement* burst node needing a shared
    // store is a later slice). checkpoints() lists the latest checkpoint per job —
    // the source for the read-only `system.checkpoints` view (RULE-7).
    compute::CheckpointStore& checkpoint_store() { return checkpoint_store_; }
    const compute::CheckpointStore& checkpoint_store() const {
        return checkpoint_store_;
    }
    std::vector<compute::JobCheckpoint> checkpoints() const;

    // CRUD for dataset partitions — the partition catalog (ENT-36 / TBL-13; see
    // dataset_partition.hpp / docs/data/dataset-partitions.md). A partition records
    // where a slice of a table's data physically lives (its table id, the
    // large-object body holding its bytes, the row range, column stats, and the
    // owner nodes). Like jobs, partitions have no human name: they are keyed by id,
    // so find / drop take the id. create stamps `created` (when unset) and requires
    // a non-empty `table` ref (empty handle otherwise); it **captures the
    // `part:<uuid>` key into the CRDT** so the catalog is replicate-to-all — the
    // partition is visible on every node (via dataset_partitions() + the
    // system.dataset_partitions SQL table). dataset_partitions() is in a stable
    // order (created, then id).
    std::shared_ptr<zetta::data::DatasetPartition> create_dataset_partition(
        zetta::data::DatasetPartitionValue value);
    std::shared_ptr<zetta::data::DatasetPartition> find_dataset_partition(
        const std::string& id) const;
    bool drop_dataset_partition(const std::string& id);
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> dataset_partitions() const {
        return dataset_partitions_;
    }
    std::size_t dataset_partition_count() const {
        return dataset_partitions_.size();
    }

    // The datasets on this node — the **computed-at-query-time** read source behind
    // the read-only `system.datasets` observability view (RULE-7; #585, the engine
    // half of the area-3 dataset view in epic #575). A dataset is a first-class
    // primitive even though none is stored: **one dataset == one table**, and this
    // aggregates per dataset its schema (inferred field names), row count, on-disk
    // size, storage format, replication strategy, and **partition placement across
    // the cluster** (which node owns which partition — joined live from the
    // replicate-to-all partition catalog, so every node reports the same placement).
    // Recomputed from the live tree (databases → tables) + dataset_partitions() on
    // every call — never stored/hand-synced — so it can't drift from the truth. The
    // `system` database (a virtual catalog) is not a dataset and is excluded. See
    // dataset.hpp / docs/data/system-datasets.md.
    std::vector<zetta::data::DatasetSummary> datasets() const;

    // In-memory cache of consent-gated cloud inspect results (CloudInspector;
    // docs/cloud/cloud-inspector.md). **Ephemeral, observed state with NO CRDT backing**
    // — it is neither replicated nor persisted (a live inspect is session-local;
    // see RULE-7 in CLAUDE.md). The Cloud Inspector records a report here only
    // AFTER the user consents to that one inspection; `system.cloud_status`
    // reads this cache and never triggers a cloud call. The latest report per
    // provider id replaces the previous one; cloud_inspections() is in provider
    // insertion order.
    void record_cloud_inspection(zetta::cloud::CloudInspectReport report);
    std::vector<zetta::cloud::CloudInspectReport> cloud_inspections() const {
        return cloud_inspections_;
    }

    // Cloud-burst provisioner deployment contexts detected from this node's own
    // local CLIs (detect_provisioner_configs; docs/cloud/provisioner-detection.md). **Ephemeral,
    // observed state with NO CRDT backing** — like cloud_inspections(), node-local
    // and never replicated/persisted, because it reflects *this* node's CLI auth +
    // defaults. It carries no credentials (the CLI keeps its own); it is the source
    // for the read-only `system.provisioners` view and for wiring the headed
    // Add-nodes control. The latest config per target id replaces the previous one;
    // provisioner_configs() is in target insertion order.
    void record_provisioner_config(zetta::cloud::ProvisionerConfig config);
    std::vector<zetta::cloud::ProvisionerConfig> provisioner_configs() const {
        return provisioner_configs_;
    }

    // Sample this node's **live host resource utilization** — CPU / memory / GPU
    // + VRAM / storage / network (see node_metrics.hpp / docs/cluster/node-metrics.md).
    // **Ephemeral, observed state with NO CRDT backing**: it is read straight
    // from the host's OS counters here and now, never stored or replicated (like
    // cloud_inspections()). It is the source for the read-only
    // `system.node_metrics` view (RULE-7 / #575 area 2). Non-const because the
    // CPU + network gauges are rates measured against the previous sample, which
    // the sampler updates each call. The storage gauge is taken over the node's
    // `data_dir` filesystem.
    zetta::cluster::NodeMetrics sample_node_metrics() {
        return node_metrics_sampler_.sample(config_.data_dir);
    }

    // The **cluster-wide** live telemetry snapshot: this node plus every peer
    // whose `node.metrics` event arrived within the freshness window — the source
    // the headed cluster view animates per-node gauges from (#584). Where
    // `sample_node_metrics()` measures only this host, the per-node series are
    // gathered over the signal plane (each node broadcasts its own sample, every
    // node caches what it receives — see node_telemetry.hpp / docs/gui/cluster-node-gauges.md).
    // Ephemeral + non-replicated like the sample itself; empty until a cluster is
    // attached (the exchange rides the JobCoordinator's signal-handler chain).
    std::vector<zetta::cluster::NodeTelemetrySample> telemetry_snapshot() const {
        if (!telemetry_) return {};
        return telemetry_->snapshot();
    }

    // Record one near-data compute job's **live pipeline instrumentation** (its
    // local-vs-wire byte counters — see pipeline_instrumentation.hpp). The
    // near-data job runners (zetta::ddf::run_near_data_* / zetta::ml::run_near_data_train_job)
    // call this after a job completes so the counters survive the call and surface
    // in the read-only `system.pipeline_jobs` view (#589, epic #575 area 5).
    // **Ephemeral / observed — NOT replicated** (no CRDT), like sample_node_metrics()
    // / checkpoints(): held only in memory on this node and **bounded** (the
    // most-recent kMaxPipelineJobs records are kept, oldest dropped). Thread-safe —
    // a background interactive run records while the GUI / query thread reads.
    static constexpr std::size_t kMaxPipelineJobs = 512;
    void record_pipeline_job(zetta::compute::PipelineJobInstrumentation instrumentation);

    // The recorded near-data pipeline jobs, oldest first (a snapshot copy, safe to
    // read off the node's threads). The source for `system.pipeline_jobs`.
    std::vector<zetta::compute::PipelineJobInstrumentation> pipeline_jobs() const;

    // The node's span ring (docs/architecture/observability.md, #640): every
    // traced operation — API calls, distributed runs, testkit cases — records
    // its spans here. **Ephemeral / observed — NOT replicated**, bounded, and
    // internally mutex-guarded (append from any thread, like record_pipeline_job).
    // The single source for the `system.traces` / `system.spans` read views,
    // the `list_traces` / `get_trace` capabilities, and the GUI trace pane.
    zetta::observability::SpanRecorder& span_recorder() { return span_recorder_; }
    const zetta::observability::SpanRecorder& span_recorder() const {
        return span_recorder_;
    }

    // CRUD for principals (authenticated actors — see principal.hpp /
    // docs/governance/access-control.md), mirroring the project CRUD shape. create stamps
    // `created` and takes an optional kind (default User); create/rename yield
    // an empty handle / false on an empty name or a clash. A principal owns its
    // access keys (Principal::create_key); a request's reach is the scope of the
    // key it logged in with (RULE-3).
    std::shared_ptr<zetta::governance::Principal> create_principal(
        std::string name, zetta::governance::PrincipalKind kind = zetta::governance::PrincipalKind::User);
    std::shared_ptr<zetta::governance::Principal> find_principal(const std::string& name) const;
    bool drop_principal(const std::string& name);
    bool rename_principal(const std::string& from, const std::string& to);
    std::vector<std::string> principal_names() const;
    std::size_t principal_count() const { return principals_.size(); }
    // The principals in insertion order — the live source for the
    // system.principals / system.keys read views (each principal owns
    // its keys via Principal::keys()).
    std::vector<std::shared_ptr<zetta::governance::Principal>> principals() const {
        return principals_;
    }

    // Resolve the access key presented at login (by its id), or an empty handle.
    // Keys are globally identified by UUID; this searches every principal.
    std::shared_ptr<zetta::governance::AccessKey> find_access_key(const std::string& key_id) const;

    // The V1 key-scoped access decision (RULE-3 / PERM-1 / ADR-6): a request that
    // logged in with `key_id` inherits that key's scope, so `resource` is allowed
    // iff the key permits it. Deny-by-default — an unknown/empty key id denies.
    //
    // Every decision is recorded as a `data_access` AuditEvent (allow/deny + the
    // reason), so authorize is the data-access seam of the audit trail (FEAT-9) —
    // which is why it is non-const (it appends to the immutable trail).
    zetta::governance::AccessDecision authorize(const std::string& key_id,
                             const zetta::governance::Resource& resource);

    // The immutable, append-only audit trail (ENT-23 / FEAT-9). Graph changes,
    // deploys, and data-access decisions each record an AuditEvent; the events
    // can be listed in chronological order. Events are never edited or removed.
    // record_audit_event is the public append seam (also the AuditSink override):
    // it stamps a fresh id + time, persists the event, captures it into the CRDT
    // when replicated, and appends it to the trail. The (future) request path
    // calls it to record data accesses at real read/write points.
    void record_audit_event(zetta::governance::AuditEventValue value) override;
    std::vector<std::shared_ptr<zetta::governance::AuditEvent>> audit_events() const {
        return audit_events_;
    }
    std::size_t audit_event_count() const { return audit_events_.size(); }

    // The capability decision (ENT-22): `operation` is allowed iff the key holds
    // a matching, unexpired capability as of the current time. Deny-by-default —
    // an unknown/empty key id, or no matching (live) capability, denies. The
    // verb/right counterpart to the resource overload above; the two compose (an
    // operation on a resource checks both).
    zetta::governance::AccessDecision authorize(const std::string& key_id,
                             const zetta::governance::Operation& operation) const;

    // The control plane that reconciles local deployments desired → actual and
    // self-heals their runs (see deployment_controller.hpp). Driven each tick();
    // exposed so tests/UI can introspect its counters or run a pass directly.
    zetta::pipeline::DeploymentController& deployment_controller() { return controller_; }
    const zetta::pipeline::DeploymentController& deployment_controller() const {
        return controller_;
    }

    // Execute an ExecutionGraph on this node, producing a Run (FEAT-5). A thin,
    // discoverable entry point over the standalone GraphExecutor (executor.hpp /
    // docs/pipeline/execution.md): it creates a Run, walks the graph's steps in dependency
    // order, accumulates per-step logs + typed outputs + lineage, and drives the
    // Run to Succeeded (or Failed on an empty/cyclic graph or a failing step).
    // `project` / `graph_id` label the Run. Returns the ExecutionResult (the Run
    // handle + whether it succeeded). For a custom per-step executor, use
    // GraphExecutor directly.
    zetta::pipeline::ExecutionResult run_graph(const ir::ExecutionGraph& graph,
                              std::string project = {},
                              std::string graph_id = {});

    bool running() const { return running_; }
    const NodeConfig& config() const { return config_; }

    // Seconds since start(); 0 when stopped.
    double uptime_seconds() const;

    // Monotonic heartbeat count since the most recent start().
    std::uint64_t ticks() const { return ticks_; }

    // One-line, human-readable status for logs and the UI status bar.
    std::string status_line() const;

    // Scan the data for integrity problems (corrupt large-object bodies, dangling
    // references) without changing anything. Run automatically on start() unless
    // NodeConfig::preflight is false; the last result is kept here.
    IntegrityReport check_integrity() const;
    const IntegrityReport& integrity() const { return integrity_; }

private:
    using Clock = std::chrono::steady_clock;

    // Rebuild databases_ by scanning config_.data_dir. Called from start().
    void load_databases();
    std::shared_ptr<zetta::data::Database> find_database_by_id(const std::string& id) const;

    // Rebuild projects_ by scanning config_.data_dir. Called from start().
    void load_projects();
    std::shared_ptr<zetta::pipeline::Project> find_project_by_id(const std::string& id) const;

    // Rebuild deployments_ by scanning config_.data_dir. Called from start().
    void load_deployments();

    // This node's stable id: the replication actor id when replicated, else the
    // UUID persisted in data_dir/node_id (created on first use). Set by start().
    std::string resolve_node_id();

    // Construct + start the JobCoordinator over the attached cluster (#579). A
    // no-op when no cluster is attached or one already runs; called from
    // attach_cluster (when running) and from start() (re-establishes it after a
    // stop()/start() cycle). stop_coordinator() stops + releases it.
    void start_coordinator();
    void stop_coordinator();

    // Rebuild runs_ by scanning config_.data_dir. Called from start().
    void load_runs();

    // Rebuild models_ by scanning config_.data_dir. Called from start().
    void load_models();

    // Rebuild indexes_ by scanning config_.data_dir. Called from start().
    void load_indexes();

    // Rebuild jobs_ by scanning config_.data_dir. Called from start().
    void load_jobs();

    // Rebuild dataset_partitions_ by scanning config_.data_dir. Called from start().
    void load_dataset_partitions();

    // Rebuild audit_events_ by scanning config_.data_dir/audit. Called from
    // start(). Malformed event files are skipped at the boundary (schema).
    void load_audit_events();
    std::filesystem::path audit_dir() const;

    // Rebuild principals_ by scanning config_.data_dir. Called from start().
    void load_principals();
    std::shared_ptr<zetta::governance::Principal> find_principal_by_id(const std::string& id) const;

    // --- replication (only when replication_ != nullptr) -------------------
    std::filesystem::path replication_path() const;
    void load_replication();           // read the persisted CRDT document
    void persist_replication() const;  // write the CRDT document to disk
    void migrate_to_replication();     // capture local-only data into the CRDT
    // apply the CRDT document (the given snapshot) to the projection
    void reconcile_from_replication(const nlohmann::json& snapshot);
    // Between-jobs partition rebalance on a cluster membership change (#517):
    // membership lives in the document as node:<id> keys, so a join/leave shows
    // up here. When the set differs from the last observed membership, recompute
    // rendezvous placement for the sharded partitions (rebalance_partitions),
    // which captures the new owner_nodes into the document so the bodies move to
    // their new owners. No-op on the first observation (it sets the baseline) and
    // when membership is unchanged.
    void rebalance_partitions_on_membership_change(const nlohmann::json& snapshot);
    // Per-entity reconcile passes, called by reconcile_from_replication() in the
    // order parent entities precede their children. Each scans the same CRDT
    // snapshot for its keys, drops local entries the document no longer has, and
    // materializes new/changed ones. reconcile_databases also covers tables, rows,
    // and large objects — they decode in one shared pass and their removals nest
    // under their owning database, so they don't split cleanly.
    void reconcile_databases(const nlohmann::json& snapshot);
    void reconcile_projects(const nlohmann::json& snapshot);
    void reconcile_inputs(const nlohmann::json& snapshot);
    void reconcile_graphs(const nlohmann::json& snapshot);
    void reconcile_deployments(const nlohmann::json& snapshot);
    void reconcile_runs(const nlohmann::json& snapshot);
    void reconcile_models(const nlohmann::json& snapshot);
    void reconcile_indexes(const nlohmann::json& snapshot);
    void reconcile_jobs(const nlohmann::json& snapshot);
    void reconcile_partitions(const nlohmann::json& snapshot);
    void reconcile_principals(const nlohmann::json& snapshot);
    void reconcile_access_keys(const nlohmann::json& snapshot);
    void reconcile_audit_events(const nlohmann::json& snapshot);
    // Materialize (no capture) — used by reconciliation:
    std::shared_ptr<zetta::data::Database> materialize_database(
        std::string id, std::string name, zetta::data::ReplicationStrategy strategy = {});
    bool materialize_drop_database(const std::string& id);
    std::shared_ptr<zetta::pipeline::Project> materialize_project(std::string id,
                                                 std::string name,
                                                 std::string created,
                                                 std::string updated,
                                                 std::string owner);
    bool materialize_drop_project(const std::string& id);
    std::shared_ptr<zetta::pipeline::Deployment> materialize_deployment(
        std::string id, std::string graph_name, zetta::pipeline::DeploymentTarget target,
        std::string node_id, std::size_t node_count, std::size_t edge_count,
        std::string created);
    bool materialize_drop_deployment(const std::string& id);
    std::shared_ptr<zetta::pipeline::Run> materialize_run(std::string id, zetta::pipeline::RunValue value);
    bool materialize_drop_run(const std::string& id);
    std::shared_ptr<zetta::ml::Model> materialize_model(std::string id, zetta::ml::ModelValue value);
    bool materialize_drop_model(const std::string& id);
    std::shared_ptr<zetta::index::Index> materialize_index(std::string id, zetta::index::IndexValue value);
    bool materialize_drop_index(const std::string& id);
    std::shared_ptr<zetta::compute::ComputeJob> materialize_job(std::string id,
                                                zetta::compute::ComputeJobValue value);
    bool materialize_drop_job(const std::string& id);
    std::shared_ptr<zetta::data::DatasetPartition> materialize_dataset_partition(
        std::string id, zetta::data::DatasetPartitionValue value);
    bool materialize_drop_dataset_partition(const std::string& id);
    std::shared_ptr<zetta::governance::Principal> materialize_principal(std::string id,
                                                     zetta::governance::PrincipalKind kind,
                                                     std::string name,
                                                     std::string created);
    bool materialize_drop_principal(const std::string& id);
    // Apply a replicated audit event (a peer's, or one rebuilt from the document)
    // by id, without capturing. Append-only — there is no drop counterpart.
    std::shared_ptr<zetta::governance::AuditEvent> materialize_audit_event(std::string id,
                                                        zetta::governance::AuditEventValue value);

    NodeConfig config_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone (not replicated)
    std::string node_id_;              // this node's stable UUID (set by start())
    std::string last_seen_version_;    // CRDT version last reconciled/persisted
    std::vector<std::string> last_membership_;  // node:<id> set last seen (#517)
    IntegrityReport integrity_;        // last preflight result
    bool running_ = false;
    Clock::time_point started_at_{};
    std::uint64_t ticks_ = 0;
    std::vector<std::shared_ptr<zetta::data::Database>> databases_;      // insertion order
    std::vector<std::shared_ptr<zetta::pipeline::Project>> projects_;        // insertion order
    std::vector<std::shared_ptr<zetta::pipeline::Deployment>> deployments_;  // sorted by created
    std::vector<std::shared_ptr<zetta::pipeline::Run>> runs_;                // sorted (created, id)
    std::vector<std::shared_ptr<zetta::ml::Model>> models_;            // sorted (created, id)
    std::vector<std::shared_ptr<zetta::index::Index>> indexes_;           // sorted (created, id)
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs_;         // sorted (created, id)
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>>
        dataset_partitions_;                                // sorted (created, id)
    std::vector<zetta::cloud::CloudInspectReport> cloud_inspections_;     // ephemeral; not replicated
    std::vector<zetta::cloud::ProvisionerConfig> provisioner_configs_;    // ephemeral; not replicated
    zetta::cluster::NodeMetricsSampler node_metrics_sampler_;               // ephemeral; not replicated
    // Live near-data pipeline instrumentation (#589): a bounded, in-memory ring of
    // the most-recent recorded jobs' local-vs-wire counters, the source for the
    // `system.pipeline_jobs` view. Ephemeral / observed; NOT replicated. Guarded by
    // its own mutex (mutable so pipeline_jobs() can lock from a const method) — a
    // background run records while a query thread reads.
    mutable std::mutex pipeline_jobs_mutex_;
    std::vector<zetta::compute::PipelineJobInstrumentation> pipeline_jobs_;
    // The node's span ring (#640): bounded, internally mutex-guarded, ephemeral /
    // observed; NOT replicated. See span_recorder() above.
    zetta::observability::SpanRecorder span_recorder_;
    // The owner-thread gate (#624, see run_on_owner above). The mutex is the
    // contained cross-thread handoff of the queue itself (the cluster-outbox
    // pattern the coroutines-first convention sanctions) — never exposed.
    // `signal_completed` type-erases the waiter's runtime::async_event so this
    // header stays free of the runtime seam's backend headers.
    struct OwnerWork {
        std::function<void()> work;
        std::function<void()> signal_completed;
    };
    std::atomic<std::thread::id> owner_thread_;
    mutable std::mutex owner_work_mutex_;
    std::deque<OwnerWork> owner_work_queue_;
    std::vector<std::shared_ptr<zetta::governance::Principal>> principals_;    // insertion order
    std::vector<std::shared_ptr<zetta::governance::AuditEvent>> audit_events_; // sorted (time, id)
    zetta::index::IndexRegistry index_registry_;            // IndexBackend container (#528)
    // Durable, local (non-replicated) checkpoint store for eviction tolerance
    // (RULE-5 / #468); rooted at <data_dir>/checkpoints, set in the ctor.
    compute::FileCheckpointStore checkpoint_store_;
    zetta::pipeline::DeploymentController controller_{*this};  // control plane (driven by tick())
    // The compute plane over the real cluster (#579): non-owning pointer to the
    // attached Cluster (null = no cluster), the requested worker count (0 = auto),
    // and the long-lived coordinator (null until attach_cluster + a running node).
    // Held as an incomplete type via unique_ptr; destroyed in stop()/~Node before
    // the cluster, so its handler unwind never touches a dead cluster.
    zetta::cluster::Cluster* cluster_ = nullptr;
    unsigned compute_workers_ = 0;
    // Cluster-wide live telemetry over the signal plane (#584): the cache of this
    // node's + peers' `node.metrics` events, wired as the coordinator's signal
    // handler fallback. Declared BEFORE coordinator_ so it outlives it (the
    // coordinator holds it as a raw fallback pointer); created in
    // start_coordinator(), and tick() broadcasts this node's sample through it.
    // `last_telemetry_sample_` throttles that broadcast to ~1 Hz. Ephemeral; not
    // replicated.
    std::unique_ptr<zetta::cluster::NodeTelemetryExchange> telemetry_;
    std::chrono::steady_clock::time_point last_telemetry_sample_{};
    // The coordinated cross-process launch listener (#601): the `run.launch`
    // seam, wired as the coordinator's signal-handler fallback (delegating
    // everything else to telemetry_, which is why it is declared between the
    // two: it must outlive the coordinator that points at it and die before
    // the telemetry it points at). Created in start_coordinator() with the
    // built-in runners registered (churn first); drained from tick() so the
    // co-run's blocking collectives execute on this owner thread, never the
    // network thread that delivered the signal.
    std::unique_ptr<compute::CoordinatedLaunchListener> launch_listener_;
    std::unique_ptr<compute::JobCoordinator> coordinator_;
    // The distributed search plane (#641): the per-partition fulltext machinery
    // over the attached cluster, chained over the coordinator's signal handler —
    // so EVERY clustered node serves `fulltext.query` scatters. Declared after
    // coordinator_ (destroyed first: its handler unwind restores the compute
    // plane's handler); created in start_coordinator(), reset in
    // stop_coordinator(). `search_backend_` optionally overrides the sub-index
    // backend (set_search_backend, before attach); `search_build_fingerprints_`
    // guards reconcile_search_indexes() rebuilds (spec key -> the input
    // fingerprint last built).
    std::shared_ptr<zetta::index::IndexBackend> search_backend_;
    std::unique_ptr<zetta::index::DistributedFulltextIndex> distributed_fulltext_;
    std::map<std::string, std::string> search_build_fingerprints_;
};

}  // namespace zetta
