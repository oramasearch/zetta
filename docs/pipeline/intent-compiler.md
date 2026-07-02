# Compiling an intent into a graph

Compiling a natural-language **intent** turns the text a user described their
problem in into a reviewable [ExecutionGraph](execution-graph.md) skeleton, laid
out over the `connect → clean → model → index → serve → act` taxonomy. This is an
atomic slice of **FEAT-2** (Architect — compile to execution-graph IR, `#133`) /
`#311`: it bridges a captured intent ([project inputs](project-inputs.md),
FEAT-1) to the first draft of a graph the [Graph Workspace](projects.md) renders
and edits.

The output is the IR itself — a graph of named, typed, **editable** steps, **not
a black box**. The compiler only seeds a sensible starting point; review and edit
are expected.

```
include/zetta/pipeline/intent_compiler.hpp   compile_intent, CompiledIntent, IntentShape
src/core/pipeline/intent_compiler.cpp         the templated/heuristic mapping
```

## Altitude: a templated mapping, not language understanding

This slice is a **templated, heuristic** mapping, the same altitude as the rest
of R1 (a [deployment](deployments.md) records a *summary*; a
[run](runs.md) derives outputs from structure). The heuristic recognizes the
**churn shape** — the first use case, UC-1 — from a handful of transparent signal
words (`churn`, `retention`, `attrition`, `cancel`, `renew`, `subscriber`,
`lapse`, …) and emits the full churn pipeline. Anything else falls back to a
generic-but-complete six-stage skeleton, so an intent **always** compiles to a
well-formed, editable graph rather than nothing.

Real NL parsing / LLM-driven synthesis plugs in later behind the same
`compile_intent` front door without changing its contract — exactly the seam
pattern the [executor](execution.md) (`StepExecutor`) and the
[gate](gate.md) (`Check`) use.

## Compiling text directly

The standalone entry point takes the intent text and returns the skeleton plus
the template that produced it (so *why* a graph was produced stays inspectable):

```cpp
#include "zetta/pipeline/intent_compiler.hpp"

std::optional<zetta::pipeline::CompiledIntent> compiled = zetta::pipeline::compile_intent(
    "Score every active account for churn risk and tell me why.");

if (compiled) {
    // compiled->shape == zetta::pipeline::IntentShape::Churn
    // compiled->graph is a full connect→clean→model→index→serve→act skeleton
}
```

`compile_intent` returns `std::nullopt` for an intent that is empty or only
whitespace (it carries no meaning — nothing to compile), mirroring
[`Project::capture_intent`](project-inputs.md)'s rejection at ingestion; when the
optional `error` out-param is non-null it is set to a human-readable reason. The
mapping is **deterministic**: the same intent yields an identical graph (stable
ids, names, and insertion order).

### The churn skeleton

A churn intent compiles to the full pipeline a senior data+AI engineer would
seed — two `connect` sources feeding a feature build, a churn-risk `model`, an
`index` over its scores, a `serve` step, and a retention `act`:

```
load_customers ─┐
                ├─▶ build_features ─▶ train_churn_model ─▶ index_scores ─▶ serve_scores ─▶ notify_retention
load_activity ──┘
   connect            clean              model               index           serve            act
```

Each node carries starting-point hints in its `attributes` bag (a `note`
explaining the step for review, plus concrete config like `source`/`table`,
`algo`/`target`) — all of which the user refines in the workspace. An
unrecognized intent compiles to the same shape with one generic node per stage,
chained linearly.

## Compiling a stored intent into a project graph

The bridge from a *stored* intent input to an editable graph in the workspace
lives on `Project`:

```cpp
auto project = node.find_project("churn");
auto intent  = project->capture_intent("Flag accounts likely to churn …");

// Compile the stored intent into a new, named, replicated ProjectGraph.
auto graph = project->compile_intent_to_graph(intent->id());
//   graph->name() == "churn skeleton" (shape-derived default; pass a name to override)
```

`compile_intent_to_graph` reads the intent input's payload body, runs
`compile_intent`, and persists the result through
[`create_graph`](projects.md) — so the compiled skeleton **replicates and is
editable in the Graph Workspace exactly like any other stored graph**. It returns
an empty handle if the input id is unknown, the input is not an `Intent`, its
body is not present locally (a peer may hold the metadata before body transfer),
the intent is empty, or the resolved name collides with an existing graph. The
intent input is left untouched — it is an immutable provenance record; the
compiled graph is a fresh, separate artifact.

## Well-formed by construction

The skeleton is built through the IR's structure-guarding API (no duplicate ids,
no dangling edges — see [execution-graph.md](execution-graph.md)), so it:

- **round-trips** through the IR's JSON trust boundary
  (`ir::to_json` → `ir::from_json`) unchanged — this is what "editable, not a
  black box" rests on, since that JSON form is what persists + replicates a
  graph; and
- **passes the baseline correctness [gate](gate.md)** (its checks are opt-in
  annotations the skeleton doesn't carry), so it is deployable after review
  without further wiring.

There is **no new JSON format** here: the input is plain text and the output is
the existing, already schema-validated IR JSON form. Validation happens where the
graph is serialized/stored, not in the compiler.

## See also

- [project-inputs.md](project-inputs.md) — capturing the intent the compiler reads (FEAT-1).
- [execution-graph.md](execution-graph.md) — the IR the compiler emits, and its JSON form.
- [projects.md](projects.md) — the project that owns the compiled graph + the Graph Workspace that edits it.
- [gate.md](gate.md) — the correctness gate the skeleton passes before deploy.
- [execution.md](execution.md) — executing the (reviewed) graph into a Run.
