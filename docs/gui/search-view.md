# The Search view

Data → Search (`src/gui/app_search.cpp`,
[#641](https://github.com/oramasearch/zetta/issues/641)): the GUI rendering of
**THE canonical search entry point**,
[`Node::search`](../indexing/node-search.md) — the exact path the `search`
MCP capability calls, so the two surfaces can never diverge.

## What it renders

- **Query card**: a target picker over the registered Search-kind `Index`
  entities (live from `Node::indexes()` each frame — table · backend), the
  query field (enter submits), a top-k cap, and the Search action.
- **Outcome**: an error `banner` carrying the engine's path-qualified reason
  verbatim; on success, `stat_tile`s for the path (distributed renders in the
  Ok tone — the whole cluster answered; consolidated renders muted — the
  degraded single-node index), nodes answered, hits, and wall time.
- **Ranked hits**: rank · row id (mono) · score — the same global ranking every
  node would return.
- **Per-node provenance**: the scatter-gather made visible — one row per
  answering node (this node vs peer, pre-merge hits, latency, wire bytes) plus
  the search's trace id when a span ring is wired
  (`App::set_span_recorder`, connected to the API server's recorder in main —
  GUI searches and MCP calls share one `list_traces` history).

## Behaviour notes

- A deep link (`navigate_to(View::Search, index_id)`) preselects that index.
- No Search-kind index yet → the `empty_state` (its action navigates to the
  graph workspace, where an `index` step creates one).
- The search runs on the UI thread because the `Node` is main-thread-only; a
  live-cluster scatter answers in milliseconds. The per-peer timeout only
  bites when a connected peer stops answering — an accepted, documented risk
  until search rides a recorded-job pattern.
- The last outcome is an action-outcome snapshot (like the SQL panel's last
  result): presentation state, never a synced engine copy.
