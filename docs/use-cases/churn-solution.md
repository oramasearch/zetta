# Wiring the churn solution to the real engine (`zetta::run_churn_solution`)

`zetta::run_churn_solution` (`include/zetta/churn_solution.hpp`) is the bridge from
the catalog **churn-prediction** blueprint — the one a user picks in the
[Deploy panel](../pipeline/solution-catalog.md) — to a **real trained [Model](../ml/models.md) +
ranked at-risk list + churn drivers**, running the actual
featurize → train → explain → rank → playbook pipeline instead of the stubbed
[`DefaultStepExecutor`](../pipeline/execution.md). It realizes **FEAT-4 / UC-1** (#398) and is
part of the native distributed accelerated-compute pass (#264).

```
include/zetta/churn_solution.hpp   run_churn_solution, ChurnSolutionOptions/Result,
                                   encode_churn_dataset, the standard churn config,
                                   demo_churn_dataset
src/core/churn_solution.cpp        the encoder + config builders + the runner
```

## Why a bridge was needed

The real per-step compute already exists as
[`zetta::ChurnStepExecutor`](churn-step-executor.md) (#396), but it is driven **by
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
| `run_churn_solution(node, coordinator, device, graph, dataset, options)` | The runner: encode → build the pipeline config → inject a `ChurnStepExecutor` into a `GraphExecutor` → execute `graph`. Returns the Run plus the real artifacts. |
| `demo_churn_dataset()` | A small, deterministic, in-memory churn dataset (no files) with a clean failed-payments churn signal — the data the headed "Run over the cluster" action trains on. |

`run_churn_solution` returns a `ChurnSolutionResult`:

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

`run_churn_solution` runs the dataset it is handed on **this** node (the whole
dataset on a single-node compute harness). On a real multi-node cluster each node
runs the same graph against its own shard and the underlying jobs combine partials
over the [collectives](../cluster/collectives.md) — the single-node == multi-node equivalence is
proven in the per-op job suites and the [native oracle](../ml/native-oracle.md); this
bridge does not re-prove it.

## Run over the live cluster (#580)

`run_churn_solution` takes a `JobCoordinator&` — originally an **isolated single-node
loopback** one, spun up just for the run so its SPMD collectives never block on this
node's real peers. Now that a running node owns a **long-lived `JobCoordinator` over
its real `Cluster`** (the [#579 spine](../compute/job-coordinator.md), `Node::attach_cluster` /
`Node::job_coordinator`), an *interactive* run should go through **that** — so the
featurize / train / explain / rank jobs combine their partials over the node's **real
cluster membership** (the [collectives](../cluster/collectives.md) ride the
[signal plane](../cluster/signal-plane.md)) and the produced Run + Model + ranked list land on
the live node:

```cpp
ChurnSolutionResult run_churn_solution_on_node(
    Node& node, compute::ComputeDevice& device,
    const ir::ExecutionGraph& graph, const ChurnDataset& dataset,
    const ChurnSolutionOptions& options = {});
```

It resolves `node.job_coordinator()` and delegates to `run_churn_solution` — same
encoding, specs, and artifacts, **no new JSON format**. A node with **no compute
plane attached** (a standalone node, or one not yet `attach_cluster`'d) has no live
coordinator: it returns a failed `ChurnSolutionResult` with `execution.error` set (it
runs nothing — the caller surfaces the reason or falls back to its own coordinator),
so it never silently no-ops.

**Single-node-or-coordinated membership.** The underlying jobs are SPMD: every
collective round waits for *all* connected peers (a bounded timeout). A single node
submitting this over a multi-node cluster whose peers are **not** co-running the same
graph fails those rounds. So today it is correct for the common interactive case — a
standalone node, or a single-member cluster, where the live coordinator **is** the
cluster. The collectives already handle the **combine**; the missing piece is the
**co-launch** — telling every peer to co-run the same SPMD graph (a coordinated
cross-process launch over the signal plane), a later slice.

`is_churn_graph(graph)` is the companion predicate: `true` when a graph has the
featurize → train backbone the pipeline needs (≥1 `connect`, ≥1 `clean`, ≥1 `model`
step). It decides whether an interactive sim-Python program runs on the real churn
engine (this `true`) or the structural `DefaultStepExecutor` (`false`).

## In the headed app

Two interactive surfaces route through the live coordinator (#580):

- The [Deploy panel](../pipeline/solution-catalog.md#run-on-the-real-engine-churn)'s **"Run over
  the cluster"** action for the churn blueprint (`App::run_churn_demo`) calls
  `run_churn_solution_on_node` over a `CpuDevice` + the `demo_churn_dataset()` —
  synchronous on the UI thread, the same per-click pattern as the
  [Cloud Inspector](../cloud/cloud-inspector.md). (It falls back to a throwaway loopback only
  if no cluster is attached — a safety net the headed app, which always attaches a
  cluster, never hits.)
- The [governed Console](../governance/governed-console.md) routes a **churn-shaped sim-Python
  program** (`is_churn_graph`) through the real engine on the live coordinator via the
  console's injectable **graph runner** seam — still capability-scoped + gate-checked +
  audited (a non-churn graph stays on `Node::run_graph`).

Either way the produced Model + Run + ComputeJobs register on the **live** node, so
the [Runs](../pipeline/runs.md), [Models](../gui/models-panel.md), and [Jobs](../gui/jobs-panel.md) panels
reflect the run.

## Tests

`tests/test_churn_solution.cpp` is the #398 acceptance:

- **Deploy + run the catalog churn solution.** It takes
  `SolutionCatalog::builtin().find("churn-prediction")`, deploys it (the gate passes
  and `deploy_graph` records the placement), then runs it through
  `run_churn_solution` over the committed [churn fixture](churn-dataset.md). It
  asserts the Run is `Succeeded` and schema-valid, a `model` output resolves to a
  **real registered `Model`** (kind `classifier`), an `index` output backs a ranked
  at-risk list (one per account, descending), `failed_payments` is the top driver,
  the playbook joins the list with each entity's driver, and one
  featurize/train/score/explain `ComputeJob` ran and `Succeeded`.
- **The demo dataset runs end to end** on the same catalog graph, with
  `failed_payments` as its only class-separating signal.
- **Run over the node's live coordinator (#580).** A node that has been
  `attach_cluster`'d is run via `run_churn_solution_on_node` (no coordinator
  argument) — it asserts the Run succeeds and the trained `Model` registers on the
  **live** node (`find_model` resolves it, `runs()` is non-empty), the #580
  acceptance. A standalone node (no coordinator) fails clearly and registers no
  Model; `is_churn_graph` recognizes the catalog backbone and rejects a graph missing
  the featurize step.

See also [churn-step-executor.md](churn-step-executor.md) (the real per-step
compute), [solution-catalog.md](../pipeline/solution-catalog.md) (the catalog + Deploy panel),
[gbdt-score.md](../ml/gbdt-score.md) (the rank-at-risk `index` step), and
[tree-shap.md](../ml/tree-shap.md) (the `serve` drivers).
