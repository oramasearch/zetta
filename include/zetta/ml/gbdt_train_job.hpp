#pragma once

#include <chrono>
#include <string>
#include <vector>

#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/gbdt.hpp"

// Run histogram GBDT training as a distributed **`train` ComputeJob** through the
// [job coordinator](../compute/job_coordinator.hpp) (FEAT-6 / ENT-14, #390) — the
// slice that makes training a recorded, durable, audited unit of accelerated
// compute that appears in `system.zetta_jobs` and **produces a `Model`**.
//
// **The distributed shape.** This is the SPMD entry the training algorithm runs
// through: each participating node holds a shard of the feature table (one row per
// entity, the design matrix X + label y), and the boosting loop in gbdt.hpp drives
// the model — combining the per-node statistics (global bin edges, the base label
// rate, the gradient/Hessian histograms at each tree level) across the cluster
// with the round's `allreduce`. Because split-finding runs off those global
// histograms, every node grows the **same trees**, so a single-node run and a
// multi-node run produce an equivalent model.
//
// **Device-backed, host-side reference.** The feature columns live on the frame's
// `ComputeDevice` (a CPU or CUDA frame, e.g. the one
// `ddf::FeatureTable::to_data_frame` materializes from featurization); they are
// read back through the device download seam and the histograms accumulated
// host-side via `runtime::parallel_for` (the numerical reference — a device-native
// weighted-histogram kernel is a later optimization behind the same API).
//
// **Produces a Model.** When `node` is provided, the deterministic collective root
// registers the trained model as a `zetta::Model` (kind `classifier`): the GBDT
// weights body (the JSON the `GbdtModel` serializes to, schema-validated) is
// attached via `set_weights` (fingerprinted + replicated p2p like any large-object
// body), with the evaluation metrics in `metrics` and the per-feature gains as the
// ranked `feature_schema.drivers` (explainability). One logical model is produced
// (at the root, which replicates to peers); deduping per-node registrations into a
// single record on every node is a later slice.
//
// **No new wire format.** The histograms ride the schema-validated
// `CollectiveMessage` (via `JobRound::allreduce`), the weights body carries its own
// validated `GbdtModel` boundary, and the job + model records are schema-validated
// at their own boundaries. See docs/gbdt.md.
namespace zetta {
class Node;
}

namespace zetta::ml {

// The default per-collective timeout for a training round (a generous 10s, like
// Collectives::kDefaultTimeout — kept local so this header need not pull in the
// collectives transport).
inline constexpr std::chrono::milliseconds kDefaultGbdtTimeout{10000};

// The outcome of running distributed GBDT training as a ComputeJob.
struct DistributedTrainResult {
    // The trained model — identical on every participating node (the trees are
    // grown off globally-allreduced histograms). Populated when the job succeeds.
    GbdtModel model;
    // Global evaluation metrics over the union of every node's rows (gathered to
    // the root, computed once, broadcast back — so identical on every node).
    GbdtMetrics metrics;
    // Per-feature total split gain, in `feature_columns` order (the drivers).
    std::vector<double> feature_importances;
    // The registered Model's id — set only on the root when `node` was provided.
    std::string model_id;
    // The job's terminal status (Succeeded when the rounds completed; Failed on a
    // bad column / single-class labels / a device error / a collective timeout).
    JobStatus status = JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    bool is_root = false;

    bool ok() const noexcept { return status == JobStatus::Succeeded; }
};

// Train a histogram GBDT across the cluster as a `train` ComputeJob, with
// `features` this node's shard (the columns named by `feature_columns`, the {0,1}
// target `label_column`). Drives `job` (kind `train`) through the coordinator's
// lifecycle (Running → Succeeded | Failed) and records it. Call **concurrently on
// every participating node** (the collectives need all participants);
// `coordinator.start()` must have run.
//
// When `node` is non-null the root registers the trained `Model` on it (kind
// classifier, weights + metrics + ranked drivers). `model_name` names that model
// (defaulted when empty). `round_tag` names the collective rounds and **must be
// identical on every participating node** (a node's own `job` id cannot serve);
// it defaults to a deterministic, spec-derived tag.
DistributedTrainResult run_train_job(
    compute::JobCoordinator& coordinator, ComputeJob& job,
    const ddf::DataFrame& features,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params,
    Node* node = nullptr, const std::string& model_name = {},
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGbdtTimeout);

}  // namespace zetta::ml
