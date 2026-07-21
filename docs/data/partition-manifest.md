# The partition manifest — how many partitions a dataset HAS

`zetta::data::DatasetManifestValue` ·
[`include/zetta/data/dataset_manifest.hpp`](../../include/zetta/data/dataset_manifest.hpp) ·
[`src/core/data/dataset_manifest.cpp`](../../src/core/data/dataset_manifest.cpp) ·
[#837](https://github.com/oramasearch/zetta/issues/837)

## The hole it closes

A scatter-gather `SELECT` over a [partition-only table](partition-only-tables.md)
plans over the `part:` catalog entries **that have reached the coordinating
node**. Everything else about that read path is careful: an
[unreachable owner fails the query](scatter-gather-select.md), precisely because
a cached result set runs at RF=1 and answering from four of five shards is
unacceptable.

But a partition whose catalog entry has not gossiped here yet is not
*unreachable* — it is **invisible**. It never enters the plan, so the
unreachable-owner guard never fires, and the query answers short and looks
perfectly healthy. `SELECT count(*)` returning a plausible fraction is the single
most dangerous outcome of the whole partition-only design.

The coordinator therefore needs to know how many partitions it *should* be
planning over, independently of which entries it currently holds. That number is
the **manifest**.

## The shape

The CRDT replicates the **metadata describing the Parquet blobs**; it never
replicates the blobs. Per-partition metadata already rides in the catalog
(`part:<uuid>` → `DatasetPartitionValue`: `blob_ref`, `row_range`, `stats`,
`owner_nodes`). The manifest adds the one **table-level** fact that was missing:

```
mfst:<table-uuid> -> {
  "table":         "<table-uuid>",   // required — the tbl: entry this describes
  "partitions":    8,                // catalog entries the finished dataset HAS
  "rows":          540,              // rows across all of them
  "bytes":         73412,            // body bytes across all of them (0 = unknown)
  "partition_ids": ["…"],            // present only when the writer knew them all
  "created":       "2026-07-18T…Z"
}
```

Validated at the boundary against the embedded draft-07 schema
(`dataset_manifest_value_schema()`, generated into
[`schemas/dataset_manifest.schema.json`](../../schemas/dataset_manifest.schema.json)).
A malformed record is **rejected**, not adopted — a corrupt count would fail
every read of a perfectly healthy dataset, which is worse than the hole.

### Why its own key, not a field of `tbl:`

A co-launched ingest has **every participant** capture `tbl:<id>` for itself when
it adopts the target table (`Node::adopt_replicated_table`), so the table key has
several writers whose writes are concurrent in the CRDT sense. A manifest folded
into that value could lose the merge. `mfst:<table-id>` has exactly **one**
writer, so it cannot.

## Who writes it, and when

Written **once**, when the write that produced the dataset completed:

| Path | Writer |
|---|---|
| A local partitioned write (`write_partitioned`) | that call, from its own totals |
| A cross-node connector ingest | the **initiator**, from the cluster-wide allreduce |

A *tile* of a cluster-wide ingest deliberately records **no** manifest
(`write_partitioned(..., record_manifest=false)`): a tile knows only its own
share, and a manifest describing a third of the dataset would fail every read of
a healthy one. The initiator's `allreduce` already agrees on `{landed, rows}`;
it now carries `{partitions, bytes}` in the same round, which is the only way to
learn the cluster-wide partition count — only **non-empty** partitions get a
catalog entry, so how many each tile produced is data, not arithmetic.

It is also persisted in the table's `table.json`, so a restarted node knows the
count before the first gossip round (and so a node with no `ReplicationManager`
has one at all), and it is removed with the table (`drop_table` removes
`mfst:<id>` alongside `tbl:<id>`) so a re-created id cannot inherit a stale
expectation.

## Who reads it

One rule, `zetta::data::catalog_shortfall(manifest, visible)`, so the planner and
the observability view can never disagree:

- **`src/core/query/distributed_select.cpp`** — before anything is scattered
  (both the clustered `DistributedSelect::execute` and the standalone
  `select_over_partitions`), the visible `part:` entries are counted against the
  manifest. Short ⇒ the query **fails** with the reason, exactly as an
  unreachable owner already does. Never an answer from a subset.
- **`Node::datasets()` / `system.datasets`** — an incomplete catalog is reported
  as `degraded` with `expected_partitions` / `expected_rows`, rather than quietly
  under-reporting `rows`. An agent reading `rows` must be able to tell a total
  from a fraction.

A view holding **more** entries than the manifest declares is not short (a table
written twice accumulates entries), so only a shortfall is reported.

**A table with no manifest is never checked.** It was written before manifests
existed, or by a path that does not produce one; failing every read of such a
dataset would be a far worse regression than the hole this closes.

## Enriched per-partition stats

The same change records the blob's **own** metadata in each `part:` entry's
`stats`, beside the row count already there:

```
"schema": [{"name": "amount", "type": "int64", "optional": false}, …],
"bytes":  18244
```

`type` is the Parquet **physical type** the column was written as for a Parquet
body, and the JSON family for a JSON Lines fallback body (there is no Parquet
type for a body Parquet could not express). Together with `columns` and
`row_range` that is everything a planner needs: a node can plan, prune, and
report `system.datasets` for a partition it does not hold a byte of — **without
reading a body**.

## Tests

[`tests/data/test_dataset_manifest.cpp`](../../tests/data/test_dataset_manifest.cpp)

- `RoundTripsThroughItsSchema`, `RejectsAMalformedValue` — the boundary.
- `ShortfallIsReportedOnlyWhenShort` — the one completeness rule.
- `PartitionedWriteRecordsIt` — `write_partitioned` records count/rows/bytes/ids.
- `SurvivesARestart` — read back from `table.json` on a fresh node.
- `PartitionStatsCarryTheBodySchemaAndSize` — plan without reading a body.
- `AnIncompleteCatalogFailsTheScatterGatherSelect` — the point of the issue,
  constructed deterministically (drop one `part:` entry, keep the manifest), plus
  `ACompleteCatalogStillAnswers` for the no-false-positive direction.

## Related

[scatter-gather-select.md](scatter-gather-select.md) ·
[partition-only-tables.md](partition-only-tables.md) ·
[partitioned-write-path.md](partitioned-write-path.md) ·
[distributed-connector-ingest.md](distributed-connector-ingest.md) ·
[system-datasets.md](system-datasets.md)
