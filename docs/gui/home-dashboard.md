# Home as a dashboard (SCR-1)

The Home view (`src/gui/app_home.cpp`, #629) answers a returning operator's
first questions from one screen: **is everything ok, what happened since
yesterday, what needs attention**. It replaced the original hero + project
list, which answered none of them. Everything on it composes existing live
read sources and the settled widget grammar — no new engine state, no new
JSON format.

## What it shows (top to bottom)

1. **Cluster health** (`section_card` of `stat_tile_link`s): node count,
   replication sync ("N of M", amber when any peer is behind), the peak live
   gauge across the cluster (the hottest cpu/memory percent in
   `Node::telemetry_snapshot()`, toned by `tone_of_utilization`), active runs
   and active jobs (amber while work is in flight), and — **only when the
   data exists** — the velocity KPI **intent to deployed**: the median time
   from a project's earliest captured intent (`ProjectInput` of kind Intent)
   to a deployment a run ties back to that project (`run.project` +
   `run.deployment` is the link), computed with
   `zetta::util::parse_iso8601_seconds`.
2. **The guided demo card** — when the node is truly empty (no projects, no
   runs, no activity) the dashboard collapses to the health strip plus the
   first-run guided UC-1 demo (#623, [onboarding.md](onboarding.md)); while
   the demo is mid-flight the card stays above the dashboard sections, which
   fill in beneath it as its steps create real state. A dismissed empty node
   falls back to the one-sentence guided-start `empty_state` whose action
   opens the New Project modal.
3. **Needs attention**: failed runs, failed jobs, denials in the last 24
   hours (activity entries with outcome Denied — the gate refusing a deploy,
   governance refusing a caller), and any peer whose replication is behind.
   Capped at 3 per kind with an "and N more" tally; all clear renders a green
   `status_label` + one quiet sentence.
4. **Recent runs**: the newest 5 from `Node::runs()` with `status_label` +
   a "N in the last 24 hours" count (lexicographic cutoff via
   `iso8601_seconds_ago`, the #622 pattern).
5. **Projects**: each with short id, graph count, and **last active** — the
   later of the project's `updated` stamp and the newest run referencing it.
   Clicking a project opens it in the Graph workspace.
6. **Recent activity**: the newest 5 entries of the same feed the Activity
   view renders (`observability::activity_entries`, recomputed per frame).

## The click-through rule (acceptance)

Every tile and row is a link into the owning panel with the item preselected:
stat tiles render through **`stat_tile_link`** (a grammar widget added for
this screen: `stat_tile` geometry + the `entity_link` hover affordance,
returning clicked so the caller can `navigate_to`), and rows use
`entity_link` with the entity id — runs deep-link into Runs, jobs into Jobs,
lagging nodes into Cluster, feed entries through their `ref_kind` (the
Activity table's mapping). The one action that is not navigation — New
project — is the `header_action` on the page header (and the `empty_state`
action in the empty cases).

## Boundaries

- **No new JSON format.** The one new engine surface is
  `zetta::util::parse_iso8601_seconds` (`include/zetta/util/timestamp.hpp`):
  the non-throwing inverse of `now_iso8601`, needed for duration math between
  entity stamps (tests: `tests/util/test_timestamp.cpp`).
- The thin-renderer rule holds: sources are read fresh each frame, the panel
  stores nothing (`selected_project_` is shared presentation state).
- **Later slices:** richer velocity KPIs (per-project, trend) once more
  intent→deploy pairs exist; a Deploy-panel deep link that preselects a
  deployment (the panel takes no entity today, so deployment links navigate
  only). The guided-start card's onboarding content shipped as the guided
  demo (#623, [onboarding.md](onboarding.md)).
