#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/node_telemetry.hpp"
#include "zetta/data/replication_strategy.hpp"

// **The partition scheduler** — one place that decides how many partitions a
// dataset gets, how big each should be, and which nodes hold them
// ([#833](https://github.com/oramasearch/zetta/issues/833)).
//
// It sits **above** [`Partitioner`](partitioner.hpp): the scheduler emits a
// `PartitionPlan`, a `Partitioner` executes it. Fetch affinity stops being a
// special case in the ingest and becomes "a plan the scheduler emitted".
//
// It replaces `AddDatasetOptions::partition_count = 8` — eight because eight.
// A fixed count is wrong in both directions at once: eight partitions of a
// 4 KB dataset are eight Parquet footers and eight replicated catalog entries
// carrying a handful of rows, and eight partitions of a 200 GB dataset are 25 GB
// blobs that no single node can decode a row group of and that no rebalance can
// spread. The count is a *consequence* of a target partition SIZE and of how
// many nodes must be given work — never an input.
//
// **The plan is no longer derivable.** `plan_partition_assignments` was a pure
// function of (roster, row counts), so every node computed the identical tiling
// and nothing travelled. Capacity-aware placement reads live telemetry, which
// differs per node by construction, so the **coordinator computes the plan once
// and ships it** in the `run.launch` payload; participants use what they
// received and nobody recomputes. That is why `PartitionPlan` has a validated
// JSON boundary. See docs/data/partition-scheduler.md.
namespace zetta::data {

// --- The source's natural shape ---------------------------------------------

// Whether a source's units can be freely recombined to match a target shape.
//
// **Fetch parallelism and resting storage layout are different decisions**, and
// conflating them is what produces either a bad layout or a pointless shuffle. A
// HuggingFace dataset published as 12 files fixes the *downloader* count at 12;
// Snowflake's `partitionInfo[]` chunks are freely assignable to whichever node
// should end up owning those rows. Neither dictates a good resting layout.
enum class SourceShape {
    // The units can be combined and assigned however the plan likes, so the
    // fetch itself can be shaped into the target layout — and then there is
    // nothing to rebalance afterwards. Snowflake result chunks.
    Flexible,
    // The units are what they are. Fetch source-shaped for maximum download
    // parallelism, then rebalance to the target — the ONE case that earns a
    // shuffle. (HuggingFace / file-count sources: designed for, not built.)
    Fixed,
};

std::string_view to_string(SourceShape shape);

// The source's natural units. `unit_rows` is the row count of each unit in
// source order (a Snowflake result partition, a HuggingFace file); empty means
// the source has no unit structure worth planning around (a local file, a block
// of in-memory rows), which is treated as flexible — the caller holds all the
// rows and can lay them out however the plan says.
struct SourceLayout {
    SourceShape shape = SourceShape::Flexible;
    std::vector<std::uint64_t> unit_rows;
    std::string name;  // e.g. "snowflake.result_partitions" — for the rationale

    bool flexible() const { return shape == SourceShape::Flexible; }
    std::size_t unit_count() const { return unit_rows.size(); }
    std::uint64_t total_rows() const;
};

// --- The ask ----------------------------------------------------------------

// What the caller knows about the dataset it is about to write.
struct PartitionRequest {
    std::size_t row_count = 0;    // known, or the source's declared total
    std::size_t approx_bytes = 0; // known or estimated; 0 ⇒ estimated from rows
    std::vector<std::string> members;  // the roster placement runs over
    std::size_t replication_factor = kDefaultReplicationFactor;
    // A connector result set: a cache, re-fetchable from its source, so RF=1 and
    // a lost partition means re-running the ingest rather than losing data.
    bool cache = false;
    SourceLayout source;
    // **The manual override, honoured exactly.** `nullopt` (the default) means
    // auto-detect. A caller that says 37 gets 37.
    std::optional<std::uint32_t> partition_count;
    // Live per-node telemetry (`Node::telemetry_snapshot()`). Placement weights
    // itself by this; **an empty or stale snapshot degrades to an even spread**,
    // never to a failure — telemetry is a measurement that goes stale in seconds
    // and the exchange drops a peer past its TTL.
    std::vector<zetta::cluster::NodeTelemetrySample> telemetry;
};

// --- The answer -------------------------------------------------------------

// The shape a dataset is to be written in, and where each partition lives.
struct PartitionPlan {
    std::uint32_t partition_count = 0;
    std::size_t target_rows_per_partition = 0;
    std::size_t target_bytes_per_partition = 0;
    // Owners per partition, primary first — `owners.size() == partition_count`
    // whenever the plan placed anything.
    std::vector<std::vector<std::string>> owners;
    std::size_t replication_factor = 1;
    // **Why**, in words. Surfaced for observability: a plan a human cannot
    // second-guess is a plan nobody will trust when a dataset lands lopsided.
    std::string rationale;

    bool ok() const { return partition_count > 0; }

    // How many partitions name `node` as their PRIMARY owner — the node that
    // physically writes those bodies. This is the number a participant of a
    // co-launched ingest splits its own tile into.
    std::size_t primary_partitions_of(const std::string& node) const;

    // The share of the dataset each of `roster` is expected to hold, as weights
    // in roster order (a node with no primary weighs 0). This is what shapes the
    // FETCH: a node that will own a fifth of the partitions should pull a fifth
    // of the rows, so it lands exactly what it is meant to keep.
    std::vector<double> fetch_weights(const std::vector<std::string>& roster) const;
};

// The PartitionPlan JSON boundary — it now travels on the wire (see the header
// note), so it is validated like every other replicated/transmitted value.
nlohmann::json to_json(const PartitionPlan& plan);
const nlohmann::json& partition_plan_schema();
std::optional<PartitionPlan> partition_plan_from_json(const nlohmann::json& value,
                                                      std::string* error = nullptr);

// --- The interface ----------------------------------------------------------

class PartitionScheduler {
public:
    virtual ~PartitionScheduler() = default;

    // Short name for logs / the plan's rationale (e.g. "default").
    virtual const char* name() const = 0;

    // Decide the plan. **Non-throwing and total**: every request yields a plan,
    // degrading to an even spread when there is nothing to weight by, and to a
    // single unplaced partition when there are no members at all.
    virtual PartitionPlan plan(const PartitionRequest& request) const = 0;
};

// --- The default policy ------------------------------------------------------

// **The target partition size.** 64 MiB of body bytes, and the number is
// argued, not picked:
//
//  * **Too small pays per-blob overhead.** Every Parquet body carries a footer
//    (Thrift metadata for every column of every row group) and at least one row
//    group, and every partition additionally costs a `lo:` large-object entry
//    and a `part:` catalog entry that is **replicate-to-all** — it gossips to
//    every node in the cluster, forever. At 1 MiB bodies a 64 GiB dataset puts
//    65 000 catalog entries on the replication plane and the metadata, not the
//    data, becomes the bottleneck.
//  * **Too large loses parallelism and costs decode memory.** A partition is the
//    unit of scatter-gather parallelism, of ownership, and of movement on a
//    rebalance — one partition cannot be spread over two nodes. A Parquet row
//    group is also decoded whole, so a 1 GiB partition is a 1 GiB decode buffer.
//  * **64 MiB sits between them.** Parquet's own row-group guidance is ~128 MiB
//    (sized against an HDFS block); a zetta partition is additionally a
//    replication and movement unit, so a notch below that is the right side of
//    the trade. It keeps a realistic cluster at hundreds of catalog entries
//    rather than millions, while leaving every node several partitions to work
//    on in parallel.
//
// Configurable per scheduler (`PartitionSchedulerConfig`) — a cluster of very
// large nodes on very fast disks can reasonably raise it.
inline constexpr std::size_t kDefaultTargetPartitionBytes = 64ull * 1024 * 1024;

// **P >> nodes.** Every node must get work, and whole partitions must be able to
// move on a membership change without rehashing every row (ADR-31), so the count
// never drops below this many per node — even when the target size alone would
// say one. Two is the floor that lets a rebalance move *something* off a node
// without moving all of it.
inline constexpr std::size_t kDefaultPartitionsPerNode = 2;

// A hard ceiling, matching the `partition_count` cap the API accepts. Past this
// the replicated catalog is the cost, whatever the data size says.
inline constexpr std::uint32_t kMaxPlannedPartitions = 4096;

// What a row is assumed to serialize to when the caller cannot say. Deliberately
// small: over-estimating bytes over-partitions a dataset, and an extra partition
// on a small dataset costs more (a whole footer + catalog entry) than an
// under-sized one does.
inline constexpr std::size_t kAssumedBytesPerRow = 256;

// A node whose filesystem is at least this full is **avoided** — weighted down
// to near nothing rather than banned outright, because banning it could leave a
// plan with nowhere to put replicas.
inline constexpr double kNearlyFullStoragePercent = 90.0;

// Below this the filesystem is simply not a consideration: a node 60% full has
// room, and a node 77% full has room. Storage is a **gate with a taper**, not a
// gradient of used-ness — weighting by the free fraction would rank a healthy
// 77%-full node at 0.23, and would mean any node that reports storage at all
// always loses to one that does not.
inline constexpr double kStorageTaperPercent = 75.0;

struct PartitionSchedulerConfig {
    std::size_t target_bytes_per_partition = kDefaultTargetPartitionBytes;
    std::size_t partitions_per_node = kDefaultPartitionsPerNode;
    std::uint32_t max_partitions = kMaxPlannedPartitions;
    double nearly_full_storage_percent = kNearlyFullStoragePercent;
    double storage_taper_percent = kStorageTaperPercent;
};

// The shipped policy: size-targeted count, capacity-weighted placement.
//
// **Count.** A manual `partition_count` is honoured exactly. Otherwise
// `ceil(bytes / target)`, raised to `partitions_per_node × |members|` so every
// node gets work, capped at `max_partitions`, and never more partitions than
// there are rows (a partition per row is the degenerate end of "every node gets
// work" and buys nothing).
//
// **Placement.** Each member gets a weight from its live telemetry:
//   * **storage headroom** — a gate with a taper, not a gradient: neutral until
//     the data_dir filesystem starts filling, shedding work across the approach
//     to the nearly-full threshold, and collapsing to a token weight past it so
//     work goes elsewhere while the node stays a legal replica target;
//   * **available bandwidth** — `1 - net_percent`, when the host can report link
//     utilization. This is deliberate: the goal is the **wall-clock fetch time**
//     of the whole ingest, which is set by the slowest uplink, not row fairness.
//     A node with a saturated link should pull less even if it has room for more;
//   * **spare CPU** — a mild factor, since decode + Parquet encode are real work
//     but not the bottleneck a link is.
// A node with no fresh sample is weighted like a **typical** member (the mean of
// the measured weights), never like a perfect one: a measured node can only ever
// score at or below neutral, so handing an unknown node 1.0 would starve every
// node that actually reported and pile the whole dataset onto the one the
// coordinator knows nothing about. With nothing measured at all the mean is 1.0
// and the cluster spreads evenly — the documented degrade, never a failure.
// Primaries go to the node with the lowest load-to-weight ratio (deterministic,
// ties on roster order); replicas walk the weight order from a per-partition
// rotation so they spread rather than piling onto the strongest node.
class DefaultPartitionScheduler final : public PartitionScheduler {
public:
    DefaultPartitionScheduler() = default;
    explicit DefaultPartitionScheduler(PartitionSchedulerConfig config)
        : config_(config) {}

    const char* name() const override { return "default"; }

    PartitionPlan plan(const PartitionRequest& request) const override;

    const PartitionSchedulerConfig& config() const { return config_; }

private:
    PartitionSchedulerConfig config_{};
};

// The scheduler used when a caller supplies none — a process-wide
// `DefaultPartitionScheduler` with the default configuration.
const PartitionScheduler& default_partition_scheduler();

// Estimate the serialized size of `rows` without serializing all of them: the
// mean dumped size of an evenly-spread sample, times the row count. Used by the
// callers that hold their rows in memory (`add_dataset_rows`) so the plan works
// from a real number instead of `kAssumedBytesPerRow`.
std::size_t estimate_rows_bytes(const std::vector<nlohmann::json>& rows);

}  // namespace zetta::data
