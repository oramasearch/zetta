---
name: gui-dev
description: Build or change anything in zetta's headed GUI (src/gui) — views, panels, widgets, navigation, sidebar, status rendering, copy. Enforces the view registry + widget grammar architecture (docs/gui/architecture.md), the Orama design system (docs/gui/design-system.md), the thin-renderer rule (no engine logic or synced copies in the GUI), cross-entity navigation via navigate_to, one status Tone language, empty states + banners for every panel, and the load-bearing rendering gotchas. Use for any src/gui work or GUI design discussion.
---

# gui-dev — headed GUI work checklist

Read first, in order: **[docs/gui/architecture.md](../../../docs/gui/architecture.md)**
(view registry · widget grammar · navigation), **[docs/gui/design-system.md](../../../docs/gui/design-system.md)**
(tokens · copy rules · self-check), and the rule card
[src/gui/CLAUDE.md](../../../src/gui/CLAUDE.md) (thin-renderer + rendering
gotchas). This checklist assumes them.

## Adding or changing a view

1. **Registry row first** — section, sentence-case product-language name,
   `draw` member, optional cheap `badge` fn. Nothing else knows the view list
   (sidebar/menus/palette/navigation all render from it).
2. **Compose from the grammar** (`app_internal.hpp`): `page_header` (always,
   first) · `section_card` · `stat_tile` · `empty_state` (mandatory for the
   nothing-yet case — one sentence + the single next action) · `banner`
   (mandatory for action outcomes — engine reason verbatim, never "failed") ·
   `entity_link` for EVERY cross-entity reference · status via `tone_of(...)`
   only. Need a new visual? Add it to the grammar (design-system check) before
   using it.
3. **Live sources only**: render from `system.*` / `Node::…()` each frame; no
   panel-owned copies of engine state (selection/animation only). New
   observable state = a `system.<table>` read view in zetta-core first
   (RULE-7).
4. **Navigation**: change views only via `App::navigate_to(view, entity_id)`;
   consume the per-view pending selection in master-detail panels.
5. **Never block the frame**: long work → worker thread or a recorded job
   polled from its `system` view (#624 pattern). Cloud consent gates are
   sacred (see the rule card's hard rule).

## Before shipping

- Design-system self-check passes; sentence case; no em-dashes; coral only on
  the active/you-are-here accent.
- No local colors/paddings/one-off buttons outside the grammar.
- Screenshot before/after in the PR for visual changes.
- Docs same commit: the view's page in `docs/gui/`, gui README index,
  code-map for new files. New TU ≤ ~40 KB (split by view, #638 precedent).
- Build headed + run the affected flow; `ctest` (GUI-adjacent core logic).
