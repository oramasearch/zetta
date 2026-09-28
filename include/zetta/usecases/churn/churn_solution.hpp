#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"    // data::DatasetContract (the table's columns)
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_step_executor.hpp"  // RetentionAction
#include "zetta/ddf/data_frame.hpp"       // ddf::DataFrameSchema
#include "zetta/ddf/featurize.hpp"        // ddf::FeaturizeSpec
#include "zetta/ir/execution_graph.hpp"   // AsyncChurnRun owns its graph snapshot
#include "zetta/pipeline/executor.hpp"             // ExecutionResult
#include "zetta/ml/gbdt.hpp"              // ml::GbdtParams
#include "zetta/ml/gbdt_score.hpp"        // ml::EntityScore
#include "zetta/ml/gbdt_train_job.hpp"    // ml::CalibrationConfig

// Wire the catalog churn SolutionCatalog blueprint to the REAL engine (#398;
// FEAT-4 / UC-1). This is the bridge from "the churn-prediction blueprint a user
// picks in the Deploy panel" to "a real trained Model + ranked at-risk list +
// churn drivers", running the actual featurize -> train -> explain -> rank ->
// playbook pipeline instead of the stubbed DefaultStepExecutor.
//
// The real per-step compute already exists as zetta::usecases::churn::ChurnStepExecutor (#396),
// but it is driven *by injection* into a GraphExecutor and needs a lot the seam
// can't supply on its own: a started JobCoordinator, a ComputeDevice, and this
// node's NUMERIC input shards (the churn pipeline operates purely on numeric DDF
// columns). This module is that glue, factored out of the test harness so the
// wiring is a first-class, reusable engine surface:
//
//   1. encode_churn_dataset  — map a ChurnDataset's string ids / ISO timestamps /
//      categorical event types into the numeric columns the DDF ops require (the
//      "featurize/encode step upstream" ChurnPipelineConfig documents);
//   2. churn_events_schema / churn_accounts_schema / churn_featurize_spec /
//      default_churn_gbdt_params — the standard, leakage-free churn pipeline config
//      over those encoded shards;
//   3. run_churn_solution_on_node — THE one public runner (#637): build a
//      ChurnStepExecutor over (1)+(2), inject it into a GraphExecutor, and execute
//      the churn graph on the node's live JobCoordinator (the #579 spine), returning
//      the Run plus the real artifacts (the registered Model id, the ranked list,
//      the drivers, the playbook). Every in-process churn submitter — the headed
//      Deploy panel, the governed console's churn-shaped path, the testkit cases,
//      the acceptance tests — routes through it.
//
// **No new JSON format.** Everything rides an existing validated boundary — the
// ChurnDataset is loaded/validated by load_churn_dataset, the catalog blueprint
// graph by parse_solution_catalog, and the Run / ComputeJob / GBDT-weights records
// at their own schema boundaries. This module adds no parser of its own.
//
// **SPMD across the live membership, coordinated (#601).** The underlying jobs
// are SPMD — every collective round waits for all connected peers — so on a
// multi-node cluster run_churn_solution_on_node fans the launch out first: it
// snapshots the roster, sends every peer a `run.launch` task over the signal
// plane (compute/coordinated_launch.hpp), and each participant (initiator
// included) co-runs the same graph over its own **disjoint shard** of the
// encoded dataset (entity % roster-size == its rank — union == the whole, so
// the allreduce combines to the global model without double-counting). On a
// standalone or single-member cluster the roster is just this node and the run
// is exactly the local whole-dataset run it always was.
//
// See docs/use-cases/churn-solution.md + docs/compute/coordinated-launch.md.
namespace zetta::storage { class StorageEngine; }
namespace zetta { class Node; }
namespace zetta::compute { class ComputeDevice; }
namespace zetta::compute { class CoordinatedLaunchListener; }

namespace zetta::usecases::churn {

// The proven, fast GBDT hyperparameters for the churn pipeline (30 trees, depth 3,
// 16 bins, learning rate 0.3) — light enough to run on a click yet strong enough to
// recover the churn signal on the synthetic fixture. Override via ChurnSolutionOptions.
ml::GbdtParams default_churn_gbdt_params();

// The DDF schema of the encoded numeric shards encode_churn_dataset produces — the
// encoder's output contract, which churn_featurize_spec then reads. Events:
// entity / ts / value / type. Accounts: entity / mrr / seats / tenure_days / churned.
ddf::DataFrameSchema churn_events_schema();
ddf::DataFrameSchema churn_accounts_schema();

// The standard leakage-free churn featurization graph over the encoded shards (the
// churn_features.sql shape): windowed event features (failed_payments / support
// severity / total events) at or before `as_of`, the carried static account features,
// and the `churned` label. `as_of` is in the encoder's day-number units (the value
// encode_churn_dataset returns).
ddf::FeaturizeSpec churn_featurize_spec(double as_of);

// The numeric encoding of a ChurnDataset, ready to load into DataFrames. Accounts are
// keyed to dense integer `entity` ids by file order; events carry the same key, a
// monotonic day-number `ts` (so a window `time <= as_of` is leakage-correct), the raw
// `value`, and an integer event-`type` code. `as_of` is the dataset's t0 in the same
// day-number units (so featurization windows to it).
struct ChurnNumericShards {
    std::vector<nlohmann::json> events;
    std::vector<nlohmann::json> accounts;
    double as_of = 0.0;
};
ChurnNumericShards encode_churn_dataset(const ChurnDataset& dataset);

// Knobs for a churn run that are not derived from the dataset itself.
struct ChurnSolutionOptions {
    ml::GbdtParams params = default_churn_gbdt_params();
    ml::CalibrationConfig calibration;       // opt-in held-out probability calibration
    std::size_t top_k = 0;                   // at-risk list / playbook cap (0 = all)
    std::string model_name = "churn_model";  // the registered Model's human name
    std::string project;                     // labels the Run + every ComputeJob
    std::string graph;                       // graph id label
    std::string result_database = "churn_results";
    std::string result_table_prefix = "at_risk_";
    std::string result_entity_column = "account";
    std::string result_score_column = "risk";
    // Fan the run out to the live membership when the cluster has peers (#601).
    // False = run this node's copy alone — only correct on a single-member
    // cluster (a lone submitter fails the SPMD collective rounds otherwise).
    bool coordinate = true;
    // Optional cooperative cancel flag (#624): non-owning, may be null, must
    // outlive the run. Checked between graph steps (GraphExecutor::execute);
    // a cancelled run ends Failed with a "run cancelled" log line.
    const std::atomic<bool>* cancel = nullptr;
};

// What running the churn solution on the real engine produced.
struct ChurnSolutionResult {
    zetta::pipeline::ExecutionResult execution;  // the Run handle + ok / failed_step / error
    std::string model_id;       // the registered Model id (resolvable via find_model)
    std::vector<ml::EntityScore> ranked;  // the global at-risk list, highest risk first
    std::vector<std::pair<std::string, double>>
        drivers;             // population driver ranking: feature -> mean |phi|, desc
    std::string top_driver;  // the globally dominant churn driver
    std::vector<RetentionAction> playbook;  // at-risk accounts + their dominant driver

    bool ok() const { return execution.ok; }
};

// THE canonical churn runner (#637): run a churn ExecutionGraph (the catalog
// "churn-prediction" blueprint, or any connect x2 -> clean -> model -> index ->
// serve -> act churn graph) through the REAL engine on `dataset`, over the node's
// **live** JobCoordinator — the long-lived SPMD compute "spine" a running node owns
// over its real Cluster (#579, Node::attach_cluster / Node::job_coordinator; the
// interactive "run over the cluster" front door, #580). It resolves
// node.job_coordinator(), encodes the dataset to numeric DDF shards, builds the
// standard featurize spec + GBDT params (from `options`), injects a
// ChurnStepExecutor over that coordinator + `device`, and executes `graph` on
// `node` — so the featurize / train / explain / rank jobs combine their partials
// over the node's real cluster membership through the existing collectives (which
// ride the signal plane), the produced Run references a real trained Model, the
// score job yields the ranked at-risk list, TreeSHAP yields the churn drivers, and
// everything lands on the live node (visible in system.runs / system.models). The
// dataset must be non-empty (an empty dataset fails the run at featurize/train,
// like any empty input). **No new JSON format.**
//
// This is the ONE public run entry point: the headed Deploy panel, the governed
// console's churn-shaped path, the testkit cases, and the acceptance tests all call
// it. The isolated-coordinator wiring it used to share the header with survives only
// as a clearly-named internal detail of churn_solution.cpp
// (run_churn_solution_with_coordinator).
//
// A node with no compute plane attached (a standalone node, or one not yet
// attach_cluster'd) has no live coordinator: this returns a failed ChurnSolutionResult
// with execution.error set (it provisions nothing — the caller surfaces the reason),
// so it never silently no-ops.
//
// **Coordinated membership (#601).** The underlying jobs are SPMD: every
// collective round waits for *all* connected peers (a bounded timeout). So on a
// multi-node cluster this runner first fans the launch out — a `run.launch` task
// to every roster peer over the signal plane (each co-runs the same graph over
// its own disjoint entity shard, via the runner install_churn_launch_runner
// registers) — then runs its own shard; the collectives combine the partials to
// the one global model, registered by the deterministic root and replicated to
// every node. `options.coordinate = false` opts out (single-member-correct
// only). A peer that misses the launch fails the rounds with the bounded
// timeout — observable, never silent.
ChurnSolutionResult run_churn_solution_on_node(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const ChurnDataset& dataset,
    const ChurnSolutionOptions& options = {});

// --- Running on a CONNECTOR-LANDED dataset (#947) -----------------------------
//
// The dataset a customer actually brings — a warehouse query landed as a
// partitioned table — instead of an in-memory ChurnDataset. This is the path that
// was missing: `add_dataset` / `create_dataset_from_query` produce a
// `database.table`, and nothing could feed one to the pipeline that trains on it.
// The canonical name of the entity key inside the pipeline. A customer's own
// spelling (`account_id`, `customer_key`, …) is read from the row via the column
// schema's `field` and renamed to this on the way in, so the feature frame, the
// score job, and the ranked list all name one thing one way.
inline constexpr std::string_view kTableEntityColumn = "entity";

struct ChurnTableInput {
    std::string table_id;  // a `Table::id()` — what the partition catalog records
    // The `database.table` the caller named: what the run's lineage says it read.
    std::string dataset_name;
    // What resolving the dataset decided (resolve_dataset's notes), including the columns
    // it could not use and why; they are logged on the run and shown in its report.
    std::vector<std::string> notes;
    // What the table's columns MEAN. Resolve it from a `dataset_path` with
    // zetta::data::resolve_dataset, which infers what the caller left unsaid and
    // reports every inference.
    zetta::data::DatasetContract contract;

    bool empty() const { return table_id.empty(); }
};

// The DDF schema of the accounts shard for `contract`: the entity key as Int64,
// each feature column as Float64, the label as Int64, and the time column (when
// set) as Float64. Pure — the encoder's output contract in table form.
ddf::DataFrameSchema table_accounts_schema(
    const zetta::data::DatasetContract& contract,
    bool continuous_label = false);

// The featurization for `contract`: **static carry-through mode** — no windowed
// event features, the contract's feature columns carried through, the label as the
// target. A connector query returns one already-featurized row per entity (the
// windowing happened in the warehouse SQL), so there are no events to aggregate.
// When the contract names a time column the row universe is still cut at `as_of`,
// so the point-in-time guarantee holds where it applies. Pure.
ddf::FeaturizeSpec table_featurize_spec(const zetta::data::DatasetContract& contract);

// Run a churn graph over a connector-landed table, **compute-near-data**: each
// participant loads only the partition bodies it physically holds
// (`ddf::load_local_partitions`), so the rows never cross the wire — unlike the
// in-memory path above, which encodes the whole dataset on the initiator and ships
// it to every peer.
//
// Multi-node co-launch is NOT wired yet: with peers on the roster and
// `options.coordinate` set, this fails with the reason rather than running a
// single node's shard and reporting it as the whole. The fan-out is a small payload
// (a table id and a contract, not the rows) and is the follow-up.
ChurnSolutionResult run_churn_solution_on_table(
    Node& node, compute::ComputeDevice& device, const ir::ExecutionGraph& graph,
    const ChurnTableInput& input, const ChurnSolutionOptions& options = {});

// --- Run the churn solution WITHOUT blocking the caller's thread (#624) ------
//
// AsyncChurnRun drives run_churn_solution_on_node on a dedicated worker thread
// so an interactive caller (the headed Deploy panel, the Graph Workspace) stays
// responsive for the whole featurize -> train -> explain -> rank pipeline. It is
// safe against the "Node tree is main-thread-only" invariant because every
// node-tree write on the run path gates through Node::run_on_owner — so the Run,
// its ComputeJobs, and the registered Model land on the live node WHILE the run
// executes, and progress is observable through the SAME live records every
// surface reads: the Run's status + logs (system.runs) and its jobs
// (system.jobs). An MCP caller polling those views and a GUI panel rendering
// them see one source (#614's job-handle semantics).
//
// Lifecycle: start() snapshots its inputs by value (graph / dataset / options —
// the worker owns them; the test-suite worker discipline), owns a CPU compute
// device for the run, and launches the worker. Poll done() from the owner loop;
// read result() only once done() is true. request_cancel() is cooperative —
// checked between graph steps, so the in-flight step completes and the run ends
// Failed with a "run cancelled" log. `node` must outlive the handle.
//
// The destructor requests cancel and joins the worker, PUMPING the node's owner
// gate while it waits (the worker may be blocked handing a node write to the
// owner loop — joining without draining would deadlock). Destroy the handle on
// the node's owner thread.
class AsyncChurnRun {
public:
    // Launch the run. Never returns null; a node with no compute plane fails
    // inside the worker like the synchronous runner (result().execution.error).
    static std::shared_ptr<AsyncChurnRun> start(Node& node,
                                                ir::ExecutionGraph graph,
                                                ChurnDataset dataset,
                                                ChurnSolutionOptions options);

    // The same, over a connector-landed table (#947) — the worker runs
    // `run_churn_solution_on_table` instead, so the shard is read near data
    // rather than encoded and carried in the handle.
    static std::shared_ptr<AsyncChurnRun> start(Node& node,
                                                ir::ExecutionGraph graph,
                                                ChurnTableInput table,
                                                ChurnSolutionOptions options);
    ~AsyncChurnRun();

    AsyncChurnRun(const AsyncChurnRun&) = delete;
    AsyncChurnRun& operator=(const AsyncChurnRun&) = delete;

    // True once the worker has finished and result() is valid.
    bool done() const { return done_.load(std::memory_order_acquire); }

    // Ask the run to stop at the next between-steps check (cooperative; the
    // in-flight step completes). Idempotent.
    void request_cancel() { cancel_.store(true, std::memory_order_release); }
    bool cancel_requested() const {
        return cancel_.load(std::memory_order_acquire);
    }

    // The project label the run + its jobs carry (options.project) — what a
    // panel filters system.runs / system.jobs by while the run executes.
    const std::string& project() const { return project_; }

    // When the run was launched (for elapsed-time affordances).
    std::chrono::steady_clock::time_point started() const { return started_; }

    // The run's outcome. Valid only once done() is true.
    const ChurnSolutionResult& result() const { return result_; }

private:
    AsyncChurnRun(Node& node, ir::ExecutionGraph graph, ChurnDataset dataset,
                  ChurnTableInput table, ChurnSolutionOptions options);

    Node& node_;
    ir::ExecutionGraph graph_;
    ChurnDataset dataset_;
    ChurnTableInput table_;  // non-empty => the table path (#947)
    ChurnSolutionOptions options_;
    std::string project_;
    std::unique_ptr<compute::ComputeDevice> device_;
    std::chrono::steady_clock::time_point started_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> done_{false};
    ChurnSolutionResult result_;  // written by the worker before done_ flips
    std::thread worker_;
};

// Whether `graph` is churn-shaped — has the featurize -> train backbone the
// ChurnStepExecutor pipeline needs: at least one connect (a source to load), one
// clean (featurize), and one model (train) step. Used to decide whether an
// interactive sim-Python program should run on the real churn engine (this true) or
// the structural DefaultStepExecutor (false), so a non-churn graph still runs as a
// plain placeholder Run. Pure + non-throwing.
bool is_churn_graph(const ir::ExecutionGraph& graph);

// A small, deterministic, in-memory churn dataset (no files) for demos — the data
// the headed Deploy panel's "Run on real engine" action trains on. Built to carry a
// clean failed-payments churn signal (churned accounts accumulate payment failures;
// healthy ones do not), so a run over it produces a sensible ranked list + drivers
// without depending on the committed test fixture.
ChurnDataset demo_churn_dataset();

// --- The coordinated cross-process launch (#601) -----------------------------

// The `run.launch` kind the churn solution registers / fans out.
inline constexpr std::string_view kChurnLaunchKind = "churn_solution";

// The churn `run.launch` payload JSON Schema (draft-07) — the **code-owned
// source of truth**; `schemas/churn_coordinated_run.schema.json` is generated
// from it and drift-checked by `tests/test_schemas.cpp`. The payload carries the
// graph (the IR's own validated boundary), the full **encoded numeric shards**
// (global entity ids — each participant filters its own disjoint shard locally;
// the raw ChurnDataset never crosses the wire), the `as_of` cutoff, and the run
// options (params ride the GbdtParams boundary).
const nlohmann::json& churn_coordinated_run_schema();

// Register the churn co-run on a node's launch listener (Node::attach_cluster
// wires this for every clustered node). The runner — executed on the node-owning
// thread when a peer's `run.launch` arrives — validates the payload against
// churn_coordinated_run_schema, re-checks the graph (is_churn_graph + the
// correctness gate: RULE-1 holds on every member, not just the initiator),
// derives this node's shard rank from the roster, and co-runs the graph over
// that shard on the node's live coordinator — so the initiator's collective
// rounds find every peer contributing.
void install_churn_launch_runner(compute::CoordinatedLaunchListener& listener,
                                 Node& node);

}  // namespace zetta::usecases::churn
