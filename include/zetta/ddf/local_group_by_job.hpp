#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/group_by_job.hpp"
#include "zetta/ddf/local_partitions.hpp"

// Run a DDF distributed group-by as a **compute-near-data** ComputeJob (#516) —
// the slice that replaces the "shard handed in out-of-band" path with
// placement-aware local execution. Where `run_group_by_job` (group_by_job.hpp,
// #387) takes a `local_frame` the caller built however it liked, this runner
// **loads the local shard from this node's owned partitions** (the catalog +
// sharded blob bodies — see zetta::ddf::load_local_partitions) and then drives the
// exact same distributed group-by + ComputeJob lifecycle over it.
//
// **Compute-near-data, observable.** Each node reads only the partition bodies it
// physically holds (`local_bytes_read`, read near the node — not moved), and only
// the small per-group partial crosses the wire (`wire_bytes_moved`, measured via
// Collectives::wire_bytes()). So the acceptance is directly checkable:
// bytes-moved ≈ partials, not ≈ dataset. The global result is identical on every
// node (distributed_group_by broadcasts the merged partial back), so it matches a
// single-node group-by over the whole dataset.
//
// **No new JSON / wire format.** The shard is read from the existing JsonlStore
// partition-body lines; the partials ride the schema-validated `CollectiveMessage`;
// the op spec carries its own `GroupBySpec` boundary; the job record is validated
// at its own boundary. See docs/compute/compute-near-data.md.
namespace zetta::ddf {

// The outcome of a compute-near-data distributed group-by ComputeJob. Extends the
// distributed group-by result with the near-data instrumentation.
struct NearDataGroupByResult {
    // The GLOBAL aggregation — identical on every participating node (the merged
    // partial is broadcast back), so it equals the single-node group-by over the
    // whole dataset.
    GroupByResult result;
    // The job's terminal status (Succeeded when the round completed, Failed on a
    // local-shard load error, a bad spec / column / device error, or a timeout).
    JobStatus status = JobStatus::Failed;
    // Whether this node was the deterministic collective root (lowest node id).
    bool is_root = false;

    // --- near-data instrumentation -----------------------------------------
    std::size_t partitions_read = 0;     // owned partitions of `table` materialized
    std::uint64_t rows_read = 0;          // rows in this node's local shard
    std::uint64_t local_bytes_read = 0;   // partition-body bytes read near data
                                          //   (NOT moved over the wire)
    std::uint64_t wire_bytes_moved = 0;   // collective-plane bytes this node moved
                                          //   running the op (the partials)

    bool ok() const noexcept { return status == JobStatus::Succeeded; }
};

// Run `spec` as a compute-near-data group-by ComputeJob across the cluster: this
// node loads its shard from its owned partitions of `table` (a `Table::id()`) via
// `load_local_partitions(node, table, schema, device)`, then runs the distributed
// group-by through `coordinator`/`job` over that local frame. Call **concurrently
// on every participating node** (the collectives need all participants);
// `coordinator.start()` must have run. `round_tag` names the collective round and
// must be identical on every node (defaults to a deterministic spec-derived tag,
// like run_group_by_job).
//
// If this node has no owned partitions of `table`, its shard is empty and it
// contributes an empty partial (the round still completes; the global result is
// the union over the nodes that do hold data). A local-shard load failure fails
// this node's job cleanly and is recorded.
NearDataGroupByResult run_near_data_group_by_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const DataFrameSchema& schema,
    const GroupBySpec& spec, compute::ComputeDevice& device,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGroupByTimeout);

}  // namespace zetta::ddf
