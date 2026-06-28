# Native correctness oracle (RSK-8)

A self-contained C++ test oracle that proves zetta's **native** featurize / GBDT-train /
TreeSHAP reimplementations are numerically correct — **without running any reference
implementation at test time** (RSK-8, #395; part of the native distributed
accelerated-compute pass for UC-1 churn, #264).

The whole point of the native pass is that every algorithm is reimplemented as
distributed C++ over the [`ComputeDevice`](compute-device.md) seam: **no Python, no
XGBoost, no foreign runtime — in the engine, the build, or CI** (ADR-17). That raises an
obvious risk (RSK-8): how do we know the from-scratch reimplementation is *right*? This
oracle answers it with three independent layers, all over the committed
[synthetic churn fixture](churn-dataset.md), and all pure C++ that runs offline in CI.

- Tests: [`tests/test_native_oracle.cpp`](../tests/test_native_oracle.cpp).
- Golden data: [`tests/fixtures/golden/churn_oracle.json`](../tests/fixtures/golden/churn_oracle.json).
- Format: [`include/zetta/ml/golden_oracle.hpp`](../include/zetta/ml/golden_oracle.hpp),
  [`src/core/ml/golden_oracle.cpp`](../src/core/ml/golden_oracle.cpp).
- Schema: [`schemas/golden_oracle.schema.json`](../schemas/golden_oracle.schema.json).

It builds directly on [featurize](ddf-featurize.md) (#388), the
[GBDT trainer](gbdt.md) (#390), and [TreeSHAP](tree-shap.md) (#393).

## The three layers

### 1. Golden numeric fixtures

Expected featurize / train / SHAP outputs over the churn fixture, **committed as static
data** (`tests/fixtures/golden/churn_oracle.json`) and asserted against within
tolerance. The committed file carries:

- **featurize** — the dense, leakage-free feature table (entity keys, feature names,
  `features[column][row]`, the label column);
- **train** — the trained GBDT's `base_score`, tree count, AUC / log-loss / accuracy,
  per-feature importances, and the per-entity predicted probability;
- **shap** — the model's cover-weighted `base_value`, the global mean-|φ| driver
  ranking, the `top_driver`, and each entity's `predicted` + per-feature `φ` + dominant
  driver.

These committed numbers are **pinned regression anchors** — a refactor that silently
changes a result is caught. But they are *not only* a snapshot: the same test
establishes that they are **correct**, not merely unchanged, by cross-checking them in
the same run:

- the golden featurize values must equal an **independent, leakage-free ground truth**
  computed straight from the raw events (no engine) — so the deliberate post-`t0` events
  the fixture carries are genuinely excluded;
- the golden SHAP values must satisfy the **efficiency identity**
  `base_value + Σ φ == predicted` for every entity — a mathematical invariant
  independent of how the values were produced;
- the equivalence layer (below) reproduces the golden under every shard count.

Generated **once, offline, by zetta's own native engine** (see *Regenerating* below);
the generator is not a repo/build/CI dependency. CI only ever reads and asserts.

### 2. Analytic micro-tests (closed-form)

Tiny hand-built datasets whose GBDT split and SHAP attributions are computable by hand,
asserted to closed-form values:

- **`GbdtSplitAndShapClosedForm`** — one balanced, perfectly-separable feature. With
  `λ = 0`, `learning_rate = 1` a single depth-1 tree puts the exact Newton step in each
  leaf: the base rate is `0.5`, so `base_score = logit(0.5) = 0` and every Hessian is
  `0.25`; the negative leaf is `w = -G/H = -2`, the positive leaf `+2`. So `predict_raw`
  is exactly `∓2`, the cover-weighted `base_value` is `0`, and with one feature SHAP
  hands it the whole gap (`φ₀(x) = predict_raw(x)`). This ties the trainer's split + leaf
  math to the explainer in one closed form.
- **`TreeShapAsymmetricCoverClosedForm`** — a hand-built depth-2 tree with **asymmetric
  training cover** (covers `10 / 6 / 4 / 1 / 3`), where the path-dependent TreeSHAP
  attributions are derived by hand (`φ₀ = 1.65, φ₁ = 0.35` for one instance,
  `-0.7 / -0.3` for another). This exercises Algorithm 2's extend/unwind bookkeeping
  beyond a single split, including the cover-weighted "missing feature follows both
  children" integration.

### 3. Cross-cluster equivalence

Single-node == 2-node == 3-node results are identical within tolerance — the distributed
implementation is deterministic regardless of node / shard count — and each equals the
single-node golden:

- **featurize** is bit-exact across shardings (the aggregates are exact integer / sum /
  correctly-rounded-mean arithmetic — no transcendentals);
- **train** predictions are equal **up to floating-point reduction order** (the standard
  GBDT guarantee: the per-bin histogram sums are accumulated in a different order across
  shardings), compared at the repo's cross-shard tolerance (`1e-4`, the same
  `GbdtDistributed` uses); AUC, being rank-based, is stable;
- **explain** runs the identical replicated model on every node, so the per-entity φ are
  bit-stable across shardings; the allreduced population mean-|φ| ranking agrees.

## The golden format (validated at the boundary)

`load_golden_oracle(path)` mirrors [`load_churn_dataset`](churn-dataset.md): it is
**non-throwing** and **schema-validates** the document at the boundary against
`schemas/golden_oracle.schema.json` (draft-07, the `pboettch` validator embedded in
`src/core/ml/golden_oracle.cpp`), then enforces the cross-field structural rules the
schema can't express — every feature column and SHAP value vector has one entry per
entity / feature. A malformed fixture fails the load with a path-qualified reason
(`LoaderRejectsMalformed` exercises this) instead of silently passing a vacuous test.
`GoldenOracle::to_json()` is the inverse, used by the offline generator.

## Regenerating the golden

The golden is regenerated by the oracle test itself in an env-gated **write mode** (the
offline generator — self-contained C++, no foreign runtime). After an *intended* numeric
change (e.g. a new feature in the canonical spec, or different hyperparameters), run:

```bash
ZETTA_WRITE_GOLDEN=1 ctest --test-dir build -R NativeOracle --output-on-failure
```

A global test environment recomputes the expected outputs from the fixture with the
single-node native references and writes `tests/fixtures/golden/churn_oracle.json` before
any test runs; the read tests then assert against the fresh file. Regeneration is
idempotent (re-running produces a byte-identical file). Commit the updated fixture. CI
never sets the env var, so it only reads and asserts.

## How it fits

The canonical pipeline the oracle pins is the UC-1 churn shape: load the
[churn fixture](churn-dataset.md) → [`point_in_time_featurize`](ddf-featurize.md) (the
leakage-free feature table) → [`train_gbdt`](gbdt.md) (the binary-logistic model) →
[`tree_shap`](tree-shap.md) (per-entity attributions + drivers). The equivalence layer
drives the same three stages through the [job coordinator](job-coordinator.md) as
`featurize` / `train` / `explain` [ComputeJobs](compute-jobs.md) across a loopback
cluster. Together the three layers are the RSK-8 proof that the native reimplementation
is correct and deterministic, with **no reference implementation shipped or executed**.
