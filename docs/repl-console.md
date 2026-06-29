# REPL console (CMP-6)

The Dear ImGui component behind the interactive **Console** — a SQL /
simulated-Python REPL over a node. Component **CMP-6** (#524), the building block
the governed **Console / REPL screen** (SCR-8, #525/#497) will host next.

It is split the same way the [test suite](test-suite.md) is — **the model lives
in `zetta-core`, the GUI is a thin renderer over it** — so the same REPL session
backs the headed component today and a **future CLI** with no rework:

- **`zetta::repl::ConsoleModel`** (`include/zetta/repl/console_model.hpp`,
  `src/core/repl/console_model.cpp`) — the engine-agnostic REPL *session*: the
  selected frontend, the input buffer, the scrollback transcript, and the submit
  logic. **No SDL/ImGui** — it lives in zetta-core, so it is headless-runnable
  (`tests/test_repl_console.cpp`) and a CLI can render it.
- **`zetta::gui::ReplConsole`** (`src/gui/repl_console.hpp` /
  `src/gui/repl_console.cpp`) — the Dear ImGui renderer: a frontend selector, a
  multiline editor, Run / Clear controls, the scrollback transcript, and the
  result grid. It owns no engine.
- **`App::draw_console`** (`src/gui/app_explorer.cpp`) — the headed **Data →
  Console** view that hosts the component over the live node, supplying the run
  handler.

## The model owns no engine — it emits run events

Running a statement is **not** the component's job. The console only collects
input and renders outcomes; the host supplies a **run handler** — the "run event"
the console emits:

```cpp
using RunHandler =
    std::function<repl::Result(repl::Frontend frontend, const std::string& source)>;
```

`ConsoleModel::submit(run)` invokes the handler with the selected frontend + the
current input, appends the `(frontend, source, result)` to the transcript, and
resets the input for the next statement. This keeps the component decoupled from
any single frontend: the App wires SQL to the [query service](query.md) and
simulated-Python to an honest seam; the governed screen (#525) will instead route
both through the frontend→IR lowering layer + the [correctness gate](gate.md) +
the [audit trail](audit-trail.md).

`submit` is a **no-op (returns `false`, records nothing)** when the input is
blank/whitespace-only or the handler is null — mirroring
`Project::capture_intent` rejecting empty intent. Both a successful and a failed
(error) result are recorded, so the transcript shows errors inline; only a
recorded run clears the prompt.

## The result is frontend-agnostic

```cpp
struct Result {
    std::vector<std::string> messages;              // status lines (one per statement)
    std::vector<std::string> columns;               // result-grid header
    std::vector<std::vector<std::string>> rows;     // result-grid cells, column-aligned
    std::string error;                              // non-empty => failed run (red, inline)
    std::string elapsed;                            // pre-formatted wall-clock (optional)
    bool ok() const { return error.empty(); }
};
```

One `Result` shape holds SQL rows and simulated-Python output alike, so one
transcript renders a mixed-frontend session. `columns` + `rows` drive the result
grid (an ImGui table in the GUI); a statement that returns no rows (DDL / INSERT)
sets only `messages`; a failed statement sets `error` and is rendered inline
instead of a grid.

## Frontends

`repl::Frontend` is `{ Sql, SimPython }`, listed in selector order by
`repl::frontends()`. Each is meant to lower to the same ExecutionGraph IR core
(ADR-33 / ARC-15); the selector only tells the run handler how to read the source.

- **SQL** is wired — the App's handler runs it through `query::QueryService` (the
  same Postgres subset the [SQL query](query.md) view uses) and maps the
  `Outcome` onto a `Result` (a JSON row becomes a column-aligned cell vector).
- **Simulated-Python** is an **honest seam**: it is selectable and *emits a run
  event*, but the frontend→IR lowering layer is not in this slice, so a run
  returns a clear "not wired yet" result inline rather than pretending. Real
  execution lands with the lowering layer (#522) and the governed screen (#525) —
  the same way the [churn dataset](churn-dataset.md)'s Parquet format is an honest
  seam until its dependency arrives.

## CLI reuse

Because `ConsoleModel` carries no UI dependency, a CLI REPL is a second thin
renderer over the same class: read a line into `model.input()`, call
`model.submit(run)` with a handler, and print the transcript. Two helpers make
that a one-liner and are the headless-testable form of the result grid:

- `repl::format_result(const Result&)` → a status + error + column-aligned text
  table for one result.
- `repl::format_transcript(const ConsoleModel&)` → the whole scrollback as text
  (`<frontend>> <source>` prompt lines + formatted results).

## No new JSON boundary

The component introduces **no stored or wire JSON format**: the transcript is
ephemeral in-memory session state, and SQL still enters through `QueryService`
(already parsed/validated at the query boundary). There is nothing new to
schema-validate here. The governed screen (#525) adds the IR-lowering, gate, and
audit boundaries — all of which validate at their own existing seams.

## Tests

`tests/test_repl_console.cpp` (links zetta-core, no GUI) covers the model: submit
records an entry + clears the prompt, the selected frontend rides into the handler
+ the entry, blank/whitespace input and a null handler are rejected, an error
result is recorded inline, `clear()` drops the transcript but keeps the frontend,
and the `format_result` / `format_transcript` text projections render the grid +
inline errors. The GUI renderer is exercised by building the headed app (the
project's convention — tests never link SDL/ImGui).

## Status / later slices

This slice is the **component** only — it renders, emits run events, and displays
results + errors inline (the #524 acceptance). The **governed Console/REPL
screen** (SCR-8, #525) — lowering every statement to the IR, running it through
the correctness gate, and recording it in the capability-scoped audit trail — is
the next slice, and reuses this component unchanged. Wiring the simulated-Python
frontend to a real evaluator is #522.
