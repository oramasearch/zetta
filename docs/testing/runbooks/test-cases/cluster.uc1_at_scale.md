# UC-1 at scale (TC-12) · `cluster.uc1_at_scale`

**Category:** Distributed & cluster · **Source:** `src/core/testkit/cases_uc1_scale.cpp` · **Engine doc:** [uc1-at-scale-e2e.md](../../../use-cases/uc1-at-scale-e2e.md)

The capstone scale test. One canonical sim-Python program drives the whole UC-1
pipeline over a real, manually-sized loopback cluster: **load the
saas-sales-conversations parquet as a partitioned table → add permanent nodes →
featurize the engineered columns compute-near-data over the partitions → train a
calibrated GBDT on `outcome` over the resulting feature table → explain (per-row
TreeSHAP drivers) → rank (the at-risk list)** — gated (RULE-1) and governed
(key-scoped access + an audit trail). It proves the at-scale pipeline runs end to
end and produces a real model identical on every node.

## What it does

1. **Compiles + gates the program.** The sim-Python script is the spec: the case
   compiles it with the [Python frontend](../../../pipeline/python-frontend.md), runs
   [`default_gate()`](../../../pipeline/gate.md) (RULE-1), and reads the data shape (the
   `load_dataset` column projection), the node count (`cluster.add_nodes`), and the
   label (`train(target=…)`) from the lowered graph.
2. **Loads the saas parquet, partitioned.** Decodes the embedded
   `saas_sample.parquet` with the native [Parquet reader](../../../data/parquet-reader.md)
   (no Arrow), dropping the `embedding_*` columns, then scales it up with
   deterministic saas-shaped rows so the train + calibration are meaningful. Each
   node writes its round-robin shard into the partitions it owns via the
   [partitioned write path](../../../data/partitioned-write-path.md).
3. **Adds permanent nodes.** Runs the lowered `cluster.add_nodes(10,
   provider='azure')` through [`run_add_nodes_step`](../../../cloud/add-nodes.md) with an
   **injected** `LocalProvisioner` — the `provider=` is provenance only, so the cloud
   burst stays consent-gated and the case is hermetic.
4. **Governs.** A scoped, capability-bearing key authorizes the run (in-scope
   allowed, out-of-scope denied — [key-scoped access](../../../governance/access-control.md)).
5. **Featurizes + trains near data, SPMD.** Every node concurrently runs
   [`run_near_data_featurize_train_job`](../../../use-cases/uc1-featurize-train-over-partitions.md)
   (#530) over its owned partitions: featurize (the broadcast feature table,
   identical on every node) → train (+ calibrate) **over the feature table** →
   explain (TreeSHAP) → rank, each a recorded
   [`ComputeJob`](../../../compute/compute-jobs.md).

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Distributed & cluster → UC-1 at scale (TC-12)**. Set the options in the
modal and click **Run**; the result pane shows pass/fail, the metrics (top driver,
AUC, bytes read near data vs partials over wire, audit events), and the log.

**Headless:** the case is exercised (at `nodes=2`, `rows=96`) by
`tests/testkit/test_testkit.cpp::DistributedCasesRunOnARealCluster`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.DistributedCasesRunOnARealCluster' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `nodes` | 3 | 1–6 | cluster size — the "manually-sized cluster" |
| `rows` | 96 | 12–200000 | conversations (the saas fixture, scaled up) |
| `calibrate` | true | bool | fit held-out Platt calibration on the model |

The script names **10** nodes (what an operator bursts for real); the loopback case
is sized by `nodes`, with requested-vs-provisioned counts in the result metrics.

## What it asserts (pass criteria)

- The broadcast **feature table covers every conversation** with the script's
  columns, identical on every node, its `featurize` job recorded.
- A real calibrated [`Model`](../../../ml/models.md) registered on the root, **identical
  model + ranking on every node** (single-node == multi-node), AUC > 0.6.
- Per-row TreeSHAP drivers (in feature order, non-empty top driver) + a ranked
  at-risk list (every conversation once, risk in `[0,1]` descending).
- **Compute-near-data** — each node read its own shard off local disk (raw data
  never crosses the wire) and, multi-node, read only its shard; every conversation
  read near data + explained once. Only the training partials cross the wire.
- **Gated + governed** — the gate passed, the scoped key authorized the run, and the
  [audit trail](../../../governance/audit-trail.md) captured the access decisions + each job's
  created/finished events.

## Related

- Engine: [uc1-at-scale-e2e.md](../../../use-cases/uc1-at-scale-e2e.md) ·
  [uc1-featurize-train-over-partitions.md](../../../use-cases/uc1-featurize-train-over-partitions.md) ·
  [uc1-train-over-partitions.md](../../../use-cases/uc1-train-over-partitions.md) ·
  [compute-near-data.md](../../../compute/compute-near-data.md) ·
  [add-nodes.md](../../../cloud/add-nodes.md)
- Sibling cases: [`cluster.featurize_partitions`](cluster.featurize_partitions.md) ·
  [`cluster.compute_near_data`](cluster.compute_near_data.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md) ·
  [`examples.uc1_at_scale`](examples.uc1_at_scale.md)
