---
name: studio-design-sync
description: Sync the Orama Studio design between Daniel's design repo (oramasearch/orama-design) and zetta's two Studio surfaces — the server-rendered web app (src/core/api/web_ui.cpp) and the desktop GUI (src/gui). Forward = import the design's tokens, IA, copy, and states into zetta; reverse = backport new engine features into the design as definition updates. Use when asked to "import the design", "sync the studio design", "update the web app style from the design", "apply Daniel's design", "backport this feature into the design", or when a new design version drops.
---

# studio-design-sync

Move the Orama Studio design between `oramasearch/orama-design` (Daniel's design
repo — the design source of truth) and this repo's two Studio surfaces:

- **Web app** — `src/core/api/web_ui.cpp` + `include/zetta/api/web_ui.hpp`
  ([#647](https://github.com/oramasearch/zetta/issues/647)): server-rendered,
  no-framework HTML; one cacheable stylesheet from `web_ui_stylesheet()`
  (`GET /style.css`); inline SVG icons from `web_ui_icon()`.
- **Desktop GUI** — `src/gui/` ([#627](https://github.com/oramasearch/zetta/issues/627)):
  SDL3 + Dear ImGui; theme rules in `docs/gui/design-system.md`.

Both surfaces **are Orama Studio** (epic
[#854](https://github.com/oramasearch/zetta/issues/854)). Zetta is the platform;
Studio is what the user sees — see "Orama Studio — the product surface this
engine wears" in the repo `CLAUDE.md`.

**The reference inventory of the design repo** (screens, flows, state machines,
components, tokens, decisions) is `docs/projects/orama-studio-marketplace/design-index.md`
in https://github.com/oramasearch/orama-bridge — read it before either
direction. [reference.md](reference.md) in this skill maps both repos
file-by-file and carries the token/icon translation tables.

## The transfer contract (both directions)

**What transfers:** semantic tokens (color roles, type scale, spacing, shape,
motion durations), layout + information architecture (nav groups, screen
structure, tab sets), copy (screen text, the vocabulary contract), states
(every screen's happy/failure/partial/canceled/restricted states), icon
*semantics* (which concept gets which glyph), interaction rules (focus,
reduced-motion, impact previews).

**What never transfers:**
- **React code.** The design prototype is React; zetta's web app stays
  no-framework server-rendered HTML and the GUI is ImGui. Re-implement, never
  port components.
- **Vendored licensed source** — the design repo's `vendor/motion-plus/` is
  licensed Motion+ code; never copy it here.
- **A365 / Renesas client IP** (design-repo rule; it binds us too).
- **Marketing-clone content** (`src/clone/` over there is verbatim orama.com
  copy — canonical there, irrelevant here).
- **Fake data as fact.** Design fixtures (Maya Chen, seeded Opportunities) are
  demo scaffolding; zetta surfaces render the real read model.

**Hard rules that bind every import:**
- The **vocabulary contract** (repo `CLAUDE.md`): user-facing copy says
  Pipeline / Verification / Workspace Profile / Saved Version / Automatic
  Recovery / Proposed Solution / **Ask Orama** — never "execution graph",
  "correctness gate", "Ask Studio". Raw platform terms are allowed only in an
  explicit Technical view.
- **Lifecycle invariants:** no Deployment without a Saved Version; nothing
  production-ready without a passed Verification; consequential actions get an
  impact preview with a cancel path.
- The web app's **self-contained page rule** (`web_ui.hpp` header comment): no
  external assets, no framework — fonts are embedded/served by the node or
  fall back to the system stack.
- **Rendering philosophy** (Steve, 2026-07-23). Web: **C++ server-rendered
  stays** — the response time is part of the product; prefer CSS for motion;
  hand-write minimal JavaScript only where interaction genuinely demands it
  (avoiding a full-page flicker on an in-place update, streaming chat — the
  Shell REPL/Notebooks pattern is the precedent), never a framework. Desktop:
  **SDL3-first** — Dear ImGui is a temporary crutch, kept only where it
  already gives a good experience; new design-led rendering goes custom SDL3
  (pixel-perfect), so the desktop matches the web and the design's style
  **and motion design** at the highest achievable parity.
- Style convergence is a **recorded decision, not a side effect**: the design
  system (IBM Plex + Carbon Blue 60 `#0f62fe`) differs from zetta's current
  web palette (Geist + `#2563EB`) and from the GUI rules (Geist + coral). An
  import issue states which token source wins for which surface; don't
  silently mix them. When in doubt, ask Steve.

## Forward sync: design → zetta

1. **Pin the source.** Record the `orama-design` commit/tag being imported
   (e.g. `main` @ `<sha>`, live version `2026-07-23`). Every artifact of the
   sync cites this ref.
2. **Read the delta** since the last sync (ledger:
   `docs/product/experience/design-sync.md`): the **Project Hub overview**
   (`/workspace/` — current focus · next review · sequence; the four
   destinations, see [reference.md](reference.md)), `git log` in the design
   repo, `packages/design-system/CHANGELOG.md`, new specs in
   `docs/superpowers/specs/`, `src/studio/studioVersions.js`, and the
   design-index for orientation.
3. **Extract the transferables** (paths + translation tables in
   [reference.md](reference.md)): DTCG semantic tokens → CSS custom properties
   / ImGui theme constants; the shell IA (nav groups, topbar utilities, the
   six project tabs); screen copy + states from
   `src/studio/{discovery,lifecycle}/…Contracts.js`; icon semantics from
   `governance/icon-usage.json`.
4. **Apply per surface.** Web: edit `web_ui_stylesheet()` / shell / views in
   `src/core/api/web_ui.cpp` — pure renderers, data in → HTML out; never a new
   behavior path (the capability registry stays the only surface); server-side
   HTML + CSS first, minimal hand-written JS only where the interaction
   demands it. GUI: theme constants + view registry per
   `docs/gui/architecture.md`, following `src/gui/CLAUDE.md` — prefer custom
   SDL3 rendering for design-led surfaces (ImGui only where it already serves
   well), and carry the design's motion timings, not just its colors.
5. **Verify.** Build + tests green; walk every changed view (web: each nav
   route, light/dark if both exist, narrow viewport; GUI: headed run). Grep
   user-facing strings for vocabulary violations. Confirm no external asset
   requests.
6. **Document in the same commit** (repo docs discipline): update the ledger
   `docs/product/experience/design-sync.md` (create on first sync; index it in
   `docs/product/experience/README.md`) with source ref → what moved → what
   was deliberately skipped; update `docs/gui/design-system.md` /
   `docs/product/experience/ux.md` if rules changed.
7. **Ship** on a feature branch → PR to `dev` (never direct). Title style:
   `feat(web): import Studio design <version> — <what>`. No AI attribution.

## Reverse sync: zetta → design (backport a feature)

The design repo has its own workflow — respect it: **one integrator** at a time
(currently Codex) owns implementation on its production branch; frozen dated
archives are immutable; one live version (its decision 0005). We land
**definitions and evidence, not screens**:

1. Identify the engine feature (issue/PR refs, the capability entries, what it
   actually does — states, failure modes, permissions).
2. Translate to product vocabulary (the contract above) and to the design
   repo's objects (Opportunity / Solution / Saved Version / …).
3. Write it as a **spec** in the design repo's `docs/superpowers/specs/`
   (dated, like `2026-07-23-…-design.md`) plus, where it answers one of the
   design's open implementation contracts / the six HANDOFF confirmations,
   say so explicitly — engine truth answering a design question is the highest
   value transfer.
4. Update the design repo's `HANDOFF.md` "what moved" note; never touch
   `public/studio-*/` archives or rewrite prototype code yourself unless
   Daniel names you the integrator.
5. Commit there per its `CLAUDE.md` (no AI attribution — org policy applies).
6. Record the backport in the ledger here too.

## Filing sync issues

One issue per surface per sync wave on `oramasearch/zetta`: priority label
(`p1`–`p4`), area label if present, body cites the pinned design ref, the
design-index URL, and the scope table (tokens / IA / copy / states / icons).
Never `fixes #N` — issues stay open through validation.
