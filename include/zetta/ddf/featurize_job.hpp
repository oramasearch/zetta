#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/group_by.hpp"

// Run the point-in-time [featurization](featurize.hpp) as a distributed
// **ComputeJob** through the [job coordinator](../compute/job_coordinator.hpp)
// (ARC-12 / FEAT-6, #388) — the slice that makes the churn featurization a recorded,
// durable, audited unit of accelerated compute that appears in `system.jobs`.
//
// **The churn_features.sql shape, distributed.** This is the literal SQL shape
// `accounts ⋈ events … GROUP BY account`, realized with the distributed DDF ops:
//
//   1. [distributed_join](join.hpp) (#382) the events shard with the accounts shard
//      on the entity key, repartitioning by key so every one of an entity's events —
//      enriched with that entity's static account columns — is co-located on one
//      node;
//   2. for each windowed event feature, [filter](ops.hpp) the enriched rows to the
//      feature's window (`time <= as_of`, optional trailing lower bound + event type)
//      and combine per entity with [distributed_group_by](group_by.hpp) (#383); the
//      static account columns (constant per entity) ride out of a parallel
//      distributed_group_by;
//   3. assemble the per-entity feature table — identical on every node, because
//      distributed_group_by broadcasts the merged result.
//
// **Leakage-free by construction.** Every event feature is windowed to `time <=
// as_of` (the filter in step 2), so a feature computed as of t0 never reads a future
// event — the same guarantee the single-node `point_in_time_featurize` gives, proved
// against the synthetic churn fixture's deliberate post-t0 events.
//
// **Compute-near-data.** Raw rows are repartitioned by the join, then only each
// node's small per-group partials cross the wire (the existing, schema-validated
// `CollectiveMessage`). No new wire format / JSON: the join + group-by partials ride
// the collective payload, the spec carries its own validated `FeaturizeSpec`
// boundary, and the job record is schema-validated at its own boundary.
//
// **Scope note (inner join).** distributed_join is inner-only (#382), so an account
// with no events in any shard is not emitted — the fixture has events for every
// account, so this matches the single-node (left-join) reference there; a true left
// join, emitting zero-event accounts with COALESCE-0 features, lands when
// distributed_join gains outer-join support, behind this same entry. See
// docs/ddf/ddf-featurize.md.
namespace zetta::ddf {

// The outcome of running a distributed featurization as a ComputeJob.
struct DistributedFeaturizeResult {
    // The GLOBAL feature table — the point-in-time features over every node's shard.
    // Because the underlying distributed_group_by broadcasts the merged result, this
    // is populated **identically on every participating node** when the job succeeds
    // (the table does not live at the root alone).
    FeatureTable table;
    // The job's terminal status (Succeeded when the rounds completed, Failed on a bad
    // spec / column / device error or a collective timeout).
    zetta::compute::JobStatus status = zetta::compute::JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    // Informational; the broadcast means `table` is set on every node regardless.
    bool is_root = false;
    // CLUSTER-WIDE count of the event rows this featurization read (#958). Each
    // node only sees its own shard, and a run report that says how much data the
    // model was built from cannot be assembled from one shard's view.
    std::uint64_t events_read = 0;

    bool ok() const noexcept { return status == zetta::compute::JobStatus::Succeeded; }
};

// Run `spec` as a point-in-time featurization ComputeJob across the cluster, with
// `events` + `accounts` as this node's shards. Drives `job` (kind `featurize`)
// through the coordinator's lifecycle (Running → Succeeded | Failed) and records it.
// Call **concurrently on every participating node** (the collectives need all
// participants); `coordinator.start()` must have run.
//
// **Static mode** (`spec.event_features` empty): the join is skipped — the engineered
// `account_features` (+ label) of the `accounts` shard are carried through per entity
// via the dimension `distributed_group_by`, point-in-time filtered to `time <= as_of`
// when a `time_column` is set (the `events` shard is then ignored). The
// compute-near-data variant over a partitioned table is `run_near_data_featurize_job`
// (local_featurize_job.hpp, #532).
//
// `round_tag` names the collective rounds and **must be identical on every
// participating node** (a node's own `job` id cannot serve — each node holds a
// distinct `ComputeJob` for the same logical work). When empty it defaults to a
// deterministic, spec-derived tag.
DistributedFeaturizeResult run_featurize_job(
    compute::JobCoordinator& coordinator, zetta::compute::ComputeJob& job, const DataFrame& events,
    const DataFrame& accounts, const FeaturizeSpec& spec,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGroupByTimeout);

}  // namespace zetta::ddf
