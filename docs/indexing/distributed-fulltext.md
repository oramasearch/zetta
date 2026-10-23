# Per-partition distributed full-text (`zetta::index::DistributedFulltextIndex`)

The **approved per-partition topology** for full-text search (ENT-39 / ENT-40,
ADR-34, [#529](https://github.com/oramasearch/zetta/issues/529) — the engine half of
**TC-13** [#503](https://github.com/oramasearch/zetta/issues/503)). Where the
[index step executor](index-step-executor.md) (#528) builds **one consolidated**
index over a whole table, this builds a **full-text sub-index per LOCAL partition**
(compute-near-data) and answers a query by **scatter-gather + merge/rank** across
those sub-indexes — never one big index.

This is the standard sharded-search shape — per-shard scoring, then a coordinator
merges the ranked hits by score — preceded by a **global-statistics pass** (the
`dfs_query_then_fetch` analogue, [#851](https://github.com/oramasearch/zetta/issues/851))
whenever the backend scores with corpus statistics: every sub-index scores with the
merged statistics of all of them. Its contract: **a search over the sharded table
returns exactly what the same search over one consolidated index returns** —
membership, order and scores — at any `top_k`, however the rows are distributed, and
every coordinator returns the same list (see
[why the statistics pass is needed](#why-every-shard-scores-with-the-merged-statistics)).

## Shape

```
build_local(node, spec)   each node enumerates the partitions it OWNS, reads each
                          body from local disk, and builds ONE full-text sub-index
                          per partition into an owned IndexBackend (no LOREQ, no
                          full-dataset pull — compute-near-data).

query(spec, query)        (0) the coordinator sums its own sub-indexes' corpus
                          statistics for the query and gathers every peer's over
                          the `fulltext.stats` RPC, then merges them;
   ┌── node A ─ sub-idx p0,p1 ──┐   (1) it queries its OWN local sub-indexes and
   ├── node B ─ sub-idx p2 ─────┤── scatters the query, merged statistics attached,
   └── node C ─ sub-idx p3,p4 ──┘   to every peer over `fulltext.query`, gathers each
                                    node's ranked hits, and MERGES everything into
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

0. **statistics** — when the backend scores with corpus statistics
   (`IndexBackend::uses_corpus_statistics()`, true for BM25F): sum this node's
   sub-indexes' statistics for the query (`collect_local_statistics`), ask every
   peer for its own over the `fulltext.stats` RPC, and merge them all
   (`CorpusStatistics::merge`). Recorded as one `search:statistics` span.
1. **local** — query this node's own per-partition sub-indexes, each scoring with
   the merged statistics, and merge them.
2. **scatter** — send the query, merged statistics attached, to every peer over the
   `fulltext.query` RPC (`Cluster::request`, from the application thread — never the
   network thread). Each peer answers from its own local sub-indexes via the same
   handler, scoring with the statistics it was sent.
3. **gather + merge** — collect every node's ranked hits and merge into one global
   ranking, capped to `query.top_k`.

Both rounds go to the same peers (one snapshot of `Cluster::peers()`), so a peer
that joins between them cannot answer with rows the statistics never counted.

An unreachable / failing peer, in either round, **fails the query** (a missing
partition would silently skew the ranking), the way the [collectives](../cluster/collectives.md) fail a round on an
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
The BM25F backend caps each sub-index in the same order (it widens its engine fetch
until a score tie at the cutoff is wholly inside it), so a tie at the boundary is
cut by row id everywhere, never by insertion order.

### Scores in the interface

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
  plus the query (`field` / `term` / `top_k`), and an optional `return_fields`
  list: the row fields each hit should carry back ([#1243](https://github.com/oramasearch/zetta/issues/1243)).
  `encode_fulltext_query` / `decode_fulltext_query`.
- **request `statistics`** — the merged corpus statistics every shard scores with
  (#851): `{ doc_count, term_doc_freq: {term: n}, fields: {field: {total_len,
  doc_count}} }`, keyed by the analyzed term. Present on the `fulltext.query`
  scatter after the statistics round; absent on the `fulltext.stats` request (which
  is the same `FulltextQueryRequest`) and for a backend that uses no statistics.
- **statistics response** — `schemas/fulltext_stats.schema.json`: one node's corpus
  statistics for the query, summed across its sub-indexes, plus `from` and
  `sub_indexes`. `encode_fulltext_stats` / `decode_fulltext_stats`.
- **response** — `schemas/fulltext_hits.schema.json`: one node's ranked
  `{ id, score, fields? }` hits (`fields` = the requested values of a row this
  node owns, read from what `build_local` kept for the index's fields), plus an
  optional `sub_indexes` count (how many
  per-partition sub-indexes the node consulted — the provenance surfaces show,
  [#643](https://github.com/oramasearch/zetta/issues/643); a payload without
  it decodes to 0). `encode_fulltext_hits` / `decode_fulltext_hits`.

The embedded schemas are the source of truth; `schemas/fulltext_*.schema.json` are
generated from them and drift-checked ([schema-as-code](../architecture/schemas.md)).
A node without the statistics round rejects `fulltext.stats` and a request carrying
`statistics`, so a cluster running mixed versions fails such a query loudly rather
than ranking it wrongly.

## Why every shard scores with the merged statistics

BM25F scoring uses corpus statistics (per-term document frequency, the document
count, average field lengths). A per-partition sub-index scoring with its **local**
statistics ranks against a different corpus than the table's
([#611](https://github.com/oramasearch/zetta/issues/611) measured it): the `k1`
saturation `tf~/(k1 + tf~)` compresses any tf separation into a bounded score ratio
(about 2.2x at `k1 = 1.2`), while local IDF varies with shard composition by more
than that — a shard where the term matches one of two rows scores it `ln 2 ≈ 0.69`
against `ln(1 + 0.5/2.5) ≈ 0.18` for a shard where both rows match, a 3.8x swing.

With `top_k = 0` that only reorders hits. With a `top_k` — any paged or limited
search — it **loses** them: each shard truncates to `top_k` on its local scores, so a
row that belongs in the global top k can fall below its own shard's cutoff and never
reach the merge, with nothing to show it is missing
([#851](https://github.com/oramasearch/zetta/issues/851)).

The statistics round removes the cause. Every sub-index reports, for the query's
analyzed terms, its document count, each term's document frequency (0 for a term it
has never seen, so the merge covers every term) and every field's
`{total_len, doc_count}`; the coordinator sums them and every sub-index scores with
the sums. Those are the integers the consolidated index computes IDF and `avglen`
from, so each row's score is **bit-identical** to its consolidated score, the per-shard
order is the consolidated order restricted to that shard, and a row in the global
top k is in its own shard's top k. The cutoff becomes safe by construction.

The cost is one extra round trip per peer per query, carrying a few counts per
query term and per field — no rows and no hits. A backend whose scores do not depend
on the corpus (the exact-match `InMemoryIndexBackend`, the tests' term-frequency
reference scorer) reports `uses_corpus_statistics() == false` and skips the round.

The engine side is `oramacore::index::MemoryIndex::stats(query)` and
`search(query, top_k, global)` in the
[oramacore-cpp](https://github.com/oramasearch/oramacore-cpp) submodule; the
`IndexBackend` side is `uses_corpus_statistics()`, `collect_statistics()` and
`IndexQuery::statistics` ([index-backend.md](index-backend.md)).

## Tests

[`tests/index/test_distributed_fulltext.cpp`](../../tests/index/test_distributed_fulltext.cpp):

- **the pure merge** — global ranking (score desc, id asc), dedup keeping the max
  score, `top_k` cap.
- **the wire boundary** — request/response round-trip + schema rejection of malformed
  messages (missing `term`, unknown `kind`, non-scalar term, a hit missing `score`).
- **single node, many local partitions** — a real `Node` with a
  [partitioned](../data/partitioned-write-path.md) table; the per-partition merge equals the
  consolidated oracle.
- **a limited search over a skewed corpus equals one index (#851)** — with the real
  BM25F engine, a corpus whose matches are concentrated (one weak match among misses,
  strong matches together), spread over 2, 3, 4 and 7 partitions on one node and over
  two nodes: at top 1, 2, 3, 4, 5, 10, 50 and all, every coordinator returns exactly
  the consolidated index's first k rows with identical scores, and the top 3 is the
  prefix of the top 50. Without the statistics round the top 2 drops a strong match.
- **the statistics wire boundary** — `fulltext.stats` responses and a request
  carrying `statistics` round-trip; malformed statistics are rejected.
- **the BM25F backend's cap** — collected shard statistics merge to the consolidated
  index's, a shard scored with them returns the whole table's top k; a cap cuts a
  score tie by row id, and keeps the best hits whatever order the engine meets them in.
- **multi-node scatter-gather** — a real 3-node loopback cluster, each node holding a
  disjoint shard; every node's distributed query produces the **identical** global
  ranking, equal to the consolidated oracle over all rows.

The merge + scatter-gather are proven **engine-agnostically** with a deterministic,
**partition-independent** reference scorer (term frequency — a doc's score depends only
on the doc + query), so the suite runs without the private `oramacore-cpp` submodule;
the real **BM25F** path is exercised when the engine is compiled in (skipped otherwise,
like the other [oramacore tests](oramacore-fulltext-index.md)), on the skewed corpus
above and on the original statistics-engineered corpus — its distributed ranking
*and scores* equal the consolidated oracle's exactly.

The same acceptance is a **GUI-runnable [test-suite](../testing/test-suite.md) case**,
`cluster.fulltext_index` (the **TC-13** acceptance, the way TC-9/TC-10/TC-11 each surface
as a runnable case): it builds the per-partition full-text index over a real loopback
cluster and asserts against the consolidated oracle on every node. Its knobs (`nodes` /
`copies` / `partitions` / `top_k`) compose shards freely, and for either backend — the
real `oramacore_fulltext` (BM25F) or the partition-independent reference scorer used
when the submodule is absent — every coordinator's ranking must **exactly equal** the
oracle's at the chosen `top_k` (before #851 the BM25F path could assert only the hit
set). Runnable headless (`tests/testkit/test_testkit.cpp`)
and in the **Operations → Test suite** panel.

## What this slice is (and isn't)

It is the **per-partition distributed full-text topology**: build a sub-index per
owned partition (compute-near-data), answer a query by scatter-gather + merge/rank,
matching the consolidated oracle. It is a usable building block alongside (not a
replacement for) the consolidated [index step executor](index-step-executor.md).
**As of [#641](https://github.com/oramasearch/zetta/issues/641) it IS the default
query path**: the Node owns one (wired in `attach_cluster`, chained over the compute
plane's signal handler via `Cluster::signal_handler()`), keeps its sub-indexes
current from `tick()` (`Node::reconcile_search_indexes()`), and routes every
[`Node::search`](node-search.md) over a placed-partition table through it —
`QueryResult` now also carries per-node `contributions` (node id / hits /
sub-indexes consulted / latency / wire bytes, both rounds included), and `query()`
optionally records `search:statistics` / `search:local` / `search:peer` child spans
under a caller trace. Wiring it into the deploy/run path as the default
`index`-step behaviour, a true point-to-point gather (vs. one RPC per peer), and the
other index kinds (vector / semantic / feature) remain additive behind the same
interface. Global-statistics scoring landed with
[#851](https://github.com/oramasearch/zetta/issues/851) (the design was
[#657](https://github.com/oramasearch/zetta/issues/657)).

## Pointers

- [`include/zetta/index/distributed_fulltext.hpp`](../../include/zetta/index/distributed_fulltext.hpp) /
  [`src/core/index/distributed_fulltext.cpp`](../../src/core/index/distributed_fulltext.cpp) —
  the component, the pure merge, and the schema-validated wire messages.
- [index-backend.md](index-backend.md) — the `IndexBackend` interface (scores, corpus
  statistics) + `IndexRegistry` + the `Index` entity.
- [oramacore-fulltext-index.md](oramacore-fulltext-index.md) — the BM25F backend the
  sub-indexes are built into by default.
- [index-step-executor.md](index-step-executor.md) — the consolidated (one-index)
  run-path slice this complements.
- [compute-near-data.md](../compute/compute-near-data.md) — the owned-partition shard source the
  per-partition build mirrors.
- [collectives.md](../cluster/collectives.md) / [signal-plane.md](../cluster/signal-plane.md) — the
  schema-validated message pattern + the RPC transport the scatter-gather rides.
