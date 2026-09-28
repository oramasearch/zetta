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
#include <unordered_map>
#include <vector>

#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/consent.hpp"
#include "zetta/governance/grant.hpp"
#include "zetta/cloud/cloud_inspector.hpp"
#include "zetta/cloud/provisioner_config.hpp"
#include "zetta/compute/checkpoint.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/virtual_table.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_controller.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/index_registry.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/solutions/monitor.hpp"
#include "zetta/workspace/workspace_profile.hpp"
#include "zetta/billing/plan.hpp"
#include "zetta/cluster/node_metrics.hpp"
#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/distributed_select.hpp"
#include "zetta/solutions/audit_ledger.hpp"
#include "zetta/governance/policy.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/governance/token.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/governance/user_role.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/solutions/experiment.hpp"

namespace zetta::cluster { class Cluster; }
namespace zetta::pipeline { struct ExecutionResult; }
namespace zetta::ir { class ExecutionGraph; }
namespace zetta::compute { class JobCoordinator; }
namespace zetta::compute { class CoordinatedLaunchListener; }

namespace zetta::cluster { class ReplicationManager; }
namespace zetta::pubsub { class PubSub; }



namespace zetta {



// Where a node's persistent state lives and how it presents itself. More
// fields (listen address, replication peers, cache sizes) land alongside the
// storage + query engine.
struct NodeConfig {
    std::string data_dir = "./zetta-data";  // database directory on disk
    bool preflight = true;  // run an integrity check on start() (--no-preflight)
    // Consent gating for `requires_consent` capabilities (#621) — **OFF by
    // default** (#995). The two approval surfaces the refusal points a caller
    // at are incomplete: the web console's /governance page has no consent
    // list at all, and the desktop app's is on Admin & audit, not Governance.
    // With no reachable approver a one-person node cannot approve anything it
    // requests, so the gate is a dead end rather than a two-person rule. Off,
    // a gated capability executes like any other call and the bypass is
    // audited. `--governance` (or ZETTA_GOVERNANCE=1) turns enforcement on.
    bool enforce_consent = false;
    // In-memory pub/sub (#1024, docs/pubsub/). On by default; its limits are
    // detected from the environment and logged at start. Topic names matching
    // one of these prefixes are auto-created on first use (D16); empty = off.
    bool pubsub = true;
    std::vector<std::string> pubsub_auto_create_prefixes;
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
// each tick the node reconciles changed keys into the projection and marks the
// document for at-most-once-per-second persistence; stop() forces a final save.
// With no ReplicationManager (the default), a Node is a standalone local store.
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
    // Calling from a non-owner thread requires a live owner loop — and the
    // owner is adopted LAZILY, at the first tick(), never at construction. A
    // node no loop has ever ticked is UNGATED: the work runs inline on the
    // caller (the pre-gate contract — a harness driving nodes from its own
    // threads without ticking them arranges exclusive access itself; both
    // apps tick from their first loop pass, before any background action
    // exists, so interactive runs always gate). Once a loop HAS adopted the
    // node it must keep ticking (or drain) while gated workers run. The wait
    // rides the runtime interface (async_event + block_on) — no caller-facing
    // condition variables.
    void run_on_owner(const std::function<void()>& work);
    // How run_on_owner wakes an idle owner loop so the queued work runs now,
    // not after the loop's idle wait (#1063: every offloaded API call's gate
    // and the pub/sub authorizer queue here). Called under the queue's lock, so
    // clearing it ({}) guarantees no call is in flight afterwards. Unset = the
    // work waits for the next tick.
    void set_owner_wakeup(std::function<void()> wakeup);

    // True when the calling thread is the node's owner thread (adopted at
    // the first tick(), refreshed every tick so a node handed to another
    // loop follows its ticker; false on a never-ticked node).
    bool on_owner_thread() const;

    // The owner loop's drain point: execute every queued run_on_owner work
    // item on the calling thread. tick() calls it every pass; call it directly
    // ONLY from the owner thread, and only where a loop must pump the gate
    // without a full tick (e.g. a teardown path joining a gated background
    // worker — see AsyncChurnRun's destructor).
    void drain_owner_work();

    // Bring the node down cleanly (flush, close). Safe to call when stopped.
    // Flushes pub/sub topics first, within their shutdown budget (#1031).
    void stop();

    // The node's pub/sub (#1024); null while stopped or when disabled.
    zetta::pubsub::PubSub* pubsub() const { return pubsub_.get(); }

    // CRUD for databases. create/rename yield an empty handle / false on an
    // empty name or a name clash; the UI surfaces that inline. An optional
    // replication strategy (ENT-35) is recorded + persisted + replicated; the
    // default ReplicateToAll preserves current behavior. Tables created in the
    // database inherit it unless they override (see Database::create_table).
    std::shared_ptr<zetta::data::Database> create_database(
        std::string name, zetta::data::ReplicationStrategy replication_strategy = {});
    std::shared_ptr<zetta::data::Database> find_database(const std::string& name) const;
    bool drop_database(const std::string& name);

    // Adopt a database + table another node already created and captured, by
    // their ids and **without capturing them again** — the reconcile path, made
    // reachable ahead of the gossip that will deliver the same `db:`/`tbl:`
    // keys. Returns the table (the existing handle when it is already present),
    // or an empty handle if the database could not be materialized.
    //
    // The distributed connector ingest (#833 section 7) is why this is public:
    // every participant must land its tile into the SAME table the initiator
    // minted, and a participant executing a `run.launch` runs on the very
    // thread that would otherwise deliver the `tbl:` key — so it cannot wait
    // for gossip, and creating its own table would mint a second id for one
    // logical dataset. Idempotent; safe to call after the key has arrived.
    std::shared_ptr<zetta::data::Table> adopt_replicated_table(
        const std::string& database_id, const std::string& database_name,
        const std::string& table_id, const std::string& table_name,
        bool partition_only);

    bool rename_database(const std::string& from, const std::string& to);
    std::vector<std::string> database_names() const;
    std::size_t database_count() const { return databases_.size(); }

    // CRUD for projects (the central, user-managed AI artifact — see
    // project.hpp / docs/pipeline/projects.md), mirroring the database CRUD shape.
    // create stamps created/updated and takes an optional owner key;
    // create/rename yield an empty handle / false on an empty name or a clash.
    std::shared_ptr<zetta::pipeline::Project> create_project(std::string name,
                                            std::string owner = {},
                                            std::string description = {});
    std::shared_ptr<zetta::pipeline::Project> find_project(const std::string& name) const;
    // drop removes the project with its inputs and graphs — from the replicated
    // document (so every peer drops them) and from disk. Runs and models are
    // NOT touched: they are records of what happened, keep the project's id as
    // a plain label, and stay listed (#1009, docs/pipeline/projects.md).
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
    // SPMD launch path, wired by attach_cluster as the coordinator's signal
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

    // Run experiment tracking (#1137). These owner-thread entry points update
    // the replicated Run record. Metric batches also sample available host
    // gauges and publish JSON to exposed topic run-metrics.<run>.
    bool log_run_metrics(const std::string& run, std::uint64_t step,
                         std::map<std::string, double> values,
                         std::string time = {}, std::string* error = nullptr);
    bool log_run_params(const std::string& run, nlohmann::json params,
                        std::string* error = nullptr);
    bool log_run_artifact(const std::string& run, nlohmann::json artifact,
                          std::string* error = nullptr);

    // CRUD for models (a trained model registered on the node — see model.hpp /
    // docs/ml/models.md; ENT-14 / FEAT-6). Models are keyed by id (like runs), so
    // find / drop take the id; the registry is models() + the
    // system.models SQL table. create stamps `created` and requires a
    // non-empty `kind` (empty handle otherwise); the caller fills the rest of the
    // ModelValue (name / feature_schema / metrics / project / run). Weights are
    // attached separately via the returned Model handle's set_weights (the body
    // is stored on disk + fingerprinted, then transfers peer-to-peer over the
    // large-object body transfer path). models() is in a stable order (created, then id).
    std::shared_ptr<zetta::ml::Model> create_model(zetta::ml::ModelValue value);
    std::shared_ptr<zetta::ml::Model> find_model(const std::string& id) const;
    bool drop_model(const std::string& id);
    std::vector<std::shared_ptr<zetta::ml::Model>> models() const { return models_; }
    std::size_t model_count() const { return models_.size(); }

    // One replicated drift and guardrail monitor per solution (#1106). create
    // validates histogram baselines and declared bounds, stamps its timestamps,
    // and refuses a duplicate solution. find accepts either the monitor uuid or
    // the solution name. Monitor mutations publish new breach transitions to
    // the node's internal solution-events topic.
    std::shared_ptr<zetta::solutions::Monitor> create_monitor(
        zetta::solutions::MonitorSpec spec,
        std::string* error = nullptr);
    std::shared_ptr<zetta::solutions::Monitor> find_monitor(
        const std::string& id_or_solution) const;
    bool drop_monitor(const std::string& id);
    std::vector<std::shared_ptr<zetta::solutions::Monitor>> monitors() const {
        return monitors_;
    }
    std::size_t monitor_count() const { return monitors_.size(); }

    // CRUD for Workspace Profiles (the "Discover Opportunities" input — see
    // workspace_profile.hpp / docs/workspace/; #886). A top-level replicated
    // entity keyed by id (like models); the registry is workspace_profiles() +
    // the system.workspace_profiles SQL table. create stamps `created` and
    // requires a non-empty `owner` (empty handle otherwise); edits go through the
    // returned handle's set_content. Stable order (created, then id).
    std::shared_ptr<zetta::workspace::WorkspaceProfile> create_workspace_profile(
        zetta::workspace::WorkspaceProfileValue value);
    std::shared_ptr<zetta::workspace::WorkspaceProfile> find_workspace_profile(
        const std::string& id) const;
    bool drop_workspace_profile(const std::string& id);
    std::vector<std::shared_ptr<zetta::workspace::WorkspaceProfile>>
    workspace_profiles() const { return workspace_profiles_; }
    std::size_t workspace_profile_count() const {
        return workspace_profiles_.size();
    }

    // CRUD for Plans (the Billing/Usage quota config — see plan.hpp /
    // docs/billing/; Conflict D). A top-level replicated entity keyed by id; the
    // registry is plans() + the system.plans SQL table (usage is metered live in
    // system.usage). create stamps `created` and requires a non-empty `name`.
    std::shared_ptr<zetta::billing::Plan> create_plan(zetta::billing::PlanValue value);
    std::shared_ptr<zetta::billing::Plan> find_plan(const std::string& id) const;
    bool drop_plan(const std::string& id);
    std::vector<std::shared_ptr<zetta::billing::Plan>> plans() const { return plans_; }
    std::size_t plan_count() const { return plans_.size(); }

    // The shared holdout / experiment manager (#1105): replicated experiment
    // definitions and per-entity assignments, decide-before-action enforcement,
    // balance checks, erosion, and uplift-training rows. The Node's owner-thread
    // rule applies to every call.
    zetta::solutions::ExperimentManager& experiment_manager() {
        return experiment_manager_;
    }
    const zetta::solutions::ExperimentManager& experiment_manager() const {
        return experiment_manager_;
    }

    // The node's CRDT document manager (null when standalone/unreplicated). Exposed
    // so a new replicated entity kind — agents (`agent:<id>`) — can capture + read
    // its keys via `agent::AgentStore` without a bespoke method per verb here. Prefer
    // a typed accessor above where one exists; a served node always has one (main.cpp).
    zetta::cluster::ReplicationManager* replication() { return replication_; }

    // CRUD for indexes (a registered index over a table — search / vector /
    // feature; ENT-39 / ENT-40, see index/index.hpp / docs/indexing/index-backend.md). Like
    // models, indexes are keyed by id (no human name), so find / drop take the id;
    // the registry is indexes() + the system.indexes SQL table. create stamps
    // `created` and requires a non-empty `table` / `backend` and at least one field
    // (empty handle otherwise — the IndexValue is validated at the schema boundary
    // on persist/reconcile). An index is metadata only: this records its
    // definition; building it is the IndexBackend implementation's job.
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

    // The scatter-gather SELECT plane over the attached cluster (#833 section 5),
    // or nullptr when no cluster is attached (standalone / before attach_cluster).
    // Owned by the node: wired in start_coordinator() chained over the search
    // plane's signal handler, torn down in stop_coordinator(). SQL goes through
    // the Executor, not this. See query/distributed_select.hpp.
    zetta::query::DistributedSelect* distributed_select() {
        return distributed_select_.get();
    }

    // The IndexBackend the distributed fulltext machinery builds its
    // per-partition sub-indexes into. Set BEFORE attach_cluster to override the
    // default (OramaCoreFulltextIndex when compiled in, else the exact-match
    // InMemoryIndexBackend) — tests inject a reference scorer through it.
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

    // The remote consent ledger (#621; docs/api/consent.md): the challenges
    // consent-gated capabilities issue over the control surface, awaiting a
    // distinctly-privileged approval. **Ephemeral, owner-thread-only, with NO
    // CRDT backing** — consent is per-action and never remembered (the cloud
    // hard rule), so challenges neither persist nor replicate. Source for the
    // read-only `system.consent_requests` view and the headed approval surface.
    zetta::governance::ConsentLedger& consent_ledger() { return consent_ledger_; }
    const zetta::governance::ConsentLedger& consent_ledger() const {
        return consent_ledger_;
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
    // reason), so authorize is the data-access boundary of the audit trail (FEAT-9) —
    // which is why it is non-const (it appends to the immutable trail).
    zetta::governance::AccessDecision authorize(const std::string& key_id,
                             const zetta::governance::Resource& resource);

    // The immutable, append-only audit trail (ENT-23 / FEAT-9). Graph changes,
    // deploys, and data-access decisions each record an AuditEvent; the events
    // can be listed in chronological order. Events are never edited or removed.
    // record_audit_event is the public append API (also the AuditSink override):
    // it stamps a fresh id + time, persists the event, captures it into the CRDT
    // when replicated, and appends it to the trail. The (future) request path
    // calls it to record data accesses at real read/write points.
    void record_audit_event(zetta::governance::AuditEventValue value) override;
    std::vector<std::shared_ptr<zetta::governance::AuditEvent>> audit_events() const {
        return audit_events_;
    }
    std::size_t audit_event_count() const { return audit_events_.size(); }

    // The solutions reward ledger (#1107). Both methods append a fresh,
    // immutable record; outcomes reference an existing action and never update
    // it. There is deliberately no mutation or deletion API.
    std::shared_ptr<zetta::solutions::LedgerEntry> record_action(
        zetta::solutions::ActionLedgerValue value);
    std::shared_ptr<zetta::solutions::LedgerEntry> record_outcome(
        zetta::solutions::OutcomeLedgerValue value);
    std::shared_ptr<zetta::solutions::LedgerEntry> find_ledger_entry(
        const std::string& id) const;
    std::vector<std::shared_ptr<zetta::solutions::LedgerEntry>> ledger_entries() const {
        return ledger_entries_;
    }
    std::size_t ledger_entry_count() const {
        return ledger_entries_.size();
    }

    // The capability decision (ENT-22): `operation` is allowed iff the key holds
    // a matching, unexpired capability as of the current time. Deny-by-default —
    // an unknown/empty key id, or no matching (live) capability, denies. The
    // verb/right counterpart to the resource overload above; the two compose (an
    // operation on a resource checks both).
    zetta::governance::AccessDecision authorize(const std::string& key_id,
                             const zetta::governance::Operation& operation) const;

    // The capability decision for a USER (the default-deny gate, #745): the
    // operation is allowed iff any role assigned to the user (via UserRole)
    // holds a matching, unexpired capability. Deny-by-default — an unknown or
    // disabled user, or no role granting the right, denies. This is the
    // token-resolved counterpart to the AccessKey `authorize` above: the gate
    // resolves a bearer to a user (authenticate_token), then this checks the
    // user's *effective* permissions (the union of its roles' capabilities).
    zetta::governance::AccessDecision authorize_user(
        const std::string& user_id,
        const zetta::governance::Operation& operation) const;

    // ----- Object-scoped authorization (fine-grained grants, #747) -----------
    // The object tier layered over roles (#746): the acting identity's grants
    // decide access to a SPECIFIC object (table / column / blob / MCP tool), not
    // just to a resource kind. Deny-by-default. Three identities BYPASS object
    // scoping: an empty acting id (the trusted unauthenticated-local session), a
    // legacy AccessKey (`acting_is_user` false, non-empty — it carries only coarse
    // scope/caps), and any user that holds the operation as a FLAT, unscoped right
    // (admin holds every one). Only a user whose right is exclusively object-scoped
    // is narrowed.

    // Whether the identity may perform `operation` on `object` — the general
    // single-object decision (blob / MCP tool / any Resource).
    bool authorize_object(const std::string& acting_id, bool acting_is_user,
                          const zetta::governance::Operation& operation,
                          const zetta::governance::Resource& object) const;

    // The column-level access the identity has to `database.table` under
    // `operation` ({data,read} / {data,write}) — drives the executor's table
    // denial + column masking (SELECT) / write rejection (INSERT).
    zetta::governance::ColumnAccess column_scope(
        const std::string& acting_id, bool acting_is_user,
        const zetta::governance::Operation& operation,
        const std::string& database, const std::string& table) const;

    // Whether the identity may invoke the MCP/REST tool `tool_name` — the per-tool
    // `mcp:call` scope. Drives tools/call denial + tools/list filtering (#747).
    bool may_call_tool(const std::string& acting_id, bool acting_is_user,
                       const std::string& tool_name) const;

    // Whether the identity may read the blob `blob_id` — `blob:read` scoped by
    // id/prefix. Drives the blob serve path (#747).
    bool authorize_blob(const std::string& acting_id, bool acting_is_user,
                        const std::string& blob_id) const;

    // Add / remove OBJECT-SCOPED grants on a role (#747): each named permission's
    // operation(s) bound to `object`, idempotently (grant is a no-op for a grant
    // already present, revoke for one absent). Unknown permission names are
    // ignored (validate at the grammar boundary with governance::is_permission).
    // false on an unknown role name. Rewrites role.json + captures into the CRDT.
    bool grant_scoped_permissions(const std::string& role_name,
                                  const std::vector<std::string>& permissions,
                                  const zetta::governance::Resource& object);
    bool revoke_scoped_permissions(const std::string& role_name,
                                   const std::vector<std::string>& permissions,
                                   const zetta::governance::Resource& object);

    // ----- Users / roles / tokens: the credential store (FEAT-9 / #744) -------
    // The foundation the rest of the identity model builds on. A User is an
    // authenticatable identity, a Role a named permission set, a Token a bearer
    // credential **stored hashed** (the plaintext is shown once), and a UserRole
    // ties a user to a role. All four are replicated through the same
    // capture/reconcile path as principals (see docs/governance/access-control.md).

    // Create a user (unique name, status Active). Empty handle on an empty name
    // or a clash. Captured into the CRDT (`user:<uuid>`) + persisted on disk.
    std::shared_ptr<zetta::governance::User> create_user(std::string name);
    std::shared_ptr<zetta::governance::User> find_user(const std::string& name) const;
    std::shared_ptr<zetta::governance::User> find_user_by_id(const std::string& id) const;
    bool drop_user(const std::string& name);
    // Enable/disable a user (a disabled user's tokens stop validating). false on
    // an unknown name.
    bool set_user_status(const std::string& name, zetta::governance::UserStatus status);
    std::vector<std::shared_ptr<zetta::governance::User>> users() const { return users_; }
    std::size_t user_count() const { return users_.size(); }

    // Create a built-in role by name ("admin"/"writer"/"reader") — empty handle
    // on an unknown built-in name or a name clash.
    std::shared_ptr<zetta::governance::Role> create_builtin_role(const std::string& name);
    // Ensure the built-in admin/writer/reader roles all exist (idempotent), and
    // reconcile any that drifted from their canonical permission set back to it —
    // used by bootstrap so the built-ins always exist and always mean the same
    // thing. Returns the number newly created (drift repairs are not counted).
    std::size_t ensure_builtin_roles();
    // Create a CUSTOM role (#746): unrestricted-scope but with NO capabilities —
    // permissions are added with grant_permissions. Empty handle on an empty name
    // or a name clash (built-in or custom). Captured into the CRDT + persisted.
    std::shared_ptr<zetta::governance::Role> create_role(const std::string& name);
    // Drop a role by name, and any user↔role assignments referencing it (mirrors
    // drop_user's cleanup). false on an unknown name.
    bool drop_role(const std::string& name);
    // Add / remove permissions (from the vocabulary, permission.hpp) on a role's
    // rights, idempotently — grant is a no-op for a right already present, revoke
    // for one absent. Unknown permission names are ignored (validate at the
    // grammar boundary with governance::is_permission). false on an unknown role
    // name. Rewrites role.json + captures the new value into the CRDT.
    bool grant_permissions(const std::string& role_name,
                           const std::vector<std::string>& permissions);
    bool revoke_permissions(const std::string& role_name,
                            const std::vector<std::string>& permissions);
    std::shared_ptr<zetta::governance::Role> find_role(const std::string& name) const;
    std::shared_ptr<zetta::governance::Role> find_role_by_id(const std::string& id) const;
    std::vector<std::shared_ptr<zetta::governance::Role>> roles() const { return roles_; }
    std::size_t role_count() const { return roles_.size(); }

    // Assign a role to a user (idempotent — the existing assignment if the pair is
    // already assigned). Empty handle if either id is unknown.
    std::shared_ptr<zetta::governance::UserRole> assign_role(const std::string& user_id,
                                                             const std::string& role_id);
    // Remove a user↔role assignment (#746). false if the pair is not assigned.
    // Drops the CRDT `user_role:` key + its on-disk folder.
    bool unassign_role(const std::string& user_id, const std::string& role_id);
    std::vector<std::shared_ptr<zetta::governance::UserRole>> user_roles() const {
        return user_roles_;
    }
    std::size_t user_role_count() const { return user_roles_.size(); }
    // The role ids assigned to a user (its effective roles).
    std::vector<std::string> roles_for_user(const std::string& user_id) const;

    // The plaintext secret surfaced once at token creation, alongside its handle.
    // The secret is never stored (only its salted hash) and never returned again.
    struct TokenIssue {
        std::shared_ptr<zetta::governance::Token> token;
        std::string secret;  // PLAINTEXT — shown once, never persisted
    };
    // Issue a token for a user: generates a random secret + salt, stores only the
    // hash (`token:<uuid>`), and returns the handle + plaintext. Empty handle if
    // the user id is unknown. `expires_at` empty => no TTL.
    TokenIssue create_token(const std::string& user_id,
                            zetta::governance::TokenType type,
                            std::string expires_at = {});
    std::shared_ptr<zetta::governance::Token> find_token_by_id(const std::string& id) const;
    // Revoke a token (a revoked token never validates). false on an unknown id.
    bool revoke_token(const std::string& id);
    std::vector<std::shared_ptr<zetta::governance::Token>> tokens() const { return tokens_; }
    std::size_t token_count() const { return tokens_.size(); }

    // ----- Row-level security policies (RLS, #748, epic #743 item 5) ----------
    // A Policy is a Postgres-style row-security rule over one table (a USING read
    // filter + a WITH CHECK write filter, evaluated by the SQL WHERE engine
    // against the acting identity). Replicated through the same capture/reconcile
    // path as roles (`policy:<uuid>`). The executor enforces them; see
    // docs/governance/access-control.md.

    // Create a policy (unique name). The value's `created` is stamped here. Empty
    // handle on an empty name or a name clash. Captured into the CRDT + persisted.
    std::shared_ptr<zetta::governance::Policy> create_policy(
        zetta::governance::PolicyValue value);
    // Drop a policy by name. false on an unknown name. Drops the CRDT `policy:` key
    // + its on-disk folder.
    bool drop_policy(const std::string& name);
    std::shared_ptr<zetta::governance::Policy> find_policy(const std::string& name) const;
    std::shared_ptr<zetta::governance::Policy> find_policy_by_id(const std::string& id) const;
    std::vector<std::shared_ptr<zetta::governance::Policy>> policies() const {
        return policies_;
    }
    std::size_t policy_count() const { return policies_.size(); }

    // ----- Virtual tables (vtable:<name>, #809 slice 3) -----------------------
    // A user-declared, read-only VIRTUAL TABLE over a CRDT key-family (see
    // data/virtual_table.hpp / docs/data/virtual-tables.md). The definition is
    // durable, replicated catalog metadata — captured under the `vtable:<name>`
    // CRDT key so it converges + survives restart, mirroring db:/tbl:. There is
    // no in-memory projection: the definitions + their rows are read live from
    // the replicated document at query time (like system.nodes reads the
    // node-list), so a peer's CREATE is visible without a reconcile pass. A
    // virtual table needs a CRDT to project over, so a standalone (non-replicated)
    // node has none and create returns false.

    // Create a virtual table (unique name). false when the node is not replicated
    // (no CRDT), the name is empty, or a virtual table by that name already
    // exists. Captures the `vtable:<name>` key into the CRDT.
    bool create_virtual_table(const zetta::data::VirtualTable& definition);
    // Drop a virtual table by name. false when not replicated or no such vtable.
    bool drop_virtual_table(const std::string& name);
    // The virtual table named `name`, read live from the replicated document, or
    // nullopt when absent / not replicated.
    std::optional<zetta::data::VirtualTable> find_virtual_table(
        const std::string& name) const;
    // Every virtual table defined in the replicated document (name order). Empty
    // when not replicated. The read source for a possible system.virtual_tables.
    std::vector<zetta::data::VirtualTable> virtual_tables() const;
    // The rows of a virtual table, composed at query time from the replicated
    // document: one row per CRDT entry under the definition's prefix — the
    // DURABLE KEY column is the id after the prefix, other DURABLE columns are
    // projected from the entry JSON (null when absent), and EPHEMERAL columns are
    // null (a resolver binding is a follow-up). Empty when not replicated.
    std::vector<nlohmann::json> virtual_table_rows(
        const zetta::data::VirtualTable& definition) const;

    // Whether the acting identity BYPASSES row-level security (sees/writes every
    // row regardless of policy). Three identities bypass, mirroring the object-
    // scoping bypass (#747): an empty acting id (the trusted unauthenticated-local
    // session), a legacy AccessKey (`acting_is_user` false, non-empty), and any
    // user that is an admin (holds the cluster:admin permission — the built-in
    // admin does). Every other user is subject to RLS. Deny-by-default: only these
    // widen; a non-bypass user with no applicable policy on a protected table sees
    // no rows.
    bool bypasses_row_security(const std::string& acting_id,
                               bool acting_is_user) const;

    // Resolve a bearer secret to the user it authenticates as, or an empty handle
    // when no live token matches (deny-by-default). Hashes + constant-time
    // compares against every non-revoked, unexpired token, and requires the
    // owning user to exist and be active. This is the boundary issue #2's gate calls.
    std::shared_ptr<zetta::governance::User> authenticate_token(std::string_view secret) const;

    // The outcome of bootstrap_owner: whether an owner was minted this call, and
    // the identifiers to log (never the secret).
    struct BootstrapResult {
        bool performed = false;   // false = no-op (store already seeded, or no secret)
        std::string user_id;
        std::string user_name;    // "owner"
        std::string token_id;
        std::string reason;       // why it was a no-op, when !performed
    };
    // Bootstrap the owner (#733): if the user store is empty, mint an "owner"
    // user, ensure the built-in roles exist, assign it "admin", and issue an
    // access token whose secret is `admin_token_secret` (the operator's, via
    // --admin-token / ZETTA_ADMIN_TOKEN). A no-op when a user already exists or
    // the secret is empty. Never logs / returns the secret — the operator already
    // holds it.
    BootstrapResult bootstrap_owner(const std::string& admin_token_secret);

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

    // Rebuild monitors_ by scanning config_.data_dir. Called from start().
    void load_monitors();

    // Rebuild workspace_profiles_ by scanning config_.data_dir. Called from start().
    void load_workspace_profiles();

    // Rebuild plans_ by scanning config_.data_dir. Called from start().
    void load_plans();

    // Rebuild indexes_ by scanning config_.data_dir. Called from start().
    void load_indexes();

    // Rebuild jobs_ by scanning config_.data_dir. Called from start().
    void load_jobs();

    // Rebuild dataset_partitions_ by scanning config_.data_dir. Called from start().
    void load_dataset_partitions();
    // Settle deferred storage writes after a reconcile pass (#952). Owner-thread
    // only, like every other tree walk.
    void flush_stores();

    // Rebuild audit_events_ by scanning config_.data_dir/audit. Called from
    // start(). Malformed event files are skipped at the boundary (schema).
    void load_audit_events();
    std::filesystem::path audit_dir() const;

    // Rebuild the immutable solutions ledger from data_dir/ledger.
    void load_ledger_entries();
    std::filesystem::path ledger_dir() const;

    // Rebuild principals_ by scanning config_.data_dir. Called from start().
    void load_principals();
    std::shared_ptr<zetta::governance::Principal> find_principal_by_id(const std::string& id) const;

    // Rebuild the credential store (users/roles/tokens/user_roles) by scanning
    // config_.data_dir for each entity's marker file. Called from start().
    // Malformed records are skipped at the boundary (schema).
    void load_users();
    void load_roles();
    void load_tokens();
    void load_user_roles();
    void load_policies();

    // --- replication (only when replication_ != nullptr) -------------------
    std::filesystem::path replication_path() const;
    void load_replication();           // read the persisted CRDT document
    void persist_replication();        // write the CRDT document to disk
    void migrate_to_replication();     // capture local-only data into the CRDT
    // apply the CRDT document (the given snapshot) to the projection
    void reconcile_from_replication(const nlohmann::json& snapshot);
    void reconcile_replication_changes(
        const nlohmann::json& snapshot,
        const std::vector<std::string>& changed_keys);
    // Between-jobs partition rebalance on a cluster membership change (#517):
    // membership lives in the document as node:<id> keys, so a join/leave shows
    // up here. When the set differs from the last observed membership, recompute
    // rendezvous placement for the sharded partitions (rebalance_partitions),
    // which captures the new owner_nodes into the document so the bodies move to
    // their new owners. No-op on the first observation (it sets the baseline) and
    // when membership is unchanged.
    void rebalance_partitions_on_membership_change(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    // Per-entity reconcile passes run in parent-before-child order. A null key
    // list applies a complete snapshot; an incremental call receives only its
    // prefix bucket and scopes removals to those keys. reconcile_databases also
    // covers tables, rows, manifests, and large objects because their removals
    // nest under their owning database.
    void reconcile_databases(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_projects(const nlohmann::json& snapshot,
                            const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_inputs(const nlohmann::json& snapshot,
                          const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_graphs(const nlohmann::json& snapshot,
                          const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_deployments(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_runs(const nlohmann::json& snapshot,
                        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_models(const nlohmann::json& snapshot,
                          const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_monitors(const nlohmann::json& snapshot,
                            const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_workspace_profiles(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_plans(const nlohmann::json& snapshot,
                         const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_indexes(const nlohmann::json& snapshot,
                           const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_jobs(const nlohmann::json& snapshot,
                        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_partitions(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_principals(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_access_keys(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_audit_events(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_ledger_entries(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_users(const nlohmann::json& snapshot,
                         const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_roles(const nlohmann::json& snapshot,
                         const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_tokens(const nlohmann::json& snapshot,
                          const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_user_roles(
        const nlohmann::json& snapshot,
        const std::vector<std::string>* changed_keys = nullptr);
    void reconcile_policies(const nlohmann::json& snapshot,
                            const std::vector<std::string>* changed_keys = nullptr);
    // Materialize (no capture) — used by reconciliation:
    std::shared_ptr<zetta::data::Database> materialize_database(
        std::string id, std::string name, zetta::data::ReplicationStrategy strategy = {});
    bool materialize_drop_database(const std::string& id);
    std::shared_ptr<zetta::pipeline::Project> materialize_project(std::string id,
                                                 std::string name,
                                                 std::string created,
                                                 std::string updated,
                                                 std::string owner,
                                                 std::string description = {});
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
    std::shared_ptr<zetta::solutions::Monitor> materialize_monitor(
        std::string id,
        zetta::solutions::MonitorValue value);
    bool materialize_drop_monitor(const std::string& id);
    void publish_monitor_breach(const zetta::solutions::MonitorBreach& breach);
    std::shared_ptr<zetta::workspace::WorkspaceProfile> materialize_workspace_profile(
        std::string id, zetta::workspace::WorkspaceProfileValue value);
    bool materialize_drop_workspace_profile(const std::string& id);
    std::shared_ptr<zetta::billing::Plan> materialize_plan(
        std::string id, zetta::billing::PlanValue value);
    bool materialize_drop_plan(const std::string& id);
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
    std::shared_ptr<zetta::governance::User> materialize_user(
        std::string id, std::string name, zetta::governance::UserStatus status,
        std::string created);
    bool materialize_drop_user(const std::string& id);
    std::shared_ptr<zetta::governance::Role> materialize_role(
        std::string id, std::string name,
        zetta::governance::RolePermissions permissions, std::string created);
    bool materialize_drop_role(const std::string& id);
    std::shared_ptr<zetta::governance::Token> materialize_token(
        std::string id, zetta::governance::TokenValue value);
    bool materialize_drop_token(const std::string& id);
    std::shared_ptr<zetta::governance::UserRole> materialize_user_role(
        std::string id, std::string user_id, std::string role_id);
    bool materialize_drop_user_role(const std::string& id);
    std::shared_ptr<zetta::governance::Policy> materialize_policy(
        std::string id, zetta::governance::PolicyValue value);
    bool materialize_drop_policy(const std::string& id);
    // Apply a replicated audit event (a peer's, or one rebuilt from the document)
    // by id, without capturing. Append-only — there is no drop counterpart.
    std::shared_ptr<zetta::governance::AuditEvent> materialize_audit_event(std::string id,
                                                        zetta::governance::AuditEventValue value);
    std::shared_ptr<zetta::solutions::LedgerEntry> materialize_ledger_entry(
        std::string id, zetta::solutions::LedgerEntryValue value);

    NodeConfig config_;
    zetta::cluster::ReplicationManager* replication_;  // null = standalone (not replicated)
    std::string node_id_;              // this node's stable UUID (set by start())
    std::string last_seen_version_;    // CRDT version last reconciled
    std::uint64_t last_replication_change_token_ = 0;
    Clock::time_point last_replication_persist_{};
    bool replication_persist_pending_ = false;
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
    std::vector<std::shared_ptr<zetta::solutions::Monitor>> monitors_; // sorted (created, id)
    std::vector<std::shared_ptr<zetta::workspace::WorkspaceProfile>>
        workspace_profiles_;                                          // sorted (created, id)
    std::vector<std::shared_ptr<zetta::billing::Plan>> plans_;         // sorted (created, id)
    zetta::solutions::ExperimentManager experiment_manager_;
    std::vector<std::shared_ptr<zetta::index::Index>> indexes_;           // sorted (created, id)
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs_;         // sorted (created, id)
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>>
        dataset_partitions_;                                // sorted (created, id)
    std::vector<zetta::cloud::CloudInspectReport> cloud_inspections_;     // ephemeral; not replicated
    std::vector<zetta::cloud::ProvisionerConfig> provisioner_configs_;    // ephemeral; not replicated
    zetta::governance::ConsentLedger consent_ledger_;                     // ephemeral; not replicated (#621)
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
    // header stays free of the runtime adapter's backend headers.
    struct OwnerWork {
        std::function<void()> work;
        std::function<void()> signal_completed;
    };
    std::atomic<std::thread::id> owner_thread_;
    mutable std::mutex owner_work_mutex_;
    std::deque<OwnerWork> owner_work_queue_;
    std::function<void()> owner_wakeup_;  // guarded by owner_work_mutex_
    std::vector<std::shared_ptr<zetta::governance::Principal>> principals_;    // insertion order
    std::vector<std::shared_ptr<zetta::governance::AuditEvent>> audit_events_; // sorted (time, id)
    // id -> event, beside audit_events_: membership in O(1) (#1070). Every
    // reconcile walks all the document's audit keys; a linear scan per key made
    // that O(n^2) per changed tick.
    std::unordered_map<std::string, std::shared_ptr<zetta::governance::AuditEvent>>
        audit_events_by_id_;
    std::vector<std::shared_ptr<zetta::solutions::LedgerEntry>>
        ledger_entries_;  // sorted (recorded_at, id)
    std::vector<std::shared_ptr<zetta::governance::User>> users_;          // credential store (#744)
    std::vector<std::shared_ptr<zetta::governance::Role>> roles_;          // named permission sets
    std::vector<std::shared_ptr<zetta::governance::Token>> tokens_;        // hashed bearer credentials
    std::vector<std::shared_ptr<zetta::governance::UserRole>> user_roles_; // user↔role assignments
    std::vector<std::shared_ptr<zetta::governance::Policy>> policies_;      // row-level security policies (#748)
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
    // In-memory pub/sub (#1024): created in start(), flushed + destroyed in stop().
    std::unique_ptr<zetta::pubsub::PubSub> pubsub_;
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
    // listener, wired as the coordinator's signal-handler fallback (delegating
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
    std::unique_ptr<zetta::query::DistributedSelect> distributed_select_;
    std::map<std::string, std::string> search_build_fingerprints_;
};

}  // namespace zetta
