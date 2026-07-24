# Design system (headed GUI)

The headed app follows the **Orama Studio design system** — the visual language
imported from the design repo (oramasearch/orama-design; the import ledger is
docs/product/experience/design-sync.md, #875). This page is the zetta-GUI
projection of it: the exact tokens, the typography and copy rules, the accent
discipline, and the component grammar **as implemented in
`src/gui/app_internal.hpp`** (the one shared grammar header). Follow it
whenever you add or change GUI.

> **Canonical source.** The design system lives in the private
> `oramasearch/orama-design` repo (`packages/design-system/` — Storybook is the
> visual source of truth; DTCG tokens under `tokens/dtcg/`). When the design
> system and this page diverge on a *token value*, the design system wins;
> sync it via the studio-design-sync skill and record it in the ledger. This page is
> self-contained on purpose (the tokens are baked into `app_internal.hpp`), so
> the zetta repo never has to reach outside itself to render correctly.

## Foundations (the four that govern everything)

1. **One action hue, used surgically.** The accent — Carbon Blue 60 `#0F62FE`
   (the Studio design system's one action color) — is the "this is Orama /
   you are here" signal: the active-view checkmark, the selected graph-node
   border, the tagline in the About dialog. **Never** for status, never
   on hover, at most a few per screen. If a second thing needs emphasis, use
   charcoal/ink, not the accent.
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
`src/gui/app_internal.hpp`, alongside the **canvas palette** — the `ImU32`
draw-list forms of the same tokens (`kCanvasBox*`, gauge track/spark, graph
edge colours) shared by every spatial canvas (graph workspace, cluster node
boxes, dataset placement, the search cluster strip). Panels define **no**
colour constants of their own; a new colour joins one of those two tables
first.

| Role | Token | Hex | In code |
|------|-------|-----|---------|
| Brand accent ("you are here") | accent (Carbon Blue 60) | `#0F62FE` | `kAccent` |
| Accent hover | accent-hover (Blue 70) | `#0043CE` | `kAccentHover` |
| Accent wash (selected fill) | accent-wash (Blue 10) | `#EDF5FF` | `kAccentWash` |
| Product text | ink-primary | `#1C1C1A` | `kInkPrimary` |
| Body / secondary | ink-secondary | `#444441` | `kInkSecondary` |
| Section labels / subtitles | ink-muted | `#5F5F5C` | `kInkMuted` |
| Counts / placeholders | ink-quiet | `#7D7D7A` | `kInkQuiet` |
| Working surface | canvas | `#FAFAF8` | `kCanvas` (+ SDL clear) |
| Nav / menubar surface | sidebar | `#F5F4F2` | `kSidebar` |
| Cards / raised | white | `#FFFFFF` | `kWhite` (`ChildBg`) |
| Hairline border | border | `#E0E0E0` | `kBorder` |

**Semantic state palette — for status, never the accent:**

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
  "Verification criteria" — never Title Case. Proper nouns (Orama Studio, AWS, SQL,
  ImGui) keep their casing.
- **No em-dashes in product copy.** Use a colon, comma, or parentheses; use the
  middle dot `·` as the separator in metadata strips (`orama zetta · running ·
  up 2m`) and as the empty-cell placeholder (never `—`). (Em-dashes also
  historically rendered as tofu under the old font; the glyph ranges now cover
  them, but the rule still stands.)
- **Product language, never engine-speak (#635).** Kind/status enums render
  through `label_of(...)` in `src/gui/labels.hpp` (the one mapping site, the
  `tone_of` pattern); free-text kind tokens through `humanize_kind`. SQL and
  the `system.*` views keep the raw wire values — only the rendering changes.
  A raw wire value may appear in a labelled mono metadata line, never as
  primary text. Where a concept must stay technical, `term_hint("…")` names
  what it means on hover. The full rules + per-view sign-off:
  [copy-audit.md](copy-audit.md).
- Eyebrow labels are rendered by the `eyebrow(mono_font_, "UPPERCASE LABEL")`
  helper.

## Component grammar (as built)

- **Theme.** `apply_orama_theme()` paints the ImGui style: `StyleColorsLight`
  base, then the tokens above — ink text on canvas, white card child-panes,
  hairline borders, radius-8 corners, soft-wash selection (never an accent/charcoal
  fill), accent checkmark/nav-highlight.
- **Buttons.** Ghost by default (faint wash + hairline border, ink text). A true
  primary "New …" action is **charcoal-filled, white text** (push `ImGuiCol_Button
  = kInkPrimary` + `Text = kWhite`); see `primary_button` / `empty_state` in
  `app_internal.hpp`. The accent is never a button
  fill (there is no "send" here).
- **Selection.** Soft black wash (`ImGuiCol_Header`), never accent or charcoal
  fill.
- **Status dots + labels + pills.** `status_dot(tone)` draws the design
  system's small indicator dot inline (green = active/open, amber =
  locked/attention); `status_label(tone, text)` is the row form (dot + word)
  and `status_pill(tone, text)` the header form (uppercase mono word on the
  tone's tinted pill). Tones come only from `tone_of(...)` in
  `src/gui/tones.hpp` (see [status-tones.md](status-tones.md)) — meaning never
  rides on colour alone, the label says it too.
- **Metadata strips.** Mono, `·`-separated, tabular counts, no debug noise — see
  `draw_dashboard` (the always-visible node strip) and the project rows on Home.
- **Callouts.** `callout(tone, id, fn)` — a tinted, bordered child box for a
  deliberate boundary; see the Cloud inspector / Add-nodes consent gates
  (amber wash when locked, green when granted). Never hand-rolled.
- **Tables.** Data tables use the shared `kDataTableFlags` (hairline borders,
  proportional stretch, **zebra off** — hierarchy comes from type, not
  stripes; OR in `ScrollY` per site as needed). Headers are sentence case;
  numeric columns render right-aligned mono via `table_numeric(value)`. Row
  height stays the theme default — no per-table padding.
- **Cards.** Child panes are white on canvas with a hairline border (the list /
  detail panes throughout).
- **Master-detail.** A list+detail panel renders through the
  [master-detail scaffold](master-detail.md) (`master_detail` in
  `app_internal.hpp`, #634): one resizable split, identical rows (status dot +
  title + right-aligned mono meta), one selection model — never a hand-rolled
  split.

## Self-check before adding GUI

- Sentence case; no Title Case, no em-dashes in copy.
- Numbers/IDs/paths/labels in Geist Mono.
- The accent only for "you are here" / one focal action — never status, never hover,
  at most a few per screen.
- Status uses the semantic green/amber/red, with a dot **and** a word.
- Flat: hairline borders + washes, no gradients, no decorative shadows.
- New colours come from the token table above, not improvised.

## Related

- [build.md](../build.md) — how the Geist fonts are embedded.
- [cloud-inspector.md](../cloud/cloud-inspector.md) — a worked example of the grammar
  (dots, eyebrows, mono code, the consent callout).
