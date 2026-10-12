# The canonical search entry point (`Node::search`)

**THE one query path every surface calls** ([#641](https://github.com/oramasearch/zetta/issues/641)):
the GUI Search view, the `search` MCP/REST capability, and tests all go through
`Node::search` — there is no second path, so surfaces can never diverge. It makes
the [per-partition distributed full-text](distributed-fulltext.md) building block
(#529) the **default** behaviour of search, per the distributed-first foundation:
a search exploits the whole cluster unless the table genuinely has nothing placed.

```cpp
zetta::index::SearchRequest request;
request.target = "articles";      // an Index id, or a table name
request.query  = "postgres";
request.top_k  = 10;
zetta::index::SearchResult result = node.search(request, recorder, &trace);
```

`Node::search` delegates to `zetta::index::search(node, request, …)`
([`include/zetta/index/search.hpp`](../../include/zetta/index/search.hpp) /
[`src/core/index/search.cpp`](../../src/core/index/search.cpp)). Non-throwing
(`IndexStatus` + a path-qualified reason), main-thread only.

## Resolution → routing → provenance

1. **Resolve.** `target` names a registered `Index` entity: an exact index id
   wins; otherwise the **most recently created Search-kind index** over the
   table named `target`. No index → `NotFound` with the create_index hint —
   which now names a real capability: `create_index` builds a Search index over
   a loaded table over MCP/REST (`orchestration:create_index`, [docs/api/README.md](../api/README.md),
   [#825](https://github.com/oramasearch/zetta/issues/825)), so `add_dataset →
   create_index → search` is fully API-driven. Non-Search kinds are
   `Unsupported` (vector/semantic route here later, behind the same request).
2. **Route.**
   - **Distributed (the default):** the table has ≥ 1 *placed* partition (a
     `DatasetPartition` with a body) AND a cluster is attached → the query goes
     through the node's `DistributedFulltextIndex` — local sub-indexes +
     scatter to every peer over `fulltext.query` + merge/rank
     ([distributed-fulltext.md](distributed-fulltext.md)). Identical global
     ranking on every node.
   - **Consolidated (degraded):** no placed partitions (or no cluster) → the
     `IndexRegistry`-resolved backend answers from the consolidated index the
     [index step executor](index-step-executor.md) built. One-node provenance.
3. **Provenance.** The result carries `nodes`: one `NodeContribution` per
   answering node — node id, `local` flag, pre-merge hit count, how many
   per-partition **sub-indexes** the node consulted
   ([#643](https://github.com/oramasearch/zetta/issues/643); 1 on the
   consolidated path, 0 for a node owning no shard), coordinator-observed
   latency, wire bytes moved (0 for the local contribution) — the
   scatter-gather made visible, plus `wire_bytes_moved` and which `path`
   answered.

## Each hit carries its row's fields (#1243)

A hit's `row_id` is the engine's internal row id, which no outside caller can
look up. So every hit also carries `fields`: the matched row's values for the
index's own fields by default, or for the ones `SearchRequest::fields` names
(each must be one of the index's fields, else `InvalidArgument` naming the
ones it has; an empty list returns ids and scores only).

- **Distributed path:** the node that owns a row's partition attaches its
  values to its own hits before it answers (`return_fields` on the
  `fulltext.query` request, `fields` on each returned hit). A row never crosses
  the wire twice, and the coordinator never fetches a row from another node.
  The values come from what the node indexed: `build_local` keeps each row's
  values for the index's fields beside its sub-indexes, because the network
  thread that answers a peer cannot read the node's partition bodies.
- **Consolidated path:** the values are read from the table the consolidated
  index was built over, on this node.

## One trace per search

With a `SpanRecorder` (+ optional parent `TraceContext`), the whole search is
**one trace** ([observability.md](../architecture/observability.md)): a root
`search` span carrying the four captures (`intent` = the request verbatim,
`plan` = path/index/participants, `result_shape` = hits/nodes/bytes) with a
`search:local` child and one `search:peer` child per scattered peer (node id,
hits, wire bytes, honest wall-time each). The MCP capability threads its call
trace through, so an agent reads the whole scatter back via `get_trace`; the
GUI records into the same ring (`App::set_span_recorder`, wired from the API
server in main).

## The node-owned search plane (wiring)

Every clustered node can answer a peer's scatter because the `Node` owns the
machinery:

- **`attach_cluster` → `start_coordinator()`** constructs a
  `DistributedFulltextIndex` chained over the compute plane's signal handler
  (`Cluster::signal_handler()` is the new getter that makes the delegate
  chain composable): `fulltext.query` is served there; steals / collectives /
  telemetry route on unchanged. `stop_coordinator()` unwinds in reverse order.
- **`Node::reconcile_search_indexes()`** (tick step 2b, and called by
  `search()` before it queries) keeps this node's per-partition sub-indexes
  current: for every Search-kind `Index` entity it (re)builds
  `build_local` over the placed partitions this node owns whenever that input
  set changes — fingerprint-guarded, so the steady-state tick cost is a string
  compare. Since `Index` entities and `DatasetPartition`s are replicated, a
  peer that has never searched still builds and serves its shard.
- **Sub-index backend:** `OramaCoreFulltextIndex` (BM25F) when the private
  submodule is compiled in; otherwise the exact-match `InMemoryIndexBackend`
  (no ranking signal — a degraded reference). `Node::set_search_backend`
  (before `attach_cluster`) overrides it — tests inject the partition-independent
  term-frequency scorer ([`tests/index/term_frequency_backend.hpp`](../../tests/index/term_frequency_backend.hpp)).

## Surfaces (no second path)

- **MCP/REST:** the `search` capability (`src/core/api/capabilities.cpp`) —
  input schema `{target, query, field?, fields?, top_k?}` validated at the
  boundary; output = ranked hits (`{id, score, fields}`) + per-node provenance (each node's `hits`,
  `sub_indexes`, `latency_ms`, `wire_bytes`) + path + trace ids on the
  `ApiResult`. `list_indexes` (#642) is the discovery step: the index registry
  (`Node::indexes()`, the `system.indexes` source) as a tool, so an agent
  finds what `search` can target first. See
  [docs/api/README.md](../api/README.md).
- **GUI:** the **Search** view (`src/gui/app_search.cpp`,
  [docs/gui/search-view.md](../gui/search-view.md)) — target picker over the
  registered Search indexes, the provenance strip + scatter-gather replay,
  master-detail ranked hits, and the per-node provenance table (#643).
- **Served HTTP endpoint (#648):** `GET /v1/indexes/{name}/search` — the
  governed per-entity ingress ([../api/serving.md](../api/serving.md)) — calls
  the same `Node::search` once the index's replicated `exposed` flag is set.
  The resolution rule is exported as `resolve_search_target` (search.hpp) so
  the serving layer and `expose_index` identify exactly the index a target
  names — still no second path.

## Boundaries

**No new JSON format.** The scatter-gather wire payloads are the existing
schema-validated `fulltext_query` / `fulltext_hits` messages
([distributed-fulltext.md](distributed-fulltext.md)); the capability's input
rides the registry's schema-as-code validation; `SearchRequest`/`SearchResult`
are in-process C++ types.

## Tests

[`tests/index/test_node_search.cpp`](../../tests/index/test_node_search.cpp):
resolution + validation failures; the consolidated degraded path (oracle-equal,
self-only provenance, root + local spans); and the acceptance — a real 3-node
loopback cluster where **every node's `Node::search` ranking equals the
consolidated oracle**, provenance lists all three nodes (one local, peers with
wire bytes), and the trace holds exactly one `search` root, one `search:local`,
and a `search:peer` per peer. The single-node `list_indexes → search`
capability journey lives in
[`tests/api/test_registry.cpp`](../../tests/api/test_registry.cpp); the
distributed journey — a 3-node loopback cluster driven purely through the
registry (`list_indexes` → `search` → `get_trace` with its per-peer spans,
#642) — in
[`tests/api/test_search_journey.cpp`](../../tests/api/test_search_journey.cpp).

Known platform issue: the **real-BM25F** ranking-oracle assertions
([#611](https://github.com/oramasearch/zetta/issues/611)) fail deterministically
on some checkouts — a per-shard IDF tie-break, unrelated to this entry point
(the entry-point suite uses the partition-independent reference scorer).

## Later slices

Vector / semantic kinds behind the same `SearchRequest`; global-statistics
scoring (`dfs_query_then_fetch`); dropping stale sub-indexes when an index
entity is dropped mid-flight (today they linger, unqueried, until restart);
peer-side spans stitched into the coordinator's trace (cross-node propagation,
[#640](https://github.com/oramasearch/zetta/issues/640)).
