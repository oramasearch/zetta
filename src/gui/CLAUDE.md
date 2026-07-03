# src/gui — headed app rules

The GUI is a **thin renderer over a `Node` + `Cluster`** — no engine logic
lives here. Read **[docs/gui/design-system.md](../../docs/gui/design-system.md)**
(the Orama design system) and the **load-bearing rendering notes** in
[docs/gui/README.md](../../docs/gui/README.md) before touching anything.

## Hard rules

- **Engine logic belongs in zetta-core, not here.** A panel reads live engine
  state each frame (`Node::…()`, `Cluster::nodes()`, the `system.<table>`
  sources) and keeps **no separately-synced copy** — GUI-owned state is
  presentation/animation only (`cluster_telemetry.hpp`, `cluster_pipeline.hpp`
  are pure-data, no ImGui). If a panel needs new observable state, add a
  `system.<table>` read view in zetta-core first (RULE-7), then render it.
- **Cloud providers are permission-gated. No exceptions, ever.** Any control
  that would read provider config/credentials or run a provider CLI is
  deny-by-default, ask-every-time, never-remembered consent, naming exactly
  what would be read/called before anything happens (the Cloud Inspector /
  Add-nodes consent grammar). This binds Claude while building, too.
- **Heavy headers stay in `app.cpp`.** `app.hpp` forward-declares SDL/ImGui
  types; only `app.cpp` includes them. Panels are `App` member TUs split
  per view (`app_home` / `app_explorer` / `app_graph_workspace` /
  `app_deploy` / `app_activity` / `app_traces` / `app_runs` / `app_models` /
  `app_datasets` / `app_jobs` / `app_cluster` / `app_cloud` /
  `app_governance` / `app_test_suite`);
  operations-view helpers shared across TUs live in
  `app_operations_internal.hpp`; the view registry + sidebar + navigation
  live in `app_views.cpp`; the Cmd+K command palette lives in
  `app_palette.cpp` (#625 — fuzzy matching itself is
  `zetta::util::fuzzy_score` in zetta-core, tested headless).
- **Navigation goes through the registry.** A view exists as one `ViewInfo`
  row in `App::view_registry()` (app_views.cpp); `navigate_to(view, entity)`
  is the ONLY way code changes the active view (never assign `active_view_`),
  and a panel consumes its deep link via `take_pending_selection(View::…)` at
  the top of its draw. Cross-entity references render with `entity_link`.
  The recipe: docs/gui/architecture.md → "Adding a view".
- **Orama design system, always:** light surfaces, ink text, Geist + Geist
  Mono (`g_mono_font` for labels/counts/metrics; `g_heading_font` only via
  `page_header`), coral used surgically, semantic green/amber/red for status,
  **sentence case**, **no em-dashes** in product copy (use `·`/colons).
  Run the design-system self-check before shipping.
- **Compose the widget grammar — never hand-roll visuals.** Panels build from
  `page_header`/`header_action`/`section_card`/`stat_tile`/`empty_state`/
  `banner` (+ the older `eyebrow`/`status_dot`/`mono_line`/`primary_button`)
  in `app_internal.hpp`, plus plain ImGui tables/plots. No local colors,
  paddings, or one-off buttons — a new visual becomes a grammar widget first.
  **Status colors come ONLY from `tone_of(...)` in `tones.hpp`** (#636); a new
  status-bearing enum adds one overload there, never a panel-local switch. A
  status renders via `status_label(tone, text)` (rows: dot + word),
  `status_pill(tone, text)` (detail-pane headlines — `detail_header` renders
  one), or `status_dot(tone)`; booleans pick a `Tone` at the call site
  (`ok ? Tone::Ok : Tone::Error`).
  See [docs/gui/architecture.md](../../docs/gui/architecture.md) and
  [docs/gui/status-tones.md](../../docs/gui/status-tones.md).
- **List+detail panels render through `master_detail(...)`** (#634) — never a
  hand-rolled split. The panel builds `MasterRow`s from its live source each
  frame, passes its `take_pending_selection(View::…)` result and its
  selection member, and draws only the detail body (opened with
  `detail_header`). See [docs/gui/master-detail.md](../../docs/gui/master-detail.md).
- **Text fields bind `std::string`** via the `imgui_stdlib` overloads — never
  `char[]` buffers.

## Rendering gotchas (easy to break — details in docs/gui/README.md)

- `App::apply_display_scale` uses `SDL_GetDisplayContentScale` (content factor
  only) — `SDL_GetWindowDisplayScale` would double-scale on Retina.
- `SDL_SetRenderScale` before clearing: the high-pixel-density framebuffer is
  larger than the logical window; without it the UI paints into one quadrant.
- The idle loop is `SDL_WaitEventTimeout` (~30 fps idle cap); keep per-frame
  background work cheap. `render_frame()` is re-entrancy-guarded and driven by
  an event watch during live resize — it does **not** tick the node.
- Font-atlas rebuilds (text size / DPI change) happen **between** frames in
  `run()`, never mid-frame.
