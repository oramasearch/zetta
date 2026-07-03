# Featurize engineered features → GBDT(outcome) over partitions

Point-in-time featurize the engineered numeric columns (embeddings dropped)
compute-near-data over a partitioned table and train the native histogram GBDT on
`outcome` **over the resulting feature table** — the UC-1 featurize → train
composition (**#530**, FEAT-6 / FEAT-8). It reuses the existing churn engine
(featurize → GBDT → calibration → TreeSHAP → rank) and adds **no new ML math and no
new near-data machinery**: it composes the two shipped near-data slices — the
[featurize over partitions](../ddf/ddf-featurize.md) (#532) and the
[train over partitions](uc1-train-over-partitions.md) (#533) — so the GBDT is fed
the **featurized table**, not the raw engineered columns.

```
include/zetta/ml/local_featurize_train_job.hpp  run_near_data_featurize_train_job + NearDataFeaturizeTrainResult — the composed featurize → train → explain → rank pipeline
src/core/ml/local_featurize_train_job.cpp       load_local_partitions once → run_featurize_job → slice the broadcast table → the shared train phases
src/core/ml/near_data_train_pipeline.hpp        zetta::ml::detail::run_train_explain_rank_phases — the #533 phases factored so #530 and #533 share one driver
tests/ml/test_gbdt_featurize_train_partitions.cpp  3-node featurize→train == single-node oracle, with a post-cutoff leakage trap
src/core/testkit/cases_uc1_scale.cpp            cluster.uc1_at_scale now runs THIS composition (featurize → train, not raw columns)
```

## What it is

The two building blocks each solved half of the problem:

- [`run_near_data_featurize_job`](../ddf/ddf-featurize.md) (#532) featurizes a node's
  owned partitions and broadcasts the **global feature table** — but nothing trained
  on it;
- [`run_near_data_train_job`](uc1-train-over-partitions.md) (#533) trains → explains
  → ranks over a node's owned partitions — but it consumed the **raw engineered
  columns** directly, skipping featurization (and with it the point-in-time filter).

`run_near_data_featurize_train_job` (`include/zetta/ml/local_featurize_train_job.hpp`)
is the composition — one shard load, four recorded [`ComputeJob`s](../compute/compute-jobs.md):

1. **load** — this node's shard is materialized **once** from its owned partitions of
   the raw engineered table
   ([`load_local_partitions`](../compute/compute-near-data.md)): only the bodies it
   physically holds, read from local disk (the dropped embeddings never reach the
   wire).
2. **featurize** — a self-created `featurize` job drives the distributed
   point-in-time featurization over the local shard (the static engineered-columns
   [`FeaturizeSpec`](../ddf/ddf-featurize.md#static-mode--engineered-columns-no-windowed-events-532);
   when the spec sets a `time_column`, rows past `as_of` are excluded first — no
   leakage). The merged table is broadcast: **identical on every node**, equal to a
   single-node featurize over the whole dataset.
3. **slice** — the node keeps the rows of the global feature table whose entity came
   from its own owned partitions. A post-cutoff local row is absent from the global
   table, so it is excluded here too. The slices are disjoint across nodes (the
   [partitioned write path](../data/partitioned-write-path.md) puts a row in exactly
   one partition) and union back to exactly the global table — so the **training
   universe IS the feature table**, each row contributed once.
4. **train → explain → rank** — the headline `train` job (+ self-created `explain`
   and `score` jobs) drives the #533 pipeline over the slice: histogram GBDT on
   `spec.label_column` with opt-in [calibration](../ml/calibration.md) (registering a
   calibrated [`Model`](../ml/models.md) whose metrics carry the global AUC /
   log-loss / accuracy), per-row exact [TreeSHAP](../ml/tree-shap.md) drivers, and
   the global [ranked at-risk list](../ml/gbdt-score.md).

The #533 phases were factored into a shared internal driver
(`src/core/ml/near_data_train_pipeline.hpp`,
`zetta::ml::detail::run_train_explain_rank_phases`) so `run_near_data_train_job` and
this composition run the *same* code and cannot drift.

```cpp
zetta::ddf::FeaturizeSpec spec;
spec.entity_column    = "conversation_id";
spec.time_column      = "event_ts";          // optional: point-in-time cutoff
spec.as_of            = t0;
spec.account_features = {"customer_engagement", "sales_effectiveness", ...};
spec.label_column     = "outcome";           // REQUIRED here — the GBDT target

auto job = node.create_job(zetta::compute::JobKind::Train);
auto out = zetta::ml::run_near_data_featurize_train_job(
    coordinator, node, *job, table_id, engineered_schema, spec, gbdt_params,
    device, calibration, "outcome-gbdt");
// out.feature_table       — the GLOBAL feature table the model trained on
// out.train               — the #533 result: model, metrics, drivers, ranked list
// out.featurize_job_id / out.train.explain_job_id / out.train.score_job_id
```

A spec without a `label_column` fails the job cleanly up front (it can featurize but
cannot train). The model's feature columns are the feature table's columns
(`spec.account_features` ++ the event-feature names, in spec order); the drivers +
ranked list are labeled by the spec's entity key.

## SPMD — single-node == multi-node

Called **concurrently on every participating node**, every phase combines per-node
partials over the round's [collectives](../cluster/collectives.md) and broadcasts its
result, so every node ends with the **same feature table, model, drivers, and
ranking** — equivalent to a single-node run over the whole dataset (within
floating-point reduction tolerance for the histogram allreduce). Distinct,
table-derived per-phase collective tags (`<round_tag>:featurize` / `:train` /
`:explain` / `:score`, identical on every node) keep the phases from colliding.

The acceptance (`tests/ml/test_gbdt_featurize_train_partitions.cpp`) shards 240
conversations across a real 3-node loopback cluster with **40 rows past the as-of
cutoff planted as a leakage trap** and asserts: the broadcast feature table is
identical on every node and contains exactly the 200 pre-cutoff conversations; no
post-cutoff conversation reaches a feature, an explanation, or the ranking; the
calibrated model + metrics + per-entity risks match a single-node oracle within
`1e-3`; the top SHAP driver is the signal column; and all four jobs are recorded.

## Entity ownership — the slice invariant

The slice step assumes an entity's rows live in **one node's partitions** — true by
construction for the partitioned write path (a row lands in exactly one partition)
and the engineered one-row-per-entity shape. An entity spanning two nodes' shards
would be trained twice. A multi-table **windowed** spec (the churn events shape,
where an entity's events may scatter) does not fit this single-table runner — run the
#533 phases over [`run_featurize_job`](../ddf/ddf-featurize.md) directly for that.

## Compute-near-data, observable

Each node reads only the partition bodies it holds (`local_bytes_read`, read near
the node — not moved); only the featurization partials + training histograms + SHAP
partials + score pairs cross the wire (`wire_bytes_moved`, via
[`Collectives::wire_bytes()`](../cluster/collectives.md)). The runner records **two**
[`system.pipeline_jobs`](../compute/pipeline-instrumentation.md) rows — a `featurize`
row carrying the one shard read + the featurize partials, and a `train` row carrying
the feature rows this node trained on + the train/explain/score partials — so
summing the view never double-counts the read. `NearDataFeaturizeTrainResult`
surfaces `partitions_read` / `rows_read` (raw rows, trap rows included) /
`feature_rows` (this node's slice) / `local_bytes_read` / `wire_bytes_moved`.

## No new JSON / wire format

The shard is read from the existing [JsonlStore](../data/storage.md) partition-body
lines; the featurization partials, training histograms / metrics, SHAP partials, and
score pairs all ride the schema-validated `CollectiveMessage`; the spec carries its
own validated [`FeaturizeSpec`](../ddf/ddf-featurize.md#the-spec-json-boundary)
boundary; the GBDT weights body carries its validated [`GbdtModel`](../ml/gbdt.md)
boundary; the job + model records are validated at their own boundaries. Nothing new
is parsed.

## Relationship to the surrounding slices

- Composes [featurize over partitions](../ddf/ddf-featurize.md) (#532) and
  [train over partitions](uc1-train-over-partitions.md) (#533) over the
  [compute-near-data](../compute/compute-near-data.md) loader (#516).
- The [UC-1-at-scale sim-Python E2E](uc1-at-scale-e2e.md) (#531,
  `cluster.uc1_at_scale`) now runs **this** composition as its pipeline step — the
  canonical UC-1 path featurizes before it trains.
- The single-cluster churn equivalent (windowed features over the synthetic churn
  fixture) is the [UC-1 end-to-end test](uc1-end-to-end.md) (#399) behind
  [`ChurnStepExecutor`](churn-step-executor.md).
