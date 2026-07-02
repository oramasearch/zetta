#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_explain_job.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/gbdt_score_job.hpp"
#include "zetta/ml/gbdt_train_job.hpp"

// Train the native histogram GBDT on the binary `outcome` label **compute-near-data**
// over a partitioned feature table, calibrate it, and produce per-row TreeSHAP drivers
// + a ranked at-risk list (#533, UC-1 at scale, FEAT-6 / FEAT-8) — the **train**
// sibling of [run_near_data_featurize_job](../ddf/local_featurize_job.hpp).
//
// **What it is.** Where the [distributed train job](gbdt_train_job.hpp) (#390) takes
// the feature-table shard the caller built however it liked, this runner **loads this
// node's shard from its OWNED partitions** of a single partitioned table (the catalog
// + sharded blob bodies — see [load_local_partitions](../ddf/local_partitions.hpp))
// and drives the whole train → explain → rank pipeline over that local shard, each a
// recorded ComputeJob. It adds **no new ML math** — it composes the existing GBDT /
// calibration / TreeSHAP / scoring engine, the way [ChurnStepExecutor](../churn_step_executor.hpp)
// runs train/explain/score over one feature frame, only with the frame loaded near
// the data instead of from an in-line featurization.
//
// **The UC-1-at-scale case.** This trains over the saas-sales-conversations feature
// table: one row per conversation (entity) carrying the already-engineered numeric
// columns (customer_engagement, sales_effectiveness, conversation_length, …; the
// embeddings dropped at load — see #532) and the binary `outcome` label. The
// engineered columns are the `feature_columns`, `outcome` is the `label_column`, and
// `conversation_id` is the `entity_column` that labels the per-row drivers + rank.
//
// **The pipeline (three recorded jobs over the one local shard).**
//   1. **train + calibrate** — `run_train_job` over the local shard (calibration
//      opt-in via [CalibrationConfig](gbdt_train_job.hpp)), driving the headline
//      `train` `job` and, on the deterministic root, **registering a calibrated
//      `Model`** (kind classifier) whose `metrics` carry the global **AUC / log-loss /
//      accuracy** and whose `feature_schema.drivers` carry the per-feature gains.
//   2. **explain** — `run_explain_job` over the same shard with the trained model,
//      producing **per-row** (per-entity) exact TreeSHAP drivers for this node's shard
//      (the union across nodes is the global per-row set) plus the global
//      population-level mean-|φ| driver ranking. Runs as a self-created `explain`
//      ComputeJob.
//   3. **rank** — `run_score_job` over the same shard with the trained model,
//      producing the **global ranked at-risk list** (highest calibrated risk first,
//      broadcast so it is identical on every node). Runs as a self-created `score`
//      ComputeJob.
//
// **SPMD / single-node == multi-node.** Called concurrently on every participating
// node, each phase combines per-node partials over the round's collectives (the train
// histograms / metrics, the explain Σ|φ|, the score gather+rank), so every node ends
// with the **same model, drivers, and ranking** — equivalent to a single-node run over
// the whole table (the acceptance, within floating-point reduction tolerance for the
// histogram allreduce). The phases run sequentially but each is collective-synced, so
// the cluster stays in lock-step (the ChurnStepExecutor convention).
//
// **Compute-near-data, observable.** Each node reads only the partition bodies it
// physically holds (`local_bytes_read`, read near the node — not moved), and only the
// per-node partials cross the wire (`wire_bytes_moved`, via Collectives::wire_bytes()).
// The raw feature rows (and the dropped embeddings) never cross the wire.
//
// **No new JSON / wire format.** The shard is read from the existing JsonlStore
// partition-body lines; the training histograms / metrics, the SHAP partials, and the
// score pairs all ride the schema-validated `CollectiveMessage`; the GBDT weights body
// carries its own validated `GbdtModel` boundary; the job + model records are validated
// at their own boundaries. See docs/use-cases/uc1-train-over-partitions.md, docs/ml/gbdt.md, and
// docs/compute/compute-near-data.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

// The outcome of a compute-near-data GBDT training ComputeJob over a partitioned
// feature table: the calibrated model + global metrics + model-level drivers, the
// per-row TreeSHAP drivers, the ranked at-risk list, and the near-data instrumentation.
struct NearDataTrainResult {
    // --- the trained model (train phase) -----------------------------------
    // The trained, optionally calibrated model — identical on every participating
    // node (the trees grow off globally-allreduced histograms). Populated on success.
    GbdtModel model;
    // Global evaluation metrics over the union of every node's rows (AUC / log-loss /
    // accuracy) — identical on every node; also recorded on the registered Model.
    GbdtMetrics metrics;
    // Per-feature total split gain, in `feature_columns` order (the model-level drivers
    // written to the Model's feature_schema.drivers).
    std::vector<double> feature_importances;
    // Whether a probability calibration was fit + attached to `model` (only when
    // requested and the held-out fit succeeded); the metrics then reflect the
    // calibrated probabilities.
    bool calibrated = false;
    // The registered Model's id — set only on the root (the model replicates to peers).
    std::string model_id;

    // --- per-row TreeSHAP drivers (explain phase) --------------------------
    // THIS node's shard: one explanation per local entity (its per-feature φ + the
    // per-row dominant driver). The union across nodes is the global per-row set.
    std::vector<EntityExplanation> explanations;
    // The model's feature ordering (== model.feature_names) — the φ column order.
    std::vector<std::string> feature_names;
    // The model's cover-weighted base value (raw/margin space): base + Σφ ≈ predicted.
    double shap_base_value = 0.0;
    // Global mean |φ| per feature over every entity in the cluster (allreduced, so
    // identical on every node) — the population-level driver importances.
    std::vector<double> mean_abs_shap;
    // The globally dominant driver: the feature with the largest mean |φ| (empty when
    // there are no features / no rows).
    std::string top_driver;

    // --- ranked at-risk list (rank phase) ----------------------------------
    // The global at-risk list, ranked by calibrated risk descending (ties by entity
    // ascending) — identical on every node (broadcast). Capped to `top_k` when set.
    std::vector<EntityScore> ranked;

    // --- the recorded jobs -------------------------------------------------
    // The explain / score ComputeJobs this runner created on the node (the headline
    // train job is the `job` passed in). Surfaced so the caller can find them in
    // system.zetta_jobs. Empty when the train phase failed before they ran.
    std::string explain_job_id;
    std::string score_job_id;

    // --- lifecycle ---------------------------------------------------------
    // The pipeline's terminal status (Succeeded only when train AND explain AND rank
    // all succeeded; Failed on a local-shard load error, a bad column / single-class
    // labels / a device error, or a collective timeout in any phase).
    JobStatus status = JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    bool is_root = false;

    // --- near-data instrumentation -----------------------------------------
    std::size_t partitions_read = 0;     // owned partitions of `table` materialized
    std::uint64_t rows_read = 0;          // rows in this node's local shard
    std::uint64_t local_bytes_read = 0;   // partition-body bytes read near data
                                          //   (NOT moved over the wire)
    std::uint64_t wire_bytes_moved = 0;   // collective-plane bytes this node moved
                                          //   across all three phases (the partials)

    bool ok() const noexcept { return status == JobStatus::Succeeded; }
};

// Train a histogram GBDT on `label_column` over this node's owned partitions of
// `table` (a `Table::id()`), calibrate, explain (per-row TreeSHAP drivers), and rank,
// all compute-near-data across the cluster. This node loads its shard via
// `load_local_partitions(node, table, schema, device)`, then drives the train → explain
// → rank pipeline over it through `coordinator`/`job`. Call **concurrently on every
// participating node** (the collectives need all participants); `coordinator.start()`
// must have run.
//
// `job` is the headline `train` ComputeJob (the caller creates it via
// `node.create_job(JobKind::Train, …)` and observes it in system.zetta_jobs); the
// explain + score jobs are created on `node` here and their ids returned in the result.
// `feature_columns` names the design-matrix columns (the model's feature ordering),
// `label_column` the {0,1} target, `entity_column` the integer key that labels the
// per-row drivers + ranked list (empty ⇒ the local row index). `params` are the GBDT
// knobs; `calibration` opts into held-out probability calibration (off by default);
// `model_name` names the registered Model (defaulted when empty); `top_k` caps the
// ranked list (0 = all). `round_tag` names the collective rounds and **must be
// identical on every participating node** (a node's own `job` id cannot serve); it
// defaults to a deterministic, table-derived tag, with distinct per-phase suffixes so
// the three phases never collide.
//
// If this node has no owned partitions of `table` its shard is empty and it
// contributes empty partials (the rounds still complete; the global result is the union
// over the nodes that hold data). A local-shard load failure fails this node's train
// job cleanly and is recorded.
NearDataTrainResult run_near_data_train_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const ddf::DataFrameSchema& schema,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params,
    compute::ComputeDevice& device, const std::string& entity_column = {},
    CalibrationConfig calibration = {}, const std::string& model_name = {},
    std::size_t top_k = 0, const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGbdtTimeout);

}  // namespace zetta::ml
