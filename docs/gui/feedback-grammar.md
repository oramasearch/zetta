# Feedback grammar — one language for action outcomes (#632)

Every mutating affordance in the headed app reports its outcome in exactly one
of three forms, so a user can always tell whether the thing they did worked —
in the panel where they did it. No modal alert boxes; no silent failures; no
generic "operation failed". Builds on the `banner` widget shipped in
[#655](https://github.com/oramasearch/zetta/pull/655) and the status tones
(#636); this page records the sweep that converted every panel to it (#632).

## The three forms

1. **Inline field validation** — problems with what the user typed ("name
   required", "metrics is not valid JSON") surface next to the form, as the
   panel's error banner, when the action is attempted. They read as `Tone::Error`
   and clear on the next successful action.
2. **The per-panel banner** — `banner(tone, message)` in `app_internal.hpp`: a
   tone-tinted strip (status dot + wrapped text) carrying the outcome of the
   last action taken in that panel. Errors carry the engine's path-qualified
   reason **verbatim** — never a paraphrase, never "operation failed".
   Successes state what happened ("Deployed \"churn\" to local (deployment
   1a2b3c4d)"). The widget renders nothing on an empty message, so panels pass
   their outcome member unconditionally. The child id is scoped by the message,
   so a panel may show more than one banner (a form error above a save result)
   without an ImGui id collision.
3. **The job pattern for long operations (#624)** — work that outlives the
   frame runs off the UI thread and reports progress from its live source
   (progress bar / `system.*` view) while in flight, then lands its completion
   outcome as the panel's banner. The Add-nodes control is the model: a
   progress bar + narration while the provision coroutine runs, a banner when
   `poll_add_nodes()` applies the result. The in-app test suite follows the
   same shape (worker thread + live log + outcome banner).

## Panel conventions

- **Outcome state is a per-panel member** on `App` (presentation state, not
  engine state): an error string (`db_error_`, `graph_error_`, …) and, where a
  success is not self-evident in place, a success string (`project_status_`,
  `graph_status_`, `add_model_status_`). Booleans pick the tone at the render
  site (`deploy_failed_ ? Tone::Error : Tone::Ok`), matching the status-tones
  rule that booleans map at the call site.
- **The outcome surfaces where the action was taken.** A register-model
  success banners in the Models panel; a graph save banners in the workspace;
  a create-project banners in the Explorer's Projects column (and navigates
  there, the Added-dataset precedent) — never only on some other view's status
  line. The shared file-ingestion line (`io_status_` + `io_status_error_`)
  renders on Home and Datasets, toned by whether the ingest failed.
- **No silent failures.** A mutating call whose result the panel would
  otherwise ignore (`drop_project`, `drop_table`, `drop_graph`,
  `LargeObjectStore::unlink`, …) checks the returned status and surfaces a
  failure into the panel's error member. The engine's boundary types carry
  reasons where they exist; a bare `bool` failure gets a plain, specific
  sentence ("could not drop table 'x'").
- **A new outcome replaces the old one** — set sites clear the counterpart
  member (an error clears the stale success and vice versa), and loading a
  different entity clears both (`open_graph` clears `graph_status_`).

## Deliberate non-banner surfaces

These are structured displays, not stray status lines — leave them:

- **Report renderings** — the cloud-inspect report's per-command outcomes and
  the gate report's verdict pill + per-check table are the result the panel
  exists to show, drawn with `status_label` / `status_pill` / tone colors.
- **The console transcript** — a statement's error renders inline in the
  scrollback (the console's own feedback surface, CMP-6).
- **The Add-nodes CLI failure box** — a provider failure runs to many lines,
  so it stays a wrapped, scrollable child with a one-click Copy (documented in
  [add-nodes-control.md](add-nodes-control.md)); the success path banners.

## Boundaries

No new JSON/wire format; presentation-only members on `App`. Issue refs:
[#632](https://github.com/oramasearch/zetta/issues/632) (this sweep),
#655 (the widget), #636 (tones), #624 (jobs), #627 (the experience epic).
