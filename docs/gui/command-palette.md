# Command palette — keyboard-first navigation and actions

The Cmd+K palette (#625; `src/gui/app_palette.cpp`): fifteen views, every named
entity, and the whole programmatic control surface behind one keystroke. It is
also the GUI/tool-parity story made tangible — the palette's action list IS the
capability registry ([../api/registry-design.md](../api/registry-design.md)),
rendered for humans.

## Using it

`Cmd+K` (Ctrl+K off-macOS; also View → Command palette) opens a top-centred
overlay. Type to filter, `Up`/`Down` to select, `Enter` to activate, `Esc` to
close. No mouse required for anything below.

## Three entry sources, all live

The entry list is rebuilt from live sources every frame (thin-renderer rule —
the palette caches nothing):

1. **Views** — the view registry (`App::view_registry()`, app_views.cpp). The
   palette is the registry's fourth consumer beside the sidebar, menus, and
   navigation, so a new view row appears here with no palette change.
2. **Entities** — projects, datasets, runs, models, compute jobs, and cluster
   nodes, read from the node/cluster as you type (the browse state stays views
   + actions so an idle open walks no entity lists). Activating one deep-links
   via `navigate_to(view, entity_id)` — the same pending-selection path
   `entity_link` uses, so the target panel selects and scrolls to the row.
3. **Actions** — `api::Registry::list()`, the SAME registry the MCP server
   serves (one registry, many renderings). **Adding a capability once adds it
   here** — name, title, kind, consent marker, and argument form all derive
   from the entry.

Matching is `zetta::util::fuzzy_score` (`include/zetta/util/fuzzy.hpp`) — a
case-insensitive subsequence scorer favouring typed fragments, word starts, and
tight candidates; it lives in zetta-core so it is unit-tested headless
(`tests/util/test_fuzzy.cpp`). Rows render with the grammar: mono eyebrow
sections, `status_dot(tone_of(...))` for lifecycle-bearing entities, mono ids
and right-aligned mono meta.

## Actions run through the one call path

Activating an action with no required input runs it immediately; one with
required fields switches the palette to a **schema-driven argument step** — one
input per `input_schema` property (description as hint, optional fields
marked), so the form is derived from the same draft-07 schema MCP callers
validate against, and a new capability's arguments render with no palette
change.

Execution is `Registry::call` on the frame loop (the loop that owns the Node,
so no bridge hop), acting as the audited "unauthenticated local" session (the
governed-console precedent). That one path gives the palette, for free:

- **Schema validation at the boundary** — staged text is coerced to the
  property's declared JSON type and validated against the entry's schema by
  the registry; no second validator, no new JSON format (boundaries: none).
- **Authorization + audit** — every call lands in the audit trail exactly like
  an MCP call.
- **Observability** — every call is a span in the node's ring; the palette
  shows the trace id, readable back in the Traces view / `get_trace`.
- **The consent gate** — a `requires_consent` (cloud-touching) entry is marked
  `· consent` in its row and never executes on a plain call: the registry
  issues a #621 consent challenge (its id + reason verbatim in the palette's
  banner) that awaits a decision on the Admin panel's approval surface
  ([../api/consent.md](../api/consent.md)) — same gate as everywhere else,
  per the cloud hard rule.

Outcomes render in the overlay: a `banner` (Ok/Error tone, the engine's
path-qualified reason verbatim), the mono trace id, and the result JSON in a
scrollable mono pane.

## Wiring

`main` passes the embedded API server's registry via
`App::set_capability_registry`. Under `--no-mcp` the palette lazily builds its
own `api::Registry` from the same `register_core_capabilities`, so the human
surface never loses actions and the list never forks.

## Later slices

- Recent/frecency ordering and per-view scoped actions.
