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
  Add-nodes consent grammar). This binds coding agents while building, too.
  **The GUI's own gates are unaffected by `--governance`.** That flag switches
  the *remote* consent challenge for API/MCP callers, which is off by default
 . The in-app gates are the human clicking in front of the machine —
  they stay on, always, and none of them may be made conditional.
- **Heavy headers stay in `app.cpp`.** `app.hpp` forward-declares SDL/ImGui
  types; only `app.cpp` includes them. Panels are `App` member TUs split
  per view (`app_home` / `app_explorer` / `app_graph_workspace` /
  `app_deploy` / `app_activity` / `app_traces` / `app_runs` / `app_models` /
  `app_datasets` / `app_jobs` / `app_cluster` / `app_cloud` /
  `app_governance` / `app_test_suite`);
  operations-view helpers shared across TUs live in
  `app_operations_internal.hpp`; the view registry + sidebar + navigation
  live in `app_views.cpp`; the Cmd+K command palette lives in
  `app_palette.cpp` (fuzzy matching itself is
  `zetta::util::fuzzy_score` in zetta-core, tested headless).
- **Navigation goes through the registry.** A view exists as one `ViewInfo`
  row in `App::view_registry()` (app_views.cpp); `navigate_to(view, entity)`
  is the ONLY way code changes the active view (never assign `active_view_`),
  and a panel consumes its deep link via `take_pending_selection(View::…)` at
  the top of its draw. Cross-entity references render with `entity_link`.
  The recipe: docs/gui/architecture.md → "Adding a view".
- **Orama Studio design system, always:** light surfaces, ink text, IBM Plex
  Sans + IBM Plex Mono (`g_mono_font` for labels/counts/metrics; the SemiBold
  heading face only via `page_header`), the accent blue (Carbon Blue 60) used surgically, semantic green/amber/red for status,
  **sentence case**, **no em-dashes** in product copy (use `·`/colons).
  Run the design-system self-check before shipping.
- **Compose the widget grammar — never hand-roll visuals.** Panels build from
  `page_header`/`header_action`/`section_card`/`stat_tile`/`stat_tile_link`/
  `empty_state`/`banner`/`callout` (+ the older `eyebrow`/`status_dot`/
  `mono_line`/`primary_button`)
  in `app_internal.hpp`, plus plain ImGui tables/plots. Data tables use the
  shared `kDataTableFlags` (zebra off), sentence-case headers, and
  `table_numeric(value)` for right-aligned mono numeric cells. Draw-list
  canvases colour ONLY from the shared canvas palette (`kCanvasBox*` etc.) in
  `app_internal.hpp`. No local colors,
  paddings, or one-off buttons — a new visual becomes a grammar widget first.
  **Status colors come ONLY from `tone_of(...)` in `tones.hpp`**; a new
  status-bearing enum adds one overload there, never a panel-local switch. A
  status renders via `status_label(tone, text)` (rows: dot + word),
  `status_pill(tone, text)` (detail-pane headlines — `detail_header` renders
  one), or `status_dot(tone)`; booleans pick a `Tone` at the call site
  (`ok ? Tone::Ok : Tone::Error`).
  **Enum/kind labels come ONLY from `label_of(...)` / `humanize_kind` in
  `labels.hpp`** — no raw wire name ("ddf", "near_data",
  "graph_deployed") as primary text; raw values live in labelled mono
  metadata lines only, SQL/`system.*` output stays raw by design, and
  unavoidable technical terms get a `term_hint("…")` hover. Rules + the
  per-view sign-off: docs/gui/copy-audit.md.
  See [docs/gui/architecture.md](../../docs/gui/architecture.md) and
  [docs/gui/status-tones.md](../../docs/gui/status-tones.md).
- **Action outcomes render through the feedback grammar** — every
  mutating affordance surfaces its outcome as the panel's `banner(tone,
  message)` (errors carry the engine's reason **verbatim**, never "operation
  failed"), inline field validation banners at the form, and long work follows
  the job pattern (progress in flight, banner on completion). No modal
  alerts; no silent failures — check a mutating call's returned status and
  surface it. Success belongs in the panel where the action was taken, not
  another view's status line. See
  [docs/gui/feedback-grammar.md](../../docs/gui/feedback-grammar.md).
- **List+detail panels render through `master_detail(...)`** — never a
  hand-rolled split. The panel builds `MasterRow`s from its live source each
  frame, passes its `take_pending_selection(View::…)` result and its
  selection member, and draws only the detail body (opened with
  `detail_header`). See [docs/gui/master-detail.md](../../docs/gui/master-detail.md).
- **Text fields bind `std::string`** via the `imgui_stdlib` overloads — never
  `char[]` buffers.
- **Long actions never block the frame.** A slow engine action runs off
  the UI thread behind an op handle polled by `App::poll_background_ops()`
  each frame — pure work (a CLI, a file decode) via `runtime::spawn`
  (`CloudInspectOp`), engine runs whose node writes gate through
  `Node::run_on_owner` via a core async handle (`AsyncChurnRun`,
  `AsyncDatasetAdd`). The owning panel shows progress (live engine records,
  never a GUI copy) + cancel where supported; completion surfaces in the
  activity feed. See [docs/gui/long-running-actions.md](../../docs/gui/long-running-actions.md).

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
