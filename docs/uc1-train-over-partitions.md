# Train GBDT(outcome) over partitions → calibrated Model + drivers

Train the native histogram GBDT on the binary `outcome` label **compute-near-data**
over a partitioned feature table, calibrate it, and produce **per-row TreeSHAP
drivers + a ranked at-risk list** — the **train** sibling of the
[near-data featurize job](ddf-featurize.md) (**#533**, the UC-1-at-scale train
slice; FEAT-6 / FEAT-8). It adds **no new ML math**: it composes the existing
[GBDT trainer](gbdt.md), [calibration](calibration.md),
[TreeSHAP](tree-shap.md), and [batch scorer](gbdt-score.md) over the
[compute-near-data](compute-near-data.md) shard source, the way
[`ChurnStepExecutor`](churn-step-executor.md) runs train/explain/score over one
feature frame — only with the frame loaded **near the data** instead of from an
in-line featurization.

```
include/zetta/ml/local_train_job.hpp   run_near_data_train_job + NearDataTrainResult — the near-data train→explain→rank pipeline
src/core/ml/local_train_job.cpp        load_local_partitions → run_train_job (+ calibrate) → run_explain_job → run_score_job
tests/test_gbdt_train_partitions.cpp   3-node near-data train == single-node oracle: calibrated model + drivers + ranking, near-data instrumentation
```

## What it is

Where the [distributed train job](gbdt.md) (`run_train_job`, #390) takes the
feature-table shard the caller built however it liked, this runner **loads this
node's shard from its OWNED partitions** of a single partitioned table — the
[partition catalog](dataset-partitions.md) + the
[sharded blob bodies](sharded-blob-placement.md) it physically holds, via
[`load_local_partitions`](compute-near-data.md) — and drives the whole pipeline
over that local shard, each phase a recorded [`ComputeJob`](compute-jobs.md).

This trains over the **saas-sales-conversations feature table** (the UC-1-at-scale
dataset): one row per conversation (entity) carrying the already-engineered numeric
columns (`customer_engagement`, `sales_effectiveness`, `conversation_length`, …;
the embeddings dropped at load — see the
[featurize-over-partitions slice](ddf-featurize.md) #532) and the binary `outcome`
label. The engineered columns are the `feature_columns`, `outcome` is the
`label_column`, and `conversation_id` is the `entity_column` that labels the
per-row drivers + ranked list.

## The pipeline — three recorded jobs over the one local shard

`run_near_data_train_job` loads the local shard **once** and runs three phases over
it, each a `ComputeJob` (so they appear in `system.zetta_jobs`):

1. **train + calibrate** — [`run_train_job`](gbdt.md) over the local shard
   (calibration opt-in via [`CalibrationConfig`](calibration.md)), driving the
   headline `train` job and, on the deterministic root, **registering a calibrated
   [`Model`](models.md)** (kind `classifier`) whose `metrics` carry the global
   **AUC / log-loss / accuracy** and whose `feature_schema.drivers` carry the
   per-feature gains.
2. **explain** — [`run_explain_job`](tree-shap.md) over the same shard with the
   trained model: **per-row** (per-entity) exact TreeSHAP drivers for this node's
   shard (the union across nodes is the global per-row set) plus the global
   population-level mean-|φ| driver ranking. A self-created `explain` job.
3. **rank** — [`run_score_job`](gbdt-score.md) over the same shard with the trained
   model: the **global ranked at-risk list** (highest calibrated risk first,
   broadcast so it is identical on every node). A self-created `score` job.

The headline `train` job is passed in (the caller creates it via
`node.create_job(JobKind::Train, …)` and observes it); the `explain` + `score`
jobs are created on the node by the runner and their ids returned in
`NearDataTrainResult::{explain_job_id, score_job_id}`.

```cpp
NearDataTrainResult run_near_data_train_job(
    compute::JobCoordinator& coordinator, Node& node, ComputeJob& job,
    const std::string& table, const ddf::DataFrameSchema& schema,
    const std::vector<std::string>& feature_columns,
    const std::string& label_column, const GbdtParams& params,
    compute::ComputeDevice& device, const std::string& entity_column = {},
    CalibrationConfig calibration = {}, const std::string& model_name = {},
    std::size_t top_k = 0, const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultGbdtTimeout);
```

## SPMD — single-node == multi-node

Called **concurrently on every participating node**, each phase combines per-node
partials over the round's [collectives](collectives.md) — the training histograms /
metrics (allreduce + gather/broadcast), the explain Σ|φ| (allreduce), the score
gather + device-`sort_by_key` rank + broadcast — so every node ends with the **same
model, drivers, and ranking**, equivalent to a single-node run over the whole table.
The three phases run **sequentially** but each is collective-synced, so the cluster
stays in lock-step (the `ChurnStepExecutor` convention). Distinct, table-derived
per-phase collective tags (`<round_tag>:train` / `:explain` / `:score`, identical on
every node) keep the phases from colliding.

The model + metrics + ranking are **broadcast**, so they are bit-identical on every
node; across different shardings they are equal within floating-point reduction
tolerance (the histogram allreduce reduces in a different order). The acceptance
(`tests/test_gbdt_train_partitions.cpp`) shards the same feature table across a
real 3-node loopback cluster and asserts the distributed result **matches a
single-node oracle** over the whole dataset — the metrics within `1e-3` and every
entity's calibrated risk within `1e-3` — while the global top SHAP driver is the
signal column (`customer_engagement`), the model is calibrated, and every step is
recorded as a `ComputeJob`.

## Compute-near-data, observable

Each node reads only the partition bodies it physically holds (`local_bytes_read`,
read near the node — **not** moved), and only the per-node partials cross the wire
(`wire_bytes_moved`, summed across all three phases via
[`Collectives::wire_bytes()`](collectives.md)). The raw feature rows (and the
dropped embeddings) never cross the wire — `NearDataTrainResult` surfaces
`partitions_read` / `rows_read` / `local_bytes_read` / `wire_bytes_moved` so the
near-data property is directly checkable: **bytes-moved ≈ partials, not ≈ dataset.**

## No new JSON / wire format

The shard is read from the existing [JsonlStore](storage.md) partition-body lines;
the training histograms / metrics, the SHAP partials, and the score pairs all ride
the schema-validated `CollectiveMessage`; the GBDT weights body carries its own
validated [`GbdtModel`](gbdt.md) boundary; the job + model records are validated at
their own boundaries. Nothing new is parsed.

## Relationship to the surrounding slices

- The **shard source** is [compute-near-data](compute-near-data.md) (#516) over the
  [partitioned write path](partitioned-write-path.md) (#511) + the
  [partition catalog](dataset-partitions.md) (#510).
- The **featurize** half (#532, [featurize over partitions](ddf-featurize.md)) is
  this runner's sibling: featurize produces the engineered feature table over the
  partitions; this trains over it. Composing them — featurize → train end to end
  over partitions — is [#530](https://github.com/oramasearch/zetta/issues/530); the
  sim-Python UC-1-at-scale E2E (load → partition → add-nodes → train → explain) is
  [#531](https://github.com/oramasearch/zetta/issues/531).
- The single-cluster churn equivalent (over the synthetic churn fixture, not the
  partitioned saas table) is the [UC-1 end-to-end test](uc1-end-to-end.md) (#399)
  behind [`ChurnStepExecutor`](churn-step-executor.md).
