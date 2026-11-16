// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace zetta { class Node; }

namespace zetta::data {

class DatasetPartition;

// One partition that changed owners during a rebalance — the audit detail of a
// move. `partition` is the partition id (from the catalog entry's `stats.partition`),
// `partition_uuid` is the catalog row's id (`part:<uuid>`), and `from`/`to` are the
// old/new owner-node sets (recorded in catalog order: primary owner first).
struct PartitionMove {
    std::string partition_uuid;          // the part:<uuid> catalog row id
    std::uint32_t partition = 0;         // the partition id within the table
    std::vector<std::string> from;       // owners before the rebalance
    std::vector<std::string> to;         // owners after the rebalance
};

// The outcome of rebalance_partitions. `examined` is how many placed partitions
// were considered, `moved` how many had their owners recomputed (a subset — the
// HRW win: only ~rf/N move when one node of N is added), `skipped` how many were
// left untouched (unplaced, replicate-to-all, or missing a partition id). `moves`
// lists each move for assertions / logging.
struct RebalanceResult {
    std::size_t examined = 0;
    std::size_t moved = 0;
    std::size_t skipped = 0;
    std::vector<PartitionMove> moves;
};

// **Between-jobs partition rebalance on a membership change** (#517). When the
// cluster gains or loses a node, the partition catalog
// (system.dataset_partitions) still records the *old* owners. This recomputes
// placement for every **sharded** partition via **rendezvous (HRW)** against the
// new `members`, and updates each changed `part:` catalog entry's `owner_nodes`
// (captured into the CRDT via DatasetPartition::set_owner_nodes, so the new
// placement is globally visible). The partition **bodies then move to their new
// owners for free**: once `owner_nodes` changes, the existing ReplicatedBlobStore
// (#515) makes the new owner list the body in missing_blobs() (so it fetches it
// over the LOREQ/LORES seam) and refuses it on non-owners — no new transport.
//
// **Why HRW** (the acceptance): rendezvous placement means adding one node to a
// cluster of N moves only ~rf/N of the partitions onto it (whole partitions move,
// no full rehash), and the catalog keeps tiling every row exactly once with `rf`
// owners apiece. The replication factor is **preserved per partition** — it is the
// number of owners the entry currently records (`owner_nodes.size()`, the count
// the partitioner placed at write time), clamped to `[1, members.size()]`. (The
// degenerate "cluster smaller than rf at write time" case under-counts rf; it is
// documented and out of scope — grow a cluster past rf before relying on full
// replication.)
//
// **Replicate-to-all partitions are skipped.** A SinglePartitioner / Hash /
// RoundRobin table places replicate-to-all (every member owns every partition),
// which is the "not really sharded yet" baseline — not the target of sharded
// rebalancing. They are detected by `previous_members`: a partition currently
// owned by exactly the previous full membership is replicate-to-all and is left
// untouched (growing it onto new nodes is a separate concern). With an empty
// `previous_members` no partition is classified replicate-to-all, so every placed
// partition is treated as rendezvous-sharded (the pure "recompute rendezvous"
// behavior).
//
// **Fetch-affinity partitions are skipped — by the OPPORTUNISTIC trigger only.**
// `include_fetch_affinity` is false for the two automatic triggers (the
// membership-change rebalance in `Node::tick`, and `add_nodes`): a fetch-affinity
// partition's owner is the node whose disk holds the bytes, chosen because it is
// the node that pulled them, so rehashing it there would order a shuffle of a
// dataset that was placed precisely to avoid one — the two features would cancel
// out.
//
// An ingest that genuinely needs to re-shape what it just wrote passes **true**,
// which is a deliberate, explicit call and not a background event. Today nothing
// does: the [partition scheduler](partition_scheduler.hpp) right-sizes the fetch,
// and a **flexible** source (Snowflake's freely-assignable result chunks) lands
// directly in the target shape, so there is nothing to move. The case that earns
// it is a **fixed** source — a dataset published as N files, where the download
// count is dictated by the source and the resting layout is not — which is a
// later slice; this parameter is the injection point it drops into.
//
// Unplaced partitions (empty `owner_nodes`) and entries with no recorded partition
// id are skipped. An empty `members` returns an empty result (nowhere to place).
// Non-throwing; it only mutates the catalog (no physical body GC — a demoted
// owner's stale on-disk copy is harmless: missing_blobs won't re-fetch it,
// store_blob refuses it, and the catalog defines ownership). See
// docs/data/partition-rebalance.md.
RebalanceResult rebalance_partitions(
    Node& node, const std::vector<std::string>& members,
    const std::vector<std::string>& previous_members = {},
    bool include_fetch_affinity = false);

}  // namespace zetta::data