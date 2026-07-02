# DDF distributed sort (`zetta::ddf::DistributedSorter`)

The **distributed sort** over the [DDF DataFrame](ddf.md) substrate (**ARC-12**,
[#385](https://github.com/oramasearch/zetta/issues/385)) — **sample-partition +
local sort + merge** across the cluster. Every participating node owns a shard (a
`DataFrame` of the same schema) and runs the *same* call (SPMD); the result is a set
of globally range-ordered, locally-sorted **partitions**, one per node, so
concatenating them in participant order ([`ddf::concat`](ddf-ops.md)) is the full
global sort.

```
include/zetta/ddf/distributed_sort.hpp   PartitionMessage + DistributedSorter
src/core/ddf/distributed_sort.cpp        the sample-sort + the all-to-all shuffle + the JSON boundary
schemas/ddf_partition.schema.json        the shuffle-message JSON boundary (draft-07)
tests/test_distributed_sort.cpp          single- + multi-node + boundary tests
```

## The algorithm

A `DistributedSorter` is constructed over a started (or startable)
[`Cluster`](../cluster/cluster.md) and the [`ComputeDevice`](../compute/compute-device.md) its shards live
on. Each node calls `sort(local_shard, key_column, order, round_tag)` against its own
shard; the call runs on the application thread and returns **this node's** output
partition. The classic sample-sort, expressed over the cluster:

1. **Local sort** — each node sorts its shard by the key column on its device
   ([`ddf::sort`](ddf-ops.md)).
2. **Sample → splitters** — each node samples its sorted keys; a
   [`gather`](../cluster/collectives.md) brings every sample to the root (lowest node id),
   which chooses `P-1` splitters (quantile boundaries over the combined sample) and
   [`broadcast`](../cluster/collectives.md)s them — so every node range-partitions against the
   *same* boundaries.
3. **Partition** — each node splits its locally-sorted shard into `P` buckets by the
   splitters (contiguous ranges, since the shard is already sorted).
4. **Shuffle (all-to-all)** — bucket `b` is shipped to participant `b` over the
   signal plane (the `ddf.shuffle` RPC); a [`barrier`](../cluster/collectives.md) fences
   delivery; then each node **merges** its own bucket with everything it received
   and **local-sorts** the union into its output partition.

Partition `r` (held by participant `r`) holds the r-th key range and is locally
sorted, so participant 0's keys all precede participant 1's, and so on — the
concatenation is globally sorted. A **single-node** cluster (no peers) skips steps
2–4 and returns the locally-sorted shard.

### Why an all-to-all over RPC doesn't deadlock

The shuffle ships each bucket with a blocking RPC ([`Cluster::request`](../cluster/cluster.md))
from the application thread, but the receiver handles `ddf.shuffle` on the **network
thread** — it only decodes + buffers the partition into a per-round inbox and
returns immediately (cheap, honoring the signal plane's "handlers must not block"
contract). So every node's outbound sends complete independently of what any other
node's application thread is doing; the post-shuffle barrier then guarantees every
inbound bucket has been delivered before a node drains its inbox and merges. Late
delivery for a finished round can't happen — all sends complete before the barrier
releases.

## Wiring

`DistributedSorter` is the cluster's signal handler while it lives, **chained over a
`Collectives` it owns**: `ddf.shuffle` is buffered here, `collective.*` routes to the
collectives, and everything else to an optional app `delegate`. The chain is set up
in the constructor (the sorter registers itself as the head, over a `Collectives`
constructed with `delegate` as *its* fallback) and unwound in the destructor (restore
the collectives, whose own destructor then restores `delegate`) — the same
register-the-chain / unwind-it-cleanly discipline as the
[job coordinator](../compute/job-coordinator.md). It rides entirely on the existing transport —
**no new wire verb**.

This is a focused **cluster-level DDF primitive**: it uses the
[collectives](../cluster/collectives.md) (splitter agreement) and the [signal plane](../cluster/signal-plane.md)
(the row shuffle) directly. Scheduling a [`ComputeJob`](../compute/compute-jobs.md) of `ddf`
kind that *calls* it through the [job coordinator](../compute/job-coordinator.md) is a separate,
later slice.

## The shuffle-message boundary

A shuffled bucket crosses the wire as a `PartitionMessage`, carried as a signal's
opaque payload and **schema-validated at the boundary**
(`schemas/ddf_partition.schema.json`, mirrored embedded in `distributed_sort.cpp`,
the same `pboettch` validator as the [collective message](../cluster/collectives.md)). The JSON
form is column-major:

```json
{ "tag": "round-1.shuffle", "from": "node-a", "rows": 2,
  "columns": [ { "name": "key", "dtype": "float64", "values": [1.5, -2.0] },
               { "name": "id",  "dtype": "int64",   "values": [7, 8] } ] }
```

`decode_partition_message` is non-throwing (nullopt + a path-qualified reason on any
failure): it enforces the schema, then the cross-field rule the schema can't express
— **every column carries exactly `rows` values**. `encode_partition_message` /
`decode_partition_message` are the serialization seam, exposed (like
`encode_collective_message`) so the boundary is directly testable.

> **Numeric range note.** Like the collective plane, the shuffle ships values as
> doubles — **exact for every dtype whose range fits a double's 53-bit mantissa**, so
> 64-bit integer columns beyond 2^53 are the documented limit (well past churn id /
> count ranges). Sort *keys* are compared via the monotonic double projection only to
> choose partition boundaries; the per-partition order is the real device sort over
> the typed values.

## Assumptions

- **Fully-connected, stable membership** for the round (the collectives assume it; a
  partitioned cluster times the round out). The participant set is this node + its
  connected peers, sorted by node id, identical on every node.
- **SPMD** — every participant calls `sort()` with the same `key_column`, `order`,
  and `round_tag` against its own same-schema shard. `round_tag` scopes the round's
  collective + shuffle messages; use a fresh tag per concurrent sort.

## Tests

`tests/test_distributed_sort.cpp` runs over **real in-process loopback clusters**
(the shape of [test_collectives.cpp](../cluster/collectives.md) /
[test_distributed_scheduler.cpp](../compute/work-stealing-scheduler.md)): the **single-node**
fast path; **two- and three-node** sorts whose scrambled shards force the shuffle to
move rows, asserting each partition is locally sorted, range-disjoint from the next,
and globally sorted on concatenation (ascending **and** descending); an **empty shard
on one node**; the `concat` merge of ordered partitions; and the `PartitionMessage`
**JSON boundary** (round-trip + rejection of malformed JSON, a missing required
field, an unknown dtype, an unknown field, and a value count that disagrees with
`rows`).

See also [ddf-ops.md](ddf-ops.md) (the local building blocks), [ddf.md](ddf.md) (the
DataFrame core), [collectives.md](../cluster/collectives.md), [signal-plane.md](../cluster/signal-plane.md),
and [job-coordinator.md](../compute/job-coordinator.md).
