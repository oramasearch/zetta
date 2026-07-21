#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zetta/data/replication_strategy.hpp"

namespace zetta { class Node; }

namespace zetta::data {

// **Promote a cached result set to durable** (#833 section 9) — raise a
// dataset's replication factor, so a cache that turned out to be worth keeping
// stops being one copy with no backup.
//
// A connector result set lands at **RF=1**: it is a cache, always re-pullable
// from its source, and paying for three copies of it is paying three times for
// something the source already holds ([connector-query.md](../../../docs/data/connector-query.md)).
// Everything else defaults to **3**. Promotion is the deliberate act that moves
// a dataset from the first world to the second — the thing that makes "promote
// this cache to permanent" real rather than aspirational.
//
// **How the bodies move: they already do.** Ownership is recorded in the
// replicate-to-all partition catalog, and `ReplicatedBlobStore` reads
// `owner_nodes` to decide what a node fetches and what it refuses. Adding an
// owner to a `part:` entry (captured by `DatasetPartition::set_owner_nodes`) is
// therefore the whole operation: the new owner lists the body in
// `missing_blobs()` and pulls it over the existing large-object transport. No
// new transport, no copy loop here — the same mechanism
// [rebalance_partitions](partition_rebalance.hpp) uses when membership changes.
//
// **Promotion never removes an owner.** The desired owner set is the partition's
// current owners (which already hold the body) followed by the rendezvous order
// until it reaches the target factor. A partition that already has at least that
// many owners is left untouched. So this raises a factor and never lowers one —
// a "promotion" that quietly dropped the only copy of a partition would be the
// exact opposite of what it is for.
//
// Non-throwing. See docs/data/promote-dataset.md.

// One partition whose owner set grew.
struct PartitionPromotion {
    std::string partition_uuid;     // the part:<uuid> catalog row id
    std::uint32_t partition = 0;    // the partition id within the table
    std::vector<std::string> from;  // owners before
    std::vector<std::string> to;    // owners after (a superset of `from`)
};

// The outcome. `replication_factor` is the factor actually applied — the request
// clamped to the live roster, so a single-node cluster reports 1 rather than
// pretending it placed three copies.
struct PromoteDatasetResult {
    std::string database;
    std::string table;
    std::string table_id;
    std::size_t replication_factor = 0;
    std::size_t partitions = 0;   // placed partitions considered
    std::size_t promoted = 0;     // partitions whose owner set grew
    std::vector<PartitionPromotion> promotions;
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// Raise every placed partition of `database`.`table` to `replication_factor`
// owners, clamped to `members` (empty = the node's live cluster membership, and
// this node alone when it is standalone). Returns an error when the database or
// table is unknown, when the table has no placed partitions (nothing to
// replicate), or when the factor is zero.
PromoteDatasetResult promote_dataset(
    Node& node, const std::string& database, const std::string& table,
    std::size_t replication_factor = kDefaultReplicationFactor,
    const std::vector<std::string>& members = {});

}  // namespace zetta::data
