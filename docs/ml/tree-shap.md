# Exact TreeSHAP explanation → per-entity drivers (`zetta::ml`)

Exact **TreeSHAP** over a trained histogram GBDT — the explainability the
rented-model approach can't give (FEAT-6, #393; part of the native distributed
accelerated-compute pass for UC-1 churn, #264). For a single entity it attributes the
model's prediction to its features (`φ_i` per feature), so a per-entity **dominant
churn driver** falls out — and, distributed for **batch explanation**, a
population-level driver ranking over a whole entity universe.

It builds directly on the [GBDT trainer](gbdt.md): the trained `GbdtModel` carries the
per-node **cover** TreeSHAP needs, and the explanation runs through the
[job coordinator](../compute/job-coordinator.md) as an `explain` [ComputeJob](../compute/compute-jobs.md).

- Header: [`include/zetta/ml/tree_shap.hpp`](../../include/zetta/ml/tree_shap.hpp) — the
  pure algorithm (`tree_shap`, `model_base_value`, `dominant_driver`).
- Header:
  [`include/zetta/ml/gbdt_explain_job.hpp`](../../include/zetta/ml/gbdt_explain_job.hpp)
  — the distributed `explain` ComputeJob (`run_explain_job`).
- Impl: [`src/core/ml/tree_shap.cpp`](../../src/core/ml/tree_shap.cpp),
  [`src/core/ml/gbdt_explain_job.cpp`](../../src/core/ml/gbdt_explain_job.cpp).
- Tests: [`tests/ml/test_tree_shap.cpp`](../../tests/ml/test_tree_shap.cpp).

## What SHAP gives you

SHAP values are the unique additive feature attributions satisfying the Shapley
axioms. For a model `f` and an instance `x`, `φ_i(x)` is feature `i`'s contribution to
moving the prediction away from a baseline, and the headline guarantee is **efficiency
/ local accuracy**:

```
f(x)  ==  base_value + Σ_i φ_i(x)
```

A positive `φ_i` pushes the prediction up (toward churn, for the churn model), a
negative one down. The single feature with the largest `|φ_i|` is the entity's
**dominant driver** (`zetta::ml::dominant_driver`), and the mean `|φ_i|` over a cohort
is a far better feature-importance ranking than split-gain (it is per-prediction,
signed, and consistent).

## Exact, path-dependent TreeSHAP

For tree ensembles the Shapley values can be computed **exactly** in polynomial time
(`O(trees · leaves · depth²)`) rather than the exponential cost of the general
definition — this is TreeSHAP (Lundberg, Erion, Lee 2018, *Consistent Individual
Feature Attribution for Tree Ensembles*, Algorithm 2). A single recursive traversal
per tree tracks, for every subset of features, the proportion of permutations
consistent with it (the `extend_path` / `unwind_path` / `unwound_path_sum`
bookkeeping).

The "missing feature" expectation is the **path-dependent** one: a feature not in the
explained subset is integrated out by following *both* children, weighted by the
training **cover** that flowed each way — `cover[child] / cover[node]`. That cover is
the per-node sum of Hessians the trainer already computes each boosting round (it is
XGBoost's "cover"), now **recorded on every `TreeNode`** (see [gbdt.md](gbdt.md)) — so
the model is **self-describing** and no background dataset is needed. The base value is
the model's intrinsic expected raw score under that cover distribution
(`model_base_value` = `base_score` + Σ over trees of the cover-weighted mean leaf).

### Margin (raw / logit) space

SHAP is additive, so the explanation lives in the model's **additive** output — the
**raw score** `predict_raw` (a logit for the binary logistic objective), not the
squashed probability. The ensemble's SHAP values are the sum of the per-tree ones (the
model is a sum of trees), so the whole ensemble is explained in one pass, and the
efficiency identity holds exactly per entity:

```
predict_raw(x)  ==  base_value + Σ_i φ_i(x)        (within floating-point tolerance)
```

`tests/ml/test_tree_shap.cpp` asserts this for a hand-built tree (a closed-form value), a
trained separable model, and every entity of the churn fixture.

### Pure, deterministic, non-throwing

`tree_shap(model, x)` is a pure function of `(model, features)` — no device, no I/O, no
shared state — so it is trivially thread-safe and reproducible. It introduces **no new
JSON boundary**: it reads the already-schema-validated `GbdtModel` and returns plain
data.

## Running it as an `explain` ComputeJob → batch drivers

`run_explain_job` (gbdt_explain_job.hpp) drives explanation through the
[job coordinator](../compute/job-coordinator.md) as a recorded, audited `explain`
[ComputeJob](../compute/compute-jobs.md) (so it appears in `system.zetta_jobs`), distributed for
**batch explanation** of a whole entity universe:

- Each participating node holds a shard of the feature table (one row per entity) and
  the **same** trained `GbdtModel` (it is replicated, identical on every node).
  Per-entity SHAP has no cross-entity dependency, so it is **embarrassingly parallel**:
  each node fans `tree_shap` across its own rows (intra-node parallelism over the
  shared runtime pool, the trainer's pattern), labelling each explanation with its
  entity key.
- **One collective** combines the per-node `Σ|φ|` and row counts into a global,
  population-level **mean-|φ| driver ranking** — identical on every node (the "dominant
  churn drivers" at the cohort level). No splitter / repartition is needed; the model
  already carries the background (cover) it integrates over.
- **No new wire format** — the `Σ|φ|` partials ride the schema-validated
  `CollectiveMessage` (via `JobRound::allreduce`); the job record is schema-validated
  at its own boundary.

Call `run_explain_job` **concurrently on every participating node** (the collective
needs all participants); `coordinator.start()` must have run, and the `round_tag` must
be identical on every node (it defaults to a deterministic, model-derived tag).

```cpp
// Train (see gbdt.md) -> a GbdtModel; then explain a feature frame across the cluster.
auto features = feature_table.to_data_frame(device);     // X (+ entity key) on a device
auto job = node.create_job(zetta::compute::JobKind::Explain);
auto out = zetta::ml::run_explain_job(
    coordinator, *job, model, features.frame, /*entity_column=*/"entity");
// out.explanations[i] — this node's i-th entity: predicted, φ per feature,
//   dominant_feature (largest |φ|) + its signed value.
// out.mean_abs_shap / out.top_driver — the global, population-level driver ranking.
// out.base_value — the model's intrinsic expected raw score.
```

## Scope + what's next

- **Objective:** binary logistic only (the trainer's only objective today). Other
  objectives change `predict_raw`, not the SHAP recursion.
- **Path-dependent only:** the background is the model's recorded cover. An
  interventional variant (explain against an explicit background dataset) is a later
  slice behind the same `tree_shap` front door.
- **Host-side reference:** the GPU-parallel exact-TreeSHAP kernel (GPUTreeShap) is a
  later optimization behind the same API.
- **Persistence:** `run_explain_job` returns the per-entity scores + the driver
  ranking; persisting the scores as a dataset / a [Run](../pipeline/runs.md) output, and writing
  the SHAP driver ranking onto the [`Model`](models.md)'s `feature_schema.drivers` (so
  it surfaces in the Runs panel via `zetta::ml::ranked_drivers`), are later slices.

## Acceptance (in `tests/ml/test_tree_shap.cpp`)

- A hand-built single-split tree gives the **closed-form** SHAP value
  (`φ_0 = predict_raw − base_value`), and a constant model (no features) attributes
  nothing.
- **Efficiency** (`base_value + Σ φ = predict_raw`) holds within `1e-7` for a trained
  separable model and for **every entity** of the synthetic
  [churn fixture](../use-cases/churn-dataset.md).
- The **per-entity dominant driver** is produced for every churn entity; the account
  with the most failed payments has `failed_payments` as its dominant, positive driver;
  and `failed_payments` is the **top global driver** (largest mean `|φ|`).
- The distributed `explain` job is **single-node == multi-node**: the same entity
  universe, the same per-entity SHAP values and dominant drivers, and the same global
  ranking regardless of how entities were sharded.
