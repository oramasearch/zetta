# studio-design-sync — reference

Repo maps and translation tables. Paths on the design side are relative to the
`oramasearch/orama-design` root; zetta paths are relative to this repo root.

## Where things live — design repo (`oramasearch/orama-design`)

**Front door: the Orama Project Hub** at `/workspace/` on
`orama-website-snowy.vercel.app` — "one place to review the work and decide
what comes next." Its overview organizes the repo's four major destinations
and shows the review status rail (current focus · next review · sequence —
currently "Web first, desktop parity next", which is also why the web import
outranks the desktop import):

1. **Orama Website** — the selected Alpha v2 marketing direction (`src/`).
2. **Orama Studio Web Platform** — the live Studio prototype + frozen
   milestone archives beside the Original baseline (`src/studio/`, `/studio/`).
3. **Storybook Design System** — Carbon-backed foundations, components, Zetta
   patterns, screen contracts (`packages/design-system/`, `/storybook/`).
4. **Roadmap and Design Documentation** — meeting record, product direction,
   screen plan, flows, decisions, research, priorities, open contracts
   (`src/workspace/`, `/workspace/documentation/<section>/`).

Start a sync by loading the Hub overview — it tells you what changed and what
Daniel considers current before you read a single file.

| What | Where |
|---|---|
| Live Studio prototype (the design to import) | `src/studio/` — one live version (July 23); router + shell in `StudioApp.jsx` |
| Discovery journey screens/states (Package A) | `src/studio/discovery/` — `discoveryContracts.js` holds views, copy, scenario states |
| Solution lifecycle screens/states (Packages B/C) | `src/studio/lifecycle/` — `lifecycleContracts.js` (views, states, copy), `lifecycleModel.js`, `lifecycleFixtures.js` |
| Version metadata + archive URLs | `src/studio/studioVersions.js` |
| Design system package | `packages/design-system/` — v0.3.0 |
| Semantic tokens (DTCG, source of truth) | `packages/design-system/tokens/dtcg/semantic/semantic.tokens.json` (+ `foundation.tokens.json`; primitives under `tokens/dtcg/primitive/`) |
| Tokens as CSS | `packages/design-system/tokens/theme.css` (semantic roles), `tokens/tailwind.css` |
| Component/pattern/screen governance | `packages/design-system/governance/catalog.json` (the structured source), `DESIGN.md` (narrative) |
| Icon catalog + semantics | `packages/design-system/icons/catalog.ts` (124 approved, 9 categories), `governance/icon-usage.json` (one icon = one meaning) |
| Carbon boundary (what Carbon owns) | `packages/design-system/governance/carbon-dependencies.json` |
| Design change record | `packages/design-system/CHANGELOG.md` |
| Design specs (dated decisions) | `docs/superpowers/specs/*.md` |
| Session state + the six gating confirmations | `HANDOFF.md` |
| Planning surface (flows, journeys, 14-screen plan, decisions) | `src/workspace/workspaceData.js` (all content), rendered at `/workspace/documentation/…` |
| Frozen archives — **never touch** | `public/studio-2026-07-22/`, the `/zetta/` baseline build |
| Licensed vendored source — **never copy** | `packages/design-system/vendor/motion-plus/` |

Cross-reference inventory: `docs/projects/orama-studio-marketplace/design-index.md`
and `ui-structure.md` in https://github.com/oramasearch/orama-bridge.

## Where things live — zetta

| What | Where |
|---|---|
| Web app renderers (pure: data in → HTML out) | `src/core/api/web_ui.cpp`, public API `include/zetta/api/web_ui.hpp` |
| Web stylesheet (all tokens as CSS custom properties) | `web_ui_stylesheet()` in `web_ui.cpp`, served at `GET /style.css` |
| Web inline icon set | `web_ui_icon()` (24×24 SVG, `currentColor`) |
| Web shell (topbar + nav) | `web_ui_shell()` |
| Web auth/query adapter (not a style target) | `src/core/api/http_server.cpp` |
| Desktop GUI | `src/gui/` (`app.cpp` pulls the heavy headers; view registry per `docs/gui/architecture.md`) |
| GUI design rules | `docs/gui/design-system.md` + `docs/gui/README.md` rendering notes |
| GUI rule-card | `src/gui/CLAUDE.md` |
| Sync ledger (create on first sync) | `docs/product/experience/design-sync.md`, indexed in `docs/product/experience/README.md` |
| Product experience contracts | `docs/product/experience/ux.md` (update when IA/copy rules change) |

## Token translation

Design DTCG semantic roles → web CSS custom properties in `web_ui_stylesheet()`.
Current zetta web palette vs the design system (the divergence an import issue
must resolve explicitly):

| Role | zetta web today | design system (Studio) |
|---|---|---|
| Action/accent | `--accent:#2563EB` | Blue 60 `#0f62fe` (light) / Blue 40 `#78a9ff` (dark) |
| Interface font | Geist (`--font-sans`) | IBM Plex Sans 400/500/600 |
| Mono font | Geist Mono | IBM Plex Mono 400/500 |
| Canvas/surfaces | `--canvas:#FAFAF8`, `--sidebar:#F5F4F2` | light: quiet gray canvas / white surface; dark: gray 100/90/80 |
| Status | `--ok/--warn/--err/--info` pairs | Carbon-derived green/yellow/red/blue, always paired with text |
| Type scale | 15px base | 11–32px scale, product default 14px, tabular numerals |

Rules that ride along with the design tokens: accent reserved for real actions
(<10% of a screen — and never on review chrome), status color never the sole
carrier of meaning, both light and dark themes if dark ships, reduced-motion
safe animation. Fonts: IBM Plex is OFL-licensed — embed/serve from the node or
keep the system-stack fallback; **no external font requests** (self-contained
page rule).

For the GUI, the same semantic roles map to one theme table (constants named
after the semantic role, not the hex value) consumed by both render paths:
custom SDL3 drawing (the destination — pixel-perfect, owns design-led
surfaces and motion) and the remaining ImGui views (the temporary crutch —
ImVec4 style values derived from the same table). The design's motion tokens
(durations/easings from `tokens/dtcg/primitive/motion.tokens.json`) transfer
to both surfaces: CSS transitions on the web, SDL3 animation timings on the
desktop — motion parity is part of design parity, and reduced-motion always
has a plain fallback.

## IA translation (the July-23 shell)

Design shell → zetta surfaces. Nav groups: **Workspace** (Home · Projects ·
Data library) · **Operate** (Runs and activity) · **Developer** (Connections) ·
**Documentation** (Overview · Changelog · MCP · CLI · API) · **Advanced**
(Infrastructure). Topbar: Studio lockup + Search · Get help · Notifications ·
**Ask Orama**. Project workspace: six tabs — Overview (Intent) · Data · Build ·
Runs · Results · Activity. Cluster/infrastructure is demoted to Advanced —
never the entry point. Map each design view to the zetta view/capability that
actually backs it; where the engine has no backing capability yet, the screen
is **out of scope for the import** (no mock screens in the product — the design
repo owns speculative screens, zetta renders engine truth).

## Icon semantics (design contract → `web_ui_icon()` / GUI glyphs)

Home=Studio home · Folder=Projects · DataBase=Data sources/library ·
ChartLine=Runs and activity · Code=Developer connections ·
ChangeCatalog=Changelog · ConnectionSignal=MCP · Terminal=CLI · Api=API ·
Network_3=Infrastructure · ChatBot=Ask Orama · DecisionTree=Technical/Pipeline
view · Security=Verification · Deploy=Deployment. One icon = one meaning across
every surface; Carbon icon artwork is Apache-2.0 (fine to trace into the inline
SVG set); sizes 16 inline / 20 nav / 24 empty-state.

## State-machine sources (for copy + states import)

- Discovery: `discoveryContracts.js` `scenarioGroups` — per-view states incl.
  permission-failure, unsupported-source, stale, partial, canceled,
  zero-results ("will not suggest something it cannot back up"), low-confidence,
  duplicate-warning.
- Lifecycle: `lifecycleContracts.js` `scenarioGroups` + `lifecycleModel.js` —
  plan (draft→approved/stale), build (idle→running→training→stage-failure/
  canceled/complete), results (production-ready / not-ready), versions
  (restore flow), deployment (none→active/paused/update-failure/revoked),
  health (healthy→change-detected→recovery-proposed→recovering→recovered/
  unrepairable), advanced (valid/validation-problems/read-only).

Every imported screen carries its non-happy-path states — the design treats
scenario coverage as a principle, and so do we (empty states + banners for
every panel, per `docs/gui/empty-states.md`).
