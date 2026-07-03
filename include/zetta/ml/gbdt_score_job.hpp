#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"

// Run batch scoring / rank-at-risk as a distributed **`score` ComputeJob** through
// the [job coordinator](../compute/job_coordinator.hpp) (FEAT-6 / CMD-10, #392) —
// the slice that makes scoring a recorded, durable, audited unit of accelerated
// compute that appears in `system.zetta_jobs` and produces the global at-risk list.
//
// **The distributed shape (SPMD).** Each participating node holds a shard of the
// entities to score (a feature `DataFrame`, one row per entity). Because the
// trained `GbdtModel` is small and identical on every node (it replicates from the
// trainer's root), every node scores its own shard locally — no model broadcast.
// The per-node `(entity, risk)` pairs are then **gathered** to the deterministic
// collective root, ranked once over the union, and the ranked list **broadcast**
// back, so every node ends with the identical global at-risk list. A single-node
// run and a multi-node run therefore produce the **same ranking** (the acceptance).
//
// **GPU-native, CPU fallback (see CLAUDE.md → AI compute direction).** Scoring runs
// through the `ComputeDevice` seam: the feature columns are read from the frame's
// device (a CPU or CUDA frame), and the global ranking is the device `sort_by_key`
// operator — so it sorts on the GPU when the frame lives there and falls back to
// the CPU otherwise. The tree-predict numerics are the host-side reference today
// (the same convention the trainer ships with); a device-native predict kernel is
// the GPU-native optimization behind this same API.
//
// **No new wire format.** The `(entity, risk)` pairs ride the schema-validated
// `CollectiveMessage` (via `JobRound::gather` / `broadcast`, values as doubles —
// entity keys are exact as doubles for ids <= 2^53, the documented bound of the
// double-based collective plane), and the job record is schema-validated at its own
// boundary. See docs/ml/gbdt-score.md.
namespace zetta::ml {

// The default per-collective timeout for a scoring round (mirrors
// kDefaultGbdtTimeout — a generous 10s, kept local so the header need not pull in
// the collectives transport).
inline constexpr std::chrono::milliseconds kDefaultScoreTimeout{10000};

// The outcome of running distributed batch scoring as a ComputeJob.
struct DistributedScoreResult {
    // The global at-risk list, ranked by risk descending (ties by entity
    // ascending) — identical on every participating node. Populated on success.
    std::vector<EntityScore> ranked;
    // The job's terminal status (Succeeded when the rounds completed; Failed on a
    // bad column / a device error / a collective timeout).
    JobStatus status = JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    bool is_root = false;

    bool ok() const noexcept { return status == JobStatus::Succeeded; }
};

// Batch-score `features` (this node's shard — the columns the model's
// `feature_names` name, plus `entity_column`) through `model` as a distributed
// `score` ComputeJob, returning the global ranked at-risk list. Drives `job` (kind
// `score`) through the coordinator's lifecycle (Running → Succeeded | Failed) and
// records it. Call **concurrently on every participating node** (the collectives
// need all participants); `coordinator.start()` must have run.
//
// `top_k` caps the returned list to the `top_k` highest-risk entities (0 = all).
// `round_tag` names the collective rounds and **must be identical on every
// participating node** (a node's own `job` id cannot serve); it defaults to a
// deterministic, model-derived tag.
DistributedScoreResult run_score_job(
    compute::JobCoordinator& coordinator, ComputeJob& job,
    const ddf::DataFrame& features, const GbdtModel& model,
    const std::string& entity_column, std::size_t top_k = 0,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultScoreTimeout);

}  // namespace zetta::ml
