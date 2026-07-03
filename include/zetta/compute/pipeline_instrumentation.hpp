#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "zetta/compute/compute_job.hpp"

// Live pipeline instrumentation — the per-job, near-data data-movement counters a
// distributed compute job reports, captured as **ephemeral, observed** records so
// they are queryable as a `system.<table>` read view (#589, epic #575 area 5 — the
// engine source the GUI's algorithm-at-work animation reads from). Where the
// near-data job runners (`zetta::ddf::run_near_data_group_by_job` /
// `run_near_data_featurize_job`, `zetta::ml::run_near_data_train_job`) already
// *compute* `local_bytes_read` vs `wire_bytes_moved` per job, they return them by
// value and nothing holds onto them; this record is the small in-memory hook the
// runner writes into on the `Node` so the counters survive the call and surface in
// `system.pipeline_jobs`.
//
// **Ephemeral / observed — never replicated (no CRDT).** Like `system.node_metrics`
// / `system.checkpoints` / `system.cloud_status`, these are a measurement of
// what this node did, not durable agreed state, so they live only in memory on the
// node that ran the job and are bounded (the registry keeps the most-recent N). They
// introduce **no new JSON boundary**: a runner constructs this struct from its own
// typed result fields — no JSON is parsed at any boundary — so there is nothing to
// schema-validate (the same as the other ephemeral read views).
//
// See docs/compute/pipeline-instrumentation.md.
namespace zetta::compute {

// The near-data op a recorded job ran — mirrors the near-data runners. Kept as its
// own small enum (rather than reusing `JobKind`) so the read view names the op the
// near-data instrumentation came from, independent of the ComputeJob's kind.
enum class NearDataJobKind { GroupBy, Featurize, Train };

// Lowercase canonical name ("group_by" / "featurize" / "train").
inline std::string_view to_string(NearDataJobKind kind) {
    switch (kind) {
        case NearDataJobKind::GroupBy:   return "group_by";
        case NearDataJobKind::Featurize: return "featurize";
        case NearDataJobKind::Train:     return "train";
    }
    return "group_by";
}

// One recorded near-data job's instrumentation. `local_bytes_read` is the partition
// body bytes this node read **near data** (from its own owned partitions, NOT moved
// over the wire); `wire_bytes_moved` is the collective-plane bytes it moved running
// the op (the small per-node partials). The acceptance the read view makes
// checkable: bytes-moved ≈ partials, not ≈ dataset.
struct PipelineJobInstrumentation {
    std::string job_id;                  // the ComputeJob id this came from
    NearDataJobKind kind = NearDataJobKind::GroupBy;
    std::string node;                    // the node that ran + recorded it
    std::uint64_t partitions_read = 0;   // owned partitions materialized
    std::uint64_t rows_read = 0;         // rows in this node's local shard
    std::uint64_t local_bytes_read = 0;  // partition-body bytes read near data
    std::uint64_t wire_bytes_moved = 0;  // collective-plane bytes moved (partials)
    JobStatus status = JobStatus::Failed;  // the job's terminal status
    std::string recorded_at;             // ISO-8601 UTC stamp at record time
};

}  // namespace zetta::compute