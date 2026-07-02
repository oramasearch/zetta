#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

// A partition index in [0, partition_count). A partitioned table is split into a
// fixed number of partitions P (chosen P >> nodes), and a partition is the unit
// of placement and rebalancing: whole partitions (as blobs) move between nodes
// on a membership change, rather than every row being rehashed. See ADR-31 /
// ENT-37 and docs/data/partitioner.md.
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
// (null = not partitioned). The concrete record→partition strategies are
// `HashPartitioner` (hash of key columns) and `RoundRobinPartitioner` (surrogate
// for even balance) below (#513); the partition→node half is `RendezvousPlacement`
// (#514), which these compose with via the write path (#511); a learned
// model-as-partitioner is a documented future direction. `SinglePartitioner`
// below is the trivial baseline.
//
// No persistence / replication lives behind this seam: recording where
// partitions actually landed is the replicated partition catalog
// (system.dataset_partitions, #510) and the partitioned write path (#511). The
// record→partition strategies below serialize their *config* (`to_json` /
// `partitioner_from_json`, #513) so a table's choice can round-trip; the
// universal, polymorphic "which partitioner is this table using" boundary —
// covering `RendezvousPlacement` and composed strategies, serialized through a
// base pointer — is the write path's holistic concern (#511), not the seam's.
class Partitioner {
public:
    virtual ~Partitioner() = default;

    // Short partitioner name for logs / UI (e.g. "single", "hash"). Also the
    // stored "strategy" identifier in to_json() / partitioner_from_json().
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

    // Serialize the (trivial) config — round-trips with partitioner_from_json.
    nlohmann::json to_json() const {
        return nlohmann::json{{"strategy", "single"}, {"partition_count", 1}};
    }
};

// record → partition by **hashing a key** (#513). `assign_partition(record)`
// returns `hash(key columns) % P` — the standard "shard by a key" strategy, so a
// row's partition is a deterministic function of its key (every node and every
// restart computes the same partition for the same key, which is what locality
// reads and rebalancing rely on). The hash is a stable 64-bit FNV-1a over the
// key columns' JSON values (NOT std::hash, whose value is unspecified and varies
// across builds/runs — unusable for durable placement). A good hash spreads
// arbitrary keys roughly uniformly across the P partitions, so it stays balanced
// for a typical key even though it does not *guarantee* even balance under a
// skewed key (that is what RoundRobinPartitioner is for).
//
// `key_columns` names the field(s) to hash, in order. An empty list (a degenerate
// config) falls back to hashing the whole record. A missing field hashes as JSON
// null, so a key-less record still lands in a deterministic partition rather than
// being dropped.
class HashPartitioner final : public Partitioner {
public:
    HashPartitioner(PartitionId partition_count,
                    std::vector<std::string> key_columns)
        // P must be >= 1 (mod-by-zero guard); a 0 is clamped up to 1.
        : partition_count_(partition_count == 0 ? 1 : partition_count),
          key_columns_(std::move(key_columns)) {}

    const char* name() const override { return "hash"; }

    PartitionId partition_count() const override { return partition_count_; }

    PartitionId assign_partition(const nlohmann::json& record) const override;

    // Placement is the partition→node half (#514); until RendezvousPlacement
    // lands, a hash-partitioned table places replicate-to-all (every member owns
    // every partition), exactly like SinglePartitioner — the record→partition
    // mapping is sharded here, the placement is sharded in #514.
    std::vector<std::string> place_partition(
        PartitionId /*partition*/,
        const std::vector<std::string>& members) const override {
        return members;
    }

    const std::vector<std::string>& key_columns() const { return key_columns_; }

    // Serialize the config — round-trips with partitioner_from_json.
    nlohmann::json to_json() const;

private:
    PartitionId partition_count_;
    std::vector<std::string> key_columns_;
};

// record → partition by **round-robin over a surrogate** (#513) — the strategy
// for **even balance regardless of key skew**. `assign_partition(record)` returns
// `surrogate % P`, where `surrogate` is a monotonically-increasing integer the
// write path stamps on each row (the `surrogate_field`, default "_seq"); a
// sequence 0,1,2,… therefore cycles 0,1,…,P-1,0,1,… across the partitions, so the
// rows spread perfectly evenly no matter how their keys are distributed.
//
// It stays **deterministic + side-effect-free** (the seam's contract): the
// partition is a pure function of the row's surrogate, NOT of a hidden internal
// counter — so a row's partition is recomputable, the way the partition catalog /
// locality reads need. A missing/non-integer surrogate degrades to partition 0
// (in range, deterministic) rather than throwing; a negative surrogate is folded
// into [0, P) with a sign-safe modulo.
class RoundRobinPartitioner final : public Partitioner {
public:
    explicit RoundRobinPartitioner(PartitionId partition_count,
                                   std::string surrogate_field = "_seq")
        : partition_count_(partition_count == 0 ? 1 : partition_count),
          surrogate_field_(std::move(surrogate_field)) {}

    const char* name() const override { return "round_robin"; }

    PartitionId partition_count() const override { return partition_count_; }

    PartitionId assign_partition(const nlohmann::json& record) const override;

    // As with HashPartitioner, placement is #514; replicate-to-all until then.
    std::vector<std::string> place_partition(
        PartitionId /*partition*/,
        const std::vector<std::string>& members) const override {
        return members;
    }

    const std::string& surrogate_field() const { return surrogate_field_; }

    // Serialize the config — round-trips with partitioner_from_json.
    nlohmann::json to_json() const;

private:
    PartitionId partition_count_;
    std::string surrogate_field_;
};

// Reconstruct a partitioner from its serialized configuration — the validated
// JSON boundary (#513) that mirrors schemas/partitioner.schema.json. Returns an
// empty handle (and, when `error` is non-null, sets *error to a reason) on
// anything malformed: not an object, an unknown "strategy", a missing/non-integer
// or < 1 "partition_count", a hash strategy without a non-empty unique-string
// "key_columns", or a round_robin strategy with an empty "surrogate_field". This
// is the hand-written non-throwing pattern of zetta::ScalingPolicy::from_json
// (a value-type DTO boundary, not the pboettch path), so malformed config fails
// fast at the boundary instead of mis-placing rows. Round-trips with to_json().
std::shared_ptr<Partitioner> partitioner_from_json(const nlohmann::json& value,
                                                   std::string* error = nullptr);

}  // namespace zetta
