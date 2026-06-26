# Correctness gate

The **correctness gate** (`zetta::CorrectnessGate`, `include/zetta/gate.hpp`) is
the **runner** for the FEAT-3 correctness gate: register a set of checks, run
them all over an `ExecutionGraph`, aggregate their outcomes into a
`ValidationReport`, and **block deploy when any check fails**. It enforces
**RULE-1** — *no deploy without a passing gate*.

It is the piece that joins the two data-model slices that came before it: the
[ExecutionGraph IR](execution-graph.md) it inspects (`zetta::ir::ExecutionGraph`)
and the [ValidationReport](validation.md) it produces (`zetta::ValidationReport`).
The IR is *what* it validates; the report is *what it returns*; this is the part
that does the running.

The gate ships the trivial `NonEmptyGraphCheck` so the path works end to end,
plus the concrete schema/type and data-leakage / point-in-time checks (`#309` —
see [gate-checks.md](gate-checks.md)) and the
[`EntityResolutionCheck`](entity-resolution-check.md) (`#312` / TC-4). They all
implement the same `Check` seam and plug in without touching the gate;
`default_gate()` registers each.

## The shape

```
Check (interface)
  name()                       std::string                    stable identifier of the check
  evaluate(graph)              CheckResult                     run it over a graph → one result

NonEmptyGraphCheck : Check     the trivial check this slice ships
make_check(name, callable)     → shared_ptr<Check>            wrap a callable as a Check (no class needed)

CorrectnessGate
  add_check(shared_ptr<Check>) CorrectnessGate&               register a check (chainable; runs in order)
  run(graph)                   ValidationReport               run all checks, aggregate, set block_deploy
  checks() / check_count()                                    inspect what's registered

default_gate()                 → CorrectnessGate              a gate pre-loaded with the shipped check
```

### `Check` — the pluggable unit

A `Check` is one named test of a graph. `name()` is its stable identifier
(recorded as `CheckResult::name`, queryable before it runs so a gate can list
what it will run); `evaluate(graph)` runs it and returns a single `CheckResult`
(status + a human-readable **reason** — *"failures surface a reason, not a stack
trace"*). A check is a pure function of the graph it is handed, though it may
carry its own configuration.

Concrete checks are usually full classes (they have config). For a check that
needs no state — and for tests — `make_check(name, callable)` wraps a callable as
a `Check` without declaring a class. The wrapper **stamps the registered `name`
onto the returned `CheckResult`**, so `Check::name()` and the result's name can
never drift.

### `NonEmptyGraphCheck` — the trivial check

The check this slice ships: a graph with no steps has nothing to deploy, so an
**empty graph fails** (`"execution graph has no steps; nothing to deploy"`) and a
**non-empty graph passes**. It is deliberately tiny — its job is to exercise both
the passing and the deploy-blocking paths, not to validate anything real.

### `CorrectnessGate` — the runner

`add_check` registers checks (chainable; null checks are ignored); they run in
**registration order**. `run(graph)` evaluates every registered check, collects
the results into a `ValidationReport` in order, and sets the report's
`block_deploy` flag.

**Deploy policy (the baseline):** the gate blocks deploy **exactly when some
check fails** — `block_deploy = !report.passed()`. A `verdict()` of `fail` blocks;
`warn` and `pass` allow the deploy. This is RULE-1 in code.

`verdict` and `block_deploy` stay [separate in the report on purpose](validation.md#verdict-and-block-deploy-are-separate-on-purpose):
the data model carries the bit, the gate sets the policy. This gate implements
the baseline *block-on-failure* policy. A stricter gate (block on a warning) or a
signed-off override (allow a failure through) is a future policy that sets
`block_deploy` differently over the same report — the report shape already
supports it.

`default_gate()` returns a gate pre-loaded with the baseline checks callers get
for free — `NonEmptyGraphCheck`, then the concrete `#309` checks (`SchemaTypeCheck`,
`DataLeakageCheck` — see [gate-checks.md](gate-checks.md)), then the
[`EntityResolutionCheck`](entity-resolution-check.md) (`#312` / TC-4). The `#309`
and entity-resolution checks are opt-in per step, so a graph that carries none of
their annotations still passes. As further concrete checks land they are
registered there too.

## Using it

```cpp
#include "zetta/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
using namespace zetta;

// Build (or receive) a graph, then run it through the gate.
ir::ExecutionGraph graph = /* … the compiled execution graph … */;

CorrectnessGate gate = default_gate();          // ships NonEmptyGraphCheck
gate.add_check(std::make_shared<MyLeakageCheck>());   // concrete checks plug in
gate.add_check(make_check("has_serve_step", [](const ir::ExecutionGraph& g) {
    return g.nodes_of_kind(ir::StepKind::Serve).empty()
        ? CheckResult{"", CheckStatus::Warn, "no serve step — nothing to expose"}
        : CheckResult{"", CheckStatus::Pass, "serves results"};
}));

ValidationReport report = gate.run(graph);
if (report.block_deploy) {
    // RULE-1: refuse to deploy; report.checks carries the per-check reasons.
}
```

## JSON

The gate introduces **no new JSON format** — it *produces* a `ValidationReport`,
which already round-trips through JSON and is validated at the boundary by
`validation_report_from_json()` against
[`schemas/validation_report.schema.json`](../schemas/validation_report.schema.json).
So a gate result serializes and re-parses with the existing, schema-checked path;
there is nothing new to validate here.

## GUI panel

In headed mode the **Correctness gate** panel (SCR-4 / CMP-2, realizing FEAT-3 in
the UI) runs the gate over a graph and shows the `ValidationReport` — a thin
renderer over `default_gate().run(graph)`, no SDL/ImGui types leaking into
`zetta-core` (`src/gui/app.cpp` → `App::draw_gate`). It lets you:

- **pick a graph source** — a *Passing example* preset (a small
  connect→clean→model→serve pipeline) or an *Empty (fails)* preset to demonstrate
  the deploy-blocking path, **load a stored graph** from the selected project's
  `graph_names()` (the real `ProjectGraph` data path), or edit the IR's
  [canonical JSON form](execution-graph.md) directly in the text box;
- **run the gate** (the *Run gate* button) — the JSON enters through
  `ir::from_json`, which validates it against the IR schema **at the boundary**;
  malformed JSON or an invalid graph is rejected inline with a **reason** (e.g.
  `invalid graph: nodes[2]: unknown step kind "frobnicate"`), never a thrown
  stack trace;
- **read the result** — a headline **block-on-fail banner** (red *"Deploy
  blocked — N check(s) failed"* vs. green *"Deploy allowed — gate passed"*,
  derived from `report.block_deploy`), then a per-check table of **Check /
  Status / Reason**, the status shown as a colour-coded `PASS` / `WARN` / `FAIL`
  badge and the reason as the human-readable text the `CheckResult` carries.

The panel holds no gate state of its own — it re-resolves the selected project by
name each frame (so a dropped/renamed project never leaves a dangling handle) and
keeps only the editable graph JSON, the last inline error, and the last report.
This is the **gate's UI**, not deploy wiring: it shows the block-on-fail state but
does not itself deploy (`deploy_graph` runs separately; wiring the gate into it is
later work).

## Where it fits

The gate sits between **compile** and **deploy**: a graph is compiled from intent,
run through the gate, and deployed only if the report allows it. The events the
rest of the system carries (`GraphValidated` / `ValidationFailed`, the
`ValidateGraph` command) wrap the `ValidationReport` this runner produces. The
`#309` schema/type and data-leakage / point-in-time checks
([gate-checks.md](gate-checks.md)) and the
[entity-resolution check](entity-resolution-check.md) (`#312`) already plug into
this seam; the gate's wiring into deploy lands on top of it as separate work.

See also [gate-checks.md](gate-checks.md) (the concrete checks),
[validation.md](validation.md) (the result type), the
[execution-graph.md](execution-graph.md) (the graph it inspects), and the related
artifacts: ENT-5, FEAT-3 (`#301` / `#134`), RULE-1.
