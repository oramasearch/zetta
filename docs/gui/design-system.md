# Design system (headed GUI)

The headed app follows the **Orama design system** — the same visual language as
the Orama Zetta marketing site and the product surfaces. This page is the
zetta-GUI projection of it: the exact tokens, the typography and copy rules, the
coral discipline, and the component grammar **as implemented in
`src/gui/app.cpp`**. Follow it whenever you add or change GUI.

> **Canonical source.** The full system lives in the `orama-work` repo
> (`modules/orama_design_review/design-system/` — `DESIGN.md` +
> `colors_and_type.css`), with the marketing site as the worked reference
> (`modules/amaro-marketingsite`). When the canonical CSS and this page diverge
> on a *token value*, the CSS wins; update it there, then here. This page is
> self-contained on purpose (the tokens are baked into `app.cpp`), so the zetta
> repo never has to reach outside itself to render correctly.

## Foundations (the four that govern everything)

1. **One coral, used surgically.** Coral `#E2545A` is the brand "this is Orama /
   you are here" signal — the active-view checkmark, the selected graph-node
   border, the single focal phrase on the Home hero. **Never** for status, never
   on hover, at most a few per screen. If a second thing needs emphasis, use
   charcoal/ink, not coral.
2. **Density over decoration.** Practitioner tool: tight, typographic, no
   ornament. Visual interest comes from type + measure, not boxes/gradients.
3. **Typographic hierarchy, not chrome.** Hierarchy from size, weight, colour,
   and case — not shadows or borders-for-show. (Geist is a variable font; we
   render its Regular instance, so there is effectively one weight — lean on size
   and colour.)
4. **Architectural rigor.** Hairline rules, mono labels, dots as markers,
   deliberate negative space. Everything sits on a known plane.

## Colour tokens

Exact values, light surfaces. Defined as `constexpr ImVec4` in
`src/gui/app.cpp` (and the canvas-palette `ImU32`s for the graph draw-list).

| Role | Token | Hex | In code |
|------|-------|-----|---------|
| Brand accent ("you are here") | coral | `#E2545A` | `kCoral` |
| Coral hover | coral-hover | `#D44349` | `kCoralHover` |
| Product text | ink-primary | `#1C1C1A` | `kInkPrimary` |
| Body / secondary | ink-secondary | `#444441` | `kInkSecondary` |
| Section labels / subtitles | ink-muted | `#5F5F5C` | `kInkMuted` |
| Counts / placeholders | ink-quiet | `#7D7D7A` | `kInkQuiet` |
| Working surface | canvas | `#FAFAF8` | `kCanvas` (+ SDL clear) |
| Nav / menubar surface | sidebar | `#F5F4F2` | `kSidebar` |
| Cards / raised | white | `#FFFFFF` | `kWhite` (`ChildBg`) |
| Hairline border | border | `#E0E0E0` | `kBorder` |

**Semantic state palette — for status, never coral, never blue:**

| State | Hex | In code |
|-------|-----|---------|
| Success / fresh / pass / in-sync | `#0F6E56` (text), `#1D9E75` (dot) | `kSuccessColor`, `kDotActive` |
| Warning / stale / locked / syncing | `#854F0B` | `kWarnColor` |
| Error / fail / deny | `#B42318` | `kErrorColor` |

Hover/selected washes and ghost-button fills are black at low alpha
(`wash(0.03–0.06)`), so they sit on any surface.

## Typography

- **Geist** (sans) is the UI face; **Geist Mono** is the label / count / metric /
  code face. Both are embedded (see [build.md](../build.md) and
  `assets/fonts/README.md`); `App::mono_font_` holds the mono face, pushed via
  `ImGui::PushFont` where the mono role applies.
- **Use mono for:** the node-status strip, eyebrow section labels (uppercase),
  counts/IDs/timestamps, and code (paths, CLI commands, SQL).
- **Sentence case for everything.** "New project", "Cloud inspector",
  "Correctness gate" — never Title Case. Proper nouns (Orama Zetta, AWS, SQL,
  ImGui) keep their casing.
- **No em-dashes in product copy.** Use a colon, comma, or parentheses; use the
  middle dot `·` as the separator in metadata strips (`orama zetta · running ·
  up 2m`). (Em-dashes also historically rendered as tofu under the old font; the
  glyph ranges now cover them, but the rule still stands.)
- Eyebrow labels are rendered by the `eyebrow(mono_font_, "UPPERCASE LABEL")`
  helper.

## Component grammar (as built)

- **Theme.** `apply_orama_theme()` paints the ImGui style: `StyleColorsLight`
  base, then the tokens above — ink text on canvas, white card child-panes,
  hairline borders, radius-8 corners, soft-wash selection (never a coral/charcoal
  fill), coral checkmark/nav-highlight.
- **Buttons.** Ghost by default (faint wash + hairline border, ink text). A true
  primary "New …" action is **charcoal-filled, white text** (push `ImGuiCol_Button
  = kInkPrimary` + `Text = kWhite`); see `draw_home`. Coral is never a button
  fill (there is no "send" here).
- **Selection.** Soft black wash (`ImGuiCol_Header`), never coral or charcoal
  fill.
- **Status dots.** `status_dot(colour)` draws the design system's small indicator
  dot inline (green = active/open, amber = locked/attention) — meaning never
  rides on colour alone, the label says it too.
- **Metadata strips.** Mono, `·`-separated, tabular counts, no debug noise — see
  `draw_dashboard` (the always-visible node strip) and the project rows on Home.
- **Callouts.** A tinted, bordered child box for a deliberate boundary — see the
  Cloud inspector consent gate (amber wash when locked, green when granted).
- **Cards.** Child panes are white on canvas with a hairline border (the list /
  detail panes throughout).

## Self-check before adding GUI

- Sentence case; no Title Case, no em-dashes in copy.
- Numbers/IDs/paths/labels in Geist Mono.
- Coral only for "you are here" / one focal action — never status, never hover,
  at most a few per screen.
- Status uses the semantic green/amber/red, with a dot **and** a word.
- Flat: hairline borders + washes, no gradients, no decorative shadows.
- New colours come from the token table above, not improvised.

## Related

- [build.md](../build.md) — how the Geist fonts are embedded.
- [cloud-inspector.md](../cloud/cloud-inspector.md) — a worked example of the grammar
  (dots, eyebrows, mono code, the consent callout).
