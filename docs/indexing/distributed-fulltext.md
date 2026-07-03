# Per-partition distributed full-text (`zetta::index::DistributedFulltextIndex`)

The **approved per-partition topology** for full-text search (ENT-39 / ENT-40,
ADR-34, [#529](https://github.com/oramasearch/zetta/issues/529) — the engine half of
**TC-13** [#503](https://github.com/oramasearch/zetta/issues/503)). Where the
[index step executor](index-step-executor.md) (#528) builds **one consolidated**
index over a whole table, this builds a **full-text sub-index per LOCAL partition**
(compute-near-data) and answers a query by **scatter-gather + merge/rank** across
those sub-indexes — never one big index.

This is the standard sharded-search shape — per-shard local scoring, then a
coordinator merges the ranked hits by score — exactly how production search
engines default to local-IDF *query-then-fetch*. Its contract: the **hit set is
exact** and every coordinator agrees on **one global ranking**; the rank *order*
equals a single consolidated index's only when every shard's corpus statistics
agree with the consolidated ones (see
[the exactness analysis below](#when-per-partition-matches-a-single-index)).

## Shape

```
build_local(node, spec)   each node enumerates the partitions it OWNS, reads each
                          body from local disk, and builds ONE full-text sub-index
                          per partition into an owned IndexBackend (no LOREQ, no
                          full-dataset pull — compute-near-data).

query(spec, query)        the coordinator queries its OWN local sub-indexes, then
   ┌── node A ─ sub-idx p0,p1 ──┐   scatters the query to every peer over the
   ├── node B ─ sub-idx p2 ─────┤── `fulltext.query` signal-plane RPC, gathers each
   └── node C ─ sub-idx p3,p4 ──┘   node's ranked hits, and MERGES everything into
                                    one global ranked list (identical on every node).
```

Each node holds a `DistributedFulltextIndex`
([`include/zetta/index/distributed_fulltext.hpp`](../../include/zetta/index/distributed_fulltext.hpp)).
It owns one `IndexBackend` — the BM25F [`OramaCoreFulltextIndex`](oramacore-fulltext-index.md)
by default — into which it builds its per-partition sub-indexes (each keyed by a
per-partition derived `IndexSpec`, so one backend instance holds many). It is the
cluster's `SignalHandler` while it lives, **chained over an optional delegate** (the
[`DistributedSorter`](../ddf/ddf-distributed-sort.md) pattern): `fulltext.query` is served
here, everything else routes to the delegate.

### build — per-partition, compute-near-data

`build_local(node, spec)` enumerates this node's owned partitions of the table — every
[`DatasetPartition`](../data/dataset-partitions.md) whose `table` matches and whose
`owner_nodes` contains this node, the same source as
[`load_local_partitions`](../compute/compute-near-data.md) — reads each body from the local
large-object store (the [sharded-blob-placement](../data/sharded-blob-placement.md) guarantee:
no `LOREQ`, no full-dataset pull), parses its JsonLines rows (the existing
`{"_zid", doc}` shape the [partitioned write path](../data/partitioned-write-path.md) emits,
**fail-fast** on a malformed line so a silent drop can't diverge from the oracle), and
builds **one sub-index per partition** through the backend. A rebuild drops the prior
sub-indexes first. A node owning no partitions of the table builds nothing (ok, 0
partitions). It reports near-data instrumentation: `partitions_indexed`, `rows_indexed`,
`local_bytes_read` (read near the node, **not** moved over the wire).

### query — scatter-gather + merge/rank

`query(spec, query)`:

1. **local** — query this node's own per-partition sub-indexes and merge them.
2. **scatter** — send the query to every connected peer over the `fulltext.query`
   RPC (`Cluster::request`, from the application thread — never the network thread).
   Each peer answers from its own local sub-indexes via the same handler.
3. **gather + merge** — collect every node's ranked hits and merge into one global
   ranking, capped to `query.top_k`.

An unreachable / failing peer **fails the query** (a missing partition would silently
skew the ranking), the way the [collectives](../cluster/collectives.md) fail a round on an
unreachable peer. A single-node cluster (no peers) returns its local ranking. The
result is the **same global ranking on every node**, so single-node == multi-node.

The merge is the pure core both levels run through:

```cpp
std::vector<FulltextHit> merge_fulltext_hits(
    std::span<const std::vector<FulltextHit>> sources, std::size_t top_k);
```

It flattens every source, **dedups by row id** (collapsing a duplicate to its highest
score so a row is never listed twice or double-counted), orders **score descending,
row id ascending** on a tie (deterministic + reproducible), and caps to `top_k`.

### Scores in the seam

A global merge needs scores, so [`IndexQueryResult`](../../include/zetta/index/index_backend.hpp)
gained a `scores` vector, **aligned to `row_ids`**, higher = better. A *ranked* backend
(`OramaCoreFulltextIndex`'s BM25F) fills it; an *exact-match* backend
(`InMemoryIndexBackend`) has no ranking signal and leaves it **empty** (the invariant:
`scores` is empty, or exactly `row_ids.size()` long). The distributed index treats a
missing score as 0.

## The scatter-gather wire boundary

The query rides the existing transport — **no new wire verb** (it is a
[signal-plane](../cluster/signal-plane.md) `request`). The two payloads are
**schema-validated at the boundary** (the [collective-message](../cluster/collectives.md)
pattern — embedded draft-07 schemas in `distributed_fulltext.cpp`, the `pboettch`
validator), so a malformed message fails fast with a path-qualified reason and never
reaches the index:

- **request** — `schemas/fulltext_query.schema.json`: the `IndexSpec` the peer looks
  its sub-indexes up by (`table` / `kind` / `fields`, with `backend` for provenance)
  plus the query (`field` / `term` / `top_k`). `encode_fulltext_query` /
  `decode_fulltext_query`.
- **response** — `schemas/fulltext_hits.schema.json`: one node's ranked
  `{ id, score }` hits. `encode_fulltext_hits` / `decode_fulltext_hits`.

These are hand-mirrored embedded schemas (like `collective_message.schema.json`), not
the generated/drift-checked [schema-as-code](../architecture/schemas.md) set.

## When per-partition matches a single index

BM25F scoring uses corpus statistics (per-term document frequency, average field
length), so a per-partition sub-index scores with **local** statistics — the standard
sharded-search caveat. What that guarantees, precisely
([#611](https://github.com/oramasearch/zetta/issues/611) settled this):

- **The hit set is exact.** Whether a row matches is a per-row property, so a full
  query returns exactly the rows the consolidated index would.
- **Every coordinator agrees.** Each node's scatter-gather runs the same
  deterministic merge over the same per-shard answers — one identical global
  ranking on every node.
- **The rank order is exact only when every shard's statistics agree with the
  consolidated ones.** "Clearly separated term frequencies" are NOT sufficient:
  the `k1` saturation `tf~/(k1 + tf~)` compresses *any* tf separation into a
  bounded score ratio (about 2.2× at `k1 = 1.2`), while local IDF varies with
  shard composition by more than that — a shard where the term matches one of two
  rows scores it `ln 2 ≈ 0.69` against `ln(1 + 0.5/2.5) ≈ 0.18` for a shard where
  both rows match, a 3.8× swing. A tf-1 row sharing a shard with a non-matching
  row can outrank a tf-5 row whose shard-mates all match — the exact failure #611
  observed.

So an *acceptance fixture* for exact rank equality must pin the statistics, not
the term frequencies: give every shard the **same df/N ratio** as the consolidated
corpus (then IDF cancels — `ln(1 + (N−n+0.5)/(n+0.5))` is equal whenever `n/N`
is) and **uniform per-field token lengths** (then the length normalization
cancels), leaving each row's score a function of its own tf only. The gtest
corpus is engineered exactly so (one matching + one non-matching row per
partition, uniform field lengths — see `postgres_corpus` in
[`tests/index/test_distributed_fulltext.cpp`](../../tests/index/test_distributed_fulltext.cpp)),
and the test asserts the fixture invariant explicitly so a placement change fails
with the reason rather than as a ranking flip.

Exact rank equality for *arbitrary* corpora and shardings needs the
**global-statistics pass** (the `dfs_query_then_fetch` analogue: gather per-term
df + field-length totals across shards first, score every shard with the merged
globals) — a later slice behind this same API, tracked in
[#657](https://github.com/oramasearch/zetta/issues/657).

## Tests

[`tests/index/test_distributed_fulltext.cpp`](../../tests/index/test_distributed_fulltext.cpp):

- **the pure merge** — global ranking (score desc, id asc), dedup keeping the max
  score, `top_k` cap.
- **the wire boundary** — request/response round-trip + schema rejection of malformed
  messages (missing `term`, unknown `kind`, non-scalar term, a hit missing `score`).
- **single node, many local partitions** — a real `Node` with a
  [partitioned](../data/partitioned-write-path.md) table; the per-partition merge equals the
  consolidated oracle.
- **multi-node scatter-gather** — a real 3-node loopback cluster, each node holding a
  disjoint shard; every node's distributed query produces the **identical** global
  ranking, equal to the consolidated oracle over all rows.

The merge + scatter-gather are proven **engine-agnostically** with a deterministic,
**partition-independent** reference scorer (term frequency — a doc's score depends only
on the doc + query), so the suite runs without the private `oramacore-cpp` submodule;
the real **BM25F** path is exercised when the engine is compiled in (skipped otherwise,
like the other [oramacore tests](oramacore-fulltext-index.md)) on the
statistics-engineered corpus described above — its distributed ranking *and scores*
equal the consolidated oracle's exactly, and the test asserts the
one-match-one-miss-per-partition fixture invariant explicitly.

The same acceptance is a **GUI-runnable [test-suite](../testing/test-suite.md) case**,
`cluster.fulltext_index` (the **TC-13** acceptance, the way TC-9/TC-10/TC-11 each surface
as a runnable case): it builds the per-partition full-text index over a real loopback
cluster and asserts against the consolidated oracle on every node. Its knobs (`nodes` /
`copies` / `partitions` / `top_k`) compose shards freely, so the assertion follows the
backend's contract: with the real `oramacore_fulltext` (BM25F) backend it asserts the
**exact hit set** plus **one identical global ranking on every coordinator** (local-IDF
query-then-fetch); with the partition-independent reference scorer (used when the
submodule is absent — the same plumbing, always green) the ranking is additionally
**exactly equal** to the oracle. Runnable headless (`tests/testkit/test_testkit.cpp`)
and in the **Operations → Test suite** panel.

## What this slice is (and isn't)

It is the **per-partition distributed full-text topology**: build a sub-index per
owned partition (compute-near-data), answer a query by scatter-gather + merge/rank,
matching the consolidated oracle. It is a usable building block alongside (not a
replacement for) the consolidated [index step executor](index-step-executor.md) —
wiring it into the deploy/run path as the default `index`-step behaviour, exact
global-statistics scoring ([#657](https://github.com/oramasearch/zetta/issues/657)), a
true point-to-point gather (vs. one RPC per peer), and the other index kinds (vector /
semantic / feature) remain additive behind the same seam.

## Pointers

- [`include/zetta/index/distributed_fulltext.hpp`](../../include/zetta/index/distributed_fulltext.hpp) /
  [`src/core/index/distributed_fulltext.cpp`](../../src/core/index/distributed_fulltext.cpp) —
  the component, the pure merge, and the schema-validated wire messages.
- [index-backend.md](index-backend.md) — the `IndexBackend` seam (now carrying scores)
  + `IndexRegistry` + the `Index` entity.
- [oramacore-fulltext-index.md](oramacore-fulltext-index.md) — the BM25F backend the
  sub-indexes are built into by default.
- [index-step-executor.md](index-step-executor.md) — the consolidated (one-index)
  run-path slice this complements.
- [compute-near-data.md](../compute/compute-near-data.md) — the owned-partition shard source the
  per-partition build mirrors.
- [collectives.md](../cluster/collectives.md) / [signal-plane.md](../cluster/signal-plane.md) — the
  schema-validated message pattern + the RPC transport the scatter-gather rides.
