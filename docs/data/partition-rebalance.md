# Between-jobs partition rebalance on a membership change

When the cluster gains a node (e.g. after [`add_nodes`](../cloud/add-nodes.md)), the
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
- **The new placement reflects on every node.** A re-placement captured on the node
  that ran the rebalance is replicated state (`part:<id>` in the CRDT), so it
  reconciles onto every peer — the [`system.dataset_partitions`](dataset-partitions.md)
  catalog converges to the *identical* placement on each node, with no per-node
  coordination (the recompute is deterministic, so a peer that observes the same
  membership delta independently arrives at the same answer).
- **Every row remains readable exactly once** — the catalog keeps tiling every row
  with exactly `rf` owners apiece (rebalance never touches `row_range`; it only
  re-places ownership). On a node **leave**, the same recompute sheds the departed
  node's partitions onto the survivors (no row stranded), `rf` and the tiling held.

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

## Two triggers

The rebalance pass fires from two places, which compose:

### The direct trigger — `add_nodes` (#519)

[`add_nodes(node, provider, …)`](../cloud/add-nodes.md#rebalance-partitions-onto-the-new-members-519)
calls `rebalance_partitions` **directly** after a successful provision, so the data
spreads onto the new permanent members as a **synchronous, deterministic**
consequence of the control action — `previous_members` is `node.cluster_members()`
(the current `node:<id>` set) and `members` is that set plus the just-provisioned
member ids. This is what makes the acceptance ("after `add_nodes(azure, N)`,
partitions are rebalanced onto the new members") verifiable immediately, without
waiting for the new nodes to boot and gossip their membership in. It captures the
moved `part:` owners into the CRDT (so they replicate); `Node::tick()`'s persist step
writes the moved document on the next tick.

### The auto-trigger — `Node::tick()` (#517)

Cluster membership is **replicated state** — each node seeds its own `node:<id>` key
into the document (with its address; peers merge), so a join shows up in the CRDT
snapshot the [`Node`](../architecture/architecture.md) already reconciles each `tick()`.
`Node::rebalance_partitions_on_membership_change(snapshot)` therefore runs **inside
`Node::tick()`** with no `Cluster` plumbing:

- `Node::start()` establishes a **membership baseline** (the first observation never
  rebalances — the loaded partitions already reflect their recorded placement).
- Each `tick()`, when the document advanced, the set of `node:<id>` keys is compared
  to the last observed membership. On a **delta** it calls `rebalance_partitions`
  with the new membership and the previous membership, then `tick()`'s persist step
  writes the moved document. A steady membership is a no-op (cheap — gated on the
  document version), so there is no feedback loop.

The two compose: the direct trigger places onto the **reported** new members now; the
auto-trigger is the **safety net** that re-confirms against the **real** replicated
membership once those nodes actually join and gossip in — idempotent (it moves
nothing the direct pass already placed), and self-correcting if a reported member
never actually joins. The direct pass does not touch the tick baseline
(`last_membership_`), so the auto-trigger's own document-version change doesn't
re-fire it (the `node:<id>` set hasn't changed yet).

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
  migration debt](../cluster/replication.md)), so the auto-trigger fires on **joins**.
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
- **`AddNodesRebalancesOntoNewMembers`** — the #519 direct trigger:
  `add_nodes(node, provider, N)` over a partitioned table spreads ≈ rf/N of the
  partitions onto the new member (verified through `node.dataset_partitions()`),
  keeps every untouched partition's exact owner, keeps the ranges tiling, and never
  tears a member down.
- **`AddNodesWithNoNewMemberLeavesPlacement`** — a zero / best-effort-short add brings
  up no new member, so there is no membership delta and the catalog is unchanged.
- **`CatalogConvergesOnEveryNodeAfterJoin`** (**TC-10** / [#500](https://github.com/oramasearch/zetta/issues/500))
  — two **replicated** nodes: the writer partitions a table over 3 members, the peer
  converges to the initial catalog, then a join drives the production tick rebalance
  on the writer; after the peer reconciles, **both nodes' `system.dataset_partitions`
  views are queried and asserted identical** — same partitions, same owners, same
  `rf` — with the joiner owning a non-trivial-but-not-all share and the row ranges
  still tiling `[0, total)` on every node.
- **`LeaveReassignsPartitionsAwayFromDepartedNode`** — the "(or leaves)" clause at
  the library level: after a node leaves, no partition references it, `rf` is
  preserved across the survivors, and the rows still tile.

The same acceptance is also a runnable **in-app test-suite case** —
`cluster.partition_rebalance` ("Partition rebalance on node join (TC-10)") under
**Operations → Test suite** ([docs/testing/test-suite.md](../testing/test-suite.md)) — so it can be
exercised headless (`tests/test_testkit.cpp`) and from the GUI with tunable members
/ replication-factor / rows.

## Where it lives

- `include/zetta/partition_rebalance.hpp` / `src/core/partition_rebalance.cpp` —
  the `rebalance_partitions` function + the `RebalanceResult` / `PartitionMove`
  report.
- `include/zetta/dataset_partition.hpp` / `src/core/dataset_partition.cpp` —
  `DatasetPartition::set_owner_nodes`, the capturing owner mutator.
- `src/core/node.cpp` — the auto-trigger
  (`Node::rebalance_partitions_on_membership_change`, driven by `Node::tick()`; the
  membership baseline in `Node::start()`) + `Node::cluster_members()` (the current
  `node:<id>` membership the direct trigger feeds in as `previous_members`).
- `include/zetta/add_nodes.hpp` / `src/core/add_nodes.cpp` — the #519 direct trigger:
  the `add_nodes(Node&, …)` overloads that provision then drive `rebalance_partitions`.

See [dataset-partitions.md](dataset-partitions.md),
[partitioned-write-path.md](partitioned-write-path.md),
[partitioner.md](partitioner.md), and
[sharded-blob-placement.md](sharded-blob-placement.md).
