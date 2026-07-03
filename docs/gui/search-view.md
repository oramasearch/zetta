# The Search view

Data → Search (`src/gui/app_search.cpp`,
[#641](https://github.com/oramasearch/zetta/issues/641) /
[#643](https://github.com/oramasearch/zetta/issues/643)): the GUI rendering of
**THE canonical search entry point**,
[`Node::search`](../indexing/node-search.md) — the exact path the `search`
MCP capability calls, so the two surfaces can never diverge. Distributed-first
AND observability-first: the user searches everything the cluster indexed and
SEES the cluster answer.

## What it renders

- **Query card**: a target picker over the registered Search-kind `Index`
  entities (live from `Node::indexes()` each frame — table · backend), the
  query field (enter submits), a top-k cap, and the Search action.
- **Outcome**: an error `banner` carrying the engine's path-qualified reason
  verbatim; on success the **provenance strip** — `stat_tile`s for the path
  (distributed renders in the Ok tone — the whole cluster answered;
  consolidated renders muted — the degraded single-node index), nodes
  answered, **sub-indexes queried** (summed from the per-node provenance),
  hits, and wall time.
- **The cluster answering** (#643): one node box per answering node
  (coordinator + every scattered peer, from the result's `NodeContribution`s),
  the same box grammar as the Cluster view's canvas — the coordinator wears
  the coral "you are here" border; each box shows the node's short id,
  pre-merge hits · sub-indexes, and latency. On a completed search the
  scatter-gather is **replayed over the boxes as
  [#590 flows](cluster-pipeline-animation.md)**: a scatter comet coordinator →
  each peer, a gather comet back labelled with the peer's hits, a local pulse
  for the coordinator's own contribution — weights anchored to the real
  per-peer wire bytes, never an invented number. The flow palette + drawing
  are shared with the Cluster view via `app_operations_internal.hpp`.
- **Ranked hits**: a [master-detail scaffold](master-detail.md) — rows are
  `rank  row-id` (mono) with the score as the right-aligned meta; the detail
  pane shows rank, score, source (`table · backend`), an `entity_link` into
  the **Datasets** view for the searched table, and the **source row's
  fields** read live from the local store each frame — the indexed fields
  first (the snippet), the remaining fields muted after them. A hit whose row
  lives in a partition owned by a peer says so honestly instead of showing
  nothing.
- **Per-node provenance**: one row per answering node (this node vs peer,
  pre-merge hits, sub-indexes consulted, latency, wire bytes) plus the
  search's trace id with an `entity_link` into the **Traces** view when a
  span ring is wired (`App::set_span_recorder`, connected to the API server's
  recorder in main — GUI searches and MCP calls share one `list_traces`
  history).

## Behaviour notes

- A deep link (`navigate_to(View::Search, index_id)`) preselects that index.
- No Search-kind index yet → the `empty_state` per #631 ("No indexes yet ·
  deploy a solution with an index step, or add an index over a dataset's
  table"); its action navigates to the graph workspace.
- The search runs on the UI thread because the `Node` is main-thread-only; a
  live-cluster scatter answers in milliseconds. The per-peer timeout only
  bites when a connected peer stops answering — an accepted, documented risk
  until search rides a recorded-job pattern.
- The last outcome is an action-outcome snapshot (like the SQL panel's last
  result): presentation state, never a synced engine copy. The flow replay
  (`search_flows_`, a `ClusterPipelineState` using only its flows /
  node_activity members) is animation state, reset per search.
- The row-content lookup scans the local table store by row id (the standard
  live-source read, like the Explorer's row pane) — a replicated table holds
  every row; a partition-placed table holds only this node's shard.
