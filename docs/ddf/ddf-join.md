# DDF distributed join (`zetta::ddf::distributed_join`)

The **DDF distributed equi-join** is the first real *op* on top of the
[DataFrame core](ddf.md) (#380): an **inner equi-join** of two frames on a key
column, executed **across the cluster** (**ARC-12**; part of the native distributed
accelerated-compute pass for UC-1 churn,
[#264](https://github.com/oramasearch/zetta/issues/264) /
[#382](https://github.com/oramasearch/zetta/issues/382)).

```
include/zetta/ddf/join.hpp   the join types + the local + distributed entry points
src/core/ddf/join.cpp        to_host_relation, hash/sort-merge join, repartition, distributed_join
tests/ddf/test_ddf_join.cpp      local joins, the serialize/partition boundary, and the
                             single-node == multi-node acceptance
```

## The shape

Each participating node holds a **shard** of the `left` frame and a shard of the
`right` frame (in SPMD: every node runs the same join body against its own data).
The join runs in two stages:

1. **Repartition by key** so every row whose key falls in partition `p` lands on
   the node that owns `p`.
2. **Local join per shard** — a [hash](#local-join-hash--sort-merge) or
   [sort-merge](#local-join-hash--sort-merge) join of this node's left partition
   against its right partition.

Because equal keys hash to the same partition, every matching `(left, right)` pair
lands on exactly one node and is emitted exactly once. **The global join is the
union of every node's returned shard.** A single-node cluster degenerates to "one
partition, keep everything" — a single shard that is the whole join — so a
single-node and a multi-node run produce **identical results** (the acceptance).

```cpp
#include "zetta/ddf/join.hpp"

// Inside a coordinated job body (SPMD: runs on every participant):
auto result = zetta::ddf::distributed_join(
    round,                       // the JobRound handed in by JobCoordinator::run
    accounts, "account_id",      // left frame  + its key column
    events,   "account_id",      // right frame + its key column
    zetta::ddf::JoinAlgorithm::Hash);
if (!result.ok()) { /* result.status.message */ }
// result.rows is THIS node's shard of the global join:
//   columns = [account_id, <left value cols…>, <right value cols…>], rows row-major float64.
```

The op runs through the [job coordinator](../compute/job-coordinator.md)'s `JobRound` — the
single surface the DDF / GBDT / SHAP algorithms submit work through — so a join is
driven as a `ComputeJob` of kind `ddf` exactly like the other compute algorithms.

## Repartition via collectives (the all-to-all)

The cluster has **no all-to-all / scatter collective** — only
[allreduce / broadcast / barrier / gather](../cluster/collectives.md). The repartition is
built from the two it has:

1. every node **`gather`s** its serialized rows to the deterministic root
   (lowest node id);
2. the root **concatenates** them into the full relation;
3. a **`broadcast`** hands that full relation back to every node;
4. each node **filters** to keep only its own partition
   (`std::hash(key) % participant_count`), then joins.

This is **correct-first**, mirroring how [collectives.md](../cluster/collectives.md) ships a
*star* reduction before a ring/tree: a true point-to-point shuffle (sending
partition `p` only to node `p`, instead of broadcasting everything everywhere) is a
later optimization behind this same API. For fixture-scale frames the broadcast
all-to-all is simple and exact; the bandwidth win is a follow-on.

The partition function is a pure function of the key value, so **equal keys always
share a partition on every node** — the property that makes the union of per-node
shards a correct, duplicate-free join. The participant order is the collectives'
sorted node-id list, identical on every node.

## Local join (hash / sort-merge)

Both local algorithms are pure, in-memory, and produce the **same set** of output
rows (a different order before `sort_rows()` canonicalizes):

- **`hash_join`** — build a hash multimap on `right`'s keys, probe it with `left`'s
  rows. Duplicate keys on either side produce the full **cartesian product** of the
  matching rows (classic inner join).
- **`sort_merge_join`** — sort both sides by key, then merge equal-key groups
  (their cartesian product). `NaN` keys are dropped (they cannot equi-match, and
  dropping them keeps the merge total-ordered).

`local_join(left, right, algorithm)` dispatches between them. The output columns are
the **key**, then `left`'s value columns, then `right`'s value columns. (Callers
should keep `left` and `right` value-column names distinct; the op concatenates them
verbatim.)

## Values ride as float64

A `DataFrame` is [numeric by construction](ddf.md), and the whole compute /
collective plane works in **float64** ([collectives carry `std::vector<double>`](../cluster/collectives.md)).
`to_host_relation` therefore materializes the key + value columns of a frame as
`double`, widening any integer dtype. Keys or payloads outside float64's exact-
integer range (|value| > 2⁵³) lose precision — a documented boundary of the
double-based plane, not specific to the join. For the churn fixture (small integer
ids, float amounts) the conversion is exact.

## No new wire format — but a checked boundary

Like the [job coordinator](../compute/job-coordinator.md), the join introduces **no JSON of its
own**. The repartition serializes each relation's rows to the flat float64 form the
collective plane already carries (`[key, value₀ … value_{w-1}]` per row) and rides
the existing, schema-validated [`CollectiveMessage`](../cluster/collectives.md). Column names do
**not** travel — every node knows them from its own (SPMD-identical) schema.

The deserialize step is the **in-code boundary check** on the way back in
(`deserialize_relation`): the blob length must be a whole number of rows of width
`1 + value_columns`, so a malformed or truncated broadcast yields a failing
`ComputeStatus` (and an empty shard) instead of corrupting state — the same
"validate at the boundary" discipline the rest of zetta applies, layered on top of
the collective message's own draft-07 schema.

## Scope

This slice is the **inner equi-join** only — left / right / full outer joins are a
later issue, behind the same entry point. There is **no Node CRUD / SQL surface and
no `ComputeJob` `ddf`-kind wiring** added here; the integration test drives the join
through an existing `Ddf` job created with `Node::create_job`. The op builds purely
on the [DataFrame core](ddf.md) and [collectives](../cluster/collectives.md).

## Tests

`tests/ddf/test_ddf_join.cpp` covers:

- **Local joins** (no cluster): hash vs sort-merge agreeing on the fixture, the
  duplicate-key cartesian product, the empty-result case, and a key-only relation.
- **The repartition primitives** (pure): serialize ↔ deserialize round-trip, the
  malformed-blob rejection, partition determinism + range, and that the partitions
  cover every row exactly once.
- **`to_host_relation`** reading a frame's key + value columns (widened to float64).
- **The acceptance**: a single-node join matching the expected rows, a 3-node
  distributed join matching the single-node result, and the sort-merge variant
  matching — each run end-to-end through a `JobCoordinator` on a loopback cluster,
  with collectives doing the repartition.

See also [ddf.md](ddf.md) (the DataFrame substrate), [collectives.md](../cluster/collectives.md)
(the repartition transport), [job-coordinator.md](../compute/job-coordinator.md) (the entry
surface), and [compute-device.md](../compute/compute-device.md) (the device memory model).
