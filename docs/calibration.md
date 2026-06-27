# Probability calibration (`zetta::ml`)

Calibrate the GBDT's output probabilities so the **risk scores are trustworthy** —
the reference is a *calibrated* XGBoost (FEAT-6, #391; part of the native
distributed accelerated-compute pass for UC-1 churn, #264). The
[GBDT trainer](gbdt.md) orders accounts well (a strong AUC), but its raw
probabilities `sigmoid(raw)` are not necessarily **calibrated**: among the accounts
it scores at risk 0.7, the realized churn fraction may not be 0.7. Calibration fits
a monotone map `raw score → calibrated probability` on a **held-out** split, so the
score a churn dashboard shows means what it says.

- Header: [`include/zetta/ml/calibration.hpp`](../include/zetta/ml/calibration.hpp)
  — the `Calibrator`, the fit, the metrics (pure, host-side, non-throwing).
- Impl: [`src/core/ml/calibration.cpp`](../src/core/ml/calibration.cpp).
- Model integration: `GbdtModel::calibration` + `calibrate_model` in
  [`include/zetta/ml/gbdt.hpp`](../include/zetta/ml/gbdt.hpp); the distributed
  `train` job wires it via `CalibrationConfig` in
  [`include/zetta/ml/gbdt_train_job.hpp`](../include/zetta/ml/gbdt_train_job.hpp).
- Schema: [`schemas/calibration.schema.json`](../schemas/calibration.schema.json).
- Tests: [`tests/test_calibration.cpp`](../tests/test_calibration.cpp) +
  the distributed slice in [`tests/test_gbdt.cpp`](../tests/test_gbdt.cpp).

## The two methods

Both fit a map from the model's **raw score (logit)** — `GbdtModel::predict_raw` —
to a calibrated probability. Calibrating on the raw score (not the probability) is
the classic Platt formulation and is equivalent for isotonic (the probability is a
monotone transform of the raw score). With the Platt identity (`a=1, b=0`) the map
reduces to plain `sigmoid(raw)`, so an uncalibrated model is unchanged.

- **Platt scaling** — a two-parameter logistic fit, `sigmoid(platt_a·raw +
  platt_b)`. Robust on small calibration sets (only two parameters), but it assumes
  the miscalibration is sigmoidal. Fit by Newton's method with line search and
  Platt's target smoothing (`hi = (N₊+1)/(N₊+2)`, `lo = 1/(N₋+2)`, the Lin, Lin &
  Weng 2007 robust form), so a tiny held-out set does not pin the fit at the
  extremes.
- **Isotonic regression** — a non-parametric, **non-decreasing step function** fit
  by the Pool-Adjacent-Violators Algorithm (PAVA). More flexible — it can fix a
  non-sigmoidal distortion — but needs more held-out data to avoid overfitting. The
  fitted map is the unique isotonic regression of the labels on the scores.

Because both maps are monotone, **calibration preserves the ranking** — AUC is
unchanged (Platt exactly; isotonic up to ties introduced by flat segments) while
reliability improves.

## Reliability metrics

`calibration_metrics(probabilities, labels, bins=10)` returns the two standard
calibration metrics (lower is better for both):

- **Brier score** — the mean squared error `mean((p − y)²)`.
- **ECE (expected calibration error)** — bin the predictions into `bins`
  equal-width buckets over `[0,1]`; each non-empty bucket contributes
  `|mean(p) − mean(y)|` weighted by its share of the samples.

## Fitting + applying a calibrator

```cpp
// Fit on a held-out (raw score, label) set, then apply.
auto fit = zetta::ml::fit_calibrator(holdout_raw_scores, holdout_labels,
                                     zetta::ml::CalibrationMethod::Platt);
double calibrated = fit.calibrator.calibrate(model.predict_raw(features));
```

`fit_calibrator` is non-throwing: it fails (a failing `ComputeStatus`, no
calibrator) when the inputs disagree in length, are empty, or carry a single label
class (calibration is undefined without both classes).

### Attaching it to a model

A `GbdtModel` carries an optional `calibration`. When present, `predict_proba`
maps the raw score through it instead of a plain sigmoid, so a **calibrated model
is self-contained** and round-trips through the same weights JSON boundary.
`calibrate_model` fits + attaches in one step:

```cpp
// holdout_columns[feature][row] in the model's feature_names order; the rows must
// NOT be the rows the model trained on (calibrating on training data is leaky).
zetta::ml::calibrate_model(model, holdout_columns, holdout_labels,
                           zetta::ml::CalibrationMethod::Isotonic);
// model.predict_proba is now calibrated; model.to_json() carries the calibration.
```

## In the distributed `train` job

`run_train_job` calibrates the produced [`Model`](models.md) when a
`CalibrationConfig{enabled, method, holdout_fraction}` is passed (disabled by
default — the model is then uncalibrated, exactly as before):

1. **Held-out split.** Each node deterministically holds out a `holdout_fraction`
   slice of its shard, decided by a **content hash** of the row (every feature value
   + the label). Because the split depends only on the row's content — not on which
   node or local index holds it — the **same physical rows** are held out under any
   sharding. So the trees are grown on the same train rows and the calibrator is fit
   on the same held-out rows, which keeps single-node and multi-node **equivalent**
   (the same guarantee the histograms give the trees).
2. **Train on the train rows only.** The boosting loop runs over the train split;
   the held-out rows never enter the trees.
3. **Fit at the root, broadcast back.** Each node scores its held-out rows; the
   `(raw score, label)` pairs are **gathered** to the deterministic root, the
   calibrator is fit there, and **broadcast** back — so every node attaches an
   identical map to its model. A failed fit (e.g. a single-class held-out set)
   broadcasts an empty payload and the model is left uncalibrated (calibration is
   best-effort, it never fails the job). The reported metrics then reflect the
   calibrated probabilities.

**No new wire format** — the held-out pairs and the fitted calibrator ride the
existing schema-validated `CollectiveMessage` (via `JobRound::gather` /
`broadcast`), and the calibrator serializes into the GBDT model weights body, which
is schema-validated at its own boundary.

## The JSON boundary

A `Calibrator` mirrors [`schemas/calibration.schema.json`](../schemas/calibration.schema.json)
(draft-07, validated with the pboettch validator — the same path as the GBDT model
body it rides inside): a `method` (`platt` | `isotonic`) plus the active method's
parameters. `Calibrator::from_json` adds the structural rules the schema can't
express — Platt needs both parameters, and the isotonic arrays share a non-empty
length with non-decreasing bounds and non-decreasing values in `[0,1]` — so a
half-written or non-monotone map fails at the boundary with a path-qualified reason.
When attached to a `GbdtModel`, the model schema allows a loose `calibration`
object and the strict validation happens once in `Calibrator::from_json` (the way
the tree child-index rule is enforced after the model's schema pass).

## Acceptance (in `tests/test_calibration.cpp`)

- The calibrator + wire boundaries round-trip and reject malformed input
  (bad method, unknown key, a Platt map missing a parameter, isotonic arrays of
  unequal length / non-monotone bounds / values out of `[0,1]`).
- `calibration_metrics` gives 0 Brier + 0 ECE on perfectly-confident-correct
  predictions and 0.5 / 0.5 on confidently-wrong ones.
- On a **held-out split** of a deliberately miscalibrated (overconfident) model,
  **both Platt and isotonic lower Brier and ECE** while preserving AUC — the
  acceptance criterion.
- `calibrate_model` attaches a map that `predict_proba` applies, preserves the
  ranking (AUC unchanged), round-trips through the GBDT model schema, and rejects a
  single-class held-out set.
- The distributed `train` job (in `tests/test_gbdt.cpp`) produces a calibrated
  model whose weights re-validate, with single-node and multi-node fitting an
  equivalent calibrated model (the content-hash split is sharding-invariant).

> The synthetic [churn fixture](churn-dataset.md) is small (10 accounts) — too few
> for a statistically meaningful held-out calibration split — so the rigorous
> Brier/ECE improvement is demonstrated on a controlled synthetic miscalibration,
> while the fixture exercises the train → Model path end to end. As the churn data
> grows, the same held-out improvement applies directly to it.

## Scope + what's next

- **Methods:** Platt + isotonic. Temperature/beta calibration and multiclass
  calibration extend the same `Calibrator` seam.
- **Split:** a single train/calibration holdout. Cross-validated calibration
  (fit on out-of-fold predictions, like sklearn's `CalibratedClassifierCV`) would
  use the data more efficiently and is a later slice behind the same API.
- **Metrics on the Model:** the produced `Model` records AUC/log-loss/accuracy;
  recording Brier/ECE alongside them is a small follow-up.
