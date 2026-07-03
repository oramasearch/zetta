# GUI architecture — view registry, widget grammar, navigation

The settled design for the headed app's foundation (#627/#628/#630/#633/#634/
#636). [design-system.md](design-system.md) owns tokens/typography/copy; this
page owns **structure**: how views are declared, how panels are built, and how
the user (or an agent's action) moves between them. Read both before GUI work;
the recipe is at the end.

## The view registry — one list, four consumers

`App::ViewInfo` (app.hpp) declares each view exactly once:

```
struct ViewInfo {
  View id;                          // the existing enum
  const char* section;              // "" (Home) · Data · Pipeline · Operations · Governance
  const char* name;                 // sentence case, product language
  void (App::*draw)();              // the panel body
  std::string (App::*badge)() const;// optional: cheap live count (running
                                    // jobs, member count) — reads the same
                                    // live sources as the panel, never stores;
                                    // nullptr = none, "" = hidden this frame
};
```

The registry (`App::view_registry()`, a static table in `app_views.cpp`) is consumed
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
`include/zetta`), alongside the pre-existing `wash`, `eyebrow`, `status_dot`,
`mono_line`, `primary_button`. The built grammar:

- `page_header(title, subtitle)` — one per view, identical geometry
  everywhere; title renders in the Geist heading face (`g_heading_font`,
  rasterized beside the mono face in `apply_display_scale`); actions render
  to its right via `header_action(label)` (call it directly after the header;
  multiple actions stack right-to-left).
- `section_card(id, title, fn)` — the bordered white content unit with an
  eyebrow title; cards never nest.
- `stat_tile(label, value, tone)` — dashboard/summary numbers, mono value
  colored by tone (`Tone::Muted` renders plain ink); lay out with `SameLine`.
- `stat_tile_link(label, value, tone) -> bool` — a stat_tile that is also a
  link (#629): same geometry plus the entity_link hover affordance (hand
  cursor + underline), returning clicked so the caller can `navigate_to` the
  owning panel. Home's health strip is the worked example.
- `empty_state(headline, body, action_label) -> bool` — the ONLY way a panel
  renders "nothing here" (#631): one sentence + the single next action;
  returns true when clicked (pass `""` for no action).
- `banner(tone, message)` — per-panel action feedback (#632); a tinted strip
  (`tone_background`) with the engine's path-qualified reason verbatim;
  renders nothing on an empty message; no modal alerts.
- `entity_link(label, view, id)` — renders as a link, calls `navigate_to`;
  `link_line(label, display, view, id)` is its detail-pane "label: ref" form.
  The wired edges of the object graph (Run ↔ Model ↔ Job, Run → Graph →
  Project, Deployment → Run, Dataset → Cluster, Audit → target) and the
  never-fake-a-destination rule live in
  [cross-entity-links.md](cross-entity-links.md) (#633).
- **The master-detail scaffold (#634)**: `master_detail(id, rows, selected,
  pending, …)` — the ONE way a list+detail panel lays itself out: a resizable
  split (ratio persisted per scaffold), identical `MasterRow` rows (status dot
  + title + right-aligned mono meta, optional section eyebrows), one selection
  model (pending navigation wins → keep while present → fall back to the first
  row), and `detail_header(title, tone, status)` opening the detail pane. See
  [master-detail.md](master-detail.md).
- **Status tones (#636)**: `enum class Tone { Ok, Active, Warn, Error, Muted }`
  with `tone_color(Tone)` / `tone_dot_color(Tone)` / `tone_background(Tone)`
  in `app_internal.hpp`, and the per-enum mapping overloads in
  **`src/gui/tones.hpp`** — the ONE mapping site: `tone_of(RunStatus)`,
  `tone_of(JobStatus)`, `tone_of(ScalingAction)`, `tone_of(CheckStatus)`,
  `tone_of(TestOutcome)`, `tone_of(ActivityOutcome)`,
  `tone_of(cluster::NodeStatus)` (node sync),
  `tone_of(StepPlacement)`, `tone_of_log_level(level)`, and
  `tone_of_utilization(percent)` (the shared gauge thresholds). A status
  renders through the two widget forms — `status_label(tone, text)` (dot +
  word, rows) and `status_pill(tone, text)` (uppercase mono word on the tinted
  pill, detail-pane headlines) — or the bare `status_dot(tone)`. Panels never
  pick status colors ad hoc; a new status-bearing enum adds one overload
  there. See [status-tones.md](status-tones.md).

Rules: a panel may compose grammar widgets and plain ImGui primitives
(tables, plots) but may not define local colors, paddings, or one-off buttons
— if a panel needs a new visual, it becomes a grammar widget first (with a
design-system check), then gets used. A list+detail panel never hand-rolls its
split: it renders through the master-detail scaffold.

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
