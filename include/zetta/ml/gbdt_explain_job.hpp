#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/tree_shap.hpp"

// Run exact TreeSHAP explanation as a distributed **`explain` ComputeJob** through the
// [job coordinator](../compute/job_coordinator.hpp) (FEAT-6, #393) — the slice that
// turns per-entity explanation into a recorded, audited unit of accelerated compute
// that appears in `system.zetta_jobs`, distributed for **batch explanation** of a
// whole entity universe.
//
// **The distributed shape.** Each participating node holds a shard of the feature
// table (one row per entity, the design matrix X) and the **same** trained
// `GbdtModel` (it is replicated, identical on every node). Per-entity SHAP is a pure
// function of `(model, x)` with no cross-entity dependency, so the explanation is
// **embarrassingly parallel**: each node fans [tree_shap](tree_shap.hpp) across its
// own rows (intra-node parallelism over the shared runtime pool, the trainer's
// pattern), with **one collective** combining the per-node `Σ|φ|` into a global,
// population-level **mean-|φ| driver ranking** (identical on every node — the
// "dominant churn drivers" at the cohort level). No splitter / repartition is needed;
// the model already carries the background (cover) it integrates over.
//
// **Device-backed, host-side.** The feature columns live on the frame's
// `ComputeDevice` (a CPU or CUDA frame, e.g. `ddf::FeatureTable::to_data_frame`); they
// are read back through the device download seam and TreeSHAP runs host-side — the same
// numerical reference as the trainer.
//
// **Margin space.** Like [tree_shap](tree_shap.hpp), the attributions are in the
// model's raw-score (logit) space, where the efficiency invariant
// `base_value + Σ φ = predict_raw(x)` holds exactly per entity (asserted in the tests).
//
// **No new wire format.** The `Σ|φ|` partials ride the schema-validated
// `CollectiveMessage` (via `JobRound::allreduce`); the job record is schema-validated
// at its own boundary; TreeSHAP reads the already-validated `GbdtModel`. Persisting the
// per-entity scores as a dataset / a Run output, and writing the SHAP driver ranking
// onto the [`Model`](../model.hpp)'s `feature_schema.drivers`, are later slices. See
// docs/ml/tree-shap.md.
namespace zetta::ml {

// The default per-collective timeout for an explain round (matches the train job).
inline constexpr std::chrono::milliseconds kDefaultExplainTimeout{10000};

// One entity's explanation in the model's raw-score (margin) space. `values` is φ per
// feature in `feature_names` order; `dominant_*` is the single feature with the largest
// |φ| (the per-entity dominant driver). Efficiency: `base_value + Σ values ≈ predicted`.
struct EntityExplanation {
    std::int64_t entity = 0;        // entity key (from entity_column), or the row index
    double predicted = 0.0;         // predict_raw(x)
    std::vector<double> values;     // φ per feature
    std::string dominant_feature;   // the feature with the largest |φ|
    int dominant_index = -1;        // its index into feature_names
    double dominant_value = 0.0;    // its signed φ (positive = pushed toward churn)
};

// The outcome of running distributed TreeSHAP explanation as a ComputeJob.
struct DistributedExplainResult {
    // THIS node's shard: one explanation per local entity (the local per-entity
    // drivers). The global batch is the union across nodes (every node has its own).
    std::vector<EntityExplanation> explanations;
    // The model's feature ordering (== model.feature_names).
    std::vector<std::string> feature_names;
    // The model's intrinsic base value (expected raw score) — constant per entity.
    double base_value = 0.0;
    // GLOBAL mean |φ| per feature over every entity in the cluster (allreduced, so
    // identical on every node) — the population-level driver importances.
    std::vector<double> mean_abs_shap;
    // The globally dominant driver: the feature with the largest mean |φ| (empty when
    // there are no features / no rows).
    std::string top_driver;
    // The number of entities explained across the whole cluster.
    std::size_t global_entity_count = 0;
    // The job's terminal status (Succeeded when the round completed; Failed on a bad
    // column / a device error / a collective timeout).
    JobStatus status = JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    bool is_root = false;

    bool ok() const noexcept { return status == JobStatus::Succeeded; }
};

// Explain `model`'s predictions over this node's shard `features` as an `explain`
// ComputeJob across the cluster. Reads the model's feature columns
// (`model.feature_names`) from the frame, computes exact per-entity TreeSHAP, and
// allreduces a global mean-|φ| driver ranking. Drives `job` (kind `explain`) through
// the coordinator's lifecycle (Running → Succeeded | Failed) and records it. Call
// **concurrently on every participating node** (the collective needs all
// participants); `coordinator.start()` must have run.
//
// `entity_column`, when given, labels each explanation with that integer key (so the
// per-entity drivers carry their entity id across the shard boundary); empty ⇒ the
// local row index is used. `round_tag` names the collective round and **must be
// identical on every participating node**; it defaults to a deterministic,
// model-derived tag.
DistributedExplainResult run_explain_job(
    compute::JobCoordinator& coordinator, ComputeJob& job, const GbdtModel& model,
    const ddf::DataFrame& features, const std::string& entity_column = {},
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultExplainTimeout);

}  // namespace zetta::ml
