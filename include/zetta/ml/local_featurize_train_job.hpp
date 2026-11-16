// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_train_job.hpp"
#include "zetta/ml/local_train_job.hpp"

// Featurize the engineered columns compute-near-data and train GBDT(outcome) on the
// **resulting feature table** — the UC-1 featurize → train composition over a
// partitioned table (#530, FEAT-6 / FEAT-8). It composes the two shipped near-data
// slices instead of inventing anything new: the point-in-time featurization of #532
// ([run_near_data_featurize_job](../ddf/local_featurize_job.hpp)) produces the
// feature table, and the #533 train → calibrate → explain → rank pipeline
// ([run_near_data_train_job](local_train_job.hpp)) consumes it — so the GBDT is fed
// the **featurized table**, not the raw engineered columns.
//
// **The pipeline (one shard load, four recorded jobs).**
//   1. **load** — this node's shard is materialized ONCE from its OWNED partitions of
//      `table` (zetta::ddf::load_local_partitions): only the bodies it physically
//      holds, read from local disk (the dropped embeddings never reach the wire).
//   2. **featurize** — a self-created `featurize` ComputeJob drives the distributed
//      point-in-time featurization over the local shard (the static engineered-columns
//      spec; when `spec.time_column` is set the rows are filtered to `time <= as_of`
//      first — no leakage). The merged feature table is broadcast, so the **global
//      feature table is identical on every node** and equals a single-node featurize
//      over the whole dataset.
//   3. **slice** — this node keeps the rows of the global feature table whose entity
//      came from its own owned partitions (its local slice; a post-cutoff local row is
//      absent from the global table, so it is excluded here too). The slices are
//      disjoint across nodes and their union is exactly the global table, so the
//      training universe is the feature table, each row contributed once.
//   4. **train → explain → rank** — the headline `train` ComputeJob (+ self-created
//      `explain` and `score` jobs) drives the #533 pipeline over the local slice:
//      histogram GBDT on `spec.label_column` (+ opt-in calibration, registering a
//      calibrated Model with the global AUC / log-loss / accuracy), per-row exact
//      TreeSHAP drivers, and the global ranked at-risk list.
//
// **SPMD / single-node == multi-node.** Called concurrently on every participating
// node, every phase combines per-node partials over the round's collectives and
// broadcasts its result, so every node ends with the same feature table, model,
// drivers, and ranking — equivalent to a single-node run over the whole dataset
// (within floating-point reduction tolerance for the histogram allreduce).
//
// **Entity ownership.** The slice step assumes an entity's rows live in one node's
// partitions (true by construction for the partitioned write path: a row lands in
// exactly one partition, and the engineered shape is one row per entity). An entity
// spanning two nodes' shards would be trained twice — don't feed this runner a
// multi-table windowed spec (run the #533 phases over run_featurize_job directly for
// that shape).
//
// **Compute-near-data, observable.** Each node reads only the partition bodies it
// holds (`local_bytes_read`, read near the node — not moved); only the featurization
// partials + the training histograms / SHAP partials / score pairs cross the wire
// (`wire_bytes_moved`, via Collectives::wire_bytes()). The runner records TWO
// system.pipeline_jobs rows: a `featurize` row carrying the shard read + the
// featurize partials, and a `train` row carrying the train/explain/score partials —
// so summing the view never double-counts the one shard read.
//
// **No new JSON / wire format.** The shard is read from the existing JsonlStore
// partition-body lines; the featurization partials, training histograms / metrics,
// SHAP partials, and score pairs all ride the schema-validated `CollectiveMessage`;
// the spec carries its own validated `FeaturizeSpec` boundary; the job + model
// records are validated at their own boundaries. See
// docs/use-cases/uc1-featurize-train-over-partitions.md, docs/ddf/ddf-featurize.md,
// and docs/use-cases/uc1-train-over-partitions.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

// The outcome of the composed featurize → train pipeline: the global feature table
// the model was trained on, the #533 train/explain/rank result over it, and the
// whole-pipeline near-data instrumentation.
struct NearDataFeaturizeTrainResult {
    // --- the feature table (featurize phase) --------------------------------
    // The GLOBAL point-in-time feature table — identical on every participating node
    // (the merged partial is broadcast), equal to a single-node featurize over the
    // whole dataset. This is the table the GBDT was trained on.
    ddf::FeatureTable feature_table;
    // The self-created `featurize` ComputeJob's id (find it in system.zetta_jobs).
    // Empty when the pipeline failed before the featurize phase ran.
    std::string featurize_job_id;

    // --- train → explain → rank over the feature table (the #533 pipeline) --
    // The calibrated model + global metrics + drivers + per-row TreeSHAP explanations
    // + the ranked at-risk list, exactly as run_near_data_train_job returns them. Its
    // shard-load instrumentation fields stay 0 — the load happened once, up front, and
    // is reported by the top-level fields below; its `wire_bytes_moved` covers the
    // train/explain/score phases only.
    NearDataTrainResult train;

    // --- lifecycle -----------------------------------------------------------
    // The pipeline's terminal status: Succeeded only when the featurize job AND the
    // whole train → explain → rank pipeline succeeded.
    zetta::compute::JobStatus status = zetta::compute::JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    bool is_root = false;

    // --- near-data instrumentation (the whole pipeline) ----------------------
    std::size_t partitions_read = 0;    // owned partitions of `table` materialized
    std::uint64_t rows_read = 0;         // raw engineered rows in this node's shard
                                         //   (post-cutoff rows included — they are
                                         //   read, then excluded by the featurization)
    std::uint64_t feature_rows = 0;      // this node's slice of the global feature
                                         //   table (the rows it trained + explained)
    std::uint64_t local_bytes_read = 0;  // partition-body bytes read near data
                                         //   (NOT moved over the wire)
    std::uint64_t wire_bytes_moved = 0;  // collective-plane bytes this node moved
                                         //   across all four phases (the partials)

    bool ok() const noexcept { return status == zetta::compute::JobStatus::Succeeded; }
};

// Featurize this node's owned partitions of `table` (a `Table::id()`) per `spec` and
// train a histogram GBDT on `spec.label_column` over the resulting feature table,
// compute-near-data across the cluster: load the local shard once, run the recorded
// `featurize` job (point-in-time, no leakage), slice the broadcast feature table to
// this node's entities, then drive train (+ calibrate) → explain → rank over the
// slice through `coordinator`. Call **concurrently on every participating node** (the
// collectives need all participants); `coordinator.start()` must have run.
//
// `job` is the headline `train` ComputeJob (the caller creates it via
// `node.create_job(JobKind::Train, …)` and observes it in system.zetta_jobs); the
// featurize, explain, and score jobs are created on `node` here and their ids
// returned in the result. `spec` must carry a non-empty `label_column` (the {0,1}
// GBDT target) — a spec without one fails the job cleanly. The model's
// `feature_columns` are the feature table's columns (`spec.account_features` ++ the
// event-feature names, in spec order), and the per-row drivers + ranked list are
// labeled by the spec's entity key. `params` are the GBDT knobs; `calibration` opts
// into held-out probability calibration (off by default); `model_name` names the
// registered Model (defaulted when empty); `top_k` caps the ranked list (0 = all).
// `round_tag` names the collective rounds and **must be identical on every
// participating node** (a node's own `job` id cannot serve); it defaults to a
// deterministic, table-derived tag, with distinct per-phase suffixes
// (":featurize" / ":train" / ":explain" / ":score") so the phases never collide.
//
// If this node has no owned partitions of `table` its shard — and so its slice of the
// feature table — is empty and it contributes empty partials (the rounds still
// complete; the global result is the union over the nodes that hold data). A
// local-shard load failure or a failed featurize round fails this node's train job
// cleanly and is recorded.
NearDataFeaturizeTrainResult run_near_data_featurize_train_job(
    compute::JobCoordinator& coordinator, Node& node, zetta::compute::ComputeJob& job,
    const std::string& table, const ddf::DataFrameSchema& schema,
    const ddf::FeaturizeSpec& spec, const GbdtParams& params,
    compute::ComputeDevice& device, CalibrationConfig calibration = {},
    const std::string& model_name = {}, std::size_t top_k = 0,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGbdtTimeout);

}  // namespace zetta::ml
