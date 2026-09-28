# Native anomaly detection

**Issue:** [#1046](https://github.com/oramasearch/zetta/issues/1046) ·
**Methodology:** M-7 · **Model kind:** `anomaly` ·
**Code:** [`anomaly.hpp`](../../include/zetta/ml/anomaly.hpp),
[`anomaly_job.hpp`](../../include/zetta/ml/anomaly_job.hpp),
[`anomaly_table.hpp`](../../include/zetta/ml/anomaly_table.hpp)

M-7 is runnable through the methodology registry, `compile_intent`, and `run`. The native trainer,
distributed job adapter, resolved-table runner, and reports have no foreign ML runtime, hosted model,
or new third-party dependency.

## Running it

Land a numeric table with `upload_dataset`, `add_dataset`, or a connector, then call the existing
REST/MCP capabilities in this order:

```json
{"tool":"create_project","input":{"name":"request-anomalies"}}
{"tool":"capture_intent","input":{"project":"request-anomalies","text":"Find unusual latency and errors"}}
{"tool":"compile_intent","input":{"project":"request-anomalies","methodology":"M-7","name":"detector","hints":{"source":"operations","table":"requests","entity_column":"request_id","feature_columns":["latency","errors"],"alert_budget":2}}}
{"tool":"run","input":{"project":"request-anomalies","graph":"detector"}}
```

Each line describes a capability call (`POST /v1/<tool>` with its `input`, or MCP `tools/call`).
`solution: "S-7"` also selects M-7. The compiled pipeline is **read observations → robust feature
deviations → anomaly model → anomaly/report tables**. Its model step has `model_kind: "anomaly"`,
the explicit alert policy, seed, feature roles, and robust scaling settings. `run` uses the connect
step's `source` database and `table` when no `dataset_path` override is supplied. It never substitutes
demo data or structural execution.

The job returns a `run_id` immediately. Poll `get_run` to `succeeded` or `failed`; then call
`get_run_report` with that id. An M-7 report contains `model.metrics`, ranked `anomalies`,
`output_tables`, and `dataset` facts: source, rows read/excluded, feature roles, and `not_used`
columns with reasons. A missing optional label produces an explicit missing-precision note, not
supervised AUC or holdout advice. All rows are scored or the run fails; no malformed row is silently
dropped, so `rows_excluded` is zero for successful runs.

Omitting `feature_columns` conservatively selects numeric columns from a sample, excluding the
declared entity, label, time, and internal row id. Full input validation still checks every row
during training. A label is never inferred or required: name `label_column` only for evaluation.
Explicit feature lists work directly from the catalog without sampling. Grouped series are refused;
select one series per table. Input must have placed partitions, as the upload and connector paths do.

For KPI detection, use `mode: "kpi_time_series"`, `time_column`, the metric `feature_columns`, and
optionally `seasonal_period`, `trend_window`, `residual_window`, and `seasonal_cycles` in the hints.
Time must be numeric and unique; aggregate duplicate periods before the run. Seasonal periods count
observations, so regular cadence is the caller's input contract. The feature stage becomes causal
trend/seasonality removal. KPI remains single-member until ordered distributed prefix state exists.

The default correctness gate includes `AnomalyGraphCheck`: it refuses ambiguous or absent budgets,
label leakage or training-label use, non-robust scaling, missing KPI time/causality, malformed or
unbounded parameters, unsupported grouping, and a graph whose operations or data flow the runner
cannot execute. Dataset overrides are checked again before creating a Run.

## Inputs and modes

`run_anomaly_on_table` takes a `data::ResolvedDataset`, `AnomalyParams`, and an optional `Run` to
receive outputs and lineage. A label is not required. When the resolved contract names a binary
label column, the runner carries it only into evaluation; no training or score calculation reads
it.

There are two modes:

- **Tabular:** numeric feature columns, with an optional integer entity column. Missing entity keys
  fall back to stable row ordinals. The default is 100 isolation trees with 256-row subsamples.
  Each row also receives robust per-feature median/MAD deviations for explanations and for strong
  univariate anomalies.
- **KPI time series:** an explicit numeric time column plus one or more numeric metric columns. The
  default seasonal period is 7 observations. Trend, seasonality, residual center, and residual
  scale are fitted from earlier observations only.

`AnomalyThresholdPolicy` requires exactly one of:

- `contamination` in `(0, 1]`; or
- a positive integer `alert_budget`.

The default parameter object explicitly selects 1% contamination. The runner refuses a policy
with both values or neither value.

## Tabular algorithm and distribution

Tree assignment depends only on the seed, tree number, and sorted catalog partition ids. The
primary owner of the selected partition draws a reproducible sample without replacement and grows
that isolation tree. It serializes the bounded tree into numeric collective values; every other
member contributes zeros, and `JobRound::allreduce` combines the one tree into the common forest.
The catalog primary-owner rule prevents replicated partition bodies from training a tree twice.

This produces the same forest when the same physical partitions move between members. The
multi-member test runs real collectives and compares every row score with the single-member oracle
over those same partitions.

The isolation score is the conventional normalization of mean path length:

```text
score = 2 ^ (-mean(path_length / expected_path_length(sample_size)))
```

Short paths produce scores near one. For robust univariate evidence, each partition contributes a
feature median and a median absolute deviation. Fixed catalog slots are summed through two
allreduces, then combined in partition order with row-count weights. A row's feature deviation is
`abs(value - center) / max(1.4826 * MAD, numeric_floor)`. The three largest deviations are retained
as contributors. The row score is the larger of its isolation score and its bounded robust score.

The registry adapter fans a validated graph out through the existing coordinated-launch listener;
each member loads its primary-owned partitions. Every launch has a unique Run-derived collective
tag. After local scoring, members gather compact score/explanation rows. The root applies the exact
global budget, resolving score ties by stable row ordinal, and broadcasts the result. Therefore
`flagged / rows_scored` matches the requested integer budget or the rounded contamination count,
even when several rows have the threshold score. The initiating member materializes one set of
replicated outputs, even when a different member is the collective root. Flags survive the result
broadcast and agree with the report's flagged count.

## KPI algorithm and the no-future rule

Rows are sorted by time. At position `N`, each metric is decomposed with only positions before
`N`:

1. A trailing Theil-Sen trend window estimates slope and median intercept, then extrapolates to
   the current period.
2. The seasonal component is the median of prior detrended values at the same seasonal phase,
   bounded by `seasonal_cycles`.
3. The residual is scored against the median and MAD of the prior `residual_window` residuals.

The current observation does not fit its own baseline, and later observations are never read.
Appending a future point therefore leaves every earlier score unchanged. The exact alert budget is
applied to the complete run's ranking; adding observations can change that retrospective flagged
set, but cannot change historical fitted baselines or scores. Ordinary weekly peaks are
learned as seasonal structure; a planted spike remains in the residual and ranks first.

KPI execution remains single-member. A multi-member call fails with an explicit reason. A
correct distributed KPI implementation needs ordered prefix state across time partitions; this
phase does not gather the raw series to one coordinator and present that as distributed compute.

## Outputs

The output member registers a `Model` with kind `anomaly`. Its weights body holds
the mode, seed, feature order, robust centers/scales, isolation trees, and KPI window settings. Its
numeric metrics include the threshold, row count, flagged count, alert rate, the selected budget,
and `precision_at_k` when labels exist.

The runner writes two replicated, queryable tables under `anomaly_results`:

- `anomalies_<run>`: `rank`, `row`, optional `period`, `score`, `flagged`, `threshold`, the top
  contributor and deviation, the contributor array, and an optional evaluation label;
- `report_<run>`: one row containing the threshold, contamination or alert budget, rows scored,
  flagged count, alert rate, and optional precision@k.

When a `Run` is supplied, it receives model and table output references. Its lineage names the
resolved `database.table` source and the `anomaly:train` and `anomaly:score` steps.

## Correctness rules enforced in code

- Labels are optional, excluded from feature columns, and used only for precision@k.
- Exactly one explicit threshold policy is required and the chosen threshold is reported.
- Alert counts are exact, including score ties.
- Isolation trees and partition summary reductions are deterministic under the seed.
- Feature and residual scales use medians and MAD, not mean/standard deviation.
- KPI baselines read only earlier observations.
- Malformed, missing, non-numeric, or non-finite input columns fail before training.
- A partition tree must be supplied by exactly one primary owner or the collective job fails.

## Boundaries and deferred work

The collective data uses the existing numeric `CollectiveMessage`; model metadata, weights, run
lineage, and table rows use existing validated storage boundaries. The existing `get_run_report`
output schema documents M-7 facts and output tables; no new capability is introduced. Peer launches
reuse the validated ExecutionGraph wire format.

Phase B implements the `Methodology` entry, own compiler shape, correctness check, asynchronous
registry runner, report surface, coordinated tabular launch, generated OpenAPI update, and registry
journeys. It is stacked on registry PR #1066. Lake Opportunity integration remains outside this
work order. Multi-member KPI prefix state and grouped series remain unsupported and are refused.

## Verification

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 6 --target zetta-tests
./build/tests/zetta-tests --gtest_filter='*Anomal*:*Isolation*:*Seasonal*:*Methodology*:ActionJourneys.*:Uc1Journey.*:*GraphCheck*:CorrectnessGate.*'
ZETTA_WRITE_SCHEMAS=1 ./build/tests/zetta-tests
```

The focused suite covers planted tabular outliers, label-free training, optional-label precision,
exact alert budgets, causal seasonal scoring, future-point invariance, real multi-member forest
equivalence, coordinated registry launches, gate refusals, and queryable output tables with run
lineage. The full command also regenerates the control-surface schemas.
