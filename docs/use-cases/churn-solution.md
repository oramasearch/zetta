# Wiring the churn solution to the real engine (`zetta::usecases::churn::run_churn_solution_on_node`)

`zetta::usecases::churn::run_churn_solution_on_node`
(`include/zetta/usecases/churn/churn_solution.hpp`) is **the ONE public churn
runner** (#637) — the bridge from the catalog **churn-prediction** blueprint — the
one a user picks in the [Deploy panel](../pipeline/solution-catalog.md) — to a
**real trained [Model](../ml/models.md) + ranked at-risk list + churn drivers**,
running the actual featurize → train → explain → rank → playbook pipeline instead
of the stubbed [`DefaultStepExecutor`](../pipeline/execution.md), over the node's
live [`JobCoordinator`](../compute/job-coordinator.md) (the #579 spine). It
realizes **FEAT-4 / UC-1** (#398/#580) and is part of the native distributed
accelerated-compute pass (#264). Every in-process churn submitter — the headed
Deploy panel, the governed console's churn-shaped path, the testkit cases, the
acceptance tests — routes through it; there is no second runner to wire a
coordinator for.

```
include/zetta/usecases/churn/churn_solution.hpp   run_churn_solution_on_node (the one runner),
                                   ChurnSolutionOptions/Result, encode_churn_dataset,
                                   the standard churn config, demo_churn_dataset
src/core/usecases/churn/churn_solution.cpp        the encoder + config builders + the runner
                                   (+ run_churn_solution_with_coordinator, the
                                   file-local isolated-coordinator detail)
```

## Why a bridge was needed

The real per-step compute already exists as
[`zetta::usecases::churn::ChurnStepExecutor`](churn-step-executor.md) (#396), but it is driven **by
injection** into a `GraphExecutor` and needs things the `StepExecutor` seam can't
supply on its own:

- a started [`JobCoordinator`](../compute/job-coordinator.md) + a
  [`ComputeDevice`](../compute/compute-device.md), and
- this node's **numeric input shards** — the churn pipeline operates purely on
  numeric DDF columns, so the dataset's string ids / ISO timestamps / categorical
  event types must be encoded to numbers first (the "featurize/encode step
  upstream" `ChurnPipelineConfig` documents).

Two things also had to line up for *the catalog blueprint* specifically:

1. The catalog churn graph was `connect×2 → clean → model → serve → act` — **missing
   the `index` step**. The `ChurnStepExecutor` maps `index → score / rank`, and the
   `act` (retention playbook) step needs that ranked at-risk list. #398 added the
   `index` step so the blueprint is `connect×2 → clean → model → index → serve →
   act`, matching the real pipeline. (The blueprint still passes the
   [gate](../pipeline/gate.md); no test pins its shape.)
2. The fixture → numeric encoding lived inline in the test harness. #398 promotes it
   to a first-class, reusable core surface so the wiring is legible and testable, and
   so the headed Deploy panel and the acceptance test share one path.

## The pieces

| Function | What it does |
|---|---|
| `encode_churn_dataset(dataset)` | Map a [`ChurnDataset`](churn-dataset.md) to numeric DDF rows: dense integer `entity` keys by file order, a monotonic day-number `ts` (so `time <= as_of` is leakage-correct for any year), the raw `value`, and an integer event-`type` code; plus the dataset's `as_of` in the same day-number units. |
| `churn_events_schema()` / `churn_accounts_schema()` | The DDF schema of those encoded shards — the encoder's output contract (events: `entity`/`ts`/`value`/`type`; accounts: `entity`/`mrr`/`seats`/`tenure_days`/`churned`). |
| `churn_featurize_spec(as_of)` | The standard leakage-free [featurization](../ddf/ddf-featurize.md) graph over the encoded shards (the `churn_features.sql` shape): windowed `failed_payments` / `support_severity` / `total_events` at or before `as_of`, the carried static account features, the `churned` label. |
| `default_churn_gbdt_params()` | The proven, fast [GBDT](../ml/gbdt.md) config (30 trees / depth 3 / 16 bins / lr 0.3) — light enough to run on a click. |
| `run_churn_solution_on_node(node, device, graph, dataset, options)` | THE runner: resolve `node.job_coordinator()` → encode → build the pipeline config → inject a `ChurnStepExecutor` into a `GraphExecutor` → execute `graph`. Returns the Run plus the real artifacts. |
| `demo_churn_dataset()` | A small, deterministic, in-memory churn dataset (no files) with a clean failed-payments churn signal — the data the headed "Run over the cluster" action trains on. |

`run_churn_solution_on_node` returns a `ChurnSolutionResult`:

```cpp
struct ChurnSolutionResult {
    ExecutionResult execution;  // the Run handle + ok / failed_step / error
    std::string model_id;       // the registered Model id (resolvable via find_model)
    std::vector<ml::EntityScore> ranked;   // the global at-risk list, highest first
    std::vector<std::pair<std::string,double>> drivers;  // feature -> mean |phi|, desc
    std::string top_driver;     // the globally dominant churn driver
    std::vector<RetentionAction> playbook;  // at-risk accounts + their dominant driver
    bool ok() const { return execution.ok; }
};
```

## No new JSON format

This module adds **no parser of its own**. Everything rides an existing validated
boundary: the `ChurnDataset` is validated by
[`load_churn_dataset`](churn-dataset.md), the catalog blueprint graph by
[`parse_solution_catalog`](../pipeline/solution-catalog.md), and the
[Run](../pipeline/runs.md) / [ComputeJob](../compute/compute-jobs.md) / [GBDT-weights](../ml/gbdt.md) records at
their own schema boundaries. The encoder's inputs are typed structs, not parsed
JSON.

## Single node here, SPMD elsewhere

`run_churn_solution_on_node` runs the dataset it is handed on **this** node (the
whole dataset, this node's shard). On a real multi-node cluster each node
runs the same graph against its own shard and the underlying jobs combine partials
over the [collectives](../cluster/collectives.md) — the single-node == multi-node equivalence is
proven in the per-op job suites and the [native oracle](../ml/native-oracle.md); this
bridge does not re-prove it.

## One runner, over the live cluster (#580, #637)

The runner goes through the node's **long-lived `JobCoordinator` over its real
`Cluster`** (the [#579 spine](../compute/job-coordinator.md), `Node::attach_cluster`
/ `Node::job_coordinator`) — so the featurize / train / explain / rank jobs combine
their partials over the node's **real cluster membership** (the
[collectives](../cluster/collectives.md) ride the
[signal plane](../cluster/signal-plane.md)) and the produced Run + Model + ranked
list land on the live node:

```cpp
ChurnSolutionResult run_churn_solution_on_node(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const ChurnDataset& dataset,
    const ChurnSolutionOptions& options = {});
```

Historically there was a second public runner (`run_churn_solution`) that took an
explicitly-supplied — usually isolated single-node loopback — coordinator, and six
parallel ways to wire a churn run; #637 consolidated them. The
isolated-coordinator wiring survives only as a clearly-named internal detail of
`churn_solution.cpp` (`run_churn_solution_with_coordinator`, the shared body the
public runner delegates to); no caller outside the module wires its own
coordinator. A node with **no compute plane attached** (a standalone node, or one
not yet `attach_cluster`'d) has no live coordinator: the runner returns a failed
`ChurnSolutionResult` with `execution.error` set (it runs nothing — the caller
surfaces the reason), so it never silently no-ops. This one entry point is what
the MCP `run` tool (#617) and the cross-process SPMD co-launch (#601) build on.

**Coordinated membership (#601).** The underlying jobs are SPMD: every collective
round waits for *all* connected peers (a bounded timeout), so a lone submitter over
a multi-node cluster would fail those rounds. The runner therefore **co-launches**:
on a cluster with peers it snapshots the roster (the collective participants),
fans a `run.launch` task out over the
[coordinated launch seam](../compute/coordinated-launch.md), and runs its own
**disjoint entity shard** of the encoded dataset (`entity % roster-size == rank`);
every peer's listener co-runs the same graph over its own shard on its
node-owning thread, the collectives combine the partials to one global model, and
the root-registered `Model` replicates to every member. On a standalone or
single-member cluster the roster is just this node and the run is the local
whole-dataset run it always was (`options.coordinate = false` forces that
behavior). Each peer **re-gates the graph before co-running** — RULE-1 holds on
every member, not just the initiator.

`is_churn_graph(graph)` is the companion predicate: `true` when a graph has the
featurize → train backbone the pipeline needs (≥1 `connect`, ≥1 `clean`, ≥1 `model`
step). It decides whether an interactive sim-Python program runs on the real churn
engine (this `true`) or the structural `DefaultStepExecutor` (`false`).

## In the headed app

Two interactive surfaces route through the live coordinator (#580):

- The [Deploy panel](../pipeline/solution-catalog.md#run-on-the-real-engine-churn)'s **"Run over
  the cluster"** action for the churn blueprint (`App::run_churn_demo`) starts an
  **`AsyncChurnRun`** over the `demo_churn_dataset()` — the run executes OFF the
  UI thread on a core worker whose node writes gate through `Node::run_on_owner`
  (#624, [../architecture/owner-gate.md](../architecture/owner-gate.md)), so the
  panel renders live progress from the Run + jobs records and offers a
  cooperative between-steps cancel. The Graph Workspace's "Run graph" routes a
  churn-shaped graph through the same shared handle (the #626 overlay animates
  it live). The headed app always attaches a cluster; if none is attached the
  runner's clear failure reason is surfaced as the run status (no throwaway
  loopback fallback, #637).
- The [governed Console](../governance/governed-console.md) routes a **churn-shaped sim-Python
  program** (`is_churn_graph`) through the real engine on the live coordinator via the
  console's injectable **graph runner** seam — still capability-scoped + gate-checked +
  audited (a non-churn graph stays on `Node::run_graph`).

Either way the produced Model + Run + ComputeJobs register on the **live** node, so
the [Runs](../pipeline/runs.md), [Models](../gui/models-panel.md), and [Jobs](../gui/jobs-panel.md) panels
reflect the run.

## Tests

`tests/usecases/churn/test_churn_solution.cpp` is the #398/#580 acceptance — every
run in it goes through `run_churn_solution_on_node` over an `attach_cluster`'d
node (#637):

- **Deploy + run the catalog churn solution.** It takes
  `SolutionCatalog::builtin().find("churn-prediction")`, deploys it (the gate passes
  and `deploy_graph` records the placement), then runs it over the committed
  [churn fixture](churn-dataset.md). It
  asserts the Run is `Succeeded` and schema-valid, a `model` output resolves to a
  **real registered `Model`** (kind `classifier`), an `index` output backs a ranked
  at-risk list (one per account, descending), `failed_payments` is the top driver,
  the playbook joins the list with each entity's driver, and one
  featurize/train/score/explain `ComputeJob` ran and `Succeeded`.
- **The demo dataset runs end to end** on the same catalog graph, with
  `failed_payments` as its only class-separating signal.
- **The Model + Run land on the live node (#580).** The trained `Model` registers
  on the node that ran (`find_model` resolves it, `runs()` is non-empty).
  A standalone node (never `attach_cluster`'d, so no coordinator) fails clearly and
  registers no Model; `is_churn_graph` recognizes the catalog backbone and rejects a
  graph missing the featurize step.

`tests/usecases/churn/test_churn_coordinated_launch.cpp` is the #601 acceptance —
the coordinated cross-process launch over a REAL replicated 3-member loopback
cluster (each member wired the way `main` wires a live process, each peer on its
own tick thread): the coordinated run's ranked list / risks / top driver match a
single-member oracle (single-node == multi-node, every account counted exactly
once), and ONE logical Model registers (the collective root's) and replicates so
the same id resolves on every member; plus the `coordinate = false` opt-out.

See also [churn-step-executor.md](churn-step-executor.md) (the real per-step
compute), [solution-catalog.md](../pipeline/solution-catalog.md) (the catalog + Deploy panel),
[gbdt-score.md](../ml/gbdt-score.md) (the rank-at-risk `index` step), and
[tree-shap.md](../ml/tree-shap.md) (the `serve` drivers).
