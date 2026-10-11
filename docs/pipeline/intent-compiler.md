# Compiling an intent into a methodology graph

`compile_intent` turns a captured natural-language intent into a reviewable
[ExecutionGraph](execution-graph.md). The compiler resolves a registered
[methodology](../ml/methodologies.md), asks that methodology to build its own graph shape, and stores
the result as an editable project graph.

```
include/zetta/pipeline/intent_compiler.hpp   compile options and result
src/core/pipeline/intent_compiler.cpp         deterministic resolution
include/zetta/ml/methodology.hpp              shape callback contract
src/core/ml/methodology.cpp                   M-1 shape callback
src/core/ml/survival_methodology.cpp          M-5 shape callback
src/core/ml/methodology_optimization.cpp      M-16 shape and run callbacks
src/core/ml/ensemble_methodology.cpp          M-18 shape callback
```

## Resolution order

The compiler uses the first applicable signal:

1. the optional `methodology` id or model kind;
2. the optional `solution`, resolved to its primary methodology;
3. a registered methodology keyword in the intent; or
4. M-1 when the intent clearly asks for supervised prediction.

M-2 is selected explicitly by id/model kind or by its identity-resolution
keywords; it is not used as a generic fallback.

M-5 is selected explicitly by `methodology: M-5` or by its registered
time-to-event keywords. S-8 selects M-7 (anomaly detection), its primary
catalogue method; survival is a supporting component. It returns a dedicated cohort →
validation → discrete-hazard model → survival/calibration output graph.

An empty intent is `invalid_input`. An unknown methodology or solution is also
`invalid_input`. If the resolved methodology is registered but not runnable,
the capability returns `not_supported`, naming the methodology and its tracking
issue. A non-predictive intent with no match is refused and lists the runnable
methodologies. The compiler never substitutes a generic graph.

Resolution is deterministic. Keywords and methodologies are evaluated in
registry order, and the same request yields the same node ids, names, and
insertion order.

## Compile request

The C++ entry point accepts the text plus optional registry guidance:

```cpp
zetta::pipeline::IntentCompileOptions options;
options.project = "retention";
options.solution = "S-1";
options.hints = {{"label", "churned"}};

std::string error;
auto compiled = zetta::pipeline::compile_intent(
    "Score every active account for churn risk and tell me why.",
    options,
    &error);

if (compiled) {
  // compiled->methodology == "M-1"
  // compiled->graph is M-1's connect -> clean -> model -> index -> serve -> act shape
}
```

The API capability accepts the same optional `methodology`, `solution`, and
`hints` fields. Its answer includes the methodology id it compiled and
`template`, the template its steps started from.

**Names (#1238).** Unless a caller supplies a name, a compiled pipeline is named
from the intent's own words (`intent_pipeline_name`): its first words, at most
eight and about sixty characters, punctuation dropped, the first letter
capitalized, and never ending on a joining word such as *and* or *the*.
*"Predict which customers will churn next quarter and rank the at-risk
accounts"* compiles to `Predict which customers will churn next quarter`. An
intent with no words falls back to `<project> pipeline` (or
`<methodology name> pipeline` without a project). Every pipeline used to be
named for its template (`churn skeleton` for M-1, `<methodology name> pipeline`
otherwise), so a person who described their own problem got a pipeline named for
someone else's.

**Template.** `CompiledIntent::template_name`, and `template` in the
capability's answer, say which template the steps came from: `the M-1 churn
template`, `the S-2 template` when a solution was selected, or `the M-3
Segmentation / clustering template`. The steps' names (`load_customers`,
`notify_retention` …) are the template's until someone edits them; the template
is said rather than hidden. Fitting step names to a dataset's columns is the
shape builders' work, not the compiler's.

`CompiledIntent::name` is independent of the legacy `IntentShape`
classification. Unresolved intents still fail rather than producing a generic
pipeline.

## M-1 reference shape

M-1 is the tabular runnable registry entry. It builds the existing churn graph:

```
load_customers ─┐
                ├─▶ build_features ─▶ train_churn_model ─▶ index_scores ─▶ serve_scores ─▶ notify_retention
load_activity ──┘
   connect            clean              model               index           serve            act
```

The model node carries `model_kind: "gbdt"` as the dispatch identity and keeps
the existing `algo: "gradient_boosting"` configuration. All attributes remain
editable starting-point hints for review.

M-2 is also runnable. Its entity-resolution shape connects the selected records
table, normalizes and blocks candidate pairs, scores them with
`model_kind: "entity_resolution"`, clusters conflict-safe matches, and writes
the resolved-entity and match-pair tables. See
[entity resolution](../ml/entity_resolution.md).

## M-16 optimization shape

M-16 compiles its own table-to-plan pipeline:

```
load_optimization_table → validate_optimization_model → solve_optimization_plan → publish_optimization_plan
       connect                    clean                       model                         act
```

The model step carries `model_kind: "optimization"` plus the scenario-input,
independent-verification, and status-reporting contract flags required by the
correctness gate. The connect step's `source` and `table` are the default run
dataset; callers can override them with `dataset_path`.
## M-18 ensemble shape

M-18 builds its own four-stage pipeline from the compile hints:

```
load_ensemble_table -> prepare_ensemble_inputs -> train_ensemble -> write_ensemble_outputs
       connect                 clean                  model                serve
```

The connect step keeps the supplied `source` and `table`. The model step records
the entity, label, feature, and optional time columns plus `out_of_fold`, fold
strategy, and full-training base refit attributes. The graph therefore carries
the facts the M-18 gate check and runner enforce. Explicit M-18 selection,
and registered ensemble keywords select this shape. S-10 stays with M-1.

## Stored intent bridge

`Project::compile_intent_to_graph` reads an immutable captured intent, calls the
same compiler, and persists the resulting graph through `create_graph`.
Callers may supply `IntentCompileOptions`. Without a name the graph is named
from the intent's words; when the project already has a graph by that name, the
new one is numbered (`… (2)`, `… (3)`), so compiling the same intent twice no
longer fails. An optional `template_name` out-parameter receives the template.
The method returns an empty handle when the input is missing, is not an intent,
has no local body, cannot be resolved to a runnable methodology, or an explicit
graph name collides.

The graph uses the existing IR JSON boundary and replication path. This feature
introduces no new stored graph format.

## Extending the compiler

A methodology supplies `build_shape` on its registry entry. The compiler
does not need another methodology-specific branch. See
[methodologies.md](../ml/methodologies.md) for the full callback contract and
extension checklist.

## See also

- [project-inputs.md](project-inputs.md) — capturing the intent.
- [projects.md](projects.md) — storing and reviewing the compiled graph.
- [execution.md](execution.md) — registry-based run dispatch.
- [gate.md](gate.md) — correctness checks before deploy and run.
