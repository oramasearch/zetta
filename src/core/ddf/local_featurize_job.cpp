#include "zetta/ddf/local_featurize_job.hpp"

#include <cstdint>
#include <string>
#include <utility>

namespace zetta::ddf {

NearDataFeaturizeResult run_near_data_featurize_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const DataFrameSchema& schema,
    const FeaturizeSpec& spec, compute::ComputeDevice& device,
    const std::string& round_tag, std::chrono::milliseconds timeout) {
    NearDataFeaturizeResult out;

    // (1) Compute near data: materialize this node's shard from its OWNED partitions
    // of `table` — only the bodies it physically holds, read from local disk (no
    // full-dataset pull, the dropped embeddings never even on disk here).
    LocalShardResult shard = load_local_partitions(node, table, schema, device);
    out.partitions_read = shard.partitions_read;
    out.rows_read = shard.rows_read;
    out.local_bytes_read = shard.bytes_read;

    if (!shard.ok()) {
        // The shard could not be built — fail this node's job cleanly (so it is still
        // recorded) rather than entering the collective with a broken frame.
        using Outcome = compute::JobCoordinator::Outcome;
        coordinator.run(job, [&](compute::JobRound&) -> Outcome {
            return {.ok = false,
                    .detail = "near-data featurize: local shard load failed: " +
                              shard.status.message};
        });
        out.status = job.status();
        return out;
    }

    // (2) Run the distributed featurization over the local shard (used as both the
    // events and accounts frame — the static engineered-columns case reads only the
    // accounts side), measuring the bytes this node moves over the collectives (the
    // per-entity partials, not the rows).
    const std::uint64_t wire_before = coordinator.collectives().wire_bytes();
    DistributedFeaturizeResult distributed = run_featurize_job(
        coordinator, job, shard.frame, shard.frame, spec, round_tag, timeout);
    out.wire_bytes_moved = coordinator.collectives().wire_bytes() - wire_before;

    out.table = std::move(distributed.table);
    out.status = distributed.status;
    out.is_root = distributed.is_root;
    return out;
}

}  // namespace zetta::ddf
