# Entity-resolution correctness eval

The **entity-resolution check** (`zetta::EntityResolutionCheck`,
`include/zetta/usecases/churn/entity_resolution_check.hpp`) is the first concrete
correctness check to plug into the [correctness gate](../pipeline/gate.md). It is the
**make-or-break** eval of FEAT-3 and realizes test case **TC-4**: it verifies
that an execution graph resolved entities *division-correct* — that **"FedEx
Freight" lands on its own CRM account without conflating the 5–6 sibling FedEx
subsidiaries** (FedEx Ground, Express, Office, Logistics, …), which are separate
accounts. Conflating them is the canonical failure this check exists to catch,
and a conflation **fails the gate and blocks deploy** (RULE-1).

It is a plain `Check` (the gate's pluggable seam): it inspects an
`ir::ExecutionGraph` and returns one `CheckResult`. It touches nothing in the
gate itself and ships registered in [`default_gate()`](../pipeline/gate.md), right after the
trivial `NonEmptyGraphCheck`.

## What it reads

The check looks at every graph node whose `attributes` bag carries an
`entity_resolution` key. That payload is the resolution result for that step:

```jsonc
{
  "entity_resolution": {
    "records": [
      { "mention": "FedEx Freight",   "entity": "fedex_freight",   "resolved": "acct_fedex_freight" },
      { "mention": "FedEx Ground",    "entity": "fedex_ground",    "resolved": "acct_fedex_ground" },
      { "mention": "FedEx Express",   "entity": "fedex_express",   "resolved": "acct_fedex_express" },
      { "mention": "FedEx Office",    "entity": "fedex_office",    "resolved": "acct_fedex_office" },
      { "mention": "FedEx Logistics", "entity": "fedex_logistics", "resolved": "acct_fedex_logistics" }
    ]
  }
}
```

Per record:

- **`mention`** — the raw string the resolver saw (e.g. `"FedEx Freight"`).
  Provenance/display only; optional.
- **`entity`** — the **ground-truth** canonical division the mention truly
  belongs to (the fixture label). Required, non-empty.
- **`resolved`** — the account the resolution step **actually assigned** the
  mention to. Required, non-empty.

A node with no `entity_resolution` attribute is not a resolution step and is
ignored; a graph with **no** resolution step passes the check vacuously (nothing
to conflate).

## The correctness rule

The check compares the partition of records **by `resolved`** against their
partition **by `entity`**. They must agree:

| Condition | Meaning | Outcome |
| --- | --- | --- |
| One `resolved` account holds **two or more distinct `entity`s** | **Conflation** (over-merge) — distinct divisions collapsed into one account. The make-or-break error. | **Fail** → blocks deploy |
| One `entity` is split across **two or more `resolved` accounts** | **Fragmentation** (under-merge) — one division scattered across accounts. The softer, symmetric error. | **Warn** → does *not* block deploy |
| Every `resolved` ↔ `entity` is one-to-one | Division-correct resolution. | **Pass** |

When a step shows both errors, conflation wins (worst-status aggregation). Across
several resolution steps, the check returns the worst status of any step. The
**reason** string names the offending account(s) and the entities that were
merged (or the entity and the accounts it was split across), so the failure is
actionable — *a reason, not a stack trace*.

## Schema validation at the boundary

The `entity_resolution` payload is **schema-validated before it is evaluated**.
The check carries an embedded draft-07 schema and runs every payload through a
`pboettch/json-schema-validator` validator (the same library and
error-handler-not-exceptions pattern as [`src/core/pipeline/run.cpp`](../pipeline/runs.md)); a payload
that violates the schema (missing `entity`/`resolved`, an empty string, an
unknown property) **fails the check** with the validation error in the reason,
rather than being silently mis-evaluated. The schema is published at
[`schemas/entity_resolution.schema.json`](../../schemas/entity_resolution.schema.json)
and mirrored by the embedded copy in
`src/core/usecases/churn/entity_resolution_check.cpp` — **keep the two in sync**.

## Using it

It runs automatically as part of the default gate:

```cpp
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/execution_graph.hpp"
using namespace zetta;

CorrectnessGate gate = default_gate();   // ships NonEmptyGraphCheck + EntityResolutionCheck
ValidationReport report = gate.run(graph);
if (report.block_deploy) {
    // RULE-1: a conflated resolution refuses to deploy; report.checks carries the reason.
}
```

To run it on its own:

```cpp
#include "zetta/usecases/churn/entity_resolution_check.hpp"
CheckResult result = EntityResolutionCheck{}.evaluate(graph);
```

## Where it fits

This is one concrete check on the gate's `Check` seam; the schema/type,
data-leakage and point-in-time checks (`#309`) plug in the same way. The
**producer** of the payload it reads is the entity resolver
([entity-resolution.md](entity-resolution.md), `zetta::EntityResolver`, #394) —
resolver produces, this gate verifies. See [gate.md](../pipeline/gate.md) (the runner that
hosts it), [validation.md](../pipeline/validation.md) (the `CheckResult` / `ValidationReport`
it produces), and [execution-graph.md](../pipeline/execution-graph.md) (the IR it inspects).
Related artifacts: FEAT-3 (`#134`), TC-4 (`#251`), RULE-1.
