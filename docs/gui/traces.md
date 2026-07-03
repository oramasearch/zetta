# Traces view — the per-operation trace pane

The per-operation "what happened" surface (#640; Operations → Traces,
`src/gui/app_traces.cpp`). The engine side is the observability foundation
([../architecture/observability.md](../architecture/observability.md)): every
operation records spans — intent, plan, timings, result shape — into the
node's bounded span ring (`Node::span_recorder()`). This view renders that
ring live; it is the GUI form of
[`system.traces` / `system.spans`](../data/query.md) and shows exactly what
`list_traces` / `get_trace` return to an MCP agent (RULE-7: one source, every
surface). The chronological who-asked feed is the sibling
[Activity view](activity-feed.md) (#622); Traces is the drill-down into one
operation's spans.

## What it renders

- **Stat tiles** — traces / spans currently in the ring, and the failed-trace
  count (error tone when non-zero).
- **Trace list** (left) — one row per trace, newest first: a status dot
  (`tone_of_span` — green ok, red failed), the root span's name, the caller's
  intent when one was stated, and a mono meta line (duration · span count ·
  started).
- **Trace detail** (right) — the selected trace's spans in recorded order;
  selecting a span shows its four captures: intent, plan, and result shape as
  wrapped mono blocks, the duration, and (for a failed span) a red banner with
  the engine's reason verbatim.

## Thin-renderer notes

- The panel reads `node_.span_recorder().snapshot()` / `.trace(id)` each
  frame (the ring is mutex-guarded and bounded, so the copy is cheap); the
  per-trace grouping in `summarize_traces` is presentation, mirroring the
  `system.traces` aggregation — never a synced copy of engine state.
- Selection is a trace id + span id, re-resolved each frame; a trace evicted
  from the ring simply empties the detail pane.
- Deep links land via `navigate_to(View::Traces, trace_id)` — the pending
  selection selects the trace on next draw (how other panels will link "what
  happened" for a run/job once they render trace ids).
- Grammar-native: `page_header` · `stat_tile` · `empty_state` (empty ring →
  "Nothing has been traced yet", action opens SQL query) · `banner` · tones
  only via `tone_of_span` (tones.hpp).

## Boundaries

No new JSON format and no new engine state: the view renders the existing
span ring. Agent capability calls are NOT duplicated here as a feed — the
audit-derived [Activity view](activity-feed.md) already surfaces every
capability call with its actor and surface, so the once-planned agent-action
strip (the `ApiServer` activity hook rendered directly) was dropped in favor
of that one source. Later slices (#674): crash reports surfaced in the GUI
(`system.crashes` rows as a banner), a feedback-bundle affordance
(`submit_feedback` — the capability shipped, #640), and per-entity "what
happened" links from the Runs/Jobs panels.
