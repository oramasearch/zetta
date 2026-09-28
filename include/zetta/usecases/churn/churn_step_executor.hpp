#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/gbdt_train_job.hpp"

// The **real** per-step executor for the UC-1 churn pipeline (#396) — the slice
// that replaces DefaultStepExecutor's logical-ref placeholders with actual
// distributed accelerated compute, behind the same StepExecutor interface and WITHOUT
// touching the orchestrator (GraphExecutor's walk is unchanged).
//
// DefaultStepExecutor (executor.hpp) derives a schema-valid Run from the graph's
// *structure* — "no real model/index is materialized in R1". This executor runs
// the churn step kinds for real, each heavy step a recorded ComputeJob through the
// [job coordinator](compute/job_coordinator.hpp), composing the building blocks
// that landed before it:
//
//   connect -> load to DDF       (load_data_frame: numeric shard -> DataFrame)
//   clean   -> featurize         (run_featurize_job, ddf/featurize_job.hpp #388)
//   model   -> train + calibrate (run_train_job, ml/gbdt_train_job.hpp #390/#391;
//                                 registers a real zetta::ml::Model on the node)
//   index   -> score / rank      (run_score_job, ml/gbdt_score_job.hpp #392 — the
//                                 ranked at-risk list)
//   serve   -> explain (TreeSHAP) (run_explain_job, ml/gbdt_explain_job.hpp #393 —
//                                 the population + per-entity churn drivers)
//   act     -> retention playbook (host-side join of the at-risk list with each
//                                 entity's dominant SHAP driver — who to contact + why)
//
// **Stateful across steps, by design.** The StepExecutor interface is a pure function
// of one step (`run_step(const ir::Node&) const`), but real churn compute threads
// artifacts down the pipeline: the feature table feeds training, the trained model
// feeds scoring + explanation, and the ranked list + drivers feed the playbook. So
// this executor carries that pipeline state in a pImpl, mutated through a unique_ptr
// member — `run_step` stays `const` and the orchestrator is untouched. It also holds
// the dependencies the interface can't supply (the Node, a started JobCoordinator, a
// ComputeDevice, the input shards). Because it accumulates state, one instance drives
// exactly one execution; construct a fresh one per run.
//
// **Used by injection, not via Node::run_graph.** run_graph builds a default
// GraphExecutor and can't conjure a coordinator / input data, so real compute is
// driven by constructing a ChurnStepExecutor and handing it to GraphExecutor:
//
//   zetta::pipeline::GraphExecutor executor{std::make_shared<ChurnStepExecutor>(
//       node, coordinator, device, std::move(config))};
//   auto result = executor.execute(node, churn_graph, project, graph_id);
//
// `coordinator.start()` must have run, and (for a multi-node cluster) every
// participating node runs the same graph with its own shard concurrently — the
// underlying jobs combine per-node partials over the collectives, so single-node
// and multi-node produce equivalent artifacts.
//
// **No new JSON format.** Like DefaultStepExecutor this introduces no new schema:
// every JSON it touches rides an existing validated boundary — the Run record, the
// ComputeJob records, the GBDT weights body (gbdt_model.schema.json), and the
// FeaturizeSpec / GbdtParams boundaries — so malformed data still fails fast there.
//
// See docs/use-cases/churn-step-executor.md.
namespace zetta::storage { class StorageEngine; }
namespace zetta { class Node; }
namespace zetta::compute { class ComputeDevice; }
namespace zetta::compute { class JobCoordinator; }

namespace zetta::usecases::churn {

// One row of the retention playbook the `act` step produces: a high-risk account
// paired with the single feature that drove its risk the most (its per-entity
// dominant SHAP driver) — the "who to contact and why" a retention team acts on.
struct RetentionAction {
    std::int64_t entity = 0;        // the at-risk account's entity key
    double risk = 0.0;             // its calibrated churn risk (predict_proba)
    std::string dominant_driver;   // the feature with the largest |phi| for this entity
    double dominant_value = 0.0;   // its signed phi (positive = pushed toward churn)
    std::vector<std::pair<std::string, double>> reason_codes;
};

// Everything the churn pipeline needs beyond the ExecutionGraph: this node's two
// numeric input shards (loaded to DDF by the connect steps), the featurization /
// training specs, and the run knobs. The stores are non-owning and must outlive the
// executor; their rows must already be numeric (a featurize/encode step upstream maps
// string ids / ISO timestamps / categorical event types into the numeric columns the
// schemas name — this op stays purely numeric, like the rest of DDF).
struct ChurnPipelineConfig {
    // The events shard: entity key + numeric timestamp + the value / type columns the
    // windowed features reduce (e.g. entity, ts, value, type).
    const zetta::storage::StorageEngine* events_store = nullptr;
    ddf::DataFrameSchema events_schema;
    // The accounts (dimension) shard: entity key + static account columns + the label
    // (e.g. entity, mrr, seats, tenure_days, churned).
    const zetta::storage::StorageEngine* accounts_store = nullptr;
    ddf::DataFrameSchema accounts_schema;

    // **Compute-near-data over a connector-landed dataset** (#947). When set to a
    // `Table::id()`, the accounts shard is loaded from THIS NODE'S OWNED PARTITIONS
    // of that table (`ddf::load_local_partitions`) instead of from `accounts_store`
    // — the rows never cross the wire, and the shard is placement-aware by
    // construction rather than by slicing a copy every node already holds.
    //
    // A connector query returns ONE already-featurized row per entity (the
    // windowing happened in the warehouse SQL), so there is no events shard to
    // pair it with: `featurize` runs in static carry-through mode
    // (`event_features` empty) and the events frame is empty. Setting this with a
    // windowed spec is a configuration error the featurize step reports.
    std::string accounts_table;
    // That table as the caller named it (`database.table`), and what resolving it decided.
    // A table-backed run's lineage names this dataset, and its connect steps that were not
    // read say so, instead of reporting the graph's own sources as read.
    std::string accounts_dataset;
    std::vector<std::string> dataset_notes;

    ddf::FeaturizeSpec featurize;        // the point-in-time (leakage-free) feature graph
    ml::GbdtParams params;               // the GBDT hyperparameters
    ml::CalibrationConfig calibration;   // opt-in held-out probability calibration

    std::string entity_column = "entity";   // the integer entity key (in both shards)
    std::size_t top_k = 0;                   // at-risk list / playbook cap (0 = all)
    std::string model_name = "churn_model";  // the registered Model's human name

    // Optional refs stamped on every ComputeJob this executor creates (observability
    // in system.jobs); the Run is labelled separately via GraphExecutor.
    std::string project;
    std::string graph;
};

// The real churn StepExecutor. Construct with the node, a started JobCoordinator, a
// ComputeDevice, and the pipeline config; inject into a GraphExecutor; execute the
// churn graph. After execution the produced-artifact accessors expose the real
// results (the registered model id, the ranked at-risk list, the drivers, the
// playbook) — the Run's outputs reference them.
class ChurnStepExecutor final : public zetta::pipeline::StepExecutor {
public:
    ChurnStepExecutor(Node& node, compute::JobCoordinator& coordinator,
                      compute::ComputeDevice& device, ChurnPipelineConfig config);
    ~ChurnStepExecutor() override;

    ChurnStepExecutor(const ChurnStepExecutor&) = delete;
    ChurnStepExecutor& operator=(const ChurnStepExecutor&) = delete;

    // Execute one churn step for real (see the kind->compute mapping above). Returns
    // the artifacts it materialized (the Run's outputs) + lineage sources, or ok=false
    // with a reason on a compute failure (the orchestrator then fail-fasts the run).
    zetta::pipeline::StepOutcome run_step(const ir::Node& step) const override;

    // --- Produced artifacts (populated as the matching step runs) ---------------

    // The registered zetta::ml::Model's id (set by the model step). Resolvable via
    // Node::find_model — this is the "real Model" the run's `model` output references.
    const std::string& model_id() const;
    // The trained GBDT model (set by the model step).
    const ml::GbdtModel& model() const;
    // The point-in-time feature table (set by the clean step).
    const ddf::FeatureTable& feature_table() const;
    // The global at-risk list, highest risk first (set by the index step).
    const std::vector<ml::EntityScore>& ranked() const;
    // The model's feature names, in driver-ranking order (set by the serve step).
    const std::vector<std::string>& feature_names() const;
    // The global population driver ranking: feature name -> mean |phi|, strongest
    // first (set by the serve step).
    std::vector<std::pair<std::string, double>> driver_ranking() const;
    // The globally dominant churn driver (set by the serve step).
    const std::string& top_driver() const;
    // The retention playbook: at-risk accounts + their dominant driver (set by act).
    const std::vector<RetentionAction>& playbook() const;

private:
    struct State;

    // const — the orchestrator only ever holds a const StepExecutor&).
    zetta::pipeline::StepOutcome run_connect(const ir::Node& step) const;
    zetta::pipeline::StepOutcome run_clean(const ir::Node& step) const;
    zetta::pipeline::StepOutcome run_model(const ir::Node& step) const;
    zetta::pipeline::StepOutcome run_index(const ir::Node& step) const;
    zetta::pipeline::StepOutcome run_serve(const ir::Node& step) const;
    zetta::pipeline::StepOutcome run_act(const ir::Node& step) const;

    // Lazily load both numeric input shards to DDF (idempotent). Returns false +
    // fills `error` on a device / schema / value failure.
    bool ensure_inputs_loaded(std::string& error) const;

    Node& node_;
    compute::JobCoordinator& coordinator_;
    compute::ComputeDevice& device_;
    ChurnPipelineConfig config_;
    std::unique_ptr<State> state_;  // pImpl: cross-step pipeline state (mutated in run_step)
};

}  // namespace zetta::usecases::churn
