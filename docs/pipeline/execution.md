# Executing a graph

Executing an [ExecutionGraph](execution-graph.md) on a node walks its steps in
dependency order and produces a [Run](runs.md) — the observable record of that
execution (status, logs, outputs, lineage). This is an atomic slice of **FEAT-5**
(Run + monitor + lineage, `#136`): it joins the two keystones it sits on — the
ExecutionGraph IR (*what* to run) and the Run record (*the result*).

This page documents the **orchestrator**: `zetta::pipeline::GraphExecutor` and the
`zetta::pipeline::StepExecutor` interface it runs each step through.

```
include/zetta/pipeline/executor.hpp   GraphExecutor, StepExecutor, DefaultStepExecutor,
                             StepOutcome, ExecutionResult, topological_order
src/core/pipeline/executor.cpp        the orchestrator + the default per-step executor
```

## Altitude: orchestration, not (yet) heavy compute

This slice **orchestrates** execution; it does not train a real model or build a
real index. A `DefaultStepExecutor` derives a faithful, schema-valid `Run` from
the graph's structure — exactly the same altitude as the rest of R1 (a
[deployment](deployments.md) records a *summary*, a run records *outputs*). Real
per-step compute plugs in behind the `StepExecutor` interface **without touching the
orchestrator**, the same shape as the gate's `Check` interface ([gate.md](gate.md))
and the `StorageEngine` interface ([storage.md](../data/storage.md)).

## Running a graph

The discoverable entry point is on the node:

```cpp
#include "zetta/pipeline/executor.hpp"
#include "zetta/node.hpp"

zetta::ir::ExecutionGraph graph = /* … build or load from a ProjectGraph … */;

// Deploy records the placement (a separate slice); then execute on the node.
node.deploy_graph(graph, zetta::pipeline::DeploymentTarget::local(), "churn_pipeline");

zetta::pipeline::ExecutionResult result =
    node.run_graph(graph, /*project=*/"churn-project", /*graph_id=*/"churn-graph-1");

if (result.ok) {
    // result.run->status() == RunStatus::Succeeded; outputs + lineage populated.
} else {
    // result.failed_step / result.error name what went wrong; the run is Failed
    // and its logs carry the failing step + reason.
}
```

`Node::run_graph` is a thin convenience over the standalone `GraphExecutor`
(which mirrors `CorrectnessGate`: a small runner holding the pluggable unit it
runs). Use `GraphExecutor` directly to inject a custom `StepExecutor`:

```cpp
zetta::pipeline::GraphExecutor executor{std::make_shared<MyStepExecutor>()};
zetta::pipeline::ExecutionResult result = executor.execute(node, graph, project, graph_id);
```

The `Run` is normally created on the node, so it is owned, persisted, and (when
the node is replicated) captured into the CRDT exactly like any other run. A
caller may instead pass an existing Run as the final argument. The Study driver
uses this form so the Run admitted as a trial is the same Run that receives
graph logs, metrics, outputs, and terminal status.

Since #624 `execute` may be driven from a **background worker**: every
node-tree write (the Run creation + the Run's mutators) gates through
`Node::run_on_owner` ([../architecture/owner-gate.md](../architecture/owner-gate.md);
inline for owner-thread callers), and an optional `cancel` flag (the last
parameter before the optional existing Run) is checked before and after steps —
a cancelled run ends Failed with a
"run cancelled by user" log line and `error`.

## What execution does

`execute` creates a `Run` (so the attempt is always recorded, even on failure),
then:

1. **Logs creation** — `run created: N step(s)`.
2. **Guards the basics** it needs to run at all. An **empty** graph (nothing to
   run) or a **cyclic** graph (no step order exists — a self-edge counts) fails
   the run immediately, with the reason logged. Full *semantic* validation (legal
   stage transitions, required inputs) is the correctness gate's job — a separate
   slice — not duplicated here.
3. **Orders the steps** with `topological_order` (Kahn's algorithm over every
   edge — both `DataFlow` and `Control` constrain order). Ready steps are emitted
   in graph **insertion order**, so the order is deterministic.
4. **Runs each step** in order through the `StepExecutor`, moving the run to
   `Running` and logging a line per step (`<kind>: <name>`). Outputs and lineage
   accumulate as it goes. When the executor was given a principal
   (`GraphExecutor::run_as`, #843), each step is first authorized for that
   principal, deny-by-default and audited; a refused step does not execute and
   fails the run like any other failed step. See
   [step authority](../governance/step-authority.md).
5. **Fail-fast** — if a step fails, the failing step + reason are logged, later
   steps do **not** run, and the run ends `Failed`. Otherwise it ends `Succeeded`.
6. **Stamps lineage** — `freshness` (an ISO-8601 stamp) and `confidence`
   (`1.0` for a completed local orchestration, `0.0` for a partial/failed run;
   real model-derived confidence is later work), then writes the terminal status.

The status therefore moves `Pending → Running → Succeeded | Failed`, matching the
[Run](runs.md) lifecycle, with each transition captured + persisted by the Run's
own mutators.

## The default step executor

`DefaultStepExecutor` interprets a step's `StepKind` to derive the artifact it
materializes. The mapping (the churn pipeline,
`connect→clean→model→index→serve→act`):

| Step kind | Lineage | Output (`kind`) |
| --------- | ------- | --------------- |
| `connect` | adds a **source** | `dataset` |
| `clean`   | —                 | `dataset` |
| `model`   | —                 | `model`   |
| `index`   | —                 | `index`   |
| `serve`   | —                 | *(none — it exposes existing artifacts)* |
| `act`     | —                 | *(none — it acts on the world)* |

- An output's **`kind`** is always one of the Run schema's closed set
  (`dataset` / `model` / `index`), so a produced run is **schema-valid by
  construction** (see *Boundary validation* below). Its **`ref`** is a logical
  handle tying the artifact back to the step that produced it
  (`<kind>:<step-id>`); no real table/model/index is materialized in R1.
- A **connect** step's lineage **source** is read from the step's free-form
  `attributes` bag: a `source`+`table` pair reads as `"source:table"`, else the
  first of `source` / `uri` / `path`, else the step's name.
- Every step that ran is recorded in `lineage.steps` (by name, falling back to
  id).

### Failing a step deterministically

A step fails when its `attributes` carry a truthy **`"fail"`** — `true` (a default
reason) or a non-empty string (used verbatim as the reason). This drives the
`Failed` path in tests and lets an author mark a step as known-broken without a
custom `StepExecutor`. For example:

```cpp
graph.add_node(StepKind::Model, "churn_model", {{"fail", "model did not converge"}});
```

## Boundary validation (JSON Schema)

This slice introduces **no new JSON format**: the only artifact it produces is a
`Run`, which already has its own draft-07 JSON Schema validated at the storage
boundary ([runs.md](runs.md) → *Boundary validation*). The orchestrator only ever
emits outputs whose `kind` is in that schema's closed enum, so a produced run is
schema-valid by construction — and reloading a node routes every `run.json`
through `parse_run_value`, so an invalid record would be rejected there. The
executor test asserts both: that `validate_run_value` accepts the produced value,
and that the run survives a restart (a reload through the boundary).

## Tests

`tests/pipeline/test_executor.cpp` covers the topological ordering (edge-respecting,
deterministic, cycle/self-loop rejection), the end-to-end acceptance flow (deploy
a churn graph, then execute it → a `Succeeded` run with the expected outputs +
lineage + logs), the `Failed` path (a failing step stops the run, downstream
steps are skipped, the failure is diagnosable), the empty/cyclic guards, that the
produced run is schema-valid and survives a restart, and that a custom
`StepExecutor` is honoured.

## Methodology run dispatch

The public `run` capability resolves the graph through the
[methodology registry](../ml/methodologies.md). A model step with an unknown model kind is refused.
A model step owned by a registered but unavailable methodology is refused with
that methodology's tracking issue. A graph with no model step may still use the
structural executor described above.

M-1's `run` callback uses `zetta::usecases::churn::ChurnStepExecutor` (#396). It
replaces `DefaultStepExecutor`'s logical-ref placeholders with actual distributed
accelerated compute for the UC-1 churn pipeline. Each heavy step runs as a recorded `ComputeJob`
(`connect`→load to DDF, `clean`→featurize, `model`→train GBDT + calibrate,
`index`→score/rank, `serve`→explain via TreeSHAP, `act`→retention playbook), so
the run produces a `Run` whose outputs reference a **real** `Model`, ranked
at-risk list, and drivers. Because `run_step` is a pure-per-step
function but the pipeline threads artifacts (feature table → model → ranked list →
drivers), the executor is constructed with its dependencies (the node, a started job
coordinator, a compute device, the input shards) and used by injection into a
`GraphExecutor`. See [churn-step-executor.md](../use-cases/churn-step-executor.md).

The API handler contains no M-1-specific dispatch. Later methodologies install
their own graph ownership and run callbacks in the registry without editing the
handler.

The **`index` step** has its own real `StepExecutor` —
[`zetta::index::IndexStepExecutor`](../indexing/index-step-executor.md) (#528) — which resolves the
step's backend from the node's `IndexRegistry`, builds a real index over the source
table's rows, records the [`Index`](../indexing/index-backend.md) entity, and emits an `index`
output referencing the real id (other step kinds delegate to `DefaultStepExecutor`),
so a built index is queryable. See [index-step-executor.md](../indexing/index-step-executor.md).

## What's next (later slices)

- **More real per-step compute** behind `StepExecutor` for the other use
  cases — materializing datasets as real tables/large objects, the distributed
  (fan-across-nodes) index build, and recording their real refs in the run's outputs.
- **Wiring the correctness gate** ([gate.md](gate.md)) as a pre-execution
  pre-flight (RULE-1), so a graph that fails semantic checks never runs.
- **Streaming + monitoring** — the Runs / Outputs / Lineage UI panel (SCR-6) that
  observes a run as it progresses, and model-derived confidence/freshness.
- **Executing from a deployment** — today `run_graph` takes the graph directly
  (a deployment records only a summary); once a deployment references a stored
  graph by id, a run can be launched straight from a deployment record.

A run's progress inside its graph is a derived read: `pipeline::project_run_progress`
(`run_progress.hpp`, #626) projects the run's logs + outputs onto the graph — per-step
pending/running/succeeded/failed, the failing reason, and each step's attributed
outputs — the engine of the headed Graph Workspace's
[live-run overlay](../gui/graph-run-overlay.md). It reads the exact log grammar the
executor writes, so the two are tested together (`tests/pipeline/test_run_progress.cpp`).

See also [runs.md](runs.md), [execution-graph.md](execution-graph.md),
[deployments.md](deployments.md), and [architecture.md](../architecture/architecture.md).

## What a run says it read (2026-09-27)

Found by lake's UX review (https://github.com/oramasearch/lake/issues/251,
https://github.com/oramasearch/lake/issues/253):

- **A table-backed churn run** (`dataset_path = "<database>.<table>"`, #947) records the dataset
  as its only lineage source (`dataset:<database>.<table>`) on its first connect step and logs
  `dataset: trained on <database>.<table>` plus each of `resolve_dataset`'s notes. Every other
  connect step logs `connect: <name> reuses dataset <database>.<table> already loaded by this run`
  instead of reporting the graph's own `source:table` as read. `StepOutcome::notes` carries these
  lines. Single-table S-1 graphs compose one connect step, so they do not imply a second source.
- **A run given no `dataset_path` reads its pipeline's own table** when there is one: the first
  connect step whose `source` + `table` attributes name a dataset this node holds, with the first
  model step's `target` as the label (`dataset.label_column`). It logs `dataset: read from the
  pipeline's connect step '<name>': <database>.<table>, label '<target>'` and `run` answers it in
  `dataset_notes`. Only a pipeline that names no existing table trains on the built-in demo
  dataset, as before. lake points a project's pipeline at a table this way (a project made from an
  Opportunity, *Build a Pipeline* from a dataset), and such runs used to train on the demo data
  silently. Verify: `Uc1Journey.ARunWithoutADatasetReadsItsPipelinesTable`.
- **Columns the model cannot use** are said: inference notes `not used: '<column>' (not numeric,
  and <why it is not a category>)`, and a text category the GBDT one-hot encodes is logged as
  `categorical feature: '<column>' one-hot encoded into <column>=<level>, …`
  ([categorical features](../data/categorical-features.md), #1203), and
  `get_run_report`'s `dataset` adds `source` and `not_used` (from the run's lineage and logs; a
  model's metrics hold numbers only). A table-backed report drops `events`, which it has none of.
- **A structural run** (a graph with no model step) logs a `warn`, `structural: … its steps
  ran without training or scoring anything …` (and that a given `dataset_path` was not read), and
  `run`'s answer carries the same sentence as `note`.
