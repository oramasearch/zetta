# Fetch-affinity placement — the node that pulled it owns it

`zetta::data::FetchAffinityPlacement` is a [`Partitioner`](partitioner.md) whose
partition→node half answers **"the node that fetched this slice"** instead of
hashing. It is the write-side mirror of
[compute-near-data](../compute/compute-near-data.md): rows land where they were
pulled and are never moved afterwards.

```
include/zetta/data/fetch_affinity_placement.hpp  FetchAffinityPlacement
src/core/data/fetch_affinity_placement.cpp       owner-first placement + `_seq` round-robin
tests/data/test_fetch_affinity_placement.cpp     the properties below
```

## Why not rendezvous

[`RendezvousPlacement`](partitioner.md) is the right default for a dataset loaded
from a local file: one node holds all the rows, so where each partition lands is
a free choice and HRW spreads it evenly with minimal movement on a membership
change.

It is the *wrong* choice for a
[distributed ingest](distributed-connector-ingest.md). There, several nodes each
pull a disjoint tile of an external result set. A node that fetched source chunk
3 writes a partition whose hash-chosen owner is, with probability
`(N-1)/N`, some other node — so the bytes it has just written to its own disk
have to move again. The data crosses the wire twice and the ingest is bounded by
that shuffle rather than by the cluster's aggregate uplink.
`tests/data/test_fetch_affinity_placement.cpp` measures exactly that: over a
4-node roster, HRW would relocate most of the partitions this partitioner keeps
in place.

## Still deterministic — no plan travels

Ownership is not an arbitrary runtime choice. **Which node fetches which source
chunk** is fixed by `connect::plan_fetch_assignments`, a pure function of
(roster, per-chunk row counts); every participant derives the identical tiling
from identical inputs. Each node then constructs this partitioner with **its
own** node id, and the resulting catalog entries tile the dataset with no
overlap and no gaps. "The fetcher owns it" is as agreed-by-construction as a
hash — it just agrees on a better answer.

It is also **better balanced for what comes next**: the tiling is
longest-processing-time-first *by row count*, so slices are near-equal in rows.
A hash equalizes partition *counts*, which says nothing about rows when source
chunks are uneven.

| | `RendezvousPlacement` | `FetchAffinityPlacement` |
|---|---|---|
| primary owner | `argmax hash(partition, member)` | the node that fetched the slice |
| bytes move after the write? | yes, whenever the hash disagrees | never |
| balanced by | partition count | row count (via the tiling) |
| membership change | ~1/N of partitions move | [rebalance](partition-rebalance.md) applies, same as any table |

## Replicas

A connector result set is a cache and runs at RF=1, where the only owner is the
fetching node. A higher factor — what [`promote_dataset`](promote-dataset.md)
raises a kept result set to — keeps the fetching node **primary** (it physically
holds the body) and picks the extra owners by `rendezvous_owners` over the
remaining members, so replica placement stays leaderless and spread.

## record → partition

The record→partition half is round-robin over the `_seq` surrogate the
[partitioned write path](partitioned-write-path.md) stamps (the row's ordinal),
so a node's fetched slice spreads evenly over its P local partitions regardless
of key skew. A record with no usable surrogate lands in partition 0 — in range
and deterministic, never a throw. Pure and non-throwing like the rest of the
interface.

## Where it is used

`AddDatasetOptions::fetch_affinity_owner` selects it: non-empty means "place
every partition on this node id". The
[distributed connector ingest](distributed-connector-ingest.md) sets it to the
fetching node's own id. Everything else keeps the HRW default.

Issue: #833 section 6.
