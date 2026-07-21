# Partitioned write path (rows → partition → blob, placement recorded)

The **partitioned write path** (`zetta::data::write_partitioned`,
`include/zetta/data/partition_writer.hpp`, **#511**) is where the partitioning pieces
finally meet: it takes a table configured with a [`Partitioner`](partitioner.md)
and **lays its rows out as partitions** — assigning each row to a partition,
serializing each partition's bytes, storing them as a
[large-object](large-objects.md) blob, and recording the placement in the
[partition catalog](dataset-partitions.md) (`system.dataset_partitions`).

It composes three already-built slices into one operation:

| Piece | Built in | Role here |
|---|---|---|
| [`Partitioner`](partitioner.md) (`assign_partition` / `place_partition`) | #512–#514 | row → partition, partition → owner node(s) |
| [`LargeObjectStore`](large-objects.md) | (existing) | a partition's serialized rows → a `lo:` body that gossips peer-to-peer |
| [`DatasetPartition`](dataset-partitions.md) catalog (`part:<uuid>`) | #510 | the recorded placement (blob ref + row range + owner nodes), globally visible |

**Acceptance:** loading a dataset into a partitioned table produces **N partition
blobs + N catalog rows with correct row ranges** — verified by
`tests/data/test_partition_writer.cpp`.

## The entry point

```cpp
PartitionWriteResult write_partitioned(
    Node& node, Database& database, Table& table,
    const std::vector<std::string>& members = {});
```

`table` is the partitioned table (its rows are read through its
[`StorageEngine`](storage.md)); `database` owns the
[`LargeObjectStore`](large-objects.md) the partition bodies land in; `node`
records the catalog entries (`Node::create_dataset_partition`). `members` is the
cluster membership placement is computed over (each a node id, e.g. from
`Cluster::node_ids()`); when empty it defaults to **this node only**
(`{node.node_id()}`), so a standalone write still records a concrete owner.

The result:

```cpp
struct PartitionWriteResult {
    std::vector<std::shared_ptr<DatasetPartition>> partitions;  // one per non-empty partition
    std::size_t rows_written = 0;
    std::string error;          // empty on success
    bool ok() const;
};
```

`partitions` holds the catalog entries created — one per **non-empty** partition,
in ascending partition-id order — so `partitions.size()` is *N*, the number of
partitions that received data. The write is **non-throwing**; it is **not
transactional** in this slice — a mid-way failure (e.g. a blob that can't be
written) returns an `error` and leaves the partitions already created in place
(listed in `partitions`).

## What it does, step by step

For a table with a partitioner configured (`Table::set_partitioner` — the
operational signal a table is partitioned; a table with no partitioner is rejected
with an `error`):

1. **Assign each row to a partition** via `partitioner->assign_partition(record)`.
   The write path stamps a **monotonically-increasing surrogate** — `_seq` = the
   row's table ordinal — onto the *assignment record* when the row doesn't already
   carry it, so a [`RoundRobinPartitioner`](partitioner.md#roundrobinpartitioner--surrogate--p)
   distributes evenly without the caller pre-stamping (the seam doc's "the write
   path stamps the monotonic surrogate"). The stamp is **assignment-only** — it is
   not written into the stored or serialized row — and `assign_partition` is pure,
   so a `HashPartitioner` (which reads its key columns) and `SinglePartitioner`
   simply ignore the extra field.

2. **Serialize each non-empty partition's rows to bytes** — a
   [**Parquet body**](partition-body.md): the rows' fields as columns, with each
   row's CRDT id in a `_zid` column beside them, so a partition body is a
   **self-describing slice of the table** that can be read a page at a time
   ([`ParquetStore`](parquet-store.md)) and projected column by column. Rows a
   flat, scalar Parquet schema cannot express (a nested value, a column mixing
   types) fall back to the previous **JSON Lines** shape rather than being
   coerced — `PartitionBodyWriteResult::format` says which, and the catalog
   records it.

3. **Store each partition's bytes as a large object** via the new
   `LargeObjectStore::import_bytes(name, bytes)` — so the body is fingerprinted
   (SHA-256), its `lo:<uuid>` metadata rides in the CRDT, and it transfers
   peer-to-peer + is verified against size + sha256 like any other
   [large-object body](large-objects.md). The blob is named
   `partition-<table-id>-<partition>.parquet` (or `….jsonl` for a fallback body),
   so its format is visible without opening it.

4. **Record placement** as a [`DatasetPartition`](dataset-partitions.md) catalog
   entry per partition (`Node::create_dataset_partition` → captured into
   `part:<uuid>`, visible in `system.dataset_partitions`):

   | Field | Value |
   |---|---|
   | `table` | the table's id |
   | `blob_ref` | the large object's id (the body holding this partition's rows) |
   | `row_range` | the partition's `[start, end)` slice (see below) |
   | `owner_nodes` | `partitioner->place_partition(partition, members)` — primary first, then replicas |
   | `stats` | `{ "partition": <id>, "rows": <count>, "format": "parquet"\|"jsonl", "placement": <partitioner name>, "columns": [...] }` (provenance; richer per-column pruning stats are a later slice) |

## Row ranges — the cumulative slice

A partition's `row_range` is its **cumulative slice in partition-id order** —
partition 0 covers `[0, n0)`, the next non-empty partition `[n0, n0 + n1)`, and so
on — so the ranges **tile `[0, rows_written)`** with no gaps or overlaps. For the
[`SinglePartitioner`](partitioner.md#singlepartitioner--the-trivial-baseline)
baseline (one partition holding everything) this reduces to the whole table
`[0, total)`.

The range is **this call's** row space. When several nodes write into the same
table — the [cross-node connector ingest](distributed-connector-ingest.md), where
each participant lands its own tile — each node's ranges start at 0, so ranges
from different nodes overlap. Nothing depends on them being globally unique:
`system.datasets` sums range *sizes*, and the
[scatter-gather SELECT](scatter-gather-select.md) uses `row_range.start` only to
order the partitions it plans.

This is deliberate. A [`HashPartitioner`](partitioner.md#hashpartitioner--hashkey-columns--p)
**scatters** a partition's rows across the table's *original* ordinals (rows with
keys hashing to the same bucket are not contiguous), which a single `[start, end)`
cannot represent — so the catalog's range names the slice in the **partitioned
(re-numbered) row space** instead, which the [`DatasetPartition` schema](dataset-partitions.md#boundary-validation-json-schema)
expresses as one contiguous range and which a future locality-aware read
(#516) consumes alongside the blob. The range size always equals the partition's
row count (and the blob's line count).

## Boundary validation

This slice introduces **no new JSON format that is parsed back in**, so it adds no
new schema:

- The **catalog entry** is the JSON that enters the replicated state, and it is
  built from typed fields and validated against the existing
  [`DatasetPartition` JSON Schema](dataset-partitions.md#boundary-validation-json-schema)
  (`parse_dataset_partition_value`) on every disk-load / reconcile boundary, like
  every other catalog entry.
- The **partition body** is a [large-object body](large-objects.md) — opaque bytes
  verified by **size + SHA-256**, the same trust boundary every LO body uses. It is
  *produced* here from already-validated stored rows; nothing in this slice parses
  it back (reading partitions for locality-aware scans is #516), so there is no new
  "data enters the system" boundary to schema-check.

## Scope

This slice is the **write path + recorded placement**. Out of scope (later
slices): a top-level command/DDL that triggers it on load (it is a reusable
library function today, like `import_records` and `run_churn_solution_on_node`), driving a
table's [`ReplicationStrategy`](replication-strategy.md) replication factor into
the placement (the write path uses the partitioner's own `place_partition`), and
locality-aware **reads** that route a scan to a partition's owner and read the blob
back (#516). **Rebalance on a membership change (#517) is built** — see
[partition-rebalance.md](partition-rebalance.md): when a node joins, rendezvous
placement is re-run and the changed partitions' `owner_nodes` updated (so their
bodies move to the new owners). `ReplicationStrategy` remains the *declarative*
intent recorded on the table; the `Partitioner` is the *mechanism* this path
drives.

## Tests

`tests/data/test_partition_writer.cpp` covers:

- **the acceptance** — a hash-partitioned 12-row load produces *N* blobs + *N*
  catalog rows whose ranges tile `[0, 12)`, each blob existing as a large object
  with the right line count;
- **faithful bodies** — every row appears in exactly one partition blob, parseable
  in the `{"_zid", "doc"}` shape (no drops, no duplication);
- **`SinglePartitioner`** — one blob, range `[0, total)`, replicate-to-all owners;
- **`RoundRobinPartitioner`** — the write path stamps the surrogate so 8 rows over
  `P = 4` land exactly 2-per-partition;
- **`RendezvousPlacement`** — each partition records `replication_factor` distinct
  owners drawn from the membership (sharded placement);
- **default members** — a standalone write owns its partitions itself
  (`{node.node_id()}`);
- **guards** — an unpartitioned table is rejected; an empty table writes nothing
  cleanly;
- **queryability** — the recorded partitions show up in
  `system.dataset_partitions` with row counts summing to the table's size.

## Pointers

- [partitioner.md](partitioner.md) — the `Partitioner` seam this path drives
  (assignment + placement strategies).
- [dataset-partitions.md](dataset-partitions.md) — the catalog entity + the
  `system.dataset_partitions` view the placements are recorded in.
- [large-objects.md](large-objects.md) — the large-object store partition bodies
  are written to (`import_bytes`).
- [storage.md](storage.md) — the `StorageEngine` rows are read from + the JSONL
  shape the blobs mirror.
- [replication-strategy.md](replication-strategy.md) — the per-table declarative
  `Partitioned` strategy this path is the mechanism behind.
