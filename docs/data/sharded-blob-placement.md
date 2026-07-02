# Sharded blob placement (partition-aware body replication)

A **partition's blob is fetched and held only by its owner node(s)** — not
replicated to every node. This is the placement engine for a
[`Partitioned` / `PartitionedWithReplicas`](replication-strategy.md) table's
large-object **bodies**: where a plain large object replicates to all, a
partition's backing body lives on exactly the nodes the
[partition catalog](dataset-partitions.md) names as its owners, honoring the
replication factor.

It realizes **issue [#515](https://github.com/oramasearch/zetta/issues/515)** — the
first slice that *acts* on a non-default `ReplicationStrategy` at the body level.

## What changed

Large-object bodies move peer-to-peer over the
[`BlobStore`](large-objects.md#replication) seam (`LOREQ`/`LORES`), driven by
`ReplicatedBlobStore::missing_blobs()` — the ids a node holds metadata for but is
missing the body of. Previously this was unconditional **replicate-to-all**: every
node listed every body it lacked and fetched it.

Now `ReplicatedBlobStore` (`src/core/data/replicated_blob_store.cpp`) is
**partition-aware**:

- **`missing_blobs()`** — a large-object body that is the backing blob of a dataset
  partition (a `part:` catalog entry's `blob_ref`) is listed **only when this node
  is in that partition's `owner_nodes`**. A non-owner never lists it, so it never
  sends a `LOREQ` for it and never fetches it.
- **`store_blob()`** — a non-owner **refuses** a pushed partition body (returns
  `false`, nothing written). A non-owner never requests one, so this is
  defense-in-depth: even an unsolicited push lands only on an owner.
- **`read_blob()`** — unchanged. A non-owner simply has no body to serve.

A **plain large object** (not referenced by any partition), an **unplaced
partition** (empty `owner_nodes`), and **model weights** all keep the default
replicate-to-all behavior — nothing about the existing path changes for them.

### Result

With `owner_nodes` of size `rf` (a replication factor of `rf`), exactly `rf` nodes
fetch and hold the body — the body **exists only on its owners**, on **exactly
`rf` nodes**. With `rf = 1` (`Partitioned`) a single node holds each shard's body;
with `rf > 1` (`PartitionedWithReplicas`) that many copies exist.

## Where the owner set comes from

The owner set is the partition's **`owner_nodes`** — recorded by placement and
**globally visible** via the read-only
[`system.dataset_partitions`](dataset-partitions.md#systemdataset_partitions-the-sql-view)
SQL view. Because the catalog is **replicate-to-all**, every node sees the same
partition set and computes the same placement, with no election (the leaderless
model — see [replication.md](../cluster/replication.md)).

`ReplicatedBlobStore` reads this from the **CRDT snapshot directly** (the `part:`
keys that `system.dataset_partitions` is itself computed from), never from the
`Node` tree — it runs on the cluster's **network thread**, so it must not touch the
in-memory tree (the same rule the rest of the store follows). It builds a
**partition-owner index** (`large-object body id → owner node ids`) once per call:

- It reads each `part:` value through the **validated boundary**
  (`parse_dataset_partition_value`, the [DatasetPartition JSON
  Schema](dataset-partitions.md#boundary-validation-json-schema)), so a malformed
  catalog entry is ignored, not trusted — the same fail-fast discipline as
  `Node::reconcile_partitions`.
- It normalizes `blob_ref` to the bare large-object id, tolerating either the
  `lo:<uuid>` catalog-key form or a bare `<uuid>`.
- A partition with an **empty** `owner_nodes` is **omitted** from the index, so its
  body falls back to replicate-to-all rather than being **stranded** with no owner
  to fetch it (the safe choice for a not-yet-placed partition).

No new JSON format is introduced: the placement rides the existing, schema-validated
[`DatasetPartition`](dataset-partitions.md) value and the
[`lo:` large-object metadata](large-objects.md).

## Scope

This slice is the **placement rule for bodies that are already written + cataloged**
— it decides which nodes fetch and keep a partition's body. **Out of scope** (later
slices, as with the rest of the [partition catalog](dataset-partitions.md)):

- the **partitioner write path** ([#511](https://github.com/oramasearch/zetta/issues/511))
  that splits a table into partitions and writes their bodies in the first place
  (and records `owner_nodes` from the [placement strategy](partitioner.md)),
- **locality-aware reads** that route a scan to a partition's owner.

The **re-balancing** of placement on a membership change (whole-partition moves,
which the [rendezvous placement](partitioner.md) keeps to ~1/N of partitions) is
**built** — see [partition-rebalance.md](partition-rebalance.md): it updates a
partition's `owner_nodes`, and this store moves the body to the new owners over the
same seam.

## Tests

`tests/data/test_large_object_replication.cpp` (alongside the other `ReplicatedBlobStore`
unit tests):

- **`PartitionBodyFetchedAndHeldOnlyByOwners`** — an owner lists the body as missing
  (it would fetch) and accepts a store; a non-owner does neither.
- **`ReplicationFactorPlacesBodyOnExactlyRfNodes`** — across three nodes with
  `owner_nodes` of size 2, exactly the two owners would fetch + hold the body.
- **`UnplacedPartitionBodyReplicatesToAll`** — a partition with no owners is not
  stranded: its body still replicates to all.
- **`OwnerCheckToleratesBareBlobRef`** — placement resolves a `blob_ref` recorded as
  a bare uuid (no `lo:` prefix), not only the `lo:<uuid>` form.

## Where it lives

- `src/core/data/replicated_blob_store.cpp` / `include/zetta/data/replicated_blob_store.hpp`
  — the partition-owner index + the owner check in `missing_blobs` / `store_blob`.
- `include/zetta/data/dataset_partition.hpp` — the `owner_nodes` / `blob_ref` source.
- `src/core/cluster/cluster.cpp` — the `LOREQ`/`LORES` loop that consumes `missing_blobs()`
  (unchanged; it just asks for fewer bodies on a non-owner).

See [large-objects.md](large-objects.md),
[dataset-partitions.md](dataset-partitions.md), and
[replication-strategy.md](replication-strategy.md).
