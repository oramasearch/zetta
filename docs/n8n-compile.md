# Compiling an n8n workflow into the ExecutionGraph IR

zetta can **compile** an imported [n8n](https://n8n.io) workflow JSON into the
[ExecutionGraph IR](execution-graph.md) — the first concrete *compiler* of FEAT-2
(issue #308). Where [n8n import](n8n-import.md) only **stores** a workflow
byte-for-byte as an artifact [`ProjectInput`](project-inputs.md) (validating its
shape but never interpreting it), the compiler turns those bytes into a graph the
[gate](gate.md), [deploy](deployments.md), and [run](execution.md) all build on:

- each n8n **node** becomes a typed IR **step** (a `zetta::ir::Node` tagged with a
  `StepKind` from the `connect→clean→model→index→serve→act` taxonomy), keyed by
  the n8n node's (unique) **name**;
- each n8n **connection** becomes a **DataFlow** `zetta::ir::Edge` between those
  steps.

R1 covers the node kinds in the **churn** workflows (the fixture
`tests/fixtures/churn_workflow.json`). The node-type → step-kind table
(`n8n_step_kind`) is the documented extension point as the use-case catalog grows.

## The API

`include/zetta/n8n_compile.hpp` (namespace `zetta`):

| Function | Returns | Behavior |
|---|---|---|
| `n8n_step_kind(n8n_type)` | `std::optional<ir::StepKind>` | Map an n8n node `type` (e.g. `"n8n-nodes-base.postgres"`) to its `StepKind`. `std::nullopt` for a type R1 doesn't compile yet. |
| `compile_n8n_workflow(content, error = nullptr)` | `std::optional<ir::ExecutionGraph>` | Validate the workflow JSON at the boundary, then compile it to an IR graph. |
| `compile_n8n_workflow(input, error = nullptr)` | `std::optional<ir::ExecutionGraph>` | Compile a stored n8n artifact `ProjectInput` by reading its payload bytes, then compiling as above. |

Like [`ir::from_json`](execution-graph.md), the compiler is **non-throwing**: on
malformed input it returns `std::nullopt` and, when `error` is non-null, sets it to
a human-readable, path-qualified reason — a bad workflow is rejected cleanly
instead of producing a half-built graph.

```cpp
auto project = node.create_project("churn");
auto imported = zetta::import_n8n_workflow_file(*project, "churn_workflow.json");

std::string error;
std::optional<zetta::ir::ExecutionGraph> graph =
    zetta::compile_n8n_workflow(*imported.input, &error);
if (!graph) {
    // error: "invalid JSON" / "not an n8n workflow: …" /
    //        'nodes[2]: unsupported n8n node type "…" (node "…")' / …
}
// graph->nodes() are typed steps; graph->edges() are the DataFlow edges.
```

## Node-type → step-kind mapping (R1)

`n8n_step_kind` is a static table keyed on the exact n8n `type` string. R1 covers
the kinds in Charles's churn workflows plus their obvious neighbours:

| n8n `type` | `StepKind` | Why |
|---|---|---|
| `scheduleTrigger`, `cron`, `manualTrigger`, `webhook` | **Connect** | triggers kick the pipeline off (its data entry point) |
| `postgres` | **Connect** | pulls data in from a source |
| `code`, `function`, `functionItem`, `set` | **Clean** | reshape / compute features / normalize |
| `httpRequest` | **Model** | in the churn use case this POSTs to the scoring model endpoint |
| `slack`, `emailSend` | **Act** | notify / write back to the world |

(All names are the `n8n-nodes-base.<name>` form.)

> **Why `httpRequest` → Model?** The taxonomy's own description lists "API" under
> *Connect*, but in the churn workflow the `httpRequest` node is the **model
> scoring** call (`POST https://models.internal/churn/score`). Compiling it to
> *Model* keeps the pipeline's meaningful **Connect → Clean → Model → Act** spine
> rather than flattening the model stage into a second source. The table is the
> per-use-case extension point: as the catalog grows, a use case that fetches data
> over HTTP would map its own node, and a richer classifier (by parameters, not
> just `type`) can replace the table without touching the compiler.

An **unmapped** node type is a hard miss: the compiler **fails** with
`nodes[i]: unsupported n8n node type "…" (node "…")` rather than guessing a wrong
stage. Extending coverage = adding a row to the table.

## Boundary validation

The workflow JSON enters the compiler through the **same schema** the importer
uses — [`validate_n8n_workflow`](n8n-import.md#boundary-validation-the-schema)
(`n8n_workflow_schema()`, draft-07, via
[pboettch/json-schema-validator](https://github.com/pboettch/json-schema-validator)).
So `compile_n8n_workflow` rejects:

1. **Invalid JSON** → `"invalid JSON"`.
2. **Valid JSON that isn't an n8n workflow** → `"not an n8n workflow: <where>: <why>"`.

What the schema **can't** express — graph-semantic, cross-reference rules — is
checked in code, exactly as `ir::from_json` validates IR structure:

- a node **name** must be non-empty and **unique** (it keys the graph) →
  `nodes[i]: duplicate node name "…"`;
- every connection must reference an **existing** node →
  `connections["A"]…: connection to unknown node "…"`;
- a malformed connection entry is rejected with its path, e.g.
  `connections["A"]["main"][0][1] needs a string "node"`.

The n8n connection map's shape is, per source node:

```jsonc
"connections": {
  "<source node name>": {
    "main": [               // outer array = output ports
      [ { "node": "<target name>", "type": "main", "index": 0 }, … ]
    ]
  }
}
```

The compiler walks it defensively (an unwired `null` port is skipped) and emits a
`DataFlow` edge `source → target` for each entry. Identical duplicate connections
are deduped (the IR holds no duplicate edges).

## Node identity + provenance

n8n connections reference nodes by **name** (names are unique within a workflow),
so the compiler uses the **n8n node name as the IR `NodeId`** — edge resolution is
then exact. Each compiled step records its origin in the node's `attributes` bag,
namespaced under `n8n` so it never collides with gate/annotation keys:

```jsonc
{ "n8n": { "type": "n8n-nodes-base.httpRequest", "id": "<n8n node id, if present>" } }
```

## The churn fixture, compiled

`tests/fixtures/churn_workflow.json` (a schedule → Postgres → code → HTTP-score →
Slack pipeline) compiles to:

```
Daily schedule ─▶ Read active accounts ─▶ Compute churn features ─▶ Score churn risk ─▶ Alert on high risk
  (connect)            (connect)               (clean)                  (model)              (act)
```

— five typed steps and four DataFlow edges, a well-formed `ExecutionGraph` (it
round-trips through the IR's own [`to_json`/`from_json`](execution-graph.md) trust
boundary unchanged).

## Tests

`tests/test_n8n_compile.cpp`:

- the churn fixture compiles into a **well-formed** graph with the right step
  kinds and edges (and survives an `ir::to_json` → `ir::from_json` round-trip);
- `n8n_step_kind` maps the known types and **misses** an unknown one;
- compile **end to end from a stored artifact input** (import → compile);
- **invalid JSON**, **non-workflow JSON**, an **unsupported node type**, a
  **connection to an unknown node**, and a **duplicate node name** are each
  rejected with a clear reason.

## Where this fits

```
n8n .json ──import──▶ ProjectInput (raw bytes, schema-validated)   ← n8n-import.md (#310)
                            │
                            ▼
              compile_n8n_workflow ──▶ ir::ExecutionGraph          ← this page (#308)
                            │
                            ▼
                  gate ▸ deploy ▸ run                              ← gate.md / deployments.md / execution.md
```

See [n8n-import.md](n8n-import.md) for the import (storage) half and
[execution-graph.md](execution-graph.md) for the IR this produces.
