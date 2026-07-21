#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/partitioner.hpp"

namespace zetta::data {

// **Fetch-affinity placement** — the node that *pulled* a slice of data owns the
// partitions it produced ([#833](https://github.com/oramasearch/zetta/issues/833)
// section 6). The write-side mirror of compute-near-data: rows land where they
// were fetched, so nothing is shuffled afterwards.
//
// It is the partition→node half of the `Partitioner` interface, exactly like
// [`RendezvousPlacement`](rendezvous_placement.hpp), and it exists because HRW
// placement is *wrong* for a distributed ingest. Under HRW a node that fetched
// source chunk 3 writes a partition whose hash-chosen owner is some other node,
// so the bytes it just pulled have to move again — the data crosses the wire a
// second time and the ingest is bounded by that shuffle rather than by the
// cluster's aggregate uplink.
//
// **Still deterministic, still no plan on the wire.** Ownership is not an
// arbitrary runtime choice: which node fetches which source chunk is fixed by
// `connect::plan_fetch_assignments`, a pure function of (roster, per-chunk row
// counts). Every participant derives the identical tiling from the identical
// inputs, so "the fetcher owns it" is as agreed-by-construction as a hash —
// each node simply constructs this partitioner with **its own** node id and the
// resulting catalog entries tile the dataset with no overlap.
//
// **Better balanced than a hash for what comes next.** The tiling is
// longest-processing-time-first by row count, so the slices are near-equal in
// rows; a hash equalizes *partition counts*, which says nothing about rows when
// source chunks are uneven. Downstream compute-near-data therefore starts from
// an evenly-loaded cluster.
//
// **Replicas.** A connector result set is a cache and runs at RF=1, where the
// only owner is the fetching node. A higher factor (what `promote_dataset`
// raises a kept result set to) keeps the fetching node as the **primary** —
// it physically holds the body — and picks the extra owners by rendezvous over
// the remaining members, so replica placement stays leaderless and spread.
//
// Pure and non-throwing like the rest of the interface. See
// docs/data/connector-query.md and docs/data/partitioned-write-path.md.
class FetchAffinityPlacement final : public Partitioner {
public:
    // `partition_count` is the fixed P this node splits **its own** slice into
    // (clamped to at least 1). `owner_node` is the node id that fetched the
    // slice and will own every partition written through this partitioner —
    // normally `Node::node_id()` of the constructing node. `replication_factor`
    // is how many owners each partition gets (clamped to at least 1); the
    // primary is always `owner_node`.
    FetchAffinityPlacement(PartitionId partition_count, std::string owner_node,
                           std::size_t replication_factor = 1);

    const char* name() const override { return "fetch_affinity"; }

    PartitionId partition_count() const override { return partition_count_; }

    // The node that fetched this slice, and so owns everything written here.
    const std::string& owner_node() const { return owner_node_; }

    std::size_t replication_factor() const { return replication_factor_; }

    // record → partition: round-robin over the `_seq` surrogate the partitioned
    // write path stamps (the row's ordinal), so a node's fetched slice spreads
    // evenly over its P local partitions regardless of key skew. A record with
    // no usable surrogate lands in partition 0 — deterministic, in range.
    PartitionId assign_partition(const nlohmann::json& record) const override;

    // partition → node(s): `owner_node()` first, always. When
    // `replication_factor() > 1`, the extra owners are the highest
    // rendezvous-weighted members **other than** the owner, so replicas spread
    // deterministically. `members` that does not name the owner is not an error
    // — the owner holds the body either way and is still returned first.
    std::vector<std::string> place_partition(
        PartitionId partition,
        const std::vector<std::string>& members) const override;

private:
    PartitionId partition_count_;
    std::string owner_node_;
    std::size_t replication_factor_;
};

}  // namespace zetta::data
