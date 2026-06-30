# Datasets (`system.datasets`)

A **dataset** is a first-class primitive in zetta's observability model: a table
*together with the data it holds*, summarized so an operator can see — at a glance,
in SQL — what data lives on the cluster, how big it is, what shape it has, and
**where its partitions physically sit** (which node owns which slice). This page
documents the **`system.datasets`** read-only view and its engine source
`zetta::Node::datasets()`, built in
[#585](https://github.com/oramasearch/zetta/issues/585) as the engine half of the
area-3 *dataset view* in the cluster-observability epic
[#575](https://github.com/oramasearch/zetta/issues/575).

There is **no stored `Dataset` entity**. zetta already models the pieces — a
[`Table`](storage.md) holds the rows, the [partition catalog](dataset-partitions.md)
(`system.dataset_partitions`) records where each slice lives, and the
[`datasets` shim](datasets-shim.md) loads a Parquet file into a (partitioned)
table. `system.datasets` does not add a fourth copy of that state: it is a
**computed-at-query-time aggregate**, recomputed from the live `Node` tree
(databases → tables) joined with the replicated partition catalog on every query.
**One dataset == one table.** Because it is computed, it can never drift from the
truth — the same rule every `system.<table>` view follows
([RULE-7](../CLAUDE.md): *surface observable state as `system.<table>`, computed
from the live source, never stored/hand-synced*).

## What it shows

`SELECT * FROM system.datasets` returns one row per table across every (non-system)
database. The columns:

| column | meaning |
| --- | --- |
| `dataset` | the dataset name (= the table name) |
| `database` | the owning database |
| `table_id` | the table id (a bare `tbl` uuid) — joins to `system.dataset_partitions.table` |
| `format` | the on-disk storage format of the rows (`jsonl` today — see the note below) |
| `replication` | the table's [replication strategy](replication-strategy.md) (`replicate_to_all` / `partitioned` / `partitioned_with_replicas`) |
| `rows` | row count |
| `size_bytes` | on-disk bytes of the row data (the JSONL chunks under the table folder) |
| `columns` | the inferred schema — field names (comma-joined, first-seen order) |
| `column_count` | number of inferred columns |
| `partitioned` | whether the dataset has any partition-catalog entries |
| `partition_count` | how many partitions the catalog records for it |
| `owner_nodes` | the de-duplicated set of nodes holding any of its partitions |
| `placement` | per-partition placement: `[start,end)@owner1\|owner2`, joined with `, ` (empty for a non-partitioned, replicate-to-all dataset) |

`WHERE` and projection work like any other system table, e.g.
`SELECT dataset, rows, owner_nodes FROM system.datasets WHERE dataset = 'events'`.

### Partition placement across the cluster

The headline of the dataset view is **which node owns which partition**. The
[partition catalog](dataset-partitions.md) is **replicate-to-all**, so every node
sees the same set of `part:<uuid>` entries — which means `system.datasets`
reports the *same* global placement on every node, not just this node's local
shards. For each dataset, the rows whose catalog `table` id matches are flattened
into `placement` (a readable "which slice → which owners" string) and `owner_nodes`
(the union). A **non-partitioned** dataset (`replicate_to_all`) has no catalog
entries — it lives on every node — so `placement` and `owner_nodes` are empty and
`replication` tells you it is replicate-to-all.

### A note on `format`

`format` is the **live on-disk storage format** of the dataset's rows, read from
the table's storage engine (`StorageEngine::name()`) — `jsonl` for every dataset
today, since the default engine is [`JsonlStore`](storage.md) and the
[partitioned write path](partitioned-write-path.md) also serializes its blobs as
JSON Lines. The *original import* format (a Parquet or CSV file the
[`datasets` shim](datasets-shim.md) read) is **not** reported: after a load the
rows are JSONL, and the import format is not retained in any live source — and
`system.datasets` is computed from the live source, never from stored provenance
(the RULE-7 constraint). When a future slice records source-format provenance, or
a new storage engine lands, this column will reflect it without a schema change.

## Engine source: `Node::datasets()`

`std::vector<DatasetSummary> Node::datasets() const`
([`include/zetta/dataset.hpp`](../include/zetta/dataset.hpp)) is the reusable
engine source behind the view. It is the **single live source** for both the SQL
view *and* a future GUI dataset panel, so the two cannot diverge (the
"prefer a table plus a panel that renders it" convention). Each `DatasetSummary`
aggregates, per table:

- **schema** — the union of each row's object keys in first-seen order (the engine
  is schemaless, so a dataset's schema is whatever its rows carry);
- **rows** — the storage engine's row count;
- **size_bytes** — the size of the `.jsonl` row chunks under the table folder;
- **format** / **replication** — the storage engine name + the table's
  replication strategy;
- **partitions** — a `DatasetPlacement` per matching catalog entry (its id, body
  ref, row range, and owner nodes) — the cross-cluster placement.

It touches the local filesystem only to size the row chunks (like
`system.checkpoints`); everything else is read from in-memory handles.

## No new JSON boundary

`system.datasets` introduces **no new JSON format**. `Node::datasets()` reads
already-validated in-memory handles — the table rows (stored by the storage
engine) and the partition catalog (validated at the
[`DatasetPartition` schema boundary](dataset-partitions.md) when it entered) — and
the view only projects them into result rows. There is no untrusted payload
entering here, so there is nothing new to schema-validate.

## Scope / later slices

- **Computed read view only.** A thin `Dataset` *entity* (a stored handle) is
  deliberately not introduced — a dataset is a table, and the observability source
  is the computed view. The convention is to add a stored entity only when a
  computed view genuinely cannot express the state; here it can.
- The **GUI dataset panel** (epic #575, area 3) renders this same source.
- **Source-format provenance** (surfacing `parquet`/`csv` rather than the storage
  format) and datasets that exist only as orphan partition-catalog entries (a
  dropped table) are out of scope.

## Tests

`tests/test_query.cpp` → `QueryService.SelectsDatasetsSystemTable` (headless):
an empty node has no datasets; a plain table reports its inferred schema / rows /
size / `jsonl` format with no placement; a table with two partition-catalog
entries reports `partition_count = 2` and a `placement` naming each slice's owner
nodes; `WHERE` + projection work over the view.

## See also

- [dataset-partitions.md](dataset-partitions.md) — the `system.dataset_partitions`
  partition catalog this view joins for placement.
- [datasets-shim.md](datasets-shim.md) — how a Parquet file becomes a (partitioned)
  dataset table.
- [query.md](query.md) — the query layer and the full set of `system` tables.
- [storage.md](storage.md) — the on-disk table format the size + format come from.
