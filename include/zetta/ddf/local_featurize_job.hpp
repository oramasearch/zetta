#pragma once

#include <chrono>
#include <cstdint>
#include <string>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/featurize_job.hpp"
#include "zetta/ddf/group_by.hpp"
#include "zetta/ddf/group_by_job.hpp"
#include "zetta/ddf/local_partitions.hpp"

// Run point-in-time [featurization](featurize.hpp) as a **compute-near-data**
// ComputeJob over a partitioned table (#532, ARC-12 / FEAT-8) — the featurize sibling
// of [run_near_data_group_by_job](local_group_by_job.hpp). Where `run_featurize_job`
// (featurize_job.hpp, #388) takes the `events` + `accounts` shards the caller built
// however it liked, this runner **loads this node's shard from its OWNED partitions**
// of a single partitioned table (the catalog + sharded blob bodies — see
// zetta::ddf::load_local_partitions) and drives the same distributed featurization +
// ComputeJob lifecycle over it.
//
// **The engineered-columns case (UC-1 at scale).** This is how the
// saas-sales-conversations dataset is featurized: each row is one entity (a
// conversation) carrying already-engineered numeric columns (customer_engagement,
// sales_effectiveness, conversation_length, …; the embeddings are dropped at load) +
// an `outcome` label. The spec is a **static** FeaturizeSpec (empty `event_features`):
// the engineered columns are the `account_features` carried through, `outcome` is the
// `label_column`. "Point-in-time, no leakage" holds **where applicable** — when the
// spec sets a `time_column`, the local shard is filtered to `time <= as_of` (a row
// after the cutoff is excluded); with no time dimension there is nothing to filter.
// (A windowed/churn spec — non-empty `event_features` over a separate events table —
// runs through `run_featurize_job` directly with two `load_local_partitions` calls;
// this single-table runner targets the static engineered-columns shape.)
//
// **Compute-near-data, observable.** Each node reads only the partition bodies it
// physically holds (`local_bytes_read`, read near the node — not moved), and only the
// per-entity featurization partials cross the wire (`wire_bytes_moved`, via
// Collectives::wire_bytes()). The raw rows (and the dropped embeddings) never cross
// the wire. distributed featurization broadcasts the merged result, so the global
// feature table is identical on every node — it matches a single-node featurize over
// the whole dataset (the acceptance).
//
// **No new JSON / wire format.** The shard is read from the existing JsonlStore
// partition-body lines; the featurization partials ride the schema-validated
// `CollectiveMessage`; the spec carries its own `FeaturizeSpec` boundary; the job
// record is validated at its own boundary. See docs/compute/compute-near-data.md and
// docs/ddf/ddf-featurize.md.
namespace zetta::ddf {

// The outcome of a compute-near-data featurization ComputeJob. Extends the distributed
// featurization result with the near-data instrumentation.
struct NearDataFeaturizeResult {
    // The GLOBAL feature table — identical on every participating node (the merged
    // partial is broadcast back), so it equals a single-node featurize over the whole
    // dataset.
    FeatureTable table;
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

// Run `spec` as a compute-near-data featurization ComputeJob across the cluster: this
// node loads its shard from its owned partitions of `table` (a `Table::id()`) via
// `load_local_partitions(node, table, schema, device)`, then runs the distributed
// featurization through `coordinator`/`job` over that local shard (used as both the
// events and accounts frame). Call **concurrently on every participating node** (the
// collectives need all participants); `coordinator.start()` must have run. `round_tag`
// names the collective rounds and must be identical on every node (defaults to a
// deterministic spec-derived tag, like run_featurize_job).
//
// If this node has no owned partitions of `table`, its shard is empty and it
// contributes an empty partial (the round still completes; the global table is the
// union over the nodes that do hold data). A local-shard load failure fails this
// node's job cleanly and is recorded.
NearDataFeaturizeResult run_near_data_featurize_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const DataFrameSchema& schema,
    const FeaturizeSpec& spec, compute::ComputeDevice& device,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGroupByTimeout);

}  // namespace zetta::ddf
