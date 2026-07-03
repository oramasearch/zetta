# Explain with TreeSHAP · `ml.tree_shap`

**Category:** Machine learning · **Source:** `src/core/testkit/cases_ml.cpp` · **Engine doc:** [tree-shap.md](../../../ml/tree-shap.md)

Trains a GBDT, then explains one account with exact path-dependent TreeSHAP and
checks the efficiency identity `base_value + Σφ = predict_raw` holds exactly. It
proves the native TreeSHAP explainer is exact (the attributions reconstruct the
prediction) and surfaces a per-account dominant churn driver.

## What it does

1. **Builds the feature table + trains the GBDT.** Same as
   [`ml.gbdt_train`](ml.gbdt_train.md): featurize the
   [synthetic churn dataset](../../../use-cases/churn-dataset.md) point-in-time (post-`t0`
   events dropped) into six numeric features + the churn label, then `train_gbdt`
   with the options.
2. **Picks the account to explain.** The one with the most `failed_payments` (the
   worst risk).
3. **Explains it.** Runs `tree_shap` over the trained model and that account's
   features, returning a `ShapExplanation`: the per-feature `φ`, the cover-weighted
   `base_value`, and `predicted`, all in raw / margin (logit) space where SHAP is
   additive.
4. **Reads the dominant driver.** The feature with the largest `|φ|` via
   `dominant_driver`.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Machine learning → Explain with TreeSHAP**. Set the options in the modal
and click **Run**; the result pane shows pass/fail, the metrics (base + Σφ,
predict_raw, dominant driver), and the log.

**Headless:** the case is exercised by `tests/test_testkit.cpp::MlCasesPass`:

```bash
cmake --build build --target zetta-tests
ctest --test-dir build -R 'Testkit.MlCasesPass' --output-on-failure
```

### Options

| option | default | range | meaning |
|---|---|---|---|
| `trees` | 30 | 1–500 | boosting rounds (trees in the ensemble) |
| `depth` | 3 | 1–12 | maximum tree depth |
| `bins` | 16 | 2–256 | histogram bins per feature |
| `learning_rate` | 0.3 | 0.01–1.0 | shrinkage applied to each tree's leaf outputs |

## What it asserts (pass criteria)

- The trainer succeeds (`train_gbdt` returns ok).
- The **efficiency identity holds exactly**: `|efficiency_residual| ≤ 1e-6`, i.e.
  `base_value + Σφ == predict_raw` to within `1e-6`. A larger residual fails the
  case ("efficiency identity broke").
- The result reports `base + Σφ` and `predict_raw` (which match) and the dominant
  driver feature.

## Related

- Engine: [tree-shap.md](../../../ml/tree-shap.md) · [gbdt.md](../../../ml/gbdt.md)
- Sibling cases: [`ml.gbdt_train`](ml.gbdt_train.md) ·
  [`ml.calibration`](ml.calibration.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md) ·
  [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)
