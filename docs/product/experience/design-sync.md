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

See the entry appended by the GUI import branch (`feat/studio-design-import-gui`).
