# Methodology registry

Implemented for [#1061](https://github.com/oramasearch/zetta/issues/1061), under
the [methodology epic #984](https://github.com/oramasearch/zetta/issues/984).

The methodology registry is the engine's single source of the modeling
approaches it recognizes and can run. It lists the complete M-1 through M-21
portfolio in id order, records each entry's inputs and outputs, and carries the
callbacks for runnable implementations. M-1 through M-20 are runnable. M-10 builds
its own governed text-to-embedding-to-retrieval-to-grounded-answer graph and
writes its native model, metrics, tables, and lineage. The other entries link to
their implementation issues.

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
model kinds, and primary solution ids. When more than one methodology names a
solution, a runnable implementation wins over a planned entry, with portfolio
order as the fallback. `methodology_for_graph()` first asks implemented
ownership callbacks, then resolves the model step's `model_kind` or legacy
`algo` value.
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
intent is refused instead of receiving a placeholder graph. When a catalogue
solution composes several methodologies, its intent keywords select the
matching entry; `S-10` therefore selects M-20 for privacy or synthetic-data
intent and retains M-1 for its tabular prediction intent.

`run` uses `methodology_for_graph()`. A graph whose model step belongs to an
unimplemented methodology is refused with the same issue-bearing message. An
unknown model kind is also refused. A graph with no model step remains eligible
for structural execution and logs that it trained and scored nothing.

`get_graph` and each graph in `get_project` report `methodology` and `runnable`.
The methodology is `null` for structural graphs and unknown model kinds;
structural graphs are runnable, while unknown model kinds are not.

## Runnable implementations

`src/core/ml/methodology.cpp` wraps the existing behavior behind the interface:

- `build_shape` builds the established churn pipeline;
- `owns` recognizes M-1 model kinds and the existing churn graph shape; and
- `run` preserves the asynchronous churn runner, including file, table, and
  built-in demo dataset inputs.

The API handler contains no M-1-specific branch. Its only decisions are registry
resolution, support checks, structural execution for graphs without a model,
and callback dispatch.

## M-2 entity resolution

`src/core/ml/entity_resolution_methodology.cpp` builds M-2's connect, blocking,
pair-scoring, conflict-safe clustering, and table-output pipeline. Its ownership
callback recognizes only entity-resolution model kinds. The run callback reads
the connect step's own `source.table` unless `dataset_path` explicitly overrides
it, calls the native runner, writes resolved-entity and match-pair tables, and
registers its metrics and thresholds on a model record for `get_run_report`.

The default correctness gate requires non-empty blocking keys, stated match and
review thresholds, and the verified-identifier conflict policy. The runtime then
enforces those declarations while clustering.

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

## M-5 survival implementation

`src/core/ml/survival_methodology.cpp` builds a dedicated survival cohort →
validation → discrete-hazard model → survival-table → calibration-table graph.
Its ownership callback recognizes only `survival` and `time_to_event`. Its run
callback resolves the connect step's own table, calls the native distributed
runner, and returns the durable Run record read by `get_run` and
`get_run_report`. See [survival.md](survival.md).

## M-6 causal uplift implementation

`src/core/ml/uplift_methodology.cpp` supplies M-6’s own connect → feature
selection → uplift model → targeting-policy shape. Its ownership predicate only
claims `causal` and `uplift` model steps. Its run callback resolves the connect
step’s table through the shared dataset contract and calls the native runner
documented in [uplift.md](uplift.md).

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

## M-8 recommendation implementation

M-8 owns graphs whose model step names `next_best_action`, `recommendation`, or
`recommender`. Its shape is source → candidate features → propensity/ALS model →
hard policy → recommendation outputs. The runner reads the source table recorded
on the graph, launches every collective participant, and writes a recommendation
table, policy report, Model, and report reference with source lineage.

The graph check refuses altered shapes, non-chronological interaction evaluation,
inferred logging propensities, causal claims for predictive propensity, or a
policy that can bypass eligibility and contact caps. See
[next_best_action.md](next_best_action.md) for the table contract and metrics.

## M-9 learning-to-rank

`src/core/ml/rank_methodology.cpp` owns `rank`, `ranking`, and
`learning_to_rank` graphs. Its shape connects one candidate table, selects
ranking-time features, trains native LambdaMART, and materializes a grouped
ranked queue. Its run callback resolves either the graph's own `source`/`table`
or an explicit `dataset_path`, then calls the shared resolved-dataset runner.

The objective, distributed job, output table, metrics, correctness refusals,
and API example are documented in [rank.md](rank.md).

## M-11 document implementation

`src/core/ml/document_methodology.cpp` supplies M-11's document-specific
connect, text-recovery, field-extraction, and review-routing shape. Its runner
resolves that graph's own input table and writes extracted-field and review
tables with model and run lineage. See
[document_extraction.md](document_extraction.md) for its input contract,
correctness rules, checkpoint licence, and operating instructions.

## M-12 graph implementation

`src/core/ml/graph_methodology.cpp` owns `graph` and `graph_ml` model steps. Its
shape is relationship-specific: connect an edge table, enforce a prediction-time
filter, compute graph features and embeddings, then write joinable output tables.
Its run callback resolves the pipeline's connect-step table when the caller does
not override `dataset_path`, executes the native graph runner, and returns the
Run read by `get_run` and `get_run_report`. See [graph.md](graph.md).

## M-13 vision implementation

`src/core/ml/vision_methodology.cpp` provides M-13's image-table shape,
ownership predicate, and runner. It resolves the table named by the graph's own
connect step, enforces the source-disjoint and large-object access rules, calls
the [native vision runner](vision.md), and returns the durable run id. The same
run and report reads used by M-1 expose M-13's Model, prediction table, metrics,
confusion matrix, and read/exclusion counts.

## M-14 speech implementation

M-14 owns `speech` and `audio` model steps. Its shape loads one recording table,
decodes and prepares local audio, runs native Whisper, and writes a redacted
transcript table. Its callback resolves the connect step's own database and
table before calling the speech runner; it never substitutes a built-in dataset
or structural execution. Consent, transcript privacy filtering, and node-local
audio are gate requirements and runner invariants. See
[speech.md](speech.md).

## M-15 bandit implementation

`src/core/ml/bandit_methodology.cpp` provides M-15 independently:

- `build_shape` builds connect → context preparation → bandit model → policy
  output → propensity-log output;
- `owns` recognizes the M-15 model kinds; and
- `run` resolves the connect step's table when no explicit dataset is supplied,
  runs the native policy, persists the policy and decision tables, and records
  their lineage and metrics on the Run and Model records.

S-11 resolves to M-8, its primary catalogue method. M-15 supports sequential
allocation but is selected by methodology ID or its bandit model kinds, not S-11.

## M-16 optimization implementation

`src/core/ml/methodology_optimization.cpp` owns the optimization shape and
adapter described in [optimization.md](optimization.md). It resolves the
pipeline's connect-step table when `run` omits `dataset_path`, calls the native
solver, and publishes the plan, constraint trace, solver-result record, and run
report with lineage. S-4 remains owned by M-1, the catalogue's first method for collections
prioritization; select optimization by M-16 or its model kinds.
## M-17 Bayesian uncertainty

`src/core/ml/bayesian_methodology.cpp` owns M-17's registry adapter:

- `build_shape` creates a grouped-observation pipeline that reads the connect
  step's own `source` and `table`, reserves a content-hash holdout, fits the
  Gibbs partial-pooling model, and writes a posterior table;
- `owns` recognizes `bayesian`, `probabilistic`, and `uncertainty`; and
- `run` resolves the graph's table through `ResolvedDataset`, invokes the native
  runner, and returns the completed Run and output table.

The pipeline carries `coverage_checked`, `nominal_coverage`, and an explicit
exchangeability statement. `UncertaintyValidationCheck` refuses a Bayesian
shape that omits those facts; split-conformal shapes must also declare that
calibration rows are disjoint from fitting rows.
## M-18 ensembling and stacking

`src/core/ml/ensemble_methodology.cpp` owns the M-18 callbacks:

- `build_shape` creates a table connect, numeric-role validation, out-of-fold
  ensemble model step, and blended-score/meta-weight output step;
- `owns` recognizes only the registered ensemble model kinds; and
- `run` resolves the connect step's `source` and `table`, applies the model
  step's column roles, calls the native runner, and returns the durable Run.

The runner in `src/core/ml/ensemble_runner.cpp` registers an `ensemble` Model and
writes queryable blended-score and meta-weight tables with lineage. S-10 stays with M-1; select M-18 by ID or model kind. See [ensemble.md](ensemble.md) for its split guarantees and current
multi-member adapter boundary.
## M-19 label-efficiency implementation

`src/core/ml/label_efficiency_methodology.cpp` builds a table-specific pipeline:
connect the examples table, prepare numeric features, train the active and weak
label models, then write the label queue and weak-label tables. Its ownership
predicate recognizes `label_efficiency`, `active_learning`, and
`weak_supervision` model steps.

The callback resolves either the run request's `dataset_path` or the pipeline's
own connect-step `source` and `table`, then starts the asynchronous runner in
[`label-efficiency.md`](label-efficiency.md). The API handler remains generic.
## M-20 privacy-preserving implementation

`src/core/ml/synthetic_private_methodology.cpp` owns the `synthetic_private`
model kind and builds M-20's connect, bounds/encoding, model, and publish
pipeline. Its run callback resolves the graph's own landed table, drives the
owned-partition job, and writes an evaluated synthetic table, DP model, privacy
report, and lineage. The methodology's formal mechanism, empirical publication
gate, inputs, metrics, and run examples are documented in
[synthetic_private.md](synthetic_private.md).

## Adding a methodology

An implementation work order adds its own source files and then:

1. implements the shape builder, graph ownership predicate, and run callback;
2. sets those callbacks and `runnable = true` on its registry entry;
3. lists every accepted model kind and keyword on that entry;
4. adds focused compile, ownership, and run tests; and
5. regenerates this table and `schemas/openapi.json`.

No compiler or API run-handler branch is required. Implementations remain
native C++ under [ADR-17](https://github.com/oramasearch/zetta/issues/17).
