# GUI architecture — view registry, widget grammar, navigation

The settled design for the headed app's foundation (#627/#628/#630/#633/#634/
#636). [design-system.md](design-system.md) owns tokens/typography/copy; this
page owns **structure**: how views are declared, how panels are built, and how
the user (or an agent's action) moves between them. Read both before GUI work;
the recipe is at the end.

## The view registry — one list, four consumers

`zetta::gui::ViewInfo` declares each view exactly once:

```
ViewInfo {
  View id;                  // the existing enum
  Section section;          // Home · Data · Pipeline · Operations · Governance
  std::string_view name;    // sentence case, product language
  void (App::*draw)();      // the panel body
  BadgeFn badge;            // optional: cheap live count (running jobs,
                            // failed runs, out-of-sync nodes) — reads the
                            // same live sources as the panel, never stores
}
```

The registry (a function-local static table in `app_views.cpp`) is consumed
by: (1) the **sidebar** (#628), (2) the **view switcher/menus**, (3) the
**command palette** (#625 — navigation entries come from here; action entries
come from the API capability registry, its programmatic twin), (4)
**navigation** below. Adding a view = adding one row (plus its draw method) —
nothing else knows the view list.

## Navigation + selection — the object graph made walkable

`App::navigate_to(View v, std::string entity_id = {})` is the ONLY way any
code changes the active view. It sets the view and, when an entity id is
given, stores it in a per-view **pending selection**; each master-detail panel
consumes its pending selection on next draw (selects + scrolls to the row).
This is the entire foundation #633 (cross-entity links) needs: an entity link
widget calls `navigate_to(View::Runs, run_id)` — no panel-to-panel coupling,
no shared selection state. External actions use it too: the API activity hook
(#620) can deep-link the operator to what an agent just did.

## The widget grammar — panels stop hand-rolling ImGui

All shared widgets live in `app_internal.hpp` (GUI-only, never in
`include/zetta`). The grammar (existing: `wash`, `eyebrow`, `status_dot`,
`mono_line`, `primary_button`; foundation adds):

- `page_header(title, subtitle)` — one per view, identical geometry
  everywhere; actions render to its right via `header_action(label)`.
- `section_card(id, title, fn)` — the bordered content unit; cards never nest.
- `stat_tile(label, value, tone)` — dashboard/summary numbers, mono value.
- `empty_state(headline, body, action_label) -> bool` — the ONLY way a panel
  renders "nothing here" (#631): one sentence + the single next action;
  returns true when clicked.
- `banner(tone, message)` — per-panel action feedback (#632); success/info/
  error with the engine's path-qualified reason verbatim; no modal alerts.
- `entity_link(label, view, id)` — renders as a link, calls `navigate_to`.
- **Status tones**: `enum class Tone { ok, active, warn, error, muted }` with
  `tone_color(Tone)` — and per-enum mapping helpers (`tone_of(RunStatus)`,
  `tone_of(JobStatus)`, `tone_of(NodeSync)`, …) in ONE header so the same
  state always looks the same (#636). Panels never pick status colors ad hoc.

Rules: a panel may compose grammar widgets and plain ImGui primitives
(tables, plots) but may not define local colors, paddings, or one-off buttons
— if a panel needs a new visual, it becomes a grammar widget first (with a
design-system check), then gets used. The master-detail scaffold (#634) is the
next grammar addition after the foundation.

## Layout shell

`draw_active_view()` renders: menu bar (File · View · Help only) → a fixed
left **sidebar** (grammar-rendered from the registry: section headers, rows
with name + badge, coral accent strictly on the active row — the design
system's "you are here") → the content region, which begins with the view's
`page_header`. The sidebar is the app's shape made visible; menus no longer
carry navigation.

## Threading + liveness (unchanged, restated)

The UI thread owns the `Node`; panels read live engine state each frame
(`system.*` sources, `Node::…()`) and keep **no synced copies** — GUI-owned
state is presentation only (selection, animation, pending navigation).
Long-running work never blocks the frame (#624: worker thread or a recorded
job polled from its `system` view). The API server's activity hook marks
panels dirty and appends to the activity feed — arriving on another thread, it
only touches atomics/queues drained in the frame loop.

## Adding a view — the recipe

1. Declare the row in the view registry (section, name, draw, badge?).
2. Build the panel from the grammar: `page_header` first; `empty_state` for
   the nothing-yet case (with its one next action); `banner` for action
   outcomes; `entity_link` for every cross-entity reference; tones from
   `tone_of(...)` only.
3. State it renders comes from a live engine source (a `system.*` view or
   `Node::…()`) — if the state is new, add the read view in zetta-core first
   (RULE-7), never a GUI-only copy.
4. Copy: sentence case, product language (no raw enum/internal strings as
   primary text), no em-dashes; run the design-system self-check.
5. Docs same commit: the view's page under `docs/gui/`, the gui README index,
   code-map for new files.
