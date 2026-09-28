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
`hints` fields. Its answer includes the methodology id it compiled.

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

## Stored intent bridge

`Project::compile_intent_to_graph` reads an immutable captured intent, calls the
same compiler, and persists the resulting graph through `create_graph`.
Callers may supply `IntentCompileOptions`; the default graph name remains
`"churn skeleton"` for M-1 unless a name is given. The method returns an empty
handle when the input is missing, is not an intent, has no local body, cannot be
resolved to a runnable methodology, or the graph name collides.

The graph uses the existing IR JSON boundary and replication path. This feature
introduces no new stored graph format.

## Extending the compiler

A later methodology supplies `build_shape` on its registry entry. The compiler
does not need another methodology-specific branch. See
[methodologies.md](../ml/methodologies.md) for the full callback contract and
extension checklist.

## See also

- [project-inputs.md](project-inputs.md) — capturing the intent.
- [projects.md](projects.md) — storing and reviewing the compiled graph.
- [execution.md](execution.md) — registry-based run dispatch.
- [gate.md](gate.md) — correctness checks before deploy and run.
