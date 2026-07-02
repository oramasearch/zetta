# Execution graph IR

The **ExecutionGraph IR** (`zetta::ir`) is the in-memory intermediate
representation of a data/AI system: a directed graph of typed *steps*. It is the
keystone the compilers, the gate, deploy, and run all build on — the artifact the
GUI will show, validate, and deploy (ENT-3 / FEAT-2).

You can build a graph in code, traverse it, and **serialize it to/from JSON** (and
persist it **under a Project** — see [projects.md](projects.md)). Still
deliberately out of scope here:

- **no *dual-form* serialization yet** — the JSON form below is the first
  serialized representation (it's what rides in the CRDT and lands on disk). The
  dual-form *code* (Lua / Orly) and the *columnar* (Parquet / CSV) projections
  from ADR-10 are a later slice.
- **no semantic validation** — acyclicity and legal stage transitions are the
  **gate's** job, also a later slice. (Serialization validates *structure* — see
  below — not these *semantics*.)

The model lives behind a few headers and translation units:

```
include/zetta/ir/step_kind.hpp        the step-kind taxonomy (enum + name helpers)
include/zetta/ir/execution_graph.hpp  Node, Edge, EdgeKind, ExecutionGraph
include/zetta/ir/serialization.hpp    to_json / from_json (the JSON form)
src/core/ir/execution_graph.cpp       the graph implementation
src/core/ir/serialization.cpp         the (de)serializer + boundary validation
```

## The step-kind taxonomy

Every node is tagged with a `StepKind` — the stage it belongs to, in canonical
authoring order:

```
connect → clean → model → index → serve → act
```

| Kind      | What it does                                                  |
| --------- | ------------------------------------------------------------- |
| `Connect` | pull data in from a source (database, file, API, stream)      |
| `Clean`   | normalize / dedupe / validate / reshape the raw data          |
| `Model`   | train, evaluate, or apply a model (ML / statistical / LLM)    |
| `Index`   | build a search / vector / feature index over the results      |
| `Serve`   | expose results behind a query / API / endpoint                |
| `Act`     | take an action on the world (notify, write back, trigger)     |

Declaration order **is** the pipeline order, so the enum's underlying values
increase along the pipeline (`Connect = 0` … `Act = 5`). A graph need not use
every kind, and the ordering between specific nodes is expressed by *edges* — the
enum only names the stages. `all_step_kinds()` walks the taxonomy in order;
`to_string` / `step_kind_from_name` map to and from the lowercase names
(`"connect"`, …) used for labels, test output, **and** the JSON form's `kind`
token (see below).

## Nodes and edges

```cpp
struct Node {
    NodeId id;                  // unique within the graph (a UUID by default)
    StepKind kind;              // its stage in the taxonomy
    std::string name;           // human-readable label
    nlohmann::json attributes;  // free-form config bag (empty for a bare node)
};

enum class EdgeKind { DataFlow, Control };   // typed, directed dependency

struct Edge { NodeId from; NodeId to; EdgeKind kind; };
```

- **`NodeId`** is a `std::string`. `add_node` assigns a fresh UUID when you don't
  supply one, or honours an explicit id (handy for readable code/tests).
- **`attributes`** is an `nlohmann::json` value bag — the place the compiler will
  later anchor a node's dual-form payload. It is serialized verbatim (see below)
  but carries no schema of its own at this slice.
- **`EdgeKind`** types the dependency: `DataFlow` (the source's output feeds the
  target's input — the common case) vs. `Control` (ordering only; the target
  runs after the source, no data passes).

## Building and traversing a graph

```cpp
#include "zetta/ir/execution_graph.hpp"
using namespace zetta::ir;

ExecutionGraph graph;
auto load  = graph.add_node(StepKind::Connect, "load_orders",
                            {{"source", "postgres"}, {"table", "orders"}});
auto clean = graph.add_node(StepKind::Clean, "dedupe");
auto model = graph.add_node(StepKind::Model, "churn_model");

graph.add_edge(load->id,  clean->id);                    // data_flow by default
graph.add_edge(clean->id, model->id, EdgeKind::DataFlow);

// Traverse by step kind (the acceptance criterion):
for (const auto& node : graph.nodes_of_kind(StepKind::Model)) { /* … */ }

// Walk neighbours / edges:
graph.successors(load->id);     // → [clean]
graph.predecessors(model->id);  // → [clean]
graph.edges_from(load->id);     // outgoing edges
graph.edges_of_kind(EdgeKind::DataFlow);
```

`add_node` / `find_node` / `successors` / `predecessors` / `nodes_of_kind` hand
back `std::shared_ptr<Node>` handles (empty on a miss), matching the rest of
zetta; the graph co-owns each node with its id index. Edges are small value types
returned by copy. Node and edge **insertion order is preserved**, so traversals
are deterministic.

### Structural integrity (built in)

The data model guards its own shape — this is *structure*, not the gate's
*semantics*:

- `add_node` never overwrites: a duplicate `id` returns an empty handle.
- `add_edge` rejects a **dangling** endpoint (both nodes must already exist) and
  an **exact duplicate** (same `from`, `to`, *and* kind); it returns `false`.
  Distinct edge kinds between the same pair, and self-edges, are allowed.

### Editing a graph (remove)

Two `remove_*` methods are the inverses of `add_node` / `add_edge`, so a graph
can be edited in place — the IR side of the [Graph Workspace](projects.md#graph-workspace)
panel:

```cpp
graph.remove_edge(clean->id, model->id);          // drop one exact edge (by kind)
graph.remove_node(model->id);                      // drop the node …
                                                   // … and every edge incident to it
```

- `remove_edge(from, to, kind)` deletes the **one exact edge** and leaves both
  endpoints untouched; it returns `false` if there is no such edge. A different
  `kind` on the same pair is left alone.
- `remove_node(id)` deletes the node **and every edge incident to it** (in or
  out), so removing a node never leaves a dangling edge — the mirror of
  `add_edge`'s no-dangling guarantee. It returns `false` if there is no such
  node. Survivors keep their insertion order, and the freed id can be reused.

## JSON serialization

`include/zetta/ir/serialization.hpp` adds a canonical JSON form — the **tabular**
half of ADR-10's dual representation, and the form used to persist and replicate
a graph (see [projects.md](projects.md)). A graph is two arrays, `nodes` and
`edges`:

```json
{
  "nodes": [
    { "id": "n-load", "kind": "connect", "name": "load_orders",
      "attributes": { "source": "postgres", "table": "orders" } },
    { "id": "n-clean", "kind": "clean", "name": "dedupe" }
  ],
  "edges": [
    { "from": "n-load", "to": "n-clean", "kind": "data_flow" }
  ]
}
```

- **`kind`** is the canonical lowercase name (`to_string(StepKind)` /
  `to_string(EdgeKind)`), never the enum's integer — so the format is stable
  across enum reordering and is human-readable.
- **`attributes`** is emitted only when a node carries a bag; a bare node has none
  and round-trips back to none (an explicit empty object `{}` is distinct and is
  preserved). An edge's **`kind`** defaults to `"data_flow"` when omitted.

```cpp
#include "zetta/ir/serialization.hpp"
using namespace zetta::ir;

const nlohmann::json document = to_json(graph);          // never fails
std::string error;
std::optional<ExecutionGraph> parsed = from_json(document, &error);
if (!parsed) { /* `error` explains why, e.g. nodes[2]: unknown step kind "x" */ }
```

`to_json` then `from_json` **round-trips a graph unchanged** — same nodes, edges,
attributes, and insertion order — and re-serializing is idempotent.

### Schema validation at the boundary

`from_json` is the trust boundary. It is **non-throwing** — it returns
`std::nullopt` (and a human-readable `error`) rather than throwing, matching
zetta's convention everywhere else (`nlohmann::json::parse(…, allow_exceptions =
false)` + skip-on-malformed), so a bad payload is rejected cleanly instead of
corrupting state. It enforces this JSON Schema (Draft 2020-12):

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "title": "zetta.ir.ExecutionGraph",
  "type": "object",
  "required": ["nodes", "edges"],
  "properties": {
    "nodes": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["id", "kind", "name"],
        "properties": {
          "id":   { "type": "string", "minLength": 1 },
          "kind": { "enum": ["connect","clean","model","index","serve","act"] },
          "name": { "type": "string" },
          "attributes": { "type": "object" }
        }
      }
    },
    "edges": {
      "type": "array",
      "items": {
        "type": "object",
        "required": ["from", "to"],
        "properties": {
          "from": { "type": "string", "minLength": 1 },
          "to":   { "type": "string", "minLength": 1 },
          "kind": { "enum": ["data_flow","control"] }
        }
      }
    }
  }
}
```

Beyond the schema it also enforces the IR's own **structural** rules (the same
ones `add_node` / `add_edge` guard): unique node ids, and no dangling or
exact-duplicate edges. Unknown top-level / object fields are ignored
(forward-compatible across versions). The error message names the offending
location, e.g. `edges[1]: edge references an unknown node ("a" -> "ghost")`.

## What's next (later slices)

- **Dual-form serialization** — the *code* (Lua/Orly) and *columnar*
  (Parquet/CSV/Automerge) projections (ADR-10), on top of the JSON form above.
  When a serialized form is introduced, it gets a JSON Schema validated at the
  boundary.
- **The gate** — the runner that validates a graph now exists
  (`zetta::CorrectnessGate`, [gate.md](gate.md)): it runs a set of checks over a
  graph and blocks deploy on failure. The concrete *semantic* checks — acyclicity,
  legal stage transitions, required inputs — are the remaining work that plugs
  into its `Check` seam.
- **Compile** — emit a graph from an input. The first concrete compiler exists:
  [`compile_n8n_workflow`](n8n-compile.md) maps an imported n8n workflow's nodes
  into typed steps and its connections into edges (#308). Compiling from
  natural-language intent is the remaining slice. **Deploy** has its first slice: a
  graph can be placed onto a local target and the placement recorded — see
  [deployments.md](deployments.md). **Run** has its first slice too: a graph can be
  executed step-by-step into a [Run](runs.md) — see [execution.md](execution.md).

See also [architecture.md](../architecture/architecture.md), [projects.md](projects.md), and the
related artifacts: ENT-3 (`#84`), FEAT-2 (`#133`), ADR-2 (`#2`), ADR-10 (`#10`).
