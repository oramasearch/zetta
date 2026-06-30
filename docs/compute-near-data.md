# Compute-near-data: DDF ops over a node's local partitions

A distributed DDF op runs over **the partitions a node already holds** rather than
over a shard handed in from outside — the node enumerates its **owned** partitions
from the [partition catalog](dataset-partitions.md), materializes only the
partition bodies it physically has, runs the op over that local shard, and
exchanges only the small per-node **partial** over the
[collectives](collectives.md). The raw rows never cross the wire (**ARC-12 /
FEAT-8**; [#516](https://github.com/oramasearch/zetta/issues/516), the engine half
of TC-11 [#501](https://github.com/oramasearch/zetta/issues/501)).

This is the placement-aware replacement for the **"shard handed in out-of-band"**
path: the [distributed group-by](ddf-groupby.md) and friends already exchanged only
partials, but the caller built each node's `DataFrame` however it liked (see the
`ComputeNode` / `shards[i]` setup in `tests/test_ddf_group_by.cpp`). Now the shard
**source** is the catalog + the node's locally-held blob bodies, the way
[sharded blob placement](sharded-blob-placement.md) (#515) intended: a partition
body is fetched + held only by its owner node(s), so a node reading its owned
partitions reads them from local disk — no `LOREQ`, no full-dataset pull.

```
include/zetta/ddf/local_partitions.hpp    load_local_partitions + LocalShardResult — the reusable near-data shard loader
src/core/ddf/local_partitions.cpp         enumerate owned partitions → read bodies locally → parse JSONL → DataFrame
include/zetta/ddf/local_group_by_job.hpp  run_near_data_group_by_job + NearDataGroupByResult — the first-class near-data group-by
src/core/ddf/local_group_by_job.cpp       load_local_partitions → run_group_by_job, reporting local-read vs wire-moved bytes
include/zetta/ddf/local_featurize_job.hpp run_near_data_featurize_job + NearDataFeaturizeResult — near-data featurize over a partitioned table (#532)
src/core/ddf/local_featurize_job.cpp      load_local_partitions → run_featurize_job, reporting local-read vs wire-moved bytes
include/zetta/collectives.hpp             Collectives::wire_bytes() — the "bytes moved over the wire" meter
tests/test_ddf_local_partitions.cpp       the loader round-trip + the near-data group-by == single-node-oracle + bytes acceptance
tests/test_ddf_featurize_partitions.cpp   the near-data featurize == single-node-oracle, leakage-free (#532)
src/core/testkit/cases_distributed.cpp    cluster.compute_near_data — TC-11 as a runnable test-suite case
```

## The loader — `zetta::ddf::load_local_partitions`

The reusable primitive every op uses as its placement-aware shard source:

```cpp
LocalShardResult load_local_partitions(
    Node& node, const std::string& table, const DataFrameSchema& schema,
    compute::ComputeDevice& device, std::size_t chunk_rows = kDefaultChunkRows);
```

It:

1. **Enumerates this node's owned partitions of `table`** — every
   [`DatasetPartition`](dataset-partitions.md) in `Node::dataset_partitions()` (the
   in-memory source of the `system.dataset_partitions` view) whose `table` matches
   and whose `owner_nodes` contains `node.node_id()`. Partitions owned by other
   nodes are skipped (their bodies live elsewhere); the list is sorted by
   `row_range.start` so the loaded shard is deterministic.
2. **Reads each owned partition's blob body from local disk** —
   `database.large_objects().read(blob_ref)`, scanning the node's databases for the
   one holding the `lo:` body. Because the node owns the partition, the body is
   present locally (sharded placement put it there); a body that is *not* present is
   surfaced as a failing `ComputeStatus`, not silently skipped.
3. **Parses the body** — the body is the existing **JsonlStore line format**, one
   `{"_zid": <id>, "doc": <row>}` JSON object per line (exactly what the
   [partitioned write path](partitioned-write-path.md) emits), read the same
   tolerant way `JsonlStore::load` reads it. It **fails fast** (rather than dropping
   a row) on a line that is not valid JSON or not a JSON object, since a silent drop
   would diverge from the single-node oracle.
4. **Loads the rows into a typed, chunked `DataFrame`** via `load_data_frame`, on
   `device`, per `schema`.

The result carries **instrumentation** that makes the near-data guarantee
observable:

| field | meaning |
|-------|---------|
| `partitions_read` | owned partitions of the table materialized |
| `rows_read` | rows in this node's local shard |
| `bytes_read` | partition-body bytes read **from local disk** — read *near* the node, **not** moved over the wire |

### Why no new JSON-Schema boundary

The partition body is **not a new JSON format** — it is the internal `JsonlStore`
`{"_zid","doc"}` line shape, already produced + consumed elsewhere without a
per-line schema, and the blob is already **SHA-256-verified** at the
[large-object](large-objects.md) layer (byte integrity is guaranteed before this
loader sees it). Following the repo's existing convention for that format, the
loader does a structural, non-throwing boundary parse (object-or-bare-row, fail
fast on malformed) rather than a per-row `pboettch` schema validation — which would
also tax the compute-near-data hot path the whole feature exists to make efficient.
The op spec (`GroupBySpec`), the partials (`CollectiveMessage`), and the job record
keep their own validated boundaries, so no genuinely new format enters unchecked.

## The near-data op — `zetta::ddf::run_near_data_group_by_job`

The first-class compute-near-data op — the literal replacement for the out-of-band
path:

```cpp
NearDataGroupByResult run_near_data_group_by_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const DataFrameSchema& schema,
    const GroupBySpec& spec, compute::ComputeDevice& device,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGroupByTimeout);
```

It loads this node's shard with `load_local_partitions`, then drives the *exact
same* distributed group-by + [`ComputeJob`](compute-jobs.md) lifecycle as
[`run_group_by_job`](ddf-groupby.md) (#387) over that local frame — so the work is
recorded, audited, and appears in `system.zetta_jobs`. Call it **concurrently on
every participating node** (the collectives need all participants);
`coordinator.start()` must have run.

Because `distributed_group_by` broadcasts the merged partial back to every node, the
**global result is identical on every node** and equals a single-node group-by over
the whole dataset. The result extends the distributed result with near-data
instrumentation: `local_bytes_read` (read near data) versus `wire_bytes_moved`
(the partials this node moved over the collectives — measured via
`Collectives::wire_bytes()`).

### Bytes moved — `Collectives::wire_bytes()`

`Collectives` keeps a cumulative meter of every `collective.contribute` /
`collective.result` payload byte the node sends or receives — the **"bytes moved
over the wire"**. For a group-by that is the per-node partial (size proportional to
the **group count**), never the rows (proportional to the **row count**), so:

```
bytes-moved  ≈  partials     ≪     bytes-read-near-data  ≈  the dataset
```

That is the **TC-11 / #501 acceptance**, asserted directly in
`tests/test_ddf_local_partitions.cpp`: across a real 3-node loopback cluster, each
node loads its shard from its own partitions, the near-data group-by matches the
single-node oracle on every node, and the whole cluster moves under a fifth of what
it read near data. The same acceptance is also surfaced as a **runnable test-suite
case** — `cluster.compute_near_data` in the [in-app test suite](test-suite.md)
catalog (`zetta::testkit`, `TestCategory::Distributed`) — so TC-11 can be run + tuned
(nodes / rows / groups / partitions per node) from the headed **Operations → Test
suite** panel and headless (`tests/test_testkit.cpp`), reporting bytes-read-near-data
vs bytes-moved-over-the-wire as live metrics. Two further gtests harden the loader:
it is the reusable shard source for **other** DDF ops (a `filter` + group-by over the
loaded shard equals the same over the full table), and a partition **owned** by this
node whose body is missing locally **fails fast** rather than silently dropping rows
(which would diverge from the oracle).

## Other ops

`load_local_partitions` is the shared primitive — `scan` / `filter` /
[featurize](ddf-featurize.md) and any other distributed DDF op compose it the same
way: load the local shard from the node's owned partitions, run the op, and let the
op's existing collective combine exchange only the partial. The group-by
(`run_near_data_group_by_job`) and **featurize**
(`run_near_data_featurize_job`, [#532](https://github.com/oramasearch/zetta/issues/532)
— see below) are wired end to end; the remaining ops reuse the loader as their shard
source as they are driven through the [job coordinator](job-coordinator.md).

### Near-data featurize — `zetta::ddf::run_near_data_featurize_job`

The featurize sibling (`include/zetta/ddf/local_featurize_job.hpp`,
`src/core/ddf/local_featurize_job.cpp`) loads a node's shard from its owned partitions
of a single [partitioned table](partitioned-write-path.md), then drives
[`run_featurize_job`](ddf-featurize.md) over it. It is how the
saas-sales-conversations **engineered columns** are featurized over the partitioned
dataset (a static [`FeaturizeSpec`](ddf-featurize.md#static-mode--engineered-columns-no-windowed-events-532)
— engineered columns carried through, `outcome` the label, point-in-time filtered to
`event_ts <= as_of`). Each node reads only the partition bodies it holds (the dropped
embeddings never reach the wire), exchanges only the per-entity partials, and the
broadcast feature table matches a single-node oracle on every node.
`tests/test_ddf_featurize_partitions.cpp` proves it over a real 3-node loopback
cluster (no leakage; matches the oracle); the in-app **Distributed →
Featurize over partitions == single-node** test-suite case
(`cluster.featurize_partitions`) runs it interactively.

## Scope / later slices

- **Group-by + featurize are wired end to end**; `scan`/`filter` reuse the loader as
  their shard source (the composition is the same — load local, op, combine).
- The loader reads a node's owned partitions; **rebalancing** placement on a
  membership change ([#517](https://github.com/oramasearch/zetta/issues/517)) and a
  **load-triggering command/DDL** that fills the catalog from a real table are their
  own slices. Here the shard source is the catalog the
  [partitioned write path](partitioned-write-path.md) (#511) fills.
- The body is read from local disk; a **locality-aware fetch** that pulls a missing
  owned body on demand is a follow-up (today a missing owned body is an error).
