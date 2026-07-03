# UC-1 at scale — the sim-Python E2E (TC-12)

The **capstone scale test** (issue **#531**, realizes **TC-12 / #502**): one
canonical **sim-Python program** drives the whole UC-1 pipeline at scale over a
**real, manually-sized cluster** —

```
load the saas-sales-conversations parquet as a PARTITIONED table
  → add permanent nodes
    → featurize the engineered columns, compute-near-data over the partitions
      → train a calibrated GBDT on `outcome` over the RESULTING feature table
        → explain (per-row TreeSHAP drivers) + rank (the at-risk list)
```

— **gated** (RULE-1) and **governed** (key-scoped access + an audit trail). The
program doubles as the **canonical usage example**: it is the same script an
operator runs for real in the headed app (start a node, add cloud nodes behind the
[consent gate](../cloud/cloud-inspector.md), launch the case). It composes already-landed,
unit-tested pieces — it adds no new engine code, only the end-to-end wiring +
assertions.

```
src/core/testkit/cases_uc1_scale.cpp   the case + the shared canonical script
src/core/testkit/cases_examples.cpp    examples.uc1_at_scale (the example registration)
tests/testkit/test_testkit.cpp                 DistributedCasesRunOnARealCluster (headless gtest)
```

## The canonical program

```python
nodes = cluster.add_nodes(10, provider='azure')
conversations = load_dataset('saas-sales-conversations', columns=['messages',
    'avg_response_seconds', 'sentiment', 'questions_asked', 'outcome'])
model = train(conversations, target='outcome')
drivers = explain(model)
at_risk = rank(model)
```

It lowers through the [simulated-Python frontend](../pipeline/python-frontend.md) to the
[ExecutionGraph IR](../pipeline/execution-graph.md) over the
`connect → … → act` taxonomy: `cluster.add_nodes` → an `act` control step,
`load_dataset` → `connect`, `train` → `model`, `explain` → `serve`, `rank` →
`index`; the variable refs become the data-flow edges
(`load_dataset → train → explain`, `train → rank`).

This **one source string** backs two registered tests (so it stays a single source
of truth — `uc1_at_scale_script()`):

- **`examples.uc1_at_scale`** — a lightweight [sim-Python example](../testing/example-scripts.md)
  under `TestCategory::Examples`: the standard harness compiles it, runs it through
  the [gate](../pipeline/gate.md), and executes it **single-node** (the
  [`DefaultStepExecutor`](../pipeline/execution.md)), asserting the **lowered shape**
  (5 steps, 3 edges, each verb's `StepKind`) and that it runs to `Succeeded`
  producing `dataset` / `model` / `index` outputs. It proves the program is real,
  well-formed sim-Python.
- **`cluster.uc1_at_scale`** — the heavy, real-engine case below, under
  `TestCategory::Distributed`.

## What `cluster.uc1_at_scale` does

The script is the **spec**: the case reads the data shape and node count **from**
the lowered graph (the `load_dataset` step's column projection via
[`dataset_load_spec`](../pipeline/datasets-shim.md), the `cluster.add_nodes` count via
[`add_nodes_count`](../cloud/add-nodes.md), the `train` step's `target=` kwarg), then runs
the real pipeline:

1. **Compile + gate.** Compile the program; run [`default_gate()`](../pipeline/gate.md)
   (RULE-1) — a blocked gate fails the case.
2. **Load the saas parquet, partitioned.** Decode the embedded
   `saas_sample.parquet` fixture with the native
   [Parquet reader](../data/parquet-reader.md) (`read_parquet_rows` — no Apache Arrow),
   **projecting to the script's columns and dropping the `embedding_*` columns**.
   The real fixture seeds the dataset; it is scaled to the chosen size with
   deterministic, saas-shaped synthetic conversations (a planted-but-imperfect
   signal, ~9% label noise) so the at-scale train + calibration are meaningful —
   the way an operator's real run loads the full ~100k-row dataset.
3. **Stand up the cluster + shard.** Bring up a real
   [`LoopbackCluster`](../testing/test-suite.md) of `nodes` nodes; each node writes its
   round-robin shard into the partitions it **owns** via the
   [partitioned write path](../data/partitioned-write-path.md)
   (`write_partitioned` + a `RoundRobinPartitioner`) — the
   [compute-near-data](../compute/compute-near-data.md) layout.
4. **Add permanent nodes.** Execute the lowered `cluster.add_nodes(10,
   provider='azure')` step through [`run_add_nodes_step`](../cloud/add-nodes.md). The
   `provider=` is **provenance only** — the `Provisioner` is **injected**, so the
   cloud stays consent-gated; here a [`LocalProvisioner`](../cloud/provisioner.md) over the
   live cluster fleet stands in for the Azure burst, keeping the case hermetic and
   headless.
5. **Govern.** On the driver node, a `uc1-pipeline` service
   [principal](../governance/access-control.md) holds a key scoped to the `uc1` project + `data`
   database with `orchestration:start_run` + `data:read` capabilities. The case
   authorizes against it (in-scope allowed; out-of-scope denied — deny-by-default).
6. **Featurize + train near data, SPMD.** Every node concurrently runs
   [`run_near_data_featurize_train_job`](uc1-featurize-train-over-partitions.md)
   (#530) with a static [`FeaturizeSpec`](../ddf/ddf-featurize.md) built from the
   script (the projected columns as `account_features`, the `target=` kwarg as the
   label): load this node's owned partitions once, featurize (the broadcast feature
   table, identical on every node), then train (+ calibrate) → explain (per-row
   TreeSHAP) → rank (the at-risk list) **over the feature table** — not the raw
   engineered columns — each a recorded [`ComputeJob`](../compute/compute-jobs.md).
   Only the per-node partials cross the wire.

## What it asserts

- **A real feature table + model + drivers + ranking on every node** — the pipeline
  `Succeeded`, the broadcast feature table covers every conversation with the
  script's columns and is identical on every node, its `featurize` job is recorded,
  a calibrated [`Model`](../ml/models.md) is registered (`find_model`), the per-row
  TreeSHAP drivers are in `feature_columns` order with a non-empty top driver, and
  the at-risk list ranks every conversation once by risk descending, each risk in
  `[0, 1]`. The model learned the signal (AUC > 0.6).
- **Single-node == multi-node** — every node converges on the **identical** model +
  ranking (the trees grow off globally-allreduced histograms, the result is
  broadcast). The rigorous single-node-vs-N-node oracle equivalence is proven in
  [`tests/ml/test_gbdt_train_partitions.cpp`](uc1-train-over-partitions.md).
- **Compute-near-data** — each node read its own shard off local disk (the raw
  data never crosses the wire) and, multi-node, read only its shard — not the whole
  dataset; every conversation was read near data and explained exactly once. Only
  the training partials (gradient histograms, SHAP, scores) cross the wire — for
  iterative GBDT training those partials can exceed the tiny local shard, so the
  near-data guarantee is "the raw data stays local", not "fewer bytes move".
- **Gated + governed** — the gate passed, the scoped key authorized the run, and the
  immutable [audit trail](../governance/audit-trail.md) captured the access decisions + each
  job's `created` + `finished` events.

## Options (manually-sized)

| option | default | meaning |
|---|---|---|
| `nodes` | 3 (1–6) | cluster size — the "manually-sized cluster" |
| `rows` | 96 | conversations (the saas fixture, scaled up) |
| `calibrate` | true | fit held-out Platt calibration on the model |

The script names **10** nodes (what an operator bursts for real); the loopback
case is sized by the `nodes` option, with the requested-vs-provisioned counts
reported in the result metrics.

## Run it

```bash
# headless gtest (nodes=2, rows=96)
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
# the lightweight example
ctest --test-dir build -R 'ExampleScripts' --output-on-failure
```

Or run `cluster.uc1_at_scale` interactively from the headed **Operations → Test
suite** panel (pick the node count, watch it run over a real loopback cluster).

## Relation

- The **pipeline step** it runs is the
  [featurize → train composition](uc1-featurize-train-over-partitions.md) (#530),
  which composes [featurize engineered columns over partitions](../ddf/ddf-featurize.md)
  (#532) with [train GBDT(outcome) over partitions](uc1-train-over-partitions.md)
  (#533); all ride the [compute-near-data](../compute/compute-near-data.md) loader (#516).
- It is the at-scale sibling of the single-node [UC-1 end-to-end](uc1-end-to-end.md)
  (#399), and the capstone of **UC-4** (#267, burst-train on temporary nodes) — the
  add-nodes + near-data-train control surface UC-4 builds on.
