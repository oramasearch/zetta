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

This slice ships **one trivial check** so the path works end to end. The concrete
correctness checks — schema/type, data-leakage and point-in-time (`#309`),
entity-resolution (`#312`) — implement the same `Check` seam and plug in
separately, without touching the gate.

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

`default_gate()` returns a gate pre-loaded with `NonEmptyGraphCheck` — the
baseline gate callers get for free. As concrete checks land, they are registered
there too.

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

## Where it fits

The gate sits between **compile** and **deploy**: a graph is compiled from intent,
run through the gate, and deployed only if the report allows it. The events the
rest of the system carries (`GraphValidated` / `ValidationFailed`, the
`ValidateGraph` command) wrap the `ValidationReport` this runner produces. The
concrete checks (`#309`, `#312`) and the gate's wiring into deploy land on top of
this seam as separate work.

See also [validation.md](validation.md) (the result type), the
[execution-graph.md](execution-graph.md) (the graph it inspects), and the related
artifacts: ENT-5, FEAT-3 (`#301` / `#134`), RULE-1.
