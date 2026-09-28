# Native time-series forecasting

**Issue:** [#1043](https://github.com/oramasearch/zetta/issues/1043) ·
**Epic:** [#984](https://github.com/oramasearch/zetta/issues/984) ·
**Code:** [`include/zetta/ml/forecast.hpp`](../../include/zetta/ml/forecast.hpp),
[`include/zetta/ml/forecast_runner.hpp`](../../include/zetta/ml/forecast_runner.hpp) ·
**Tests:** `tests/ml/test_forecast.cpp`, `tests/ml/test_forecast_runner.cpp`

Forecasting is a native C++26 methodology with model kind `forecast`. It trains a
global squared-error GBDT over lagged histories, evaluates it at rolling temporal
origins, and writes future point forecasts with residual-quantile prediction
intervals. It uses no foreign runtime or hosted model.

## Inputs

`run_forecast_table` takes a `ResolvedDataset` plus `ForecastRunParams`. The table
has:

- a required numeric timestamp column;
- a required numeric target column;
- an optional integer series-key column within the exact numeric collective
  range (−2^53 through 2^53); without it, the table is one series.

Rows may arrive unordered. The runner sorts each series by timestamp and refuses
missing, non-finite, or duplicate timestamps. The median observed timestamp step
sets future timestamps, so daily and weekly numeric timestamp encodings use the
same code path.

Each series must be complete on one partition owner. Before training, members
gather only their series keys and refuse a key owned by more than one member. This
keeps history local: raw observations stay with their partition owner while GBDT
histograms, metric partials, and the residual histogram cross the collective.

## Model and temporal evaluation

The model learns the correction over the seasonal-naive forecast:

```text
seasonal naive = value[t - seasonal_period]
training label = actual[t] - seasonal naive
point forecast = seasonal naive + predicted correction
```

Features are configured lags and an optional rolling mean. Every one ends before
the row being predicted. At a backtest origin, the fold model trains only on rows
before that origin. Multi-step evaluation is recursive: prediction one is appended
to history before prediction two is featurized. Actual values inside the evaluation
window never become features.

The final model trains after every fold has been measured. Its squared-error
`GbdtModel` is stored as the registered model's weights. Forecasts never attach a
probability calibrator because the output is a value, not a probability.

## Metrics

Every fold and the fold mean report:

| Metric | Meaning |
|---|---|
| MAE | Mean absolute forecast error. |
| MASE | Absolute error divided by the training history's seasonal-naive scale. |
| sMAPE | Symmetric absolute percentage error, in percent. |
| seasonal-naive MAE | Error from repeating the value one seasonal period ago. |
| forecast value added | `1 - model MAE / seasonal-naive MAE`; positive means the model won. |
| interval coverage | Share of backtest targets inside the prediction interval. |

`ForecastMetrics::comparison` says either `forecast beat seasonal-naive` or
`forecast did not beat seasonal-naive`. The baseline is never omitted.

The prediction interval is symmetric around the point forecast. Its half-width is
the requested quantile of absolute rolling-origin residuals, computed from a
fixed-bin histogram whose per-member counts are allreduced. This is prediction
uncertainty, not probability calibration.

## Outputs and lineage

The deterministic collective root writes:

- a replicated model with `kind = "forecast"`, mean and per-fold numeric
  backtest metrics, the seasonal-naive comparison text, and the serialized
  squared-error GBDT weights;
- a forecast table with `series`, `timestamp`, `point_forecast`, `lower`, and
  `upper` columns;
- `RunOutput` values for the table and model, plus lineage naming the resolved
  `database.table` source and the temporal-backtest, train, and write stages.

The runner refuses to replace an existing output table.

## Enforced correctness rules

- Rolling temporal origins only; no content-hash or random holdout.
- Lag zero is invalid, and rolling features end before their target row.
- Backtest fold models cannot see rows at or after their origin.
- Recursive multi-step forecasts cannot consume actual values after the origin.
- No probability calibration is accepted or attached.
- Horizon must be an integer count of observations and no longer than
  `maximum_horizon_fraction` of every local series history.
- A seasonal-naive baseline is always scored and compared through FVA.
- A series split across members is refused instead of training on partial history.
- GBDT histograms and evaluation partials use allreduce; single-member and
  multi-member results are equivalent within floating-point tolerance.

The algorithm contains no random sampling. `seed` is recorded in model provenance
and collective tags, so the same data, params, and seed produce the same result.

## Boundaries and later work

Phase A provides the native trainer and resolved-table runner. It does not yet
make forecasting runnable through the methodology registry or public API.

No new wire or JSON boundary is introduced. Numeric partials use the existing
schema-validated `CollectiveMessage`; model weights use the existing `GbdtModel`
boundary, expanded to admit its already-supported `squared_error` objective.

Phase B adds the thin methodology-table adapter after #1061 lands. Intent compiler,
capability handlers, registry entries, correctness checks over pipeline graphs,
run-report rendering, and control-surface schema changes are deliberately not part
of this phase.

## Verify

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Forecast*:*Backtest*:*Lag*:*Temporal*'
./build/tests/zetta-tests
```
