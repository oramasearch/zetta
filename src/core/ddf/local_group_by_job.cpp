#include "zetta/ddf/local_group_by_job.hpp"

#include <cstdint>
#include <string>

#include "zetta/node.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ddf {

NearDataGroupByResult run_near_data_group_by_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const DataFrameSchema& schema,
    const GroupBySpec& spec, compute::ComputeDevice& device,
    const std::string& round_tag, std::chrono::milliseconds timeout) {
    NearDataGroupByResult out;

    // Record this job's near-data instrumentation into the node before returning,
    // so the counters survive the call and surface in system.pipeline_jobs (#589).
    auto record_instrumentation = [&] {
        node.record_pipeline_job({.job_id = job.id(),
                                  .kind = NearDataJobKind::GroupBy,
                                  .node = node.node_id(),
                                  .partitions_read = out.partitions_read,
                                  .rows_read = out.rows_read,
                                  .local_bytes_read = out.local_bytes_read,
                                  .wire_bytes_moved = out.wire_bytes_moved,
                                  .status = out.status,
                                  .recorded_at = now_iso8601()});
    };

    // (1) Compute near data: materialize this node's shard from its OWNED
    // partitions of `table` — only the bodies it physically holds, read from local
    // disk (no full-dataset pull).
    LocalShardResult shard =
        load_local_partitions(node, table, schema, device);
    out.partitions_read = shard.partitions_read;
    out.rows_read = shard.rows_read;
    out.local_bytes_read = shard.bytes_read;

    if (!shard.ok()) {
        // The shard could not be built — fail this node's job cleanly (so it is
        // still recorded) rather than entering the collective with a broken frame.
        using Outcome = compute::JobCoordinator::Outcome;
        coordinator.run(job, [&](compute::JobRound&) -> Outcome {
            return {.ok = false,
                    .detail = "near-data group_by: local shard load failed: " +
                              shard.status.message};
        });
        out.status = job.status();
        record_instrumentation();
        return out;
    }

    // (2) Run the distributed group-by over the local shard, measuring the bytes
    // this node moves over the collectives — the partials, not the rows.
    const std::uint64_t wire_before = coordinator.collectives().wire_bytes();
    DistributedGroupByResult distributed =
        run_group_by_job(coordinator, job, shard.frame, spec, round_tag, timeout);
    out.wire_bytes_moved = coordinator.collectives().wire_bytes() - wire_before;

    out.result = std::move(distributed.result);
    out.status = distributed.status;
    out.is_root = distributed.is_root;
    record_instrumentation();
    return out;
}

}  // namespace zetta::ddf
