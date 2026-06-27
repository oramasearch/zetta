# Real StepExecutors — the churn pipeline (`zetta::ChurnStepExecutor`)

`zetta::ChurnStepExecutor` is the **real** per-step executor for the UC-1 churn
pipeline (#396) — the slice that replaces [`DefaultStepExecutor`](execution.md)'s
logical-ref placeholders ("no real model/index is materialized in R1") with actual
**native distributed accelerated compute**, behind the same `StepExecutor` seam and
**without touching the orchestrator** (`GraphExecutor`'s walk is unchanged). It
realizes **FEAT-5 / FEAT-6 / UC-1** and is part of the native distributed
accelerated-compute pass (#264).

```
include/zetta/churn_step_executor.hpp   ChurnStepExecutor, ChurnPipelineConfig,
                                        RetentionAction
src/core/churn_step_executor.cpp        the per-kind compute (each a ComputeJob)
```

It is the consumer every prior compute slice was built toward: it **composes** the
already-built, already-tested building blocks — featurization (#388), GBDT training
+ calibration (#390 / #391), batch scoring / rank-at-risk (#392), and TreeSHAP
explanation (#393) — driving each heavy step as a recorded
[`ComputeJob`](compute-jobs.md) through the [job coordinator](job-coordinator.md).

## The kind → compute mapping

`DefaultStepExecutor` interprets the `connect→clean→model→index→serve→act` taxonomy
to derive *logical* artifacts. `ChurnStepExecutor` interprets the same taxonomy to run
*real* compute:

| Step kind | Real compute | Output (`kind`) |
| --------- | ------------ | --------------- |
| `connect` | **load to DDF** — `load_data_frame` reads this node's numeric shard into a typed, chunked, device-backed [`DataFrame`](ddf.md) | `dataset` + a lineage source |
| `clean`   | **featurize** — [`run_featurize_job`](ddf-featurize.md) builds the leakage-free point-in-time feature table (a `featurize` job), materialized to a device feature frame | `dataset` |
| `model`   | **train + calibrate** — [`run_train_job`](gbdt.md) trains a histogram GBDT (a `train` job) and **registers a real [`Model`](models.md)** on the node; opt-in held-out [calibration](calibration.md) | `model` (ref = the **real Model id**) |
| `index`   | **score / rank** — [`run_score_job`](gbdt-score.md) produces the global **at-risk list** (a `score` job), highest risk first | `index` |
| `serve`   | **explain** — [`run_explain_job`](tree-shap.md) computes per-entity TreeSHAP + the population mean-\|φ\| **driver ranking** (an `explain` job) | `dataset` |
| `act`     | **retention playbook** — host-side join of the at-risk list with each entity's dominant SHAP driver (who to contact + why) | `dataset` |

Every step that materializes an artifact emits one of the Run schema's **closed
output kinds** (`dataset` / `model` / `index`), so the produced `Run` is schema-valid
by construction (see *Boundary validation* below). The `model` step's output `ref` is
the **real registered `Model` id** — `Node::find_model(ref)` resolves it — which is
exactly the acceptance: `run_graph` over the churn graph produces a `Run` whose
outputs reference a real `Model` + ranked list + drivers.

`act` is the one step that is **not** a heavy `ComputeJob` — it composes the `index`
and `serve` outputs (the ranked list + per-entity drivers) into the playbook
host-side.

## Stateful across steps, by design

The `StepExecutor` seam is a **pure function of one step**
(`run_step(const ir::Node&) const → StepOutcome`), but real churn compute **threads
artifacts down the pipeline**: the feature table feeds training, the trained model
feeds scoring and explanation, and the ranked list + drivers feed the playbook. So
`ChurnStepExecutor`:

- **carries the pipeline state in a pImpl**, mutated through a `unique_ptr` member —
  `run_step` stays `const` and the orchestrator only ever holds a
  `const StepExecutor&`, exactly as before; and
- **holds the dependencies the seam can't supply** — the `Node` (to create jobs +
  register the model), a started [`JobCoordinator`](job-coordinator.md), a
  [`ComputeDevice`](compute-device.md), and this node's input shards + the specs.

Because it accumulates state, **one instance drives exactly one execution** —
construct a fresh `ChurnStepExecutor` per run.

A step that runs out of order (e.g. `model` before `clean` built features) or whose
underlying job fails returns `ok=false` with a reason; the orchestrator then
**fail-fasts** the run (later steps don't run, the run ends `Failed`) — the same
behavior as the default executor.

## Using it

`ChurnStepExecutor` is used by **injection into a `GraphExecutor`**, not via
`Node::run_graph` — `run_graph` builds a default executor and can't conjure a
coordinator or input data, so it stays on `DefaultStepExecutor`. Real compute is
driven like this:

```cpp
#include "zetta/churn_step_executor.hpp"
#include "zetta/executor.hpp"

// A started coordinator over this node's cluster, and a compute device.
zetta::compute::JobCoordinator coordinator{cluster, /*worker_count=*/2};
coordinator.start();
zetta::compute::CpuDevice device;   // or a CUDA device when present

// This node's numeric input shards + the churn specs.
zetta::ChurnPipelineConfig config;
config.events_store    = &events_store;     // numeric events rows (non-owning)
config.events_schema   = events_schema;
config.accounts_store  = &accounts_store;   // numeric accounts rows (non-owning)
config.accounts_schema = accounts_schema;
config.featurize       = churn_featurize_spec;
config.params          = gbdt_params;
config.entity_column   = "entity";

auto churn = std::make_shared<zetta::ChurnStepExecutor>(
    node, coordinator, device, std::move(config));

zetta::GraphExecutor executor{churn};
zetta::ExecutionResult result =
    executor.execute(node, churn_graph, "churn-project", "churn-graph-1");

// result.run->outputs() now references the real Model + ranked list + drivers:
churn->model_id();        // the registered Model id (Node::find_model resolves it)
churn->ranked();          // the at-risk list (highest risk first)
churn->top_driver();      // the dominant global churn driver ("failed_payments")
churn->playbook();        // at-risk accounts + their dominant driver
```

The `churn_graph` is the intent compiler's churn skeleton
([intent-compiler.md](intent-compiler.md)) — two `connect` sources → a `clean` feature
build → a `model` → an `index` → a `serve` → an `act` — so it lines up exactly with
the kind→compute mapping above.

### Distributed (multi-node)

`coordinator.start()` must have run. The model is **SPMD**: on a multi-node cluster,
every participating node runs the same churn graph against **its own shard**
concurrently, and the underlying jobs combine per-node partials over the
[collectives](collectives.md) (the featurize join + group-by, the gradient-histogram
allreduce, the score gather/rank/broadcast, the SHAP mean-|φ| allreduce). Because each
of those jobs is proven **single-node == multi-node** in its own suite, the produced
artifacts are equivalent regardless of sharding; the trained `Model` is registered on
the deterministic collective root and replicates to peers. The inputs (the shard
stores) are this node's slice of the data.

### Inputs are numeric

Both shards are **numeric** DDF stores, like the rest of DDF: an integer entity-key in
both, a numeric timestamp + the value / type-code columns in the events shard, and the
static account columns + the label in the accounts shard. A featurize/encode step
upstream maps string ids / ISO-8601 timestamps / categorical event types into those
numeric columns (the same encoding the GBDT / TreeSHAP fixtures use); this executor
stays purely numeric.

## Boundary validation (JSON Schema)

Like `DefaultStepExecutor`, this slice introduces **no new JSON format** — every JSON
it touches enters through an **existing validated boundary**:

- the produced **[`Run`](runs.md)** is validated against the Run JSON Schema (and the
  executor only ever emits the schema's closed output kinds, so the run is valid by
  construction — the test asserts `validate_run_value` accepts the produced value);
- each heavy step's **[`ComputeJob`](compute-jobs.md)** record is validated at its own
  boundary;
- the trained **[`Model`](models.md)**'s serialized GBDT weights body rides the
  `gbdt_model.schema.json` boundary (re-validated when read back from disk / a peer);
- the **[`FeaturizeSpec`](ddf-featurize.md)** / **`GbdtParams`** carry their own
  validated boundaries.

So malformed data still fails fast at each existing seam; there is nothing new to
schema-validate here. The executor's own config (`ChurnPipelineConfig`) is a typed C++
struct, not parsed JSON.

## Tests

`tests/test_churn_step_executor.cpp` is the integration test (single-node loopback
cluster + a started coordinator):

- **Acceptance** — `run_graph` over the churn graph (`compile_intent`'s churn skeleton)
  on the [synthetic churn fixture](churn-dataset.md) produces a **Succeeded**,
  schema-valid `Run` whose outputs reference a **real `Model`** (the `model` output's
  ref resolves via `Node::find_model` to a `classifier`), an **`index`** at-risk list
  (non-empty, ranked descending, one row per account), and **drivers** (a `dataset`
  output; the dominant global driver is `failed_payments`), with a retention
  **playbook** joining the two. It also asserts each heavy step ran as the expected
  recorded `ComputeJob` (one each of `featurize` / `train` / `score` / `explain`, all
  `Succeeded`).
- **Fail-fast** — a graph whose configured shards are empty fails the run (no features
  → nothing trainable), leaving it `Failed` with a diagnosable failing step and no
  registered model.

The single-node == multi-node equivalence of each composed job is proven in the
per-op suites (`test_ddf_featurize`, `test_gbdt`, `test_gbdt_score`, `test_tree_shap`).

## What's next (later slices)

- **Persisting the artifacts** the run references as first-class entities — the at-risk
  scores + the SHAP driver ranking as datasets / Run outputs with real refs (today the
  `index` / `serve` / `act` outputs carry logical handles, with the computed results
  exposed on the executor for the caller).
- **Real `connect` from stored tables** — `ddf::scan` over a registered numeric table
  (plus a reusable churn encode step) instead of injected stores.
- **GPU-native numerics** — the ML kernels are host-side references over device-backed
  data today; pushing them onto device kernels is the optimization behind the same API
  (see *AI compute — GPU-native, CPU fallback, distributed* in `CLAUDE.md`).
- **Real executors for the other use cases** behind the same seam, the way this one
  realizes churn.

See also [execution.md](execution.md), [execution-graph.md](execution-graph.md),
[ddf-featurize.md](ddf-featurize.md), [gbdt.md](gbdt.md),
[gbdt-score.md](gbdt-score.md), [tree-shap.md](tree-shap.md), and
[job-coordinator.md](job-coordinator.md).
