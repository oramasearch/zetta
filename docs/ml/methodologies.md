# Methodology registry

Implemented for [#1061](https://github.com/oramasearch/zetta/issues/1061), under
the [methodology epic #984](https://github.com/oramasearch/zetta/issues/984).

The methodology registry is the engine's single source of the modeling
approaches it recognizes and can run. It lists the complete M-1 through M-21
portfolio in id order, records each entry's inputs and outputs, and carries the
callbacks for runnable implementations. M-1, M-3, M-4 and M-7 are runnable. M-10 is marked partial
because retrieval and indexes ship, but it does not yet provide a methodology
runner. The other entries link to their implementation issues.

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

`methodologies()` owns the ordered table. `find_methodology()` resolves ids,
model kinds, and primary solution ids. `methodology_for_graph()` first asks
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

## M-3 segmentation implementation

M-3 is implemented in `segmentation_methodology.cpp`, with its own
connect → global feature preparation → segmentation model → assignment/profile
output graph. M-3, S-9 and segmentation keywords compile this shape. Its runner
resolves the connect step's table without a supervised label, coordinates the
native trainer across owned partitions, and returns a durable Run immediately.
`get_run_report` reads measured K selection, separation, stability, input/exclusion
facts and both output table references. The `SegmentationCheck` is part of the
default gate. See [segmentation.md](segmentation.md#running-it).

## M-4 forecasting

`forecast_methodology.cpp` builds M-4's own four-stage graph: read the named
history table → strictly past lag features → rolling-origin backtest and
squared-error forecast → forecast table with intervals. `forecast_execution.cpp`
resolves that graph's source/table and roles into a `ResolvedDataset`, launches
the distributed Phase-A runner, and records a durable Run. The initiating node
does not have to be the collective root. The root's model/output references are
broadcast so the Run names the same replicated artifacts everywhere.

`ForecastGraphCheck` is part of the default gate. It refuses altered stage
dependencies, extra feature transforms, future lags, random holdouts,
calibration, a missing seasonal-naive baseline, malformed roles and invalid
horizon bounds. Actual history bounds are also enforced per series at runtime.
`get_run_report` renders forecast metrics and table rows without classification
metrics or probability language. See [forecast.md](forecast.md#running-it) for
the tool sequence and supported inputs.

## M-7 anomaly implementation

`src/core/ml/anomaly_methodology.cpp` supplies its own four-stage pipeline:
connect → robust features (or causal KPI residuals) → anomaly model → output tables.
`compile_intent` accepts either `methodology: "M-7"` or `solution: "S-7"` and
reports `runnable: true`. It does not reuse the churn shape.

The asynchronous runner reads the graph's connect source/table, resolves optional
entity/label/time roles without requiring a label, and launches the native trainer.
Tabular runs coordinate primary-owned partitions on all participating members;
KPI runs currently require one member. The initiator writes the model, anomaly and
budget tables, and Run lineage. `get_run_report` exposes those tables, metrics, and
read/excluded input facts. `AnomalyGraphCheck` and the runner share plan validation,
including budget, label-use, robust-scaling, and causal-window refusals.
See [anomaly.md](anomaly.md#running-it) for parameters and capability calls.

## Adding a methodology

An implementation work order adds its own source files and then:

1. implements the shape builder, graph ownership predicate, and run callback;
2. sets those callbacks and `runnable = true` on its registry entry;
3. lists every accepted model kind and keyword on that entry;
4. adds focused compile, ownership, and run tests; and
5. regenerates this table and `schemas/openapi.json`.

No compiler or API run-handler branch is required. Implementations remain
native C++ under [ADR-17](https://github.com/oramasearch/zetta/issues/17).
