# Partition-only tables — rows that never enter the CRDT

A **partition-only** table's rows are not captured into the replicated document.
Its data is the **partition bodies** the [partitioned write path](partitioned-write-path.md)
emitted, plus the replicate-to-all [partition catalog](dataset-partitions.md) that
records who owns them. `zetta::data::Table::partition_only()` is the marker; it is
fixed at create time and rides in both `table.json` and the `tbl:` CRDT value, so
every node reads the table the same way.

## Why

A distributed ingest has each node fetch a tile of the source and land it as
partitions **it** owns. If those rows were also captured, every one of them would
replicate back to every peer — the data would cross the wire a second time, which
is exactly what tiling the fetch was meant to avoid. `AddDatasetOptions::partition_only`
turns the capture off for that dataset.

## Why a capture-free write is still durable

Two facts about the existing engine, verified by
[`tests/data/test_partition_only.cpp`](../../tests/data/test_partition_only.cpp):

- **Reconcile never prunes rows.** `Node::reconcile_databases` prunes databases,
  tables and large objects against the document, but rows are only *additively*
  materialized from it. A row present locally and absent from the CRDT is
  therefore left alone — there is no pass that would delete it.
- **`JsonlStore` reloads from disk in its constructor.** The local slice comes
  back on restart independently of the CRDT, and the partition bodies are large
  objects whose bytes live on their owners' disks.

So the only thing that had to be built was the marker plus the two read paths
below.

## What the marker changes

| Path | Normal table | Partition-only table |
|---|---|---|
| `Table::add_row` / `update_row` / `delete_row` | writes `row:<id>` into the CRDT, then the local store | local store only — no `row:` key, no tombstone |
| `Node::migrate_to_replication` | captures every local row | captures the table, skips its rows |
| `Node::datasets()` row count + schema | `store().size()` and the rows' keys | summed from the catalog's `row_range`s and `stats.columns` |
| `SELECT` | scans the local store | [scatter-gather across the partition owners](scatter-gather-select.md) |

The local store still holds the slice the ingesting node produced — that is where
`write_partitioned` reads the rows from — but nothing *reads a partition-only
dataset back* through it. Both read paths go through the catalog, which is why a
node that produced no slice still reports the whole dataset instead of zero, and
why a node that produced *all* of it does not report the rows twice.

## `system.datasets` counts from the catalog

`Node::datasets()` sums `row_range.end - row_range.start` over the table's catalog
entries. The catalog is replicate-to-all, so the count is identical on every node.
The column list comes from the same place: `write_partitioned` records each
partition's column names in `stats.columns`, because a peer's local store is empty
and a schema inferred from it would be empty too.

`stats.columns` is the *first-seen* key order within each partition, unioned
across partitions — the same schemaless rule a store-based inference uses, just
sourced from the catalog.

## What is deliberately not here

- **No update path for the marker.** Flipping it on a populated table would
  strand rows on one side or the other (captured rows with no bodies, or bodies
  with rows the document also holds). A dataset is created partition-only or not.
- **No pruning of the ingesting node's staging rows, except after an object
  load.** Dropping them would make `write_partitioned` unable to re-run and would
  not save the write; it is bytes on one node's disk, not on the wire, and the read
  paths ignore them. `load_dataset_object` is the exception (#1302): its loads run
  to 1 GiB, so once its partitions are written it frees the loading node's copy,
  memory and chunk files both, with `Table::release_local_rows`
  ([add-dataset.md](add-dataset.md#load-from-a-stored-object-1302)).

## Where it is used

`add_dataset_rows` / `add_dataset_file` take it through
`AddDatasetOptions::partition_only`, and `load_dataset_object` always sets it. The connector path (a result set that is a
*cache*, [connector-query.md](connector-query.md)) is its intended user: RF=1
bodies placed by fetch affinity, nothing captured.

Related: [scatter-gather-select.md](scatter-gather-select.md) (the read path),
[system-datasets.md](system-datasets.md), [replication-strategy.md](replication-strategy.md)
(a *different* knob — how many copies of a shard exist, not whether rows are
captured). Issue: #833 section 4.
