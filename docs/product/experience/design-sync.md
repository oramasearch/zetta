# design-sync — the Studio design import ledger

The record of every sync between Daniel's design repo (`oramasearch/orama-design`)
and zetta's two Studio surfaces — the web app (`src/core/api/web_ui.cpp`) and the
desktop GUI (`src/gui/`). One entry per sync wave: the pinned design ref, what
moved, and what was deliberately skipped (with the reason, so the next sync
doesn't re-litigate it). Workflow + translation tables: the **studio-design-sync**
skill (`.claude/skills/studio-design-sync/`). Reference inventory of the design
repo: `docs/projects/orama-studio-marketplace/design-index.md` in
https://github.com/oramasearch/orama-bridge.

Boundaries: rendering only — no new JSON/wire formats, no new behavior paths.
The web app stays C++ server-rendered (minimal hand-written JS only where the
interaction demands it); the desktop is SDL3-first with ImGui as the temporary
crutch (repo `CLAUDE.md` → "Orama Studio — the product surface this engine
wears").

## 2026-07-23 — web app (#874)

**Source pin:** `oramasearch/orama-design` `main` @ `5bdbc27` — live version
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

**Deliberately skipped (and why):**

- **IBM Plex font files** are not embedded/served yet — pages keep the
  self-contained no-external-assets rule and fall back to the system stack
  until the font-serving follow-up lands. (OFL licensing is fine; the open
  question is binary size vs a `/fonts/*` route.)
- **Ask Orama topbar utility**: not added. zetta's `chat` view talks to
  *served/user-built agents*; the design keeps Ask Orama (the product agent)
  distinct from user-built agents, and the product-agent surface (#849) isn't
  the web chat yet. Adding the branded entry now would conflate the two —
  exactly what the design forbids.
- **Search / Get help / Notifications topbar utilities**: no backing
  capability surfaced in the web shell yet (search exists per-index inside the
  data views); Documentation group covers help.
- **The six project tabs / discovery / lifecycle screens**: no backing engine
  capabilities for Opportunities, Verification criteria editing, Saved
  Versions, or Continuous health as designed — the web app renders engine
  truth, no mock screens. These arrive with their capabilities.

## 2026-07-23 — desktop GUI (#875)

**Source pin:** same as the web import — `oramasearch/orama-design` `main` @
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
  (canonical source = `oramasearch/orama-design` `packages/design-system/`);
  the coral-era wording swept from every `docs/gui/` page and the rule-cards.

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
