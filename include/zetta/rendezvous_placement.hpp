#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/partitioner.hpp"

namespace zetta {

// Rendezvous (highest-random-weight, HRW) placement of a partition onto cluster
// node(s) — the partition→node half of the `Partitioner` seam (ADR-31 / ENT-37,
// #514). For each member it computes a deterministic weight
// `hash(partition, member)` and keeps the `replication_factor` members with the
// **highest** weight: the highest-weight member is the primary owner, the next
// ones are replicas.
//
// Why HRW: a membership change (a node joining or leaving) only affects the
// partitions whose top-weight member *was* that node, so adding or removing one
// node out of N reassigns only ~1/N of the partitions — whole partitions move,
// with no full rehash. It is also leaderless-friendly: the result is a pure,
// deterministic function of `(partition, members, replication_factor)`, so every
// node computes the same placement with no election (the same property the
// [collectives](docs/cluster/collectives.md) rely on to pick a root).
//
// `members` is the current cluster membership (each a node id / CRDT actor UUID,
// e.g. from `Cluster::nodes()`); order does **not** matter — the function sorts
// by weight, so two nodes that see the same member set in different orders agree.
//   * empty `members`            → empty result (nowhere to place).
//   * replication_factor == 0    → treated as 1 (a partition always has a
//                                  primary owner when there is at least one
//                                  member).
//   * replication_factor > size  → every member is returned (you cannot place
//                                  more replicas than there are nodes).
// Ties in weight (astronomically unlikely with a 64-bit hash) break by node id
// ascending, so the result is fully deterministic. Non-throwing.
std::vector<std::string> rendezvous_owners(
    PartitionId partition,
    const std::vector<std::string>& members,
    std::size_t replication_factor = 1);

// The concrete rendezvous-placement `Partitioner`. It is a **complete**,
// directly-usable partitioner: `place_partition` is rendezvous placement (the
// novel half this slice delivers, via `rendezvous_owners` above), and
// `assign_partition` is a deterministic FNV-1a hash of the record into
// `[0, partition_count())` so rows actually spread across the partitions.
//
// The record→partition half is a documented stand-in: the dedicated
// record→partition strategies (HashPartitioner over a key column,
// RoundRobinPartitioner — #513) are a separate slice, and they can reuse the
// pure `rendezvous_owners` function for their own `place_partition` rather than
// subclassing this. So this class is "hash-the-whole-record into P, then
// rendezvous-place onto nodes".
//
// Pure + non-throwing like the rest of the seam; carries no serializable state
// in this slice (persisting *which* partitioner a table uses is the partitioned
// write path, #511), so there is no new JSON boundary here.
class RendezvousPlacement final : public Partitioner {
public:
    // `partition_count` is the fixed P (chosen P >> nodes); it is clamped to at
    // least 1. `replication_factor` is how many owners each partition gets (the
    // primary plus replicas); it is clamped to at least 1.
    explicit RendezvousPlacement(PartitionId partition_count,
                                 std::size_t replication_factor = 1);

    const char* name() const override { return "rendezvous"; }

    PartitionId partition_count() const override { return partition_count_; }

    // How many owners (primary + replicas) each partition is placed onto.
    std::size_t replication_factor() const { return replication_factor_; }

    // record → partition: FNV-1a hash of the record's canonical serialization,
    // folded into [0, partition_count()). Deterministic and side-effect-free.
    PartitionId assign_partition(const nlohmann::json& record) const override;

    // partition → node(s): rendezvous placement of `partition` onto `members`,
    // returning up to `replication_factor()` owners (primary first). Empty when
    // `members` is empty.
    std::vector<std::string> place_partition(
        PartitionId partition,
        const std::vector<std::string>& members) const override;

private:
    PartitionId partition_count_;
    std::size_t replication_factor_;
};

}  // namespace zetta
