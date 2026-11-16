# design-sync — the Studio design import ledger

The record of every sync between the Orama design repository (not public) and
zetta's two Studio surfaces — the web app (`src/core/api/web_ui.cpp`) and the
desktop GUI (`src/gui/`). One entry per sync wave: the pinned design ref, what
moved, and what was deliberately skipped (with the reason, so the next sync
doesn't re-litigate it).

Boundaries: rendering only — no new JSON/wire formats, no new behavior paths.
The web app stays C++ server-rendered (minimal hand-written JS only where the
interaction demands it); the desktop is SDL3-first with ImGui as the temporary
crutch.

## 2026-08-05 — Reports, both surfaces ([#960](https://github.com/oramasearch/zetta/issues/960) item 4)

**Source pin:** the design repository `feat/reports-surface` @ `4621d87`
(2026-08-04) — the Reports screens.

**What moved:**

- **IA** — Reports is **one item under Data library**, not a nav group of its
  own. Both surfaces: the web sidebar's Workspace group after Data library, and
  the GUI's `Data library` section in `App::view_registry()`. This was the
  design's own correction mid-branch (`74be8e1`, "Reports is one item under Data
  library, not its own section"), so it is imported as the settled answer rather
  than as the first draft.
- **Screen structure** — how good the model is · what it was built from · what
  drives the outcome · who is most at risk · what to check before acting. Metric
  cards, the driver bars, the ranked table with its evidence columns, and the
  caveats block.
- **Copy and the report's discipline** — the held-out figure leads and says what
  it was measured on; accuracy is never shown without the accuracy a constant
  answer would score; the ranked list says to read the evidence columns *across*
  rather than down; a driver is named a weight, never a cause.
- **The driver-bar ink rule** — every bar shares one ink and length encodes the
  ranking. This is imported as a *rule*, not just a color: the design branch
  shipped a version where the top bar had the accent and the rest a background
  tint, and twelve of thirteen were nearly invisible (`bb7ad72`). It is now the
  `weight_bar` grammar widget in `app_internal.hpp` so a third surface cannot
  reintroduce it.
- **The empty state** — "a Pipeline run produces its report here when it
  finishes", not "no data".

**What the design does NOT supply, and the engine does:**

The design's reports render a fixture run report.
Both zetta surfaces render **the engine's own numbers** through `get_run_report`
([#958](https://github.com/oramasearch/zetta/issues/958)) — one behavior path,
so the GUI, the web app, MCP and chat cannot disagree about what a run found.
Two consequences the fixture never exercised:

- **`missing` is rendered.** A run that asked for no holdout has no honest
  headline figure, and the surface has to *say so* where that figure would have
  been. The fixture always had one.
- **An account key is an identifier, not a quantity.** The web table's numeric
  formatting rendered entity 8336 as "8,336" — a number that does not exist and
  cannot be searched for. Both surfaces now render it unformatted.

**Deliberately skipped (and why):**

- **The React components** (`ReportsScreens.jsx`, `reports.css`, `useReveal`,
  `Ticker`, `GapChart`, `RiskProfile`, `PipelineFlow`). Per the transfer
  contract, code never crosses; the IA, copy and states do.
- **The reveal-on-scroll and count-up animations.** The web surface is
  server-rendered with no framework, and the GUI's motion work rides the SDL3
  surfaces as they land rather than ImGui. The design's *timings* are recorded
  here for when those surfaces exist.
- **The pipeline flow diagram.** It is a rendered Graphviz asset in the design;
  serving it needs the asset-embedding question answered (the same one the IBM
  Plex font binaries are waiting on). The report links nothing external — the
  self-contained page rule holds.
- **The second report** ("Churn model results"). It summarizes a body of work
  rather than a run, so it has no `run_id` to assemble from. A run report is the
  thing the engine can produce honestly today.

## 2026-07-23 — web app (#874)

**Source pin:** the design repository `main` @ `5bdbc27` — live version
"Orama Studio, July 23rd", design system `@orama/design-system` v0.3.0.

**What moved:**

- **Tokens** (`web_ui_stylesheet()`): IBM Plex Sans / IBM Plex Mono stacks
  (system-font fallback), the one action hue Carbon Blue 60 `#0f62fe`
  (`--accent` for links/focus/active, `--accent-solid` for the primary button),
  Carbon-derived neutral surfaces and status colors (always paired with text),
  product default 14px type, and a full **paired dark theme** via
  `prefers-color-scheme` (gray 100/90/80 surfaces, Blue 40 links) — pure CSS,
  no JS. `--white` renamed to the semantic `--surface`.
- **Shell IA** (`web_ui_shell()`): the five Studio sidebar groups — Workspace
  (Home · Projects · Data library · Models), Operate (Runs and activity ·
  Observability · Governance), Developer (Shell · Notebooks · Agents · Chat ·
  Data generators), Documentation (Overview `/llms.txt` · MCP `/mcp.md` · API
  `/v1/openapi.json`), Advanced (Infrastructure — the cluster view, demoted per
  the design). Nav keys (`?view=` values) unchanged; only grouping and labels
  moved. Brand lockup, page titles, login, and console copy are **Orama
  Studio**.
- **Vocabulary contract**: user-facing copy says Pipeline / Verification (never
  "execution graph" / "correctness gate"); the visual graph editor is retitled
  **Pipeline editor** and explicitly labeled *Technical view* — the one
  sanctioned surface for raw platform terms.
- **Icon semantics**: home = Studio home (the `overview` nav key), plus `docs`
  / `mcp` / `api` glyphs for the Documentation group; favicon on Blue 60.
- **Locked by** `tests/api/test_web_ui_studio.cpp` (tokens, groups, vocabulary,
  icons — the renderers are pure, so the contract is directly testable).

**Second pass — the Studio home.** The `overview` view became the design's
create-first welcome: hero ("What would you like to create?"), the two entry
paths (Create a Solution — fully backed; Discover Opportunities — honest
in-training state for the Auto-Suggest model that reads a Workspace Profile),
and Continue your work (every Project with live status + next useful action
from `system.runs` / the authoring stage). The workspace counts moved to the
top of Runs and activity; the cluster table already lived on Infrastructure.

**Third pass — full design parity.** The follow-ups closed: **IBM Plex is
now real on both surfaces** — the node serves the woff2 latin subsets itself
at `GET /fonts/*.woff2` (embedded at build time, immutable-cacheable, no
external requests; `@font-face` in the stylesheet), and the GUI embeds the
Plex TTFs (Sans Regular + SemiBold for headings + Mono; `assets/fonts/`,
`cmake/embed_font.cmake`). The web shell gained the design's chrome: topbar
utility cluster (Search → Data library, Get help → the docs index, the
activity bell → Runs and activity — every icon a real destination), the
signed-in identity card pinned to the sidebar bottom (owning Sign out), the
live Projects count badge, dotted status pills, and a bounded content column.
The GUI sidebar leads with the Orama Studio lockup and the Pipeline-workspace
row carries the project-count badge.

**Fourth pass — the Ask Orama rail, the theme toggle, and the identity.** The
design's collaborator column is real: every page renders the right-hand **Ask
Orama rail**, backed by the node's served agent through `POST /assist` -> the
same `chat` capability the MCP/REST surfaces expose (cookie-authed, audited,
traced). With no served agent the rail shows its honest empty state pointing
at Agents - never a mock conversation. This supersedes the first-pass skip:
the base agent (#555/#849) **is** the product agent, so the naming is honest
now. The topbar gains the **theme toggle** (light -> dark -> follow-the-system;
a pre-paint pin so a stored choice never flashes; `data-theme` +
`prefers-color-scheme` share one override set), and the sidebar identity card
shows the **resolved user name** (`authenticate_token` -> `User::name`)
instead of a key id. The rail's chat JS is the sanctioned kind - streaming
chat is exactly the "only when the interaction demands it" case. Desktop: the
entry cards carry the design's hierarchy (accent eyebrow -> heading-face
title -> body -> action) and Continue your work is the design's three-column
layout (state · project · next action). The Pipeline editor stays on both
surfaces, untouched.

**Still deliberately out (and why):**
- **The six project tabs / discovery / lifecycle screens**: no backing engine
  capabilities for Opportunities, Verification criteria editing, Saved
  Versions, or Continuous health as designed — the web app renders engine
  truth, no mock screens. These arrive with their capabilities.
- **Notifications as a real inbox**: the bell routes to Runs and activity (the
  live feed); a per-user notification store does not exist in the engine.
- (The first-pass skips — fonts, the utility cluster, Ask Orama — were closed
  by passes three and four above.)

## 2026-07-23 — desktop GUI (#875)

**Source pin:** same as the web import — the design repository `main` @
`5bdbc27`, design system v0.3.0. The GUI inherits the token decisions the web
import recorded.

**What moved:**

- **Theme table** (`src/gui/app_internal.hpp` — the one shared grammar
  header): the accent is now named after its role — `kCoral`/`kCoralHover`/
  `kCoralWash` became **`kAccent` (Carbon Blue 60 `#0f62fe`) / `kAccentHover`
  (Blue 70) / `kAccentWash` (Blue 10)** — and the canvas draw-list selected
  fill/border follow. Both render paths consume the same table: the ImGui
  style (`apply_orama_theme`) and the custom SDL3 draw-list canvases
  (`kCanvasBox*`). The "one accent, used surgically; never for status"
  discipline is unchanged — only the hue and the naming converged.
- **Shell IA** (`App::view_registry()`, app_views.cpp): the Studio sidebar
  groups — Workspace (Pipeline workspace · Verification · Deploy · Models),
  Data library (Explorer · Datasets · Search), Operate (Activity · Traces ·
  Runs · Compute jobs · Test suite · Admin & audit), Developer (SQL query ·
  Console · Agents · Chat · Data generators), Advanced (Infrastructure ·
  Cloud inspector). View enum names stay the engine's; labels speak Studio.
- **Vocabulary contract**: "Graph workspace" → **Pipeline workspace**,
  "Correctness gate" → **Verification**, "Cluster" → **Infrastructure**
  (demoted to Advanced); every user-facing "execution graph" / "correctness
  gate" string in panel copy and onboarding rewrote to Pipeline /
  Verification. Window title, About dialog, and menu are **Orama Studio**
  (the About copy states Studio is the surface of the Zetta platform).
- **Docs**: `docs/gui/design-system.md` reground on the Studio system
  (canonical source = the design repository `packages/design-system/`);
  the coral-era wording swept from every `docs/gui/` page and the rule-cards.

**Second pass — the Studio home** (mirrors the web): `draw_home` is
create-first — the hero question via `page_header`, the two entry-path
`section_card`s side by side (Create a Solution → the New Project modal;
Discover Opportunities with the honest in-training state), Needs attention
only when non-empty, and Continue your work (per-Project live state + next
action: newest run → Review run/failure/results; compiled Pipelines →
Verify and deploy; else Capture an intent). The cluster-health strip, recent
runs, and recent activity dropped off Home — their owning panels are one
click away in Operate. `docs/gui/home-dashboard.md` rewrote to match.

**Fifth pass — the July 24 design sync (walked live against the prototype).**
Synced against the live "Orama Studio, July 24th" prototype with a page-by-page
browser walk. The canonical page anatomy now applies to **every** web view:
mono eyebrow -> huge title -> one-line sub -> mono meta strip with hairline
separators (`web_ui_page_hero`), accent-square section headers, tinted table
header rows, and status banners (live execution on Runs and activity, the
red unreachable-node banner + stat cards on Infrastructure). The sidebar
follows the July 24 IA (Connections + Models under Developer; Governance +
Infrastructure under Advanced), the topbar carries the current page title with
the Connected pill, and the Ask Orama rail is a **toggleable drawer, closed by
default** (the July 24 behavior) behind the topbar agent button. New
**Connections** view: the three connection paths (API / MCP / Shell — every
card a live destination), the saved source connections, and the safety
contract. The desktop registry mirrors the same IA. Fixed along the way: the
`sql` capability calls used the wrong input key (`sql` vs `statement`) so the
home worklist fell back to authoring states, and `nlohmann::value()` throwing
on explicit nulls in system rows could kill the node from a render path — all
row reads are now null-safe (the no-exceptions boundary applies to renderers
too).

**Architecture revision (2026-07-24).** The web surface adopts the
design repo's **React Studio app as the real web UI**: screens wired to the
`/v1` JSON surface (the capability registry stays the only behavior path),
the node serves the built bundle (built in the design repo — no npm enters
zetta's build), and the C++ SSR pages remain as the zero-JS fallback until
screen parity, then retire. Supersedes the 07-23 SSR-first rule; the
division of labor follows the design repo's lane docs (Codex builds the
screens; zetta owns serving, auth, and the API contract). Tracking:
the adoption-plan issue on oramasearch/zetta.

**Deliberately skipped (and why):**

- **IBM Plex faces**: the GUI embeds Geist/Geist Mono TTFs; swapping requires
  adding the Plex font binaries — a follow-up shared with the web surface's
  font-serving question. The type *scale* and grammar are unchanged.
- **Custom-SDL3 rewrites of ImGui views**: the rendering philosophy (SDL3
  first, ImGui as the temporary crutch) governs *new* design-led surfaces;
  this import recolors and regroups the existing views rather than rewriting
  them — motion parity work rides the SDL3 surfaces as they land.
- **Documentation nav group**: the GUI has no in-app docs views yet; the web
  surface links the public discovery docs. No mock screens.
- **Ask Orama entry**: same reason as the web — the `chat` view talks to
  user-built/served agents, which the design keeps distinct from the product
  agent.
