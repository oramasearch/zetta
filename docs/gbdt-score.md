# Batch scoring / rank-at-risk over a Model (`zetta::ml`)

Batch-score a set of entities through a trained GBDT [`Model`](models.md) to
produce, per entity, a **calibrated churn risk**, then rank them into an **at-risk
list** (highest risk first) — the FEAT-6 / CMD-10 "rank-at-risk" deliverable a
retention team acts on (#392; part of the native distributed accelerated-compute
pass for UC-1 churn, #264). This is the *inference* counterpart to the
[GBDT trainer](gbdt.md): training learns a model from the customer's data; scoring
runs the customer's accounts back through it.

It builds on everything that landed before it — the [Model entity](models.md), the
[GBDT trainer](gbdt.md), the [CPU device ops](cpu-device.md) (the `sort_by_key`
ranking operator), the [collectives](collectives.md), and the
[job coordinator](job-coordinator.md).

- Header: [`include/zetta/ml/gbdt_score.hpp`](../include/zetta/ml/gbdt_score.hpp) —
  the pure scorer (`predict_risk` / `score_gbdt`) and the model-load boundary
  (`load_gbdt_model`).
- Header: [`include/zetta/ml/gbdt_score_job.hpp`](../include/zetta/ml/gbdt_score_job.hpp)
  — the distributed `score` ComputeJob producing the global at-risk list.
- Impl: [`src/core/ml/gbdt_score.cpp`](../src/core/ml/gbdt_score.cpp),
  [`src/core/ml/gbdt_score_job.cpp`](../src/core/ml/gbdt_score_job.cpp).
- Tests: [`tests/test_gbdt_score.cpp`](../tests/test_gbdt_score.cpp).

## Calibrated risk, for free

The model is binary-logistic — trained on log-loss, which is a **proper scoring
rule** — so its predicted probability `GbdtModel::predict_proba` (the sigmoid of the
raw ensemble score) is already a **calibrated risk** in `[0, 1]`. No separate Platt
/ isotonic calibration step is needed: the probability that an account churns *is*
`predict_proba`. The at-risk ranking is simply those probabilities, sorted
descending (ties broken by entity ascending, so the ranking is **deterministic**
regardless of input order).

## GPU-native, CPU fallback, distributed

Per zetta's AI-compute direction (see
[CLAUDE.md → "AI compute — GPU-native, CPU fallback, distributed"](../CLAUDE.md))
scoring runs **through the `ComputeDevice` seam**, so it executes on whatever device
the feature data lives on:

- The feature columns are read from the frame's `ComputeDevice` (a CPU or CUDA
  frame — e.g. the one [`FeatureTable::to_data_frame`](ddf-featurize.md)
  materializes), so scoring runs the same over a CPU or CUDA frame.
- The **global ranking is a device operator**: the distributed job ranks the union
  of `(entity, risk)` pairs with the device `sort_by_key` (the op the
  [device header](compute-device.md) documents as *"sort entities by risk score,
  carrying their ids"*) — it sorts on the GPU when the frame lives there and falls
  back to the CPU otherwise.

The tree-traversal numerics (`predict_proba`) are the **host-side reference** today
— the same convention the [trainer](gbdt.md) ships with (read device-backed columns
back, compute host-side). A device-native tree-predict kernel is the GPU-native
optimization behind this same API; it does not change the public surface.

Non-throwing throughout (zetta-core's no-exceptions convention): every entry point
returns a `compute::ComputeStatus`.

## The pure scorer

```cpp
// Score every row, rank by risk descending; top_k = 0 returns all.
zetta::ml::ScoreResult scored =
    zetta::ml::score_gbdt(model, columns, feature_names, entities, /*top_k=*/0);
for (const zetta::ml::EntityScore& s : scored.ranked) {
    // s.entity (the CRM account key) at calibrated risk s.risk in [0,1]
}
```

`score_gbdt` aligns the provided `columns` to the model's own `feature_names` **by
name** (so the scoring input may carry columns in any order, or extra columns the
model does not use), predicts each row, and returns the entities ranked highest-risk
first. `predict_risk` is the lower-level half (risks row-aligned, not ranked) the
distributed job uses. Both are non-throwing — they fail with `InvalidArgument` when a
column the model needs is missing, the columns are ragged, or the entity/row counts
disagree.

## The Model-weights boundary

`load_gbdt_model(model)` reads a registered [`Model`](models.md)'s serialized weights
body and parses it through the **GBDT model JSON Schema boundary** (`GbdtModel::from_json`
— the pboettch draft-07 validator,
[`schemas/gbdt_model.schema.json`](../schemas/gbdt_model.schema.json), the same
schema the trainer writes the body with). Because the body is read back from disk / a
peer, re-validating here is the boundary that rejects a malformed body with a
path-qualified reason instead of scoring against corrupt weights. It returns
`nullopt` when the body is not present locally (transfer still in flight), is not
valid JSON, or fails schema validation.

## Running it as a `score` ComputeJob

`run_score_job` (gbdt_score_job.hpp) drives scoring through the
[job coordinator](job-coordinator.md) as a recorded, audited `score`
[ComputeJob](compute-jobs.md) (so it appears in `system.zetta_jobs`). The model is
**SPMD**, the way the [trainer](gbdt.md) and [featurization](ddf-featurize.md) run:

1. Each participating node holds a shard of the entities to score (a feature
   `DataFrame`). Because the trained `GbdtModel` is small and identical on every node
   (it replicates from the trainer's root), every node scores its **own shard**
   locally — no model broadcast.
2. The per-node `(entity, risk)` pairs are **gathered** to the deterministic
   collective root (no new wire format — they ride the schema-validated
   `CollectiveMessage` as doubles; entity keys are exact as doubles for ids ≤ 2⁵³,
   the documented bound of the double-based collective plane).
3. The root **ranks the union on the device** (`sort_by_key`), truncates to `top_k`,
   and **broadcasts** the ranked list back — so every node ends with the **identical
   global at-risk list**.

A single-node run and a multi-node run therefore produce the **same ranking** — the
acceptance.

```cpp
// A device-backed feature frame (one row per entity, entity-key column + features).
auto frame = feature_table.to_data_frame(device);
auto job = node.create_job(zetta::JobKind::Score);
auto result = zetta::ml::run_score_job(
    coordinator, *job, frame.frame, model, /*entity_column=*/"entity",
    /*top_k=*/100);  // the 100 most at-risk accounts
// result.ranked is the global at-risk list, identical on every node.
```

Call `run_score_job` **concurrently on every participating node** (the collectives
need all participants); `coordinator.start()` must have run, and the `round_tag` must
be identical on every node (it defaults to a deterministic, model-derived tag).

## Scope + what's next

- **In-memory result.** Scoring returns the ranked list (like
  [`run_featurize_job`](ddf-featurize.md) returns a `FeatureTable`); persisting the
  scores as a replicated entity / surfacing them in SQL + the GUI is a later slice.
- **Tree-predict kernel.** Host-side reference today; a device-native (GPU) predict
  kernel is the optimization behind the same API.
- **Objective.** Binary logistic only, matching the trainer; other objectives extend
  alongside `GbdtModel::objective`.

## Acceptance (in `tests/test_gbdt_score.cpp`)

- `score_gbdt` ranks by risk descending (entity-ascending tie-break), every listed
  risk equals `predict_proba`, `top_k` truncates to the highest-risk prefix, the
  result is independent of input column order, and a missing model feature column is
  rejected.
- `load_gbdt_model` round-trips a `GbdtModel` through a registered `Model`'s weights
  body and rejects a malformed body at the schema boundary.
- The distributed `score` job produces an **identical ranked list single-node vs
  multi-node**, matching the pure host-side scorer (including `top_k` across shards).
- Scored over the synthetic [churn fixture](churn-dataset.md) (featurize → train →
  score), the ranking reproduces the trained model and ranks **churned accounts as
  higher risk on average** than retained ones.
