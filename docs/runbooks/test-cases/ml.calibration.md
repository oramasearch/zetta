# Calibrate probabilities · `ml.calibration`

**Category:** Machine learning · **Source:** `src/core/testkit/cases_ml.cpp` · **Engine doc:** [calibration.md](../../calibration.md)

Trains a GBDT, then fits a probability calibrator (Platt or isotonic) on its raw
scores, confirms it produces valid probabilities, and reports the Brier score
before vs after. It proves the calibration path fits and yields well-formed risk
scores, the prerequisite for trustworthy churn probabilities.

## What it does

1. **Builds the feature table + trains the GBDT.** Same as
   [`ml.gbdt_train`](ml.gbdt_train.md): featurize the
   [synthetic churn dataset](../../churn-dataset.md) point-in-time into six numeric
   features + the churn label, then `train_gbdt` with the options.
2. **Collects raw scores + uncalibrated probabilities.** Per account, the model's
   `predict_raw` (the margin) and `predict_proba` (`sigmoid(raw)`, the
   before-calibration probability).
3. **Fits the calibrator.** Runs `fit_calibrator(raw, labels, method)`: **Platt**
   scaling (`sigmoid(a·raw + b)`) or **isotonic** regression (PAVA), per the
   `method` option. Both maps are monotone, so calibration preserves the ranking
   (AUC).
4. **Applies + measures.** Calibrates every raw score and computes the Brier score
   before and after via `calibration_metrics`.

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Machine learning → Calibrate probabilities**. Set the options in the modal
and click **Run**; the result pane shows pass/fail, the metrics (Brier before,
Brier after, method), and the log.

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
| `method` | platt | platt / isotonic | calibration map to fit (Platt sigmoid / isotonic PAVA) |

## What it asserts (pass criteria)

- The trainer succeeds and the **calibrator fits** (`fit_calibrator` returns ok).
- Every calibrated value stays a **valid probability in `[0, 1]`**: an
  out-of-range value fails the case.
- The result reports the Brier score before vs after + the method. The engine
  guarantee that calibration **improves Brier / ECE while preserving AUC** (the
  ranking) is demonstrated in `tests/test_calibration.cpp`; this case checks the
  fit + validity and surfaces the Brier delta.

## Related

- Engine: [calibration.md](../../calibration.md) · [gbdt.md](../../gbdt.md) ·
  [gbdt-score.md](../../gbdt-score.md)
- Sibling cases: [`ml.gbdt_train`](ml.gbdt_train.md) ·
  [`ml.tree_shap`](ml.tree_shap.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md) ·
  [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)
