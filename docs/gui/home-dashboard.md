# Home — the Studio welcome (SCR-1)

The Home view (`src/gui/app_home.cpp`, #629; restructured by the Studio design
import, #875 — the ledger entry is
[../product/experience/design-sync.md](../product/experience/design-sync.md))
is **create-first**, per the Orama Studio design: the hero question, the two
entry paths, then the work in flight. The operator dashboard it previously
carried moved to the Operate views (Runs, Activity, Compute jobs,
Infrastructure), where every section already had a fuller owning panel.
Everything composes existing live read sources and the settled widget grammar —
no new engine state, no new JSON format.

## What it shows (top to bottom)

1. **The hero** — `page_header("What would you like to create?")` with the
   design's subtitle; **New project…** stays the `header_action`.
2. **The two entry paths**, side by side (one two-column table of
   `section_card`s):
   - **Create a Solution** ("Start with a goal") — the backed path:
     `primary_button` → the New Project modal → capture an intent → compile.
   - **Discover Opportunities** ("Start with your data") — rendered honestly:
     the Auto-Suggest model that reads a Workspace Profile (your data,
     connections, and daily work) is **in training**; the card holds the
     design's place with a muted `status_label`, no mock flow behind it.
3. **The guided demo card** — unchanged (#623,
   [onboarding.md](onboarding.md)): a truly empty node opens on the guided
   UC-1 walkthrough; a dismissed empty node falls back to the guided-start
   `empty_state` whose action opens the New Project modal.
4. **Needs attention** — failed runs, failed jobs, denials in the last 24
   hours, and any peer whose replication is behind; capped at 3 per kind with
   an "and N more" tally. Rendered **only when non-empty** (the create-first
   home earns no all-clear card).
5. **Continue your work** — every Project with its live state and the next
   useful action, derived from engine state each frame: the newest run
   referencing the project wins (`running` → *Review run*, `failed` → *Review
   failure*, `succeeded` → *Review results*, each an `entity_link` into Runs);
   no runs but compiled Pipelines → `ready` · *Verify and deploy* (→
   Verification); otherwise `draft` · *Capture an intent*. Clicking the
   project name opens it in the Pipeline workspace; each row carries **last
   active** (the later of the project's `updated` stamp and its newest run).

## The click-through rule (acceptance)

Every row action is a link into the owning panel with the item preselected
(`entity_link` + `navigate_to`); the only non-navigation actions are the New
project `header_action` / `primary_button` / `empty_state` action. The
`stat_tile_link` grammar widget this screen introduced lives on in the Operate
views.

## Boundaries

- **No new JSON format**; sources are read fresh each frame and the panel
  stores nothing (`selected_project_` is shared presentation state) — the
  thin-renderer rule holds.
- `zetta::util::parse_iso8601_seconds` (added for the velocity KPI) remains in
  `include/zetta/util/timestamp.hpp` with its tests.
- **Later slices:** the Discover Opportunities path goes live when the
  Auto-Suggest capability ships (#554-adjacent — the card then opens the
  Workspace Profile flow); the intent→deployed velocity KPI returns on Runs
  and activity once per-project trend data exists; richer per-Project next
  actions (deep-linking Verification to the specific Pipeline).
