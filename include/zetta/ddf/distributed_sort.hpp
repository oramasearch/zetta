#pragma once

#include <chrono>
#include <cstddef>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_device.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/cluster/signal.hpp"

// The **distributed sort** over the DDF substrate (ARC-12, #385) — sample-partition
// + local sort + merge across the cluster. Every participating node owns a shard
// (a `DataFrame`) of the same schema and runs the *same* call (SPMD); the result is
// a set of globally range-ordered, locally-sorted **partitions**, one per node:
// partition `r` (held by participant `r`) holds the r-th key range, so
// concatenating the partitions in participant order ([`ddf::concat`](data_frame_ops.hpp))
// yields the full global sort.
//
// The algorithm, run from the calling (application) thread:
//   1. **Local sort** each node's shard by the key column (on its device).
//   2. **Sample** keys from every shard, [`gather`](../collectives.hpp) them to the
//      root, which chooses P-1 splitters and [`broadcast`](../collectives.hpp)s them
//      — so every node range-partitions against the *same* boundaries.
//   3. **Partition** the local sorted shard into P buckets by the splitters.
//   4. **Shuffle** (all-to-all): bucket `b` is shipped to participant `b` over the
//      signal plane (the `ddf.shuffle` RPC), a [`barrier`](../collectives.hpp)
//      fences delivery, then each node **merges** its own bucket with everything it
//      received and **local-sorts** the union into its output partition.
//
// **Wiring.** A `DistributedSorter` is the cluster's signal handler while it lives,
// chained over a `Collectives` it owns: `ddf.shuffle` is buffered here,
// `collective.*` routes to the collectives, everything else to an optional app
// delegate. It rides entirely on the existing transport — no new wire verb — and
// the shuffle payload (`PartitionMessage`) is **schema-validated at the boundary**
// (schemas/ddf_partition.schema.json, the same `pboettch` validator as the
// collective message), so a malformed partition fails fast.
//
// See docs/ddf/ddf-distributed-sort.md.
namespace zetta::ddf {

// One shard of a DataFrame shipped over the signal plane during the shuffle. `tag`
// names the shuffle round; `from` is the sender's node id; the partition is carried
// column-major (`columns[c]` has `name` / `dtype` / `rows` values). This is the
// wire seam, exposed (like encode/decode_collective_message) so the schema boundary
// is directly testable; otherwise an internal protocol detail.
struct PartitionMessage {
    struct ColumnData {
        std::string name;
        compute::DataType dtype = compute::DataType::Float64;
        std::vector<double> values;  // `rows` values, exact for every dtype's range
    };

    std::string tag;
    std::string from;
    std::size_t rows = 0;
    std::vector<ColumnData> columns;
};

// Serialize a partition message to its compact JSON form.
std::string encode_partition_message(const PartitionMessage& message);

// Parse + validate a partition message at the **boundary**: against the draft-07
// schema (schemas/ddf_partition.schema.json, mirrored embedded in
// distributed_sort.cpp), then the cross-field rule that every column carries
// exactly `rows` values. Non-throwing — nullopt with a path-qualified reason in
// `error` (when non-null) on any failure.
std::optional<PartitionMessage> decode_partition_message(
    std::string_view payload, std::string* error = nullptr);

// The per-node distributed sorter. Construct one over a started (or startable)
// Cluster and the device its shards live on; it registers itself as the cluster's
// signal handler (chained over an owned Collectives) and restores the previous
// handler (the optional `delegate`) on destruction.
class DistributedSorter : public SignalHandler {
public:
    // This node's output partition (populated only when `status.ok()`).
    struct Result {
        DataFrame partition;
        compute::ComputeStatus status;
        bool ok() const noexcept { return status.ok(); }
    };

    // `delegate`, when given, receives every signal that is neither a `ddf.shuffle`
    // nor a collective, so the sorter can coexist with another signal consumer.
    // `cluster` and `device` must outlive the sorter.
    DistributedSorter(Cluster& cluster, compute::ComputeDevice& device,
                      SignalHandler* delegate = nullptr);
    ~DistributedSorter() override;

    DistributedSorter(const DistributedSorter&) = delete;
    DistributedSorter& operator=(const DistributedSorter&) = delete;

    // Sort `local_shard` (this node's shard) by `key_column`; returns THIS node's
    // globally range-ordered, locally-sorted output partition. Every participant
    // must call sort() with the same `key_column`, `order`, and `round_tag` against
    // its own same-schema shard. `round_tag` scopes the round's collective +
    // shuffle messages — use a fresh tag per concurrent sort. A single-node cluster
    // (no peers) returns the locally-sorted shard. A failing status on a missing
    // key column, a schema/device error, an unreachable peer, or a timeout.
    Result sort(const DataFrame& local_shard, std::string_view key_column,
                compute::SortOrder order = compute::SortOrder::Ascending,
                const std::string& round_tag = "ddf.sort",
                std::chrono::milliseconds timeout = Collectives::kDefaultTimeout);

    // The participants of the round: this node + connected peers, sorted by node id
    // (so participant index r is the same on every node — partition r's owner).
    std::vector<std::string> participants() const {
        return collectives_.participants();
    }

    Collectives& collectives() { return collectives_; }

    // --- SignalHandler (runs on the cluster network thread) ------------------
    SignalResponse on_request(const Signal& request) override;
    void on_event(const Signal& event) override;
    void on_task(const Signal& task) override;

private:
    Cluster& cluster_;
    compute::ComputeDevice& device_;
    SignalHandler* delegate_ = nullptr;

    // The post-shuffle barrier fences delivery, so `sort()` drains the inbox after
    // it rather than waiting on a condition — the mutex alone guards concurrent
    // on_request buffering against the drain.
    std::mutex mutex_;
    // Received shuffle partitions, keyed by round shuffle tag.
    std::map<std::string, std::vector<PartitionMessage>> inbox_;

    // Declared LAST: constructed with (cluster, delegate) so it registers itself
    // and restores `delegate` on destruction (the safe, external handler). The
    // sorter then registers *itself* as the chain head in the constructor body and
    // restores the collectives in its destructor body. Do not reorder.
    Collectives collectives_;
};

}  // namespace zetta::ddf
