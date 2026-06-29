# Between-jobs partition rebalance on a membership change

When the cluster gains a node (e.g. after [`add_nodes`](add-nodes.md)), the
[partition catalog](dataset-partitions.md) still records the **old** owners for
every partition. The rebalance **recomputes placement via rendezvous (HRW)** against
the new membership and updates each changed `part:` catalog entry's `owner_nodes`,
so the new placement is globally visible and the partition **bodies move to their
new owners**. It runs **between jobs** — not mid-job (elastic mid-job is deferred).

It realizes **issue [#517](https://github.com/oramasearch/zetta/issues/517)**
(satisfies TC-10 / [#500](https://github.com/oramasearch/zetta/issues/500)).

## The acceptance

After adding one node to a cluster of N:

- **≈ rf/N of the partitions move onto the new node** — whole partitions move, with
  no full rehash. This is the [rendezvous / highest-random-weight](partitioner.md)
  property: a partition only moves if the new node wins its top-`rf` weight, which
  happens for a `~rf/(N+1)` fraction of partitions. (The classic "≈1/N" is the
  `rf = 1` case.)
- **Every row remains readable exactly once** — the catalog keeps tiling every row
  with exactly `rf` owners apiece (rebalance never touches `row_range`; it only
  re-places ownership).

## How it works

`rebalance_partitions(Node& node, const std::vector<std::string>& members,
const std::vector<std::string>& previous_members = {})`
(`include/zetta/partition_rebalance.hpp`, `src/core/partition_rebalance.cpp`) is a
pure, non-throwing function — a sibling of [`write_partitioned`](partitioned-write-path.md).
For each **placed** partition in the catalog (`owner_nodes` non-empty):

1. **Read its partition id** from the catalog entry's `stats.partition` (recorded by
   the [write path](partitioned-write-path.md)). An entry with no recorded id can't
   be HRW-recomputed, so it is skipped.
2. **Preserve the replication factor** — `rf` is the number of owners the entry
   currently records (`owner_nodes.size()`, the count the partitioner placed at
   write time), clamped to `[1, members.size()]`. The catalog is the source of
   truth for `rf`, not the table's [`ReplicationStrategy`](replication-strategy.md):
   the strategy is *declarative intent*, while a partition could have been placed by
   any [`Partitioner`](partitioner.md) (e.g. a `RendezvousPlacement(rf = 2)` on a
   table merely declared `partitioned`), so the owners actually recorded are the
   faithful signal.
3. **Recompute** `rendezvous_owners(partition_id, members, rf)` — the same HRW core
   the write path placed with ([`rendezvous_placement.hpp`](partitioner.md)).
4. **If the owner set changed**, call `DatasetPartition::set_owner_nodes(new_owners)`
   — the **capturing** owner mutator: it rewrites the value, captures the
   `part:<id>` key into the CRDT (so the re-placement replicates like a create), and
   rewrites the on-disk projection.

### The bodies move for free

Once `owner_nodes` changes in the catalog, the existing **sharded blob placement**
([#515](sharded-blob-placement.md)) does the rest: the new owner lists the body in
`ReplicatedBlobStore::missing_blobs()` (so it fetches it over the `LOREQ`/`LORES`
seam), and a non-owner refuses it in `store_blob()`. No new transport, no new wire
format — the rebalance only mutates the catalog.

A **demoted** owner (a node that loses a partition) keeps its harmless stale on-disk
copy: `missing_blobs()` won't re-fetch it and `store_blob()` refuses it, and the
catalog defines ownership, so "readable exactly once / `rf` owners" holds at the
catalog level. Physical garbage collection of the demoted copy is a possible
follow-up.

## The auto-trigger (between jobs)

Cluster membership is **replicated state** — each node seeds its own `node:<id>` key
into the document (with its address; peers merge), so a join shows up in the CRDT
snapshot the [`Node`](architecture.md) already reconciles each `tick()`.
`Node::rebalance_partitions_on_membership_change(snapshot)` therefore runs **inside
`Node::tick()`** with no `Cluster` plumbing:

- `Node::start()` establishes a **membership baseline** (the first observation never
  rebalances — the loaded partitions already reflect their recorded placement).
- Each `tick()`, when the document advanced, the set of `node:<id>` keys is compared
  to the last observed membership. On a **delta** it calls `rebalance_partitions`
  with the new membership and the previous membership, then `tick()`'s persist step
  writes the moved document. A steady membership is a no-op (cheap — gated on the
  document version), so there is no feedback loop.

The `previous_members` is used to leave **replicate-to-all** partitions untouched —
see Scope.

## Scope

- **Sharded (rendezvous) partitions only.** A `SinglePartitioner` / `HashPartitioner`
  / `RoundRobinPartitioner` table places **replicate-to-all** (every member owns
  every partition), which is the "not really sharded yet" baseline — not the target
  of sharded rebalancing. Such a partition is detected (it is owned by exactly the
  `previous_members` set) and **left untouched**; growing a replicate-to-all
  partition onto new nodes is a separate concern. With an empty `previous_members`,
  no partition is classified replicate-to-all and every placed partition is treated
  as rendezvous-sharded (the pure "recompute rendezvous" behavior).
- **Joins, today.** `node:<id>` keys are append-only in the current membership
  representation (a leaving node's key isn't removed — the documented [membership
  migration debt](replication.md)), so the auto-trigger fires on **joins**.
  Leave-driven rebalance lands when membership removal does. The library function
  itself handles any membership delta.
- **The `rf >= cluster-size at write time` edge** under-counts `rf` (the owner count
  was clamped to the smaller cluster); grow a cluster past `rf` before relying on
  full replication. Documented, out of scope.
- **No physical body GC** (above), **no mid-job (elastic) rebalance** (deferred).

No new JSON format: the catalog entry rides the existing, schema-validated
[`DatasetPartition`](dataset-partitions.md) boundary, and `owner_nodes` is already a
validated field.

## Tests

`tests/test_partition_rebalance.cpp`:

- **`RendezvousMovesAboutOneOverNToNewNodeRfOne`** — the acceptance at `rf = 1`:
  adding a node moves *some* partitions onto it but **not all** (the HRW win); each
  move hands the partition to the new node and every untouched partition keeps its
  exact owner; the row ranges still tile `[0, total)`; re-running is a no-op.
- **`ReplicationFactorPreservedAcrossRebalance`** — `rf = 2`: every partition still
  has exactly two distinct owners drawn from the live members, the new node takes a
  share of the replicas, and the ranges still tile.
- **`MovedPartitionBodiesFetchByNewOwner`** — over the rebalanced document, the new
  owner's `ReplicatedBlobStore::missing_blobs()` is **exactly** the partition bodies
  it now owns (proving the blobs actually transfer to it).
- **`ReplicateToAllPartitionsAreSkipped`** — a `SinglePartitioner` partition is left
  untouched.
- **`TickRebalancesWhenAMemberJoins`** — the auto-trigger end to end: a `node:<id>`
  key appearing in the document drives a rebalance on the next `tick()`, and a
  further tick with no change is idempotent.

## Where it lives

- `include/zetta/partition_rebalance.hpp` / `src/core/partition_rebalance.cpp` —
  the `rebalance_partitions` function + the `RebalanceResult` / `PartitionMove`
  report.
- `include/zetta/dataset_partition.hpp` / `src/core/dataset_partition.cpp` —
  `DatasetPartition::set_owner_nodes`, the capturing owner mutator.
- `src/core/node.cpp` — the auto-trigger
  (`Node::rebalance_partitions_on_membership_change`, driven by `Node::tick()`; the
  membership baseline in `Node::start()`).

See [dataset-partitions.md](dataset-partitions.md),
[partitioned-write-path.md](partitioned-write-path.md),
[partitioner.md](partitioner.md), and
[sharded-blob-placement.md](sharded-blob-placement.md).
