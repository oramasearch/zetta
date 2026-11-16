// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <string>

#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"

// Run a DDF groupby-aggregate as a **ComputeJob** through the job coordinator
// (ARC-12 / FEAT-8, #387) — the slice that makes the distributed
// [groupby-aggregate](group_by.hpp) a recorded, durable unit of accelerated
// compute. This is the consumer the [DDF core](data_frame.hpp) + the
// [distributed groupby op](group_by.hpp) (#383) + the
// [job coordinator](../compute/job_coordinator.hpp) were built toward.
//
// **Reuses the op, adds the record.** The distributed math already lives in
// `zetta::ddf::distributed_group_by` (#383): SPMD over every node's own shard,
// partials gathered to the deterministic root, merged, and broadcast back so the
// result is identical on every node. `distributed_group_by` is a template over any
// `Collective` exposing `gather`/`broadcast`, and a `JobRound` is one — so this
// runner simply drives it inside the coordinator's job lifecycle. The only thing
// #387 adds over #383 is the **ComputeJob**: the work is recorded, audited, and
// appears in `system.jobs`.
//
// **Compute-near-data, node-scoped.** Raw rows never leave a node — only each
// node's small per-group partial crosses the wire (the existing,
// schema-validated `CollectiveMessage`). No egress of source data by default.
//
// **Recorded.** `run_group_by_job` drives the supplied `ComputeJob` (kind `ddf`)
// through the coordinator's lifecycle (Running → Succeeded | Failed) and records
// it, so it appears in `system.jobs` and is audited like any other job.
//
// **No new wire format / JSON.** The partials ride the schema-validated
// `CollectiveMessage`; the op spec carries its own validated boundary
// (`GroupBySpec`, group_by.hpp); the job record is schema-validated at its own
// boundary. See docs/ddf/ddf-groupby.md.
namespace zetta::ddf {

// The outcome of running a distributed groupby-aggregate as a ComputeJob.
struct DistributedGroupByResult {
    // The GLOBAL aggregation — the merged result over every node's shard. Because
    // `distributed_group_by` broadcasts the merged partial back to all nodes, this
    // is populated **identically on every participating node** when the round
    // succeeds (the result does not live at the root alone).
    GroupByResult result;
    // The job's terminal status (Succeeded when the round completed, Failed on a
    // bad spec / column / device error or a collective timeout).
    zetta::compute::JobStatus status = zetta::compute::JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id) —
    // the node that gathered + merged the partials. Informational; the broadcast
    // means `result` is set on every node regardless.
    bool is_root = false;

    bool ok() const noexcept { return status == zetta::compute::JobStatus::Succeeded; }
};

// Run `spec` as a groupby-aggregate ComputeJob across the cluster, with
// `local_frame` as this node's shard. Each node computes its local partial, the
// root gathers + merges them, the merged result is broadcast to every node, and
// `job` is driven through its lifecycle and recorded. Call **concurrently on
// every participating node** (the collectives need all participants);
// `coordinator.start()` must have run.
//
// `round_tag` names the collective round and **must be identical on every
// participating node** (a node's own `job` id cannot serve — each node holds a
// distinct `ComputeJob` for the same logical work). When empty it defaults to a
// deterministic, spec-derived tag (same key + aggregates → same tag), which is
// correct whenever the cluster runs one groupby of a given shape at a time; pass
// an explicit shared tag to isolate concurrent groupby jobs of the *same* shape.
DistributedGroupByResult run_group_by_job(
    compute::JobCoordinator& coordinator, zetta::compute::ComputeJob& job,
    const DataFrame& local_frame, const GroupBySpec& spec,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGroupByTimeout);

}  // namespace zetta::ddf
