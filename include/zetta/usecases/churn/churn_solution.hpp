#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_step_executor.hpp"  // RetentionAction
#include "zetta/ddf/data_frame.hpp"       // ddf::DataFrameSchema
#include "zetta/ddf/featurize.hpp"        // ddf::FeaturizeSpec
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
// The real per-step compute already exists as zetta::ChurnStepExecutor (#396),
// but it is driven *by injection* into a GraphExecutor and needs a lot the seam
// can't supply on its own: a started JobCoordinator, a ComputeDevice, and this
// node's NUMERIC input shards (the churn pipeline operates purely on numeric DDF
// columns). This module is that glue, factored out of the test harness so the
// wiring is a first-class, reusable engine surface (the headed Deploy panel's
// "Run on real engine" action and the acceptance test both call it):
//
//   1. encode_churn_dataset  — map a ChurnDataset's string ids / ISO timestamps /
//      categorical event types into the numeric columns the DDF ops require (the
//      "featurize/encode step upstream" ChurnPipelineConfig documents);
//   2. churn_events_schema / churn_accounts_schema / churn_featurize_spec /
//      default_churn_gbdt_params — the standard, leakage-free churn pipeline config
//      over those encoded shards;
//   3. run_churn_solution — build a ChurnStepExecutor over (1)+(2), inject it into a
//      GraphExecutor, and execute the churn graph, returning the Run plus the real
//      artifacts (the registered Model id, the ranked list, the drivers, the
//      playbook).
//
// **No new JSON format.** Everything rides an existing validated boundary — the
// ChurnDataset is loaded/validated by load_churn_dataset, the catalog blueprint
// graph by parse_solution_catalog, and the Run / ComputeJob / GBDT-weights records
// at their own schema boundaries. This module adds no parser of its own.
//
// **Single node here, SPMD elsewhere.** run_churn_solution runs the dataset it is
// handed on *this* node (the whole dataset on a single-node compute harness). On a
// real multi-node cluster each node runs the same graph against its own shard and
// the underlying jobs combine partials over the collectives — the single-node ==
// multi-node equivalence is proven in the per-op job suites; this bridge does not
// re-prove it.
//
// See docs/use-cases/churn-solution.md.
namespace zetta {

class Node;
class StorageEngine;

namespace compute {
class ComputeDevice;
class JobCoordinator;
}  // namespace compute

namespace ir {
class ExecutionGraph;
}

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
};

// What running the churn solution on the real engine produced.
struct ChurnSolutionResult {
    ExecutionResult execution;  // the Run handle + ok / failed_step / error
    std::string model_id;       // the registered Model id (resolvable via find_model)
    std::vector<ml::EntityScore> ranked;  // the global at-risk list, highest risk first
    std::vector<std::pair<std::string, double>>
        drivers;             // population driver ranking: feature -> mean |phi|, desc
    std::string top_driver;  // the globally dominant churn driver
    std::vector<RetentionAction> playbook;  // at-risk accounts + their dominant driver

    bool ok() const { return execution.ok; }
};

// Run a churn ExecutionGraph (the catalog "churn-prediction" blueprint, or any
// connect x2 -> clean -> model -> index -> serve -> act churn graph) through the
// REAL engine on `dataset`. Encodes the dataset to numeric DDF shards, builds the
// standard featurize spec + GBDT params (from `options`), injects a
// ChurnStepExecutor over a started `coordinator` + `device`, and executes `graph`
// on `node` — so the produced Run references a real trained Model, the score job
// yields the ranked at-risk list, and TreeSHAP yields the churn drivers. The
// dataset must be non-empty (an empty dataset fails the run at featurize/train,
// like any empty input).
ChurnSolutionResult run_churn_solution(Node& node,
                                       compute::JobCoordinator& coordinator,
                                       compute::ComputeDevice& device,
                                       const ir::ExecutionGraph& graph,
                                       const ChurnDataset& dataset,
                                       const ChurnSolutionOptions& options = {});

// Run a churn graph through the REAL engine on the node's **live** JobCoordinator —
// the long-lived SPMD compute "spine" a running node owns over its real Cluster
// (#579, Node::attach_cluster / Node::job_coordinator). This is the **interactive
// "run over the cluster"** front door (#580): unlike run_churn_solution, which takes
// a coordinator by reference (the test suites + the Deploy panel demo used to spin up
// an *isolated single-node loopback* one), this resolves node.job_coordinator() and
// runs there — so the featurize / train / explain / rank jobs combine their partials
// over the node's real cluster membership through the existing collectives (which ride
// the signal plane), and the produced Run + Model + ranked list land on the live node
// (visible in system.zetta_runs / system.zetta_models). It is otherwise identical to
// run_churn_solution — same encoding, specs, gated/governed surfaces wrap it the same
// way, **no new JSON format**.
//
// A node with no compute plane attached (a standalone node, or one not yet
// attach_cluster'd) has no live coordinator: this returns a failed ChurnSolutionResult
// with execution.error set (it provisions nothing — the caller surfaces the reason or
// falls back to its own coordinator), so it never silently no-ops.
//
// **Single-node-or-coordinated membership.** The underlying jobs are SPMD: every
// collective round waits for *all* connected peers (a bounded timeout). A single node
// submitting this over a multi-node cluster whose peers are not co-running the same
// graph will fail those rounds. So today this is correct for the common interactive
// case — a standalone node, or a single-member cluster — where the live coordinator
// IS the cluster. Fanning the launch out to every peer so they co-run (a coordinated
// cross-process SPMD launch over the signal plane) is a later slice; the collectives
// already handle the *combine*, the *co-launch* is what is missing.
ChurnSolutionResult run_churn_solution_on_node(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const ChurnDataset& dataset,
    const ChurnSolutionOptions& options = {});

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

}  // namespace zetta
