# Execution graph IR

The **ExecutionGraph IR** (`zetta::ir`) is the in-memory intermediate
representation of a data/AI system: a directed graph of typed *steps*. It is the
keystone the compilers, the gate, deploy, and run all build on — the artifact the
GUI will show, validate, and deploy (ENT-3 / FEAT-2).

This is the **data-model slice only**. You can build a graph in code and traverse
it. There is deliberately:

- **no serialization** — the dual-form *code* (Lua / Orly) + *tabular*
  (Parquet / CSV / Automerge) representation from ADR-10 is a later slice, and
- **no semantic validation** — acyclicity and legal stage transitions are the
  **gate's** job, also a later slice.

The model lives behind two headers and one translation unit:

```
include/zetta/ir/step_kind.hpp        the step-kind taxonomy (enum + name helpers)
include/zetta/ir/execution_graph.hpp  Node, Edge, EdgeKind, ExecutionGraph
src/core/ir/execution_graph.cpp       the implementation
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
(`"connect"`, …) for labels and test output — these are naming helpers, **not**
serialization.

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
- **`attributes`** is an in-memory `nlohmann::json` value bag — the place the
  compiler will later anchor a node's dual-form payload. It carries no schema at
  this slice (nothing is parsed from or written to disk yet).
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

## What's next (later slices)

- **Serialization** — the dual-form code (Lua/Orly) and tabular
  (Parquet/CSV/Automerge) representations (ADR-10). When a serialized form is
  introduced, it gets a JSON Schema validated at the boundary.
- **The gate** — the runner that validates a graph now exists
  (`zetta::CorrectnessGate`, [gate.md](gate.md)): it runs a set of checks over a
  graph and blocks deploy on failure. The concrete *semantic* checks — acyclicity,
  legal stage transitions, required inputs — are the remaining work that plugs
  into its `Check` seam.
- **Compile / deploy / run** — emit a graph from intent, then deploy and execute
  it.

See also [architecture.md](architecture.md) and the related artifacts: ENT-3
(`#84`), FEAT-2 (`#133`), ADR-2 (`#2`), ADR-10 (`#10`).
