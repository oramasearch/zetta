# GUI docs

The headed app: an SDL3 + Dear ImGui desktop that is itself a cluster member. The central rule: the
GUI is a **thin renderer** driven by a `zetta::Node` — panels read live engine state
(`system.*` views, `Node::…()` accessors) each frame and hold no engine state of their own; the
`zetta-core` library has no SDL/ImGui dependency, and that seam is what keeps headless mode
possible. **[design-system.md](design-system.md) is the mandatory read before any GUI work** — the
Orama tokens, typography, copy rules, and component grammar it defines are enforced, not advisory.

- [architecture.md](architecture.md) — **the structural design**: the view registry, the widget grammar (page_header/card/empty_state/banner/entity_link/tones), navigation + pending selection, and the adding-a-view recipe.
- [design-system.md](design-system.md) — the Orama design system as applied to the headed GUI: colour tokens, Geist typography, copy rules, coral discipline, component grammar. **Read before touching the GUI.**
- [master-detail.md](master-detail.md) — the master-detail scaffold: one split + row shape + selection model for every list panel (Runs / Models / Jobs / Datasets / Cloud / Admin swept onto it, #634).
- [command-palette.md](command-palette.md) — the Cmd+K palette (#625): fuzzy view navigation + entity jump + the capability registry's actions rendered for humans, run through the one `Registry::call` path (consent gate included).
- [status-tones.md](status-tones.md) — one status visual language: `Tone` + `tone_of(...)` in the single mapping site `tones.hpp`, rendered by `status_label` (rows) / `status_pill` (headers) (#636).
- [copy-audit.md](copy-audit.md) — the product-language pass (#635): `label_of(...)`/`humanize_kind` in the single mapping site `labels.hpp` (SQL keeps raw wire values, panels render words), `term_hint` for unavoidable technical terms, and the per-view copy sign-off checklist.
- [feedback-grammar.md](feedback-grammar.md) — one language for action outcomes (#632): inline field validation, the per-panel `banner` (engine reason verbatim), the job pattern for async work; no modals, no silent failures.
- [cross-entity-links.md](cross-entity-links.md) — the object graph made clickable (#633): `entity_link`/`link_line` placed on every cross-entity reference (Run ↔ Model ↔ Job, Run → Graph → Project, Deployment → Run, Dataset → Cluster, Audit → target).
- [empty-states.md](empty-states.md) — the per-panel empty-state sweep (#631): every nothing-yet case renders `empty_state` (one sentence + the single next action, or none when the affordance is directly above); no bare empty tables on a fresh data_dir.
- [home-dashboard.md](home-dashboard.md) — Home as a real dashboard (SCR-1, #629): cluster health strip, needs-attention, recent runs, projects with last activity, the velocity KPI — every tile a click-through.
- [onboarding.md](onboarding.md) — first-run onboarding (#623): the guided five-step UC-1 churn demo on the empty Home — demo data → project → gate → cluster run → at-risk list + drivers, each step naming its panel, replayable from Help · Guided demo.
- [new-project-screen.md](new-project-screen.md) — the New Project screen (SCR-2): name a project, paste an NL intent, import/drop an n8n workflow artifact in one atomic create (FEAT-1).
- [datasets-panel.md](datasets-panel.md) — the Datasets view: the GUI form of [`system.datasets`](../data/system-datasets.md), with partition placement as a node-box overlay (#586).
- [search-view.md](search-view.md) — the Search view: the GUI rendering of [`Node::search`](../indexing/node-search.md) — provenance strip + scatter-gather replay over node boxes, master-detail hits with source-row fields, per-node provenance; the same path as the `search` MCP tool (#641, #643).
- [jobs-panel.md](jobs-panel.md) — the Compute Jobs panel: live `system.jobs` + per-job objective/scaling controls previewing `ScalingPolicy::evaluate()` (FEAT-5).
- [models-panel.md](models-panel.md) — the standalone Models view: the GUI form of `system.models` — metrics, ranked drivers, weights fingerprint + an inline Add model affordance (#575).
- [explorer-system-tables.md](explorer-system-tables.md) — the explorer's `system` table list rendered from the engine catalog (`system_table_names()` / `system.tables`), never a GUI-side copy (#684).
- [repl-console.md](repl-console.md) — the REPL console component (CMP-6 / #524): `repl::ConsoleModel` in zetta-core + the thin `gui::ReplConsole` renderer behind Data → Console.
- [admin-panel.md](admin-panel.md) — the Admin & Settings panel (SCR-7 / CMP-5): read-only principals + keys, scope + capability grants, and the audit log.
- [activity-feed.md](activity-feed.md) — the Activity view (#622): one chronological feed of everything the cluster was asked to do (audit trail + runs merged at render time; `system.activity` is its queryable twin), each entry with actor + surface (GUI · console · MCP) + outcome.
- [traces.md](traces.md) — the Traces view (#640): the per-operation trace pane over the node's span ring (intent · plan · duration · result shape per span), the GUI form of `system.traces` / `system.spans`.
- [cluster-node-gauges.md](cluster-node-gauges.md) — animated per-node resource gauges + sparklines on the cluster node boxes, fed by the signal-plane telemetry exchange (#584).
- [cluster-pipeline-animation.md](cluster-pipeline-animation.md) — the distributed-algorithm-at-work animation over the cluster canvas: a thin renderer diffing the live instrumentation views (#590).
- [graph-run-overlay.md](graph-run-overlay.md) — the Graph Workspace live-run overlay: per-step status on the canvas from the live Run record, pinned failures, output click-through (#626).
- [long-running-actions.md](long-running-actions.md) — long actions off the UI thread (#624): cloud inspect, the churn run, dataset add — worker + per-frame poll, progress in the owning panel, cancel where supported, completion in the activity feed.
- [external-activity.md](external-activity.md) — live surfacing of externally-initiated actions (#620): the registry's activity hook drained in the frame loop, the "MCP · action · actor" strip line with click-through deep link; why panel refresh + animations need zero wiring.
- [add-nodes-control.md](add-nodes-control.md) — the Add nodes control in the Cluster view: Local runs on a click, every cloud target is consent-gated deny-by-default (#583).

## Load-bearing rendering notes

These live in `src/gui/app.cpp` and are easy to break:

1. **Per-monitor scaling (`App::apply_display_scale`).** SDL3 separates *pixel density* (Retina;
   the ImGui SDLRenderer backend already handles this) from *content scale* (the OS "make the UI
   bigger" hint; ImGui does NOT apply it). `SDL_GetWindowDisplayScale` multiplies the two — using
   it directly double-scales on Retina. We read `SDL_GetDisplayContentScale` for the content factor
   only, and re-rasterize the font atlas at framebuffer-pixel density so text stays crisp when the
   display changes.

2. **The `SDL_SetRenderScale` call before clearing.** The window uses
   `SDL_WINDOW_HIGH_PIXEL_DENSITY`, so the framebuffer is larger than the logical window. The
   SDLRenderer3 backend does NOT set the render scale for us — without this line the UI paints into
   the top-left quadrant only.

**Idle loop + live resize.** The headed loop uses `SDL_WaitEventTimeout` (≈30fps idle cap) rather
than a busy vsync spin — it wakes immediately on input so interaction stays full-speed but doesn't
burn CPU on identical frames. Keep any background work cheap enough to run inside that cadence (or
move it off the loop thread). One frame's worth of build+present is factored into
`App::render_frame()`; an `SDL_AddEventWatch` (`App::on_sdl_event`) calls it on
`WINDOW_RESIZED`/`PIXEL_SIZE_CHANGED`/`EXPOSED` so the UI tracks a live resize drag instead of the
OS stretching the last frame (macOS runs a modal runloop during resize that starves the normal
loop). `render_frame()` has a re-entrancy guard; it does **not** tick the node (that stays in
`run()`).

**Font + text size.** The GUI font size is `kBaseFontPixels × OS-content-scale × user-text-size`.
The user multiplier is **View → Text size** (`ui_scale_`); changing it sets
`font_rebuild_requested_`, and `run()` rebuilds the font atlas via `apply_display_scale()`
**between** frames (never mid-frame, while the atlas is in use). The atlas also rebuilds on
display-scale/DPI changes. Both Geist faces + the whole style scale together, so text-size is
effectively a UI zoom. The fonts — **Geist** (UI sans) + **Geist Mono** (labels / counts / metrics /
code) — are embedded from `assets/fonts/` via `cmake/embed_font.cmake`, so the binary has no runtime
asset dependency; `App::mono_font_` holds the mono face for `PushFont`.
