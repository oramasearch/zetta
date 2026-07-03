# Train GBDT (AUC) · `ml.gbdt_train`

**Category:** Machine learning · **Source:** `src/core/testkit/cases_ml.cpp` · **Engine doc:** [gbdt.md](../../../ml/gbdt.md)

Trains a histogram gradient-boosted decision tree on a leakage-free churn feature
table built in code, then reports its AUC, log loss, accuracy, and strongest
driver. It proves the from-scratch native GBDT trainer learns a real signal from
the synthetic data.

## What it does

1. **Builds a leakage-free feature table.** Featurizes the built-in
   [synthetic churn dataset](../../../use-cases/churn-dataset.md) (or a custom dataset, if the
   test's data path is set) point-in-time: for each account it reduces only the
   events at or before the as-of cutoff, dropping the post-`t0` leakage trap, into
   six numeric features (`failed_payments`, `support_severity`, `total_events`,
   `mrr`, `seats`, `tenure_days`) plus the `{0,1}` churn label.
2. **Trains the GBDT.** Runs `train_gbdt` over the feature columns with the options
   (trees / depth / bins / learning rate): the binary-logistic histogram trainer
   that boosts one regression tree per round on the log-loss gradient + Hessian.
3. **Scores + evaluates.** Predicts a calibrated probability per account
   (`predict_proba`) and computes AUC / log loss / accuracy via `evaluate_binary`.
4. **Ranks the drivers.** Picks the strongest feature by total split gain
   (`feature_importances`) as the headline driver (`failed_payments` on the
   fixture).

## How to run

**In the app** (headed): launch `./build/bin/zetta`, then **Operations → Test
suite → Machine learning → Train GBDT (AUC)**. Set the options in the modal and
click **Run**; the result pane shows pass/fail, the metrics (AUC, log loss,
accuracy, top driver), and the log.

**Headless:** the case is exercised by `tests/testkit/test_testkit.cpp::MlCasesPass`:

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
- The trained **AUC is better than random** (≥ 0.5): a worse-than-random AUC fails
  the case ("no better than random"). On the fixture the trainer reaches a strong
  AUC and surfaces `failed_payments` as the top driver.
- The result reports AUC, log loss, accuracy, and the top driver feature.

## Related

- Engine: [gbdt.md](../../../ml/gbdt.md) · [gbdt-score.md](../../../ml/gbdt-score.md) ·
  [models.md](../../../ml/models.md)
- Sibling cases: [`ml.tree_shap`](ml.tree_shap.md) ·
  [`ml.calibration`](ml.calibration.md) ·
  [`pipeline.uc1_end_to_end`](pipeline.uc1_end_to_end.md) ·
  [`cluster.uc1_at_scale`](cluster.uc1_at_scale.md)
