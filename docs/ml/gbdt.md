# Distributed histogram GBDT training (`zetta::ml`)

Histogram gradient-boosted decision trees with **binary logistic** and
**squared-error** objectives. Binary logistic is the native training engine for
UC-1 churn (FEAT-6 / ENT-14, #390; part of the native distributed
accelerated-compute pass, #264), while squared error supports native forecasting.
S-3 customer lifetime value also uses squared error for continuous net-value
targets.
Zetta **trains a model on the customer's own data**, rather than running inference
over a rented model.

It builds on everything that landed before it — the
[CPU device ops](../compute/cpu-device.md), the [collectives](../cluster/collectives.md),
[point-in-time featurization](../ddf/ddf-featurize.md), the [Model entity](models.md),
and the [job coordinator](../compute/job-coordinator.md).

- Header: [`include/zetta/ml/gbdt.hpp`](../../include/zetta/ml/gbdt.hpp) — the model,
  the params, the metrics, and the trainer (pure, host-side).
- Header: [`include/zetta/ml/gbdt_train_job.hpp`](../../include/zetta/ml/gbdt_train_job.hpp)
  — the distributed `train` ComputeJob that produces a `Model`.
- Impl: [`src/core/ml/gbdt.cpp`](../../src/core/ml/gbdt.cpp),
  [`src/core/ml/gbdt_train_job.cpp`](../../src/core/ml/gbdt_train_job.cpp).
- Tests: [`tests/ml/test_gbdt.cpp`](../../tests/ml/test_gbdt.cpp).

## What a histogram GBDT is, briefly

A model is a base score plus an **ordered ensemble of small regression trees**.
For binary logistic training, each boosting round fits one tree to the
gradient/Hessian of the binary log-loss at the current predictions:

```
p_i = sigmoid(raw_i)          # current predicted probability
g_i = p_i - y_i               # gradient of the log-loss
h_i = p_i (1 - p_i)           # Hessian
```

Squared-error training uses the residual gradient and a unit Hessian while
retaining the same distributed histogram and tree-building path.

A tree is grown level by level. At every (tree-node, feature) the feature's values
are **binned** into a small number of equal-width bins, and the per-bin sums of
gradients + Hessians — the **gradient histogram** — are accumulated. The best split
for a node is the bin boundary that maximizes the standard regularized gain

```
gain = 1/2 [ G_L²/(H_L+λ) + G_R²/(H_R+λ) - G²/(H+λ) ] - γ
```

and a leaf's output is the Newton step `w = -G/(H+λ)` scaled by the learning rate.
Binning makes split-finding `O(bins)` per feature instead of `O(rows)`, which is
what lets it scale.

## Distributed by histogram allreduce

The training rows (one per entity — the design matrix `X` + label `y`) are sharded
across the cluster. The trainer is **SPMD**: every node runs the same boosting loop
against its own shard, and the per-node statistics are combined across the cluster
each round:

1. **Global bin edges.** Each node computes its local per-feature min/max; an
   `allreduce(Min)` / `allreduce(Max)` makes the bin edges identical on every node
   (so a row bins the same way everywhere). A node with no rows contributes the
   identities (`+inf` / `-inf`).
2. **Global base score.** A node's `[Σy, n]` is `allreduce(Sum)`'d; the base score is
   `logit(Σy / n)` for binary logistic or the continuous target mean `Σy / n`
   for squared error over the whole cluster.
3. **Gradient histograms.** At each tree level, each node builds the
   gradient/Hessian histograms over only its **local** rows, then those per-node
   histograms are **summed across the cluster** with one `allreduce(Sum)`.

Because split-finding then runs off the **global** histograms (identical on every
node), the SPMD bodies grow the **same trees** with no further coordination — so a
single-node run and a multi-node run produce an equivalent model. Within a single
run, every node sees byte-identical allreduce results and so grows **bit-identical**
trees; across two *different* shardings the per-bin sums differ only in the last
floating-point bits (the standard GBDT "equivalent up to floating-point" guarantee),
so the models agree to a tight tolerance. `tests/ml/test_gbdt.cpp` asserts both.

## Host-side reference, device-backed data

Like the [as-of](../ddf/ddf-as-of.md) and [group-by](../ddf/ddf-groupby.md) ops, the trainer is a
**host-side reference over device-backed data**: the feature columns live on the
frame's `ComputeDevice` (a CPU or CUDA frame — e.g. the one
[`FeatureTable::to_data_frame`](../ddf/ddf-featurize.md) materializes from featurization),
and the trainer reads them back through the device download seam (so it runs the same
over a CPU or CUDA frame) and accumulates the gradient/Hessian histograms host-side
via `runtime::parallel_for` (parallel over features). A device-native
weighted-histogram kernel is a later optimization behind this same API. The trainer
is non-throwing throughout (zetta-core's no-exceptions convention): the entry points
return a `compute::ComputeStatus`.

## The model + the JSON boundaries

- `GbdtParams` — the hyperparameters (`num_trees`, `max_depth`, `max_bins`,
  `learning_rate`, `l2_regularization` λ, `min_split_gain` γ, `min_child_hessian`),
  with a validated JSON boundary
  ([`schemas/gbdt_params.schema.json`](../../schemas/gbdt_params.schema.json), the
  hand-written non-throwing `from_json` pattern of `DataFrameSchema` /
  `FeaturizeSpec`; omitted fields keep their default).
- `GbdtModel` — the trained model: a base score, the learning rate, the fixed
  feature ordering, and the ensemble of `RegressionTree`s. **This struct is the
  serialized weights body**: it round-trips through `to_json` / `from_json`, and
  `from_json` validates against the GBDT model JSON Schema
  ([`schemas/gbdt_model.schema.json`](../../schemas/gbdt_model.schema.json), the
  pboettch draft-07 validator — like the Run record — because the body is read back
  from disk / a peer) plus the structural rule that every child index is in range.
  `predict_raw` / `predict_proba` evaluate it.
- `GbdtMetrics` — `auc` (Mann-Whitney, ties at 0.5), `log_loss` (mean
  cross-entropy), `accuracy@0.5`, via `evaluate_binary`. Distributed
  squared-error jobs instead report RMSE and MAE. Both shapes are stored in a
  Model's free-form `metrics`.

## Running it as a `train` ComputeJob → a Model

`run_train_job` (gbdt_train_job.hpp) drives training through the
[job coordinator](../compute/job-coordinator.md) as a recorded, audited `train`
[ComputeJob](../compute/compute-jobs.md) (so it appears in `system.jobs`) and, when a
`Node` is provided, **produces a [`Model`](models.md)**:

- Each participating node reads its shard back from the device and runs the boosting
  loop, combining statistics with the round's `allreduce` (no new wire format — the
  histograms ride the schema-validated `CollectiveMessage`).
- **Global metrics** are computed once: each node scores the rows it fit, the
  `(prediction, label)` pairs are gathered to the root, evaluated over the union,
  and the scalars broadcast back — so every node ends with identical metrics.
  Binary-logistic jobs report AUC, log loss, and accuracy; squared-error jobs
  report RMSE and MAE. When `holdout_fraction` is set, the held-out pairs ride
  that same gather and a second set of objective-appropriate metrics comes back
  beside the in-sample ones — see
  [held-out-metrics.md](held-out-metrics.md) (#953).
- When the job is attached to a pipeline Run, each committed boosting round
  records globally reduced `train.log_loss` or `train.mean_squared_error`, row
  throughput, host gauges, one-time parameters, and a live batch, and, when the
  run held rows out, its held-out scores once at the end (#1239). See
  [run metrics](run-metrics.md) (#1137).
- The deterministic collective **root** registers the trained model as a
  `Model` (kind `classifier` for binary logistic and `regressor` for squared
  error): the GBDT weights body (the schema-validated JSON) is attached via
  `set_weights` (fingerprinted + replicated peer-to-peer like any large-object
  body), with the objective and ranked per-feature gains in `feature_schema` and
  the matching evaluation metrics in `metrics` (explainability — read by
  `zetta::ml::ranked_drivers`, surfaced in the [Runs panel](../pipeline/runs.md)). One logical model
  is produced (at the root, which replicates to peers); deduping per-node
  registrations into a single record on every node is a later slice.

Call `run_train_job` **concurrently on every participating node** (the collectives
need all participants); `coordinator.start()` must have run, and the `round_tag`
must be identical on every node (it defaults to a deterministic, spec-derived tag).

```cpp
// Featurize → device-backed feature frame → train → Model.
auto features = feature_table.to_data_frame(device);          // X + y on the device
auto job = node.create_job(zetta::compute::JobKind::Train);
auto result = zetta::ml::run_train_job(
    coordinator, *job, features.frame,
    feature_table.feature_names, feature_table.label_name,
    zetta::ml::GbdtParams{}, &node);
// result.model is the ensemble; result.metrics.auc the score;
// result.model_id the registered zetta::ml::Model (on the root).
```

## Probability calibration

The trained model's probabilities `sigmoid(raw)` rank accounts well but are not
necessarily **calibrated**. [Probability calibration](calibration.md) (Platt scaling
/ isotonic regression) fits a monotone `raw score → probability` map on a held-out
split so the risk scores are trustworthy (Brier / ECE improve, AUC preserved). A
fitted `Calibrator` attaches to a `GbdtModel` (`predict_proba` then routes through
it, and it rides inside the weights body), and `run_train_job` fits one for the
produced `Model` when a `CalibrationConfig` is passed — via a content-hash held-out
split that keeps single-node and multi-node equivalent. See
[docs/ml/calibration.md](calibration.md).

## Checkpoint / restart (eviction tolerance)

Distributed training is the long-running job most exposed to a **spot eviction** —
its many boosting rounds run for minutes across burst nodes. `run_train_job` is the
first consumer of the [checkpoint / restart](../compute/job-checkpoint.md) capability (RULE-5 /
RSK-9, #468): pass a `TrainCheckpointConfig{ store, checkpoint_every }` and the
boosting loop **checkpoints the partial model** (its committed trees + accumulated
feature gains) after each round and, on (re)start, **resumes from the latest
checkpoint** instead of re-growing the committed trees. So an evicted node costs at
most the trees grown since the last checkpoint, not the whole ensemble.

The checkpoint is keyed by the job's `round_tag` (the cross-node-agreed logical job
key), and the resume round is **agreed across the cluster** with an allreduce-min:
since the per-round histogram allreduce is lock-step, every node must resume from the
same round, and a node lacking a checkpoint (e.g. a replacement burst node) simply
forces a fresh start rather than desyncing the rounds. Because the trees up to a
given round are identical on every node (grown off the global histograms), seeding +
truncating to the agreed round is safe and the resumed model is **byte-identical to
an uninterrupted run** (the acceptance, `GbdtCheckpoint.*` in `tests/ml/test_gbdt.cpp`).
Disabled by default (no store) — the loop is then exactly the trainer above. The
mechanics of the seam (the durable `JobCheckpoint`, the store, the resume driver)
live in [docs/compute/job-checkpoint.md](../compute/job-checkpoint.md).

## Scope + what's next

- **Objective:** binary logistic (log-loss) and squared-error regression.
  Multiclass objectives remain out of scope.
- **Histogram:** host-side reference today; a device-native weighted-histogram
  kernel (and the parent-minus-sibling subtraction trick) are later optimizations
  behind the same API.
- **Model dedup:** the root registers one model; collapsing per-node registrations
  into one record on every node is a later slice.
- **Explainability:** the model records gain-based drivers, and each `TreeNode` carries
  its training **cover** (the per-node Σ-Hessian) so the trees are explainable.
  Per-prediction SHAP attribution is the **exact TreeSHAP** slice — see
  [tree-shap.md](tree-shap.md).

## Acceptance (in `tests/ml/test_gbdt.cpp`)

- The model + params JSON boundaries round-trip and reject malformed input
  (bad objective, out-of-range child index, unknown key, bad hyperparameters).
- `evaluate_binary` gives AUC 1.0 on perfect separation, 0.0 reversed, 0.5 for a
  single class / boundary ties.
- The single-node trainer learns a separable signal to AUC 1.0 and ranks the
  carrying feature as the dominant driver.
- The distributed `train` job grows **equivalent trees** single-node vs multi-node
  (identical within a run; equal up to floating-point across shardings).
- A squared-error `train` job registers a regressor with objective, RMSE, MAE,
  held-out RMSE, and held-out MAE preserved across the Model boundary.
- Trained on the synthetic [churn fixture](../use-cases/churn-dataset.md) (via featurization),
  it reaches a strong AUC, produces a registered `Model` whose weights re-validate
  against the schema, and surfaces `failed_payments` as a positive churn driver.

## Metric isolation

Metric hooks leave model bytes unchanged. Without an enabled hook, training
performs no metric pass or metric allreduce. Distributed jobs agree on reporting
before training; DP callers force suppression before reading private rows for
metrics. See [run-metrics.md](run-metrics.md) (#1137).
