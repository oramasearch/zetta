# Governed Console / REPL screen (SCR-8)

The headed **Console/REPL screen** — the **governed** front door for asking a node
an ad-hoc question, in SQL or simulated-Python. Screen **SCR-8** (#525), built on
the [REPL console component](repl-console.md) (CMP-6, #524): the component renders
the surface (frontend selector, editor, transcript, result grid); this screen
supplies a **governed run handler** so every statement is **capability-scoped**,
the simulated-Python frontend **lowers to the ExecutionGraph IR and passes the
[correctness gate](gate.md)** before it runs, and **every statement is recorded in
the [audit trail](audit-trail.md)** (FEAT-9). It realizes **UC-3** — an ad-hoc
question, governed.

Like the rest of the console it is split **model-in-`zetta-core` /
GUI-as-thin-renderer**: the governance pipeline is
**`zetta::repl::GovernedConsole`** (`include/zetta/repl/governed_console.hpp`,
`src/core/repl/governed_console.cpp`) — **no SDL/ImGui**, so it runs headless (a
future CLI REPL, `tests/test_governed_console.cpp`) and under the GUI screen
(`App::draw_console`, `src/gui/app_explorer.cpp`), which only sets the session
inputs (the acting key + current database) and draws the **"Run as"** selector.

## The pipeline

`GovernedConsole::run(frontend, source)` matches `repl::RunHandler`, so it slots
straight into `ConsoleModel::submit`. Each statement runs the same chain:

1. **Capability check.** When a session key is set, the statement is authorized
   against it (ENT-22): the verb/right the frontend needs
   (`repl::required_operation`) — `api:query` for SQL, `orchestration:start_run`
   for simulated-Python — must be a matching, unexpired capability on the key. A
   key that doesn't exist, or doesn't hold the right, is **denied** — the
   statement runs no engine work. An **empty** session key is an *unauthenticated
   local session*: it runs without an enforcement check (so the basic "see
   results" path is frictionless) but is still audited.
2. **Lower + gate** *(simulated-Python only)*. The source lowers to the
   [ExecutionGraph IR](execution-graph.md) through
   [`input::PythonFrontend`](python-frontend.md). The lowered graph runs through
   [`default_gate()`](gate.md); a **failing gate blocks the run** (RULE-1) — no
   execution, the failing check's reason is surfaced inline. SQL does **not** lower
   to the IR — the [query layer](query.md) is its own validated executor — so SQL
   is governed by the capability check + audit, not the gate.
3. **Execute.** SQL runs through the [query service](query.md); simulated-Python
   executes the gated graph via `Node::run_graph`, producing a real, persisted
   [Run](runs.md) (so an ad-hoc Python statement shows up in the Runs panel).
4. **Audit.** Exactly one **`data_access`** [audit event](audit-trail.md) is
   recorded per statement — the acting key (actor), what it touched (target), the
   governance decision (`allowed`), and a one-line summary (detail). `allowed` is
   **false** when governance blocked the statement (capability denied or gate
   failed) and **true** when it was permitted to run (a permitted statement that
   then fails to parse/execute is still `allowed` — the failure is the author's,
   not a governance denial).

A governed run reuses the existing **`data_access`** audit action — the same seam
`Node::authorize` records on — rather than a new action, so the audit schema is
unchanged. It is queryable through `system.zetta_audit` and the
[Admin panel](admin-panel.md)'s audit log alongside every other governed event.

## Tabular results

Both frontends return the frontend-agnostic [`repl::Result`](repl-console.md):

- **SQL** maps the query service's `Outcome` to a column-aligned grid. A `SELECT *`
  returns rows as JSON objects with no explicit column list, so the header is
  derived from the union of row keys (first-seen order) — otherwise a `SELECT *`
  would render no grid.
- **Simulated-Python** presents the executed graph as a grid of its lowered IR
  steps (`step` / `kind` / `name`), with status messages summarizing the run
  (steps lowered, gate passed, the produced Run's id + status).

## The "Run as" selector

The screen adds one control to the component: **Run as**, a selector over the
node's access keys (read straight off `Node::principals()` → each principal's
keys, the same source as the Admin panel, re-read each frame). The default is
**`(unauthenticated local)`** (the empty key id). Selecting a key makes the session
**capability-scoped** to it: a key that holds the right runs, a key that lacks it
is blocked — and the block is audited, demonstrating the deny path. A selected key
that is later dropped falls back to the local session.

## No new JSON boundary

The screen introduces **no new stored or wire JSON format**: SQL enters through
`QueryService` (already validated at the query boundary), simulated-Python through
`ir::from_json` (the IR trust boundary) + the gate, and each audit event through
the existing AuditEvent JSON Schema at `Node::record_audit_event`. The transcript
is ephemeral session state.

## Tests

`tests/test_governed_console.cpp` (links `zetta-core`, no GUI) covers the pipeline
headless: a SQL run returns a grid + records one `data_access` event; a
simulated-Python statement lowers, passes the gate, executes (a Run is produced),
and is audited; a compile error is recorded inline (still audited, still allowed);
an injected always-fail gate blocks the run and audits the block as denied; a key
lacking the capability blocks SQL + simulated-Python (audited, denied), a key
holding it runs both (audited, allowed), and an unknown key is denied by default.
The GUI renderer is exercised by building the headed app (the project convention —
tests never link SDL/ImGui).

## Status / later slices

This screen wires governance around the existing component. The simulated-Python
execution runs the gated graph through `Node::run_graph` (the
`DefaultStepExecutor` orchestration); wiring it to a richer evaluator is #522.
Scope-axis (resource) checks and a first-class console capability vocabulary can
layer on later behind the same `required_operation` seam.

See also [repl-console.md](repl-console.md) (the component), [gate.md](gate.md)
(the correctness gate), [audit-trail.md](audit-trail.md) (the trail), and
[access-control.md](access-control.md) (key-scoped capabilities).
