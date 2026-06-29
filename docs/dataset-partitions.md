# Dataset partitions (the partition catalog)

A **DatasetPartition** (`zetta::DatasetPartition`) is one entry in the **partition
catalog** — a record of **where a slice of a table's data physically lives**. It
realizes the entity spec **ENT-36 / TBL-13**: a node knows, for every table, which
contiguous row ranges exist, which large-object body holds each one's bytes, and
which nodes own a copy. The catalog is **replicate-to-all**, so placement is
**globally visible** — every node sees the same partition set.

This page documents the **replicate-to-all slice** built in
[#510](https://github.com/oramasearch/zetta/issues/510): the `DatasetPartition`
type, its `part:<uuid>` CRDT key, how it reconciles onto every node, its
JSON-Schema boundary, and the read-only **`system.dataset_partitions`** SQL view
that exposes it. The *producer* that fills this catalog — the
[partitioned write path](partitioned-write-path.md) (`#511`,
`zetta::write_partitioned`), which splits a partitioned table into partitions,
writes each one's bytes as a blob, and records an entry here — is documented
separately. This slice is **storage + provenance + replication of the catalog
only** — recording and replicating where data lives.

The catalog's `owner_nodes` is now **acted on** by blob replication
([#515](https://github.com/oramasearch/zetta/issues/515)): a partition's backing
body (`blob_ref`) is fetched and held **only by its owner node(s)**, not replicated
to all — see [sharded-blob-placement.md](sharded-blob-placement.md). **Still out of
scope** (later slices): locality-aware reads that route a scan to a partition's
owner and read its blob back (`#516`), and re-balancing placement across the fleet
(`#517`).

It is modeled **exactly like** [`zetta::ComputeJob`](compute-jobs.md) /
[`zetta::Run`](runs.md): a top-level entity keyed by id, a flat CRDT key + on-disk
JSON projection, boundary validation against a formal JSON Schema. If you know
`ComputeJob`, you know `DatasetPartition`.

## The type

A `DatasetPartition` (`include/zetta/dataset_partition.hpp`) is a top-level entity
with no parent. Its value:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `table` | `std::string` | Owning table id (`tbl:<uuid>`). **Required** — the catalog's anchor. |
| `blob_ref` | `std::string` | Large-object body id (`lo:<uuid>`) holding this slice's bytes. Optional. |
| `row_range` | `PartitionRowRange` | The half-open `[start, end)` row ordinals this slice covers. |
| `stats` | `nlohmann::json` | Free-form per-column statistics (for partition pruning). Default `{}`. |
| `owner_nodes` | `vector<std::string>` | The node ids holding a copy of the body. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation. |

`table` is the only required field: a partition with no table ref has nothing to
attach to, so `create_dataset_partition` rejects it (empty handle). `blob_ref`
points at the [large-object](large-objects.md) body the same way a `Model`'s
weights reference one — the metadata rides in the catalog, the bytes live on disk
and move peer-to-peer (the body transfer for partitions is a later slice).
`row_range` names a contiguous slice of the table's logical row positions without
copying its keys; `stats` is a free-form object so the future partitioner can
record whatever pruning metadata it computes (min/max/count per column) without a
schema change here. `owner_nodes` is the placement: which nodes hold this
partition's body.

## CRUD on a Node

`Node` owns dataset partitions alongside databases / projects / runs / jobs. Like
jobs, partitions have **no human name** — they are keyed by `id`, so `find` /
`drop` take the id:

| Method | Returns | Behavior |
|---|---|---|
| `create_dataset_partition(value)` | `shared_ptr<DatasetPartition>` | Stamps `created` (when unset), **captures `part:<id>` into the CRDT** so the catalog replicates. Empty handle if `value.table` is empty. |
| `find_dataset_partition(id)` | `shared_ptr<DatasetPartition>` | Empty handle on miss. |
| `drop_dataset_partition(id)` | `bool` | `true` if one was removed (also removes the CRDT key). |
| `dataset_partitions()` | `vector<shared_ptr<DatasetPartition>>` | Stable order: by `created`, then `id`. |
| `dataset_partition_count()` | `size_t` | — |

```cpp
zetta::DatasetPartitionValue value;
value.table = items_table->id();
value.blob_ref = body_id;            // a lo:<uuid> holding the slice's bytes
value.row_range = {0, 10'000};       // rows [0, 10000)
value.owner_nodes = {node.node_id()};
auto partition = node.create_dataset_partition(value);
```

There is no user-facing mutator in this slice — a partition is recorded once and
replicates. (Re-placement that mutates `owner_nodes` / `stats` over time lands with
the partitioner / re-balancer; the entity already supports `apply_value`, which the
reconcile path uses for any peer-side change.)

## Queryable via SQL (`system.dataset_partitions`)

The partition catalog is exposed as a virtual, read-only system table —
**`system.dataset_partitions`** — the same computed-at-query-time pattern as the
other [system tables](query.md). It is **computed from `Node::dataset_partitions()`**
(nothing extra is stored or replicated for the view), so a `SELECT` always reflects
the live, replicated set:

```sql
SELECT * FROM system.dataset_partitions;
SELECT partition_id, table, owner_count
  FROM system.dataset_partitions WHERE table = '<tbl-uuid>';
```

| Column | Source |
|---|---|
| `partition_id` | `id` |
| `table` | the owning table id |
| `blob_ref` | the large-object body id |
| `row_start` / `row_end` | the `[start, end)` row range |
| `row_count` | `end - start` (convenience) |
| `owner_nodes` | the owner node ids, comma-joined |
| `owner_count` | how many nodes own a copy |
| `stats` | the free-form per-column stats object |
| `created` | as stored |

`SELECT *`, projection, and a single `WHERE` comparison work exactly as for any
system table. Qualified name only (`system.dataset_partitions`).

> **Naming.** This is the **first system table without the `zetta_` prefix**. The
> `system` database is reserved + zetta-controlled, so `zetta_` is redundant — the
> corrected convention names system tables plainly. The legacy `zetta_*` tables
> (`zetta_nodes`, `zetta_jobs`, …) are kept until the system-wide rename tracked in
> [#538](https://github.com/oramasearch/zetta/issues/538).

## Persistence & replication

Dataset partitions follow zetta's **"EVERYTHING is replicated state"** rule: the
Automerge CRDT document is the source of truth, and the in-memory list + on-disk
folder are a **projection** of it ([replication.md](replication.md)).

- **Replicated state.** Each partition is one flat key in the document:
  `part:<uuid> → { table, blob_ref, row_range:{start,end}, stats, owner_nodes,
  created }` (`src/core/catalog.hpp`). `create_dataset_partition` captures it; on
  each `tick()` the node **reconciles** the projection to match the document
  (`reconcile_partitions`, alongside the `lo:` / `job:` blocks), so a peer's
  partition materializes locally with **no re-capture** (`apply_value`, so no
  feedback loop). Versions converge conflict-free. Because the catalog is
  replicate-to-all, every node ends with the same partition set — placement is
  globally visible.
- **On-disk projection.** A partition is a folder named by its UUID under the data
  dir, holding `partition.json` (the value above plus `id`):

  ```
  data_dir/
    <part-uuid>/
      partition.json
  ```

  `Node::start()` rebuilds the in-memory list by scanning the data dir
  (`load_dataset_partitions()`) and — when replicated — reconciling from the CRDT
  document, so a partition **survives restart** in both standalone and replicated
  nodes.

## Boundary validation (JSON Schema)

Like [`ComputeJob`](compute-jobs.md) and [`Run`](runs.md), a `DatasetPartition`
value is validated against a **formal JSON Schema** (draft-07) rather than ad-hoc
shape checks. The schema is the single source of truth for the value shape — the
shape is sealed (`additionalProperties: false`), `table` is required, and
`owner_nodes` must be an array of strings — and is exposed via
`zetta::dataset_partition_value_schema()`. It lives inline in
`src/core/dataset_partition.cpp`; [`schemas/dataset_partition.schema.json`](../schemas/dataset_partition.schema.json)
is **generated** from it and kept in sync by `tests/test_schemas.cpp` (regenerate
with `ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R Schemas`).

Every value that **enters** the system passes through it:

- `validate_dataset_partition_value(json, &error)` — checks a value against the
  schema, filling `error` with a human-readable reason on failure.
- `parse_dataset_partition_value(json, &error)` — validate **then** decode into a
  `DatasetPartitionValue`, returning `nullopt` when validation fails.

Both the disk-load path (`Node::load_dataset_partitions`, reading `partition.json`)
and the CRDT-reconcile path (`Node::reconcile_partitions`, reading a `part:` value)
go through `parse_dataset_partition_value`, so a malformed record (missing `table`,
wrong types, stray fields) is **rejected at the boundary with a clear reason**
instead of corrupting the in-memory tree — fail fast, don't propagate. The
validator is the `pboettch/json-schema-validator` library, linked **PRIVATE** to
`zetta-core`; failures route through its error-handler callback (not exceptions).

## Tests

`tests/test_dataset_partition.cpp` covers the value type's JSON round-trip + the
schema boundary rejecting malformed values, the Node CRUD (create / find / drop +
the empty-`table` rejection), survival across a restart (disk projection), the
`system.dataset_partitions` view (rows, projection, `WHERE`), and — the
acceptance — a partition recorded on one node converging onto a second node over
the cluster and being **queryable there** via `SELECT ... FROM
system.dataset_partitions`.
