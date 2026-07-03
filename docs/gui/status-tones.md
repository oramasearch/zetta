# Status tones — one status visual language (#636)

Run status, job lifecycle, node sync, gate verdicts, placement classes, test
outcomes, and cloud/consent states all render through **one semantic status
set**, so the same state looks the same in every panel and users can scan
state across views. Related: #627 (the widget grammar).

## The pieces

- **`Tone`** (`src/gui/app_internal.hpp`) — the closed set of visual states:
  `Ok` (green text `#0F6E56`) · `Active` (dot-green `#1D9E75`, live things) ·
  `Warn` (amber, in flight / needs attention) · `Error` (red) · `Muted`
  (ink-quiet, pending / neutral). `tone_color(Tone)` and `tone_background(Tone)`
  give the text colour and the tint; `tone_dot_color(Tone)` gives the dot
  variant (the design system draws the green state's dot brighter than its
  text; every other tone's dot matches its text).
- **`src/gui/tones.hpp`** — **the ONE mapping site** from an engine status to a
  tone. The overloads: `tone_of(RunStatus)`, `tone_of(JobStatus)`,
  `tone_of(ScalingAction)`, `tone_of(CheckStatus)`, `tone_of(TestOutcome)`,
  `tone_of(cluster::NodeStatus)` (node sync: self → Active, unknown → Muted,
  in sync → Ok, behind → Warn), `tone_of(StepPlacement)` (near-data → Active,
  coordinator → Warn, fan-out / all-nodes → Muted), plus
  `tone_of_log_level(level)` and `tone_of_utilization(percent)` (the shared
  gauge thresholds: ≥85 red, ≥60 amber, else green). `static_assert`s at the
  bottom lock the cross-lifecycle agreements (every Succeeded is Ok, every
  Failed is Error, …) at compile time.
- **Two widget forms** (`app_internal.hpp`):
  - `status_label(tone, text)` — the **row form**: dot + word, both from the
    tone. Lists, table cells, inline states.
  - `status_pill(tone, text)` — the **header form**: an uppercase mono word on
    the tone's tinted pill. A detail pane's headline state (a run's status
    beside its id, the gate verdict).
  - `status_dot(tone)` — the bare tone-routed dot, for rows whose following
    text is content rather than a status word (a cloud command line).

## The rules

- **Panels never pick status colours ad hoc.** A status renders via
  `tone_of(...)` + `status_label`/`status_pill`/`status_dot`/`tone_color` —
  never a direct `kSuccessColor`/`kWarnColor`/`kErrorColor` pick, never a
  panel-local enum→colour switch. A new status-bearing enum adds **one
  overload in `tones.hpp`** and nothing else.
- **Meaning never rides on colour alone** — the word says it too (the widget
  forms build this in).
- **Booleans pick a `Tone` at the call site** (`ok ? Tone::Ok : Tone::Error`);
  the tone names the semantics, so that is not an ad-hoc colour pick.
- **The Cluster view keeps its richer treatment** (eased gauge bars,
  sparklines, the pipeline-animation draw-list palette) but takes its colour
  breakpoints from `tone_of_utilization` and its node health colour from
  `tone_of(NodeStatus)`, so a "hot" gauge and a "failed" run speak the same
  language.

## Boundaries

No new JSON/wire format — this is a pure render-layer slice; the engine enums
it maps are unchanged. `tones.hpp` is GUI-internal (it includes
`app_internal.hpp`, which includes ImGui) and never appears under
`include/zetta`. Deliberately later: sweeping the remaining plain-ink
metadata strips onto `stat_tile`/`section_card` (tracked by the grammar work,
#630/#634).
