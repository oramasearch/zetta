# Native time-series forecasting

**Issue:** [#1043](https://github.com/oramasearch/zetta/issues/1043) ·
**Epic:** [#984](https://github.com/oramasearch/zetta/issues/984) ·
**Code:** [`include/zetta/ml/forecast.hpp`](../../include/zetta/ml/forecast.hpp),
[`include/zetta/ml/forecast_runner.hpp`](../../include/zetta/ml/forecast_runner.hpp) ·
**Tests:** `tests/ml/test_forecast.cpp`, `tests/ml/test_forecast_runner.cpp`,
`tests/ml/test_forecast_methodology.cpp`, `tests/api/test_action_journeys.cpp`

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

Phase B makes M-4 runnable through the methodology registry (#1061). Model
attributes are validated by `forecast_step_schema()`; peer-launch payloads by
`forecast_launch_schema()` followed by the existing graph parser and the same
forecast correctness check. Numeric partials use `CollectiveMessage`, and
weights use `GbdtModel` with the `squared_error` objective. Generated schemas are
served through the existing spec index.

The runner consumes owned partition bodies, not a full-table download. Each
series must belong to one member, and the total owned row count must equal the
catalog count exactly once. Replicated input partitions with multiple readers
and histories split across members are refused; repartition the input with one
complete history per owner before running. Missing bodies and incomplete
catalogs are errors. String series keys, arbitrary external features, automatic
repartitioning and directory-backed inputs are not supported.

Output materialization is deliberately single-node on the deterministic
collective root; training and metric partials are distributed. Node-tree reads
and artifact writes use the owner-thread gate. The background action uses the
shared coroutine runtime, and the existing coordinated-launch listener starts
peers. Model references are broadcast back to all participants.

## Running it

Land a numeric history table through `add_dataset` or a connector. Then call
`create_project`, `capture_intent`, and `compile_intent` with `methodology: "M-4"`
(or primary solution `S-5` / `S-6`). For example, the compile input is:

```json
{
  "project": "demand-plan",
  "input_id": "<captured input id>",
  "methodology": "M-4",
  "hints": {
    "source": "demand",
    "table": "history",
    "time_column": "day",
    "target_column": "demand",
    "series_column": "store_id",
    "horizon": 7,
    "seasonal_period": 7,
    "lags": [1, 7, 28],
    "maximum_horizon_fraction": 0.25,
    "interval_level": 0.8,
    "seed": 41
  }
}
```

Omit `series_column` for a single series. The source defaults are
`datasets.history`, `timestamp`, and `value`; name your actual columns when
different. Optional knobs are `rolling_window`, `backtest_folds`, `num_trees`,
`max_depth`, and `learning_rate`; their schema and defaults are code-owned.
Unknown hints are rejected by the gate rather than silently ignored.

Compilation returns `methodology: "M-4"` and `runnable: true`. Call `run` with
only the project and returned graph name. It reads the connect step's table;
there is no demo fallback or structural execution. Dataset overrides, a random
`holdout_fraction`, and `coordinate: false` are refused. Column roles are
resolved explicitly against the catalog into `ResolvedDataset`; the classifier's
binary-label/entity inference is intentionally not applied to forecast targets.

Poll `get_run` with the returned `run_id` until `succeeded` or `failed`. A
successful run writes `forecasts.forecast_<run id>`, one `forecast` model, and
their lineage. `get_run_report` returns:

- `model.metrics`: mean and `fold_<n>_*` MAE, MASE, sMAPE, seasonal-naive MAE,
  FVA and interval coverage;
- `baseline_comparison`: explicitly says whether seasonal-naive was beaten;
- `dataset`: source, selected columns, actual rows/partitions/bytes read,
  `rows_excluded: 0`, and all other columns with exclusion reasons;
- `outputs`: table, model and report references;
- `forecast`: output-table rows ordered by series and then timestamp, with
  `limit` applied after sorting. This order is stable across replicated members.

Unused columns are not model features. Invalid histories are refused in their
entirety, never silently dropped. Empirical backtest interval coverage is not a
guarantee about future observations. A peer can briefly report an output as not
yet visible while replicated artifacts arrive.

## Verify

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Forecast*:*Backtest*:*Lag*:*Temporal*:*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*'
./build/tests/zetta-tests
```
