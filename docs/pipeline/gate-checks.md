# Graph correctness checks (schema/type · data-leakage · point-in-time)

The first **concrete** correctness checks that plug into the
[correctness gate](gate.md) (`#309`, an atomic slice of FEAT-3). They implement
the gate's `Check` interface (`include/zetta/pipeline/graph_checks.hpp`,
`src/core/pipeline/graph_checks.cpp`) and turn a **mistyped or leaky** execution graph into
a clear-reason gate **failure** that blocks deploy (RULE-1), while letting a clean
graph through.

The reusable annotation checks and the M-3-specific check are:

| Check | Name | What it catches |
| ----- | ---- | --------------- |
| `SchemaTypeCheck`  | `schema_type`  | a step that requires a field no upstream source provides, or provides under a different type |
| `DataLeakageCheck` | `data_leakage` | an ML step whose feature uses data dated **after** its as-of cutoff (point-in-time violation), or that **is / derives from the label** (target leakage) |
| `SegmentationCheck` | `segmentation` | entity/outcome features, non-global preprocessing, an incorrect K criterion or minimum share, malformed parameters and disconnected assignment/profile outputs |

All are registered in [`default_gate()`](gate.md) (after `NonEmptyGraphCheck`),
so every gate run applies them. They are **opt-in per step**: a step that carries
none of the relevant annotations is skipped and passes, so adding them never
penalises a graph that doesn't use them.

`SegmentationCheck` is mandatory for M-3 model steps, even with no other annotations.
It validates the nested `segmentation` config against `segmentation_config_schema()`
and requires matching preparation/model contracts. Its implementation and refusal
tests live in `src/core/ml/segmentation_graph.cpp` and
`tests/ml/test_segmentation_methodology.cpp`. See the full
[segmentation correctness rules](../ml/segmentation.md#running-it).

## How a check reads a step

The default gate also registers `AnomalyGraphCheck` (`anomaly_correctness`,
`src/core/ml/anomaly_plan.cpp`, #1046). It engages on M-7 model kinds and validates
the complete executable shape. Missing or ambiguous alert policies, training on
evaluation labels, role/feature overlap, non-robust scaling, future-fitting KPI
settings, missing time roles, invalid bounds, grouping, and unsupported operations
or data flow are hard refusals. Each refusal has a regression in
`tests/ml/test_anomaly_methodology.cpp`; the same validator checks run overrides
before a Run is created. See [M-7 correctness rules](../ml/anomaly.md#correctness-rules-enforced-in-code).

A graph node's `attributes` bag (`zetta::ir::Node::attributes`,
[execution-graph.md](execution-graph.md)) is free-form JSON. These checks read a
small, **documented set of annotation fields** out of it. Everything else in the
bag (`source`, `table`, …) is ignored. The annotations are validated against a
JSON Schema **at the boundary** — see [below](#json-schema-at-the-boundary) — so a
malformed annotation fails fast with a precise reason instead of being silently
misread.

### `schema_type` — fields a step needs vs. the source profile

Two annotation fields drive it:

- **`schema`** — the fields a step **outputs** and their types: a *source profile*
  for a `Connect` step, the transformed shape for a `Clean` step. A map of field
  name → type token.
- **`requires`** — the fields a step **expects from its data-flow inputs**, and
  the type each must have. A map of field name → type token.

For every step that declares `requires`, the check gathers the union of the
`schema` maps of **all the step's transitive data-flow ancestors** (following
`DataFlow` edges back through the graph — intermediate steps that don't re-declare
a schema are transparent). Each required field must appear there with a matching
type:

- field absent from every ancestor profile → **fail** (`requires field "revenue"
  (float) but no upstream data source provides it`).
- field present but no provided type matches → **fail** (`requires field "revenue"
  as float but the upstream source provides it as string (type mismatch)`).

Type tokens are **opaque strings compared case-insensitively** (`Int` matches
`int `); the check does not interpret a type system, only equality of the names
the author writes.

### `data_leakage` — point-in-time and target leakage for ML steps

Engages on any step that declares a non-empty **`features`** array. Three fields:

- **`as_of`** — the point-in-time cutoff: every feature must be computed only from
  data available at or before this instant. ISO-8601 UTC date or date-time
  (`"2024-01-01"` or `"2024-01-01T00:00:00Z"`). **Required** when `features` is
  present — without a cutoff there is nothing to validate against, so a step that
  declares features but no `as_of` **fails**.
- **`label`** — the target column the model predicts (optional).
- **`features`** — each entry has a `name`, and optionally `data_as_of` (the
  latest instant of data the feature draws on), `derived_from`, and
  `leaks_target`.

A feature **fails** the check when:

- **point-in-time:** `data_as_of` is **after** `as_of` — the feature uses future
  data the model would not have at prediction time (`feature "spend_after" uses
  data as of 2024-02-01, after the as_of cutoff 2024-01-01 — point-in-time
  violation (data leakage)`). At the cutoff is allowed.
- **target leakage:** the feature **is** the label (`name` equals `label`), is
  **derived from** it (`derived_from` is the literal `"label"` or the `label`
  column name), or is explicitly flagged (`leaks_target: true`) — `feature
  "churn_proxy" is derived from the label ("churned") — target leakage`.

Date-only and date-time instants compare correctly (a date is treated as
midnight UTC), so a same-day afternoon feature leaks against a date-only cutoff.

## Example

A clean churn graph — the source declares its profile, the model declares the
typed fields it needs and features all dated before the cutoff:

```json
{
  "nodes": [
    { "id": "n-load", "kind": "connect", "name": "load_orders",
      "attributes": { "source": "postgres",
                      "schema": { "customer_id": "int", "revenue": "float",
                                  "signup_date": "timestamp" } } },
    { "id": "n-clean", "kind": "clean", "name": "dedupe" },
    { "id": "n-model", "kind": "model", "name": "churn_model",
      "attributes": {
        "as_of": "2024-01-01",
        "label": "churned",
        "requires": { "customer_id": "int", "revenue": "float" },
        "features": [
          { "name": "spend_2023", "data_as_of": "2023-12-31" },
          { "name": "tenure_days" }
        ] } }
  ],
  "edges": [
    { "from": "n-load",  "to": "n-clean",  "kind": "data_flow" },
    { "from": "n-clean", "to": "n-model",  "kind": "data_flow" }
  ]
}
```

Both checks pass and the gate allows deploy. Change `revenue` to `"string"` in the
source profile and `schema_type` fails; add a feature with `"data_as_of":
"2024-06-01"` and `data_leakage` fails — either blocks deploy with a reason.

```cpp
#include "zetta/pipeline/gate.hpp"
#include "zetta/ir/serialization.hpp"
using namespace zetta;

ir::ExecutionGraph graph = *ir::from_json(document);   // the compiled graph
ValidationReport report = default_gate().run(graph);   // ships both #309 checks
if (report.block_deploy) {
    // RULE-1: refuse to deploy; report.checks carries the per-check reasons.
}
```

## JSON Schema at the boundary

The annotation fields have a formal contract,
[`schemas/graph_step_annotations.schema.json`](../../schemas/graph_step_annotations.schema.json)
(JSON Schema draft-07). Each step's `attributes` bag is validated against it
**before** a check reads it (via `pboettch/json-schema-validator`, the same path
the [Run record](runs.md) uses); the runtime copy of the schema lives inline in
`src/core/pipeline/graph_checks.cpp` — **keep the two in sync**. Only the named fields are
constrained, so any other attribute is allowed; but a present field that is the
wrong shape (`requires` as an array, a `features` entry without a `name`, an
`as_of` that isn't an ISO-8601 instant) fails the check with the validator's
reason (`step "model" (n-model): malformed correctness annotations — /requires:
…`). This is the issue's *"a mistyped graph fails with a clear reason"* applied to
the annotations themselves.

## Where it fits

These checks sit on the `Check` interface the [gate](gate.md) defines, alongside the
trivial `NonEmptyGraphCheck`. The gate aggregates their `CheckResult`s into a
[`ValidationReport`](validation.md) and sets `block_deploy` — there is **no new
result format** here. Entity-resolution checks (`#312`) are the next concrete
checks to land on the same interface.

See also [gate.md](gate.md) (the runner), [execution-graph.md](execution-graph.md)
(the graph + the `attributes` bag), and [validation.md](validation.md) (the report
type), and the related artifacts: ENT-5, FEAT-3 (`#134` / `#301`), RULE-1.
