# The master-detail scaffold — one layout for every list panel

The grammar's list+detail container (#634, the first grammar addition after
the foundation of [architecture.md](architecture.md)). Before it, each
list+detail panel hand-rolled its split — different list widths, selection
styles, detail header shapes — and users re-learned each panel. Now **one
scaffold in `src/gui/app_internal.hpp`** owns the layout, and a panel supplies
only its rows and its detail body.

## The pieces

- **`MasterRow`** — one list row: `id` (the stable entity id the selection
  tracks), `title` (set `id_title` when the title *is* an identifier, so it
  renders in Geist Mono per the design system), `meta` (a muted mono
  annotation right-aligned on the row), `section` (consecutive rows sharing a
  section group under a mono eyebrow — the Cloud panel's provider categories),
  and `tone` (the status dot, `Tone::Muted` when the entity has no lifecycle).
- **`detail_header(title, tone, status)`** — the standard first line of a
  detail pane: the entity's title in mono ink plus an optional `status_pill`
  carrying the tone ([status-tones.md](status-tones.md), #636).
- **`master_detail(id, rows, selected_id, pending, empty_label, select_hint,
  draw_detail, height = 0)`** — the split container. Renders the list left,
  a draggable splitter (ratio 0.15–0.60, default 0.30, persisted in the
  window's ImGui state storage so a resize survives view switches), and the
  detail child right, calling `draw_detail(row)` for the selected row.
  `height` = 0 fills the remaining content region (the standard); a panel
  with content below the scaffold (Admin's audit log) passes a height.

## The one selection model

Selection is the panel's existing `selected_…_` member (an id string,
re-resolved each frame — never a handle). Each frame, in order:

1. A **pending navigation** (the panel's `take_pending_selection(View::…)`
   result, passed in) wins, and its row is scrolled into view — this is how
   `entity_link` / `navigate_to(view, entity_id)` deep links land.
2. Otherwise the current selection is kept **while its row still exists**.
3. Otherwise it falls back to the **first row**, so the detail pane always
   shows a real entity while any exist.

Refresh is the standard thin-renderer rule made structural: panels rebuild
`rows` from their live engine source every frame; the scaffold caches nothing.

## The sweep (#634)

Runs, Models, Compute jobs, Datasets, Cloud inspector, and Admin & audit all
render through the scaffold (and open with the grammar's `page_header`).
Notable per-panel facts:

- **Runs / Jobs** — UUID-segment titles (`entity_short_id`, now shared from
  `app_operations_internal.hpp`), lifecycle tone dots.
- **Datasets** — meta is `format · database` so same-named tables across
  databases stay distinguishable.
- **Cloud** — rows grouped by provider category via `section`; the consent
  gate is unchanged and selection changes still revoke consent
  (deny-by-default, ask every time). The scaffold's first-row fallback lands
  on Local, which reads nothing and needs no consent.
- **Admin** — passes an explicit height so the audit log keeps its place
  below the scaffold.
- **Explorer** keeps its multi-column browser (its own pattern by design);
  its rows are plain title Selectables, the same selection style the
  scaffold's rows use.

A new list panel is now: build `rows` from the live source, call
`master_detail(...)`, draw the detail body — well under 50 lines of layout.

## Boundaries

No new JSON format, no engine changes — the scaffold is GUI-only
(`app_internal.hpp` never enters `include/zetta`). The empty-state (#631),
feedback-banner (#632), and cross-entity-link (#633) sweeps rewrite these same
panel bodies next; the scaffold is deliberately silent about them (its
`empty_label`/`select_hint` are plain muted text until #631 lands).
