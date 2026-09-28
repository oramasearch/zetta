# Methodology registry

Implemented for [#1061](https://github.com/oramasearch/zetta/issues/1061), under
the [methodology epic #984](https://github.com/oramasearch/zetta/issues/984).

The methodology registry is the engine's single source of the modeling
approaches it recognizes and can run. It lists the complete M-1 through M-21
portfolio in id order, records each entry's inputs and outputs, and carries the
callbacks for runnable implementations. M-1 and M-21 are runnable. M-10 is marked partial
because retrieval and indexes ship, but it does not yet provide a methodology
runner. The other entries link to their implementation issues. M-21's detailed
contract is in [streaming.md](streaming.md).

The generated table in [README.md](README.md#methodology-portfolio) is checked
against the C++ registry by `MethodologyRegistry.DocumentationTableMatchesTheRegistry`.

## Contract

`include/zetta/ml/methodology.hpp` defines three request types and the registry
entry:

- `ShapeRequest` carries project, intent, selected solution, and optional hints
  into `build_shape`.
- `RunRequest` carries the API context, stored project and graph, and the run
  input into `run`.
- `Methodology` describes identity, discovery metadata, required data roles,
  outputs, metrics, implementation status, and the `build_shape`, `owns`, and
  `run` callbacks.

`methodologies()` owns the ordered table. `find_methodology()` resolves ids and
model kinds first, then primary solution ids before secondary solution mappings.
This makes M-21 the owner of its primary S-10 compile path while M-1 can still
list S-10 as a composing methodology. `methodology_for_graph()` first asks
implemented ownership callbacks, then resolves the model step's `model_kind` or
legacy `algo` value.
Compile and run refusal messages list runnable entries from the same registry.

The `list_methodologies` read capability exposes every entry as JSON over REST
and MCP. Each object contains `id`, `name`, `family`, `model_kinds`, `solutions`,
`inputs`, `outputs`, `metrics`, `runnable`, `partial`, and `issue`.

## Compile and run behavior

[`compile_intent`](../pipeline/intent-compiler.md) resolves an explicit
methodology, an explicit primary solution, registered keywords, or a clear
supervised-prediction request, in that order. It calls only the selected
entry's `build_shape`. Registered entries that are not runnable return
`not_supported` with their name, id, and issue. An unmatched non-predictive
intent is refused instead of receiving a placeholder graph.

`run` uses `methodology_for_graph()`. A graph whose model step belongs to an
unimplemented methodology is refused with the same issue-bearing message. An
unknown model kind is also refused. A graph with no model step remains eligible
for structural execution and logs that it trained and scored nothing.

`get_graph` and each graph in `get_project` report `methodology` and `runnable`.
The methodology is `null` for structural graphs and unknown model kinds;
structural graphs are runnable, while unknown model kinds are not.

## M-1 reference implementation

`src/core/ml/methodology.cpp` wraps the existing behavior behind the interface:

- `build_shape` builds the established churn pipeline;
- `owns` recognizes M-1 model kinds and the existing churn graph shape; and
- `run` preserves the asynchronous churn runner, including file, table, and
  built-in demo dataset inputs.

The API handler contains no M-1-specific branch. Its only decisions are registry
resolution, support checks, structural execution for graphs without a model,
and callback dispatch.

## M-21 streaming implementation

`src/core/ml/streaming_methodology.cpp` builds and owns the event-time streaming
shape and resolves its connect-step table. Its run callback starts the native
runner in `src/core/ml/streaming.cpp`, which subscribes to the graph's input
topic, computes per-entity window features, updates online logistic regression,
publishes scores, persists weights, writes a score table with lineage, and
exposes prequential metrics through `get_run_report`.

The API handler contains no M-21-specific dispatch branch. The registry callback
uses the same generic run path as M-1.

## Adding a methodology

An implementation work order adds its own source files and then:

1. implements the shape builder, graph ownership predicate, and run callback;
2. sets those callbacks and `runnable = true` on its registry entry;
3. lists every accepted model kind and keyword on that entry;
4. adds focused compile, ownership, and run tests; and
5. regenerates this table and `schemas/openapi.json`.

No compiler or API run-handler branch is required. Implementations remain
native C++ under [ADR-17](https://github.com/oramasearch/zetta/issues/17).
