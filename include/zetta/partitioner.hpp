#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

// A partition index in [0, partition_count). A partitioned table is split into a
// fixed number of partitions P (chosen P >> nodes), and a partition is the unit
// of placement and rebalancing: whole partitions (as blobs) move between nodes
// on a membership change, rather than every row being rehashed. See ADR-31 /
// ENT-37 and docs/partitioner.md.
using PartitionId = std::uint32_t;

// The partitioning seam (ADR-31 / ENT-37) — the pluggable policy that decides
// **data placement** for a partitioned table, the way a `StorageEngine` decides
// how rows are stored. It answers two questions:
//
//   1. record  → partition   (`assign_partition`) — which of the P partitions a
//      row belongs to (a hash of a key column, round-robin for even balance, …).
//   2. partition → node(s)    (`place_partition`) — which cluster node(s) own a
//      partition, given the current membership (rendezvous / consistent hashing,
//      so a join/leave moves only ~1/N of the partitions).
//
// It is **pure**: both methods are deterministic functions of their inputs and
// must not throw (zetta-core is built `-fexperimental-library`; the seam never
// unwinds). The partition count is **fixed** for the life of a partitioner — P
// does not change as nodes come and go, which is exactly what lets rebalancing
// move whole partitions instead of rehashing every row.
//
// It is **selected per table**: a `Table` holds a `std::shared_ptr<Partitioner>`
// (null = not partitioned). Concrete strategies are separate slices —
// HashPartitioner / RoundRobinPartitioner for the record→partition half (#513)
// and RendezvousPlacement for the partition→node half (#514); a learned
// model-as-partitioner is a documented future direction. `SinglePartitioner`
// below is the trivial baseline.
//
// No persistence / replication lives behind this seam: recording where
// partitions actually landed is the replicated partition catalog
// (system.dataset_partitions, #510) and the partitioned write path (#511). This
// header is the interface only.
class Partitioner {
public:
    virtual ~Partitioner() = default;

    // Short partitioner name for logs / UI (e.g. "single", "hash").
    virtual const char* name() const = 0;

    // The fixed number of partitions P this partitioner splits a table into,
    // chosen P >> nodes so rebalancing moves whole partitions. Constant for the
    // life of the partitioner; every assign_partition result is in [0, P).
    virtual PartitionId partition_count() const = 0;

    // record → partition: the partition a row belongs to, always in
    // [0, partition_count()). Deterministic and side-effect-free.
    virtual PartitionId assign_partition(const nlohmann::json& record) const = 0;

    // partition → node(s): the node id(s) that own `partition`, given the
    // current cluster `members` (each a node id / CRDT actor UUID, e.g. from
    // Cluster::nodes()). The first entry is the primary owner; any further
    // entries are replicas. Deterministic for a fixed (partition, members) — so
    // every node computes the same placement with no election — and returns
    // empty when there are no members to place onto. `partition` is expected to
    // be in [0, partition_count()).
    virtual std::vector<std::string> place_partition(
        PartitionId partition,
        const std::vector<std::string>& members) const = 0;
};

// The trivial baseline partitioner — the identity, "not really partitioned yet"
// default. One partition (P = 1) holding every row, owned by **every** member
// (replicate-to-all placement). Configuring a table with it therefore leaves
// today's behaviour unchanged (every node holds all the data) while exercising
// both halves of the seam. The balanced / sharded strategies are #513 / #514.
class SinglePartitioner final : public Partitioner {
public:
    const char* name() const override { return "single"; }

    PartitionId partition_count() const override { return 1; }

    // Everything lands in the single partition 0.
    PartitionId assign_partition(const nlohmann::json& /*record*/) const override {
        return 0;
    }

    // Every member owns the single partition (replicate-to-all).
    std::vector<std::string> place_partition(
        PartitionId /*partition*/,
        const std::vector<std::string>& members) const override {
        return members;
    }
};

}  // namespace zetta
