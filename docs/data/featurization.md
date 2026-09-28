# Data-first featurization

**Issue:** [#1062](https://github.com/oramasearch/zetta/issues/1062)
**Code:** [`include/zetta/data/featurize_plan.hpp`](../../include/zetta/data/featurize_plan.hpp),
[`src/core/data/analyze_dataset.cpp`](../../src/core/data/analyze_dataset.cpp),
[`src/core/data/featurize_plan.cpp`](../../src/core/data/featurize_plan.cpp)
**Tests:** `tests/data/test_featurize_plan.cpp`, `tests/api/test_registry.cpp`,
`tests/api/test_action_journeys.cpp`, `tests/pipeline/test_run.cpp`

Featurization begins by reading the landed data. The read-only `analyze_dataset`
capability describes the sampled columns and table shape. The read-only
`plan_features` capability applies the selected methodology's rules to that analysis.
It either returns an explicit feature/exclusion plan or refuses to plan until the
dataset shape is corrected.

Neither capability writes a table, changes the input rows, or starts a run.

## Capability inputs

Both capabilities accept one dataset reference:

```json
{ "dataset": "accounts", "database": "churn_demo", "sample_rows": 1000 }
```

or:

```json
{ "dataset_path": "churn_demo.accounts", "sample_rows": 1000 }
```

`dataset` may also be qualified as `database.table`; `database` disambiguates a bare
name. Supplying both reference forms is rejected. `plan_features` additionally
requires a methodology string and accepts `label`, `time_column`, `group_column`, and
`as_of`:

```json
{
  "dataset_path": "churn_demo.events",
  "methodology": "M-1",
  "label": "churned",
  "time_column": "event_at",
  "as_of": "2026-09-01T00:00:00Z"
}
```

Methodology names use the registry's string IDs, M-1 through M-16, and their human
aliases. Their behavior lives in one rule table exposed through `methodology_rule`;
trainers do not carry copies of these rules. M-2 is entity resolution, M-3
segmentation, M-4 forecasting, M-5 survival, and M-6 uplift.

## Column analysis

Each column reports its semantic type, the evidence for that type, possible roles,
null count/rate, distinct count/ratio, most common values, numeric quantiles, string
length statistics, and detected numeric sentinel values. Inference applies these
rules in order:

- **boolean:** native booleans or a numeric column with exactly two values;
- **datetime:** at least 95% of non-null values parse as dates or epoch values in a
  plausible range;
- **free text:** median length over 30 characters or high token diversity;
- **identifier:** more than 98% distinct, sequential, UUID-shaped, or named like an
  identifier;
- **categorical:** no more than 50 distinct values and no more than 5% of sampled
  rows by default; `AnalysisOptions` exposes both thresholds;
- **numeric:** numeric after sentinel codes and integer-coded categoricals are checked;
- **JSON/unknown:** retained as explicit unsupported types rather than guessed.

Names also nominate entity keys, labels, times, treatment assignments, ranking
groups, and post-treatment values. Those roles are evidence for planning, never a
silent edit to the data.

## Table analysis

The analysis returns candidate keys with their observed uniqueness and one table
grain:

- **entity:** a unique candidate key gives one row per entity;
- **event:** an entity repeats and an event timestamp is present;
- **time series:** entity/time pairs are unique while entities repeat over time;
- **snapshot:** an entity has distinct observation times;
- **ambiguous:** the sample does not establish one of the preceding shapes.

Other non-degraded tables in the same database are sampled through the same read
path. A table is reported as joinable when an identifier pair has at least 20%
sampled key overlap. The result includes both column names and the roles available
on the other table, allowing the planner to identify a likely label table.

Leakage candidates include columns named like the label, values that reproduce the
label, absolute numeric correlation of at least 0.98, and timestamps later than the
declared `as_of`. Each finding includes its reason and score.

## Feature plans

Every included entry has `source_columns`, `features`, `transform`, and `reason`.
Every excluded column has its own reason. The common rules are:

- numeric values use training-row median imputation plus a missing flag; sentinel
  values become missing first; distance-based segmentation/anomaly methods use
  robust scaling, while tree methods do not scale;
- categoricals with at most 20 levels use one-hot encoding; higher-cardinality
  supervised categoricals use out-of-fold target encoding with smoothing `m = 20`;
  levels occurring fewer than 10 times or below 0.5% become `OTHER`, and missing is
  its own level;
- identifiers are excluded and are never target encoded;
- datetimes become elapsed age/tenure/recency relative to `as_of`, plus calendar
  parts and cyclical hour/weekday values; the raw timestamp is excluded;
- free text is excluded with the reason `needs M-10 embeddings`;
- forecasting adds prior-only lags, rolling windows, and calendar values, uses a
  temporal split, and never target encodes across the forecast origin;
- uplift excludes post-treatment columns;
- segmentation excludes identifiers and labels;
- ranking requires and reserves a group column.

For event data used by a per-entity methodology, the planner requires point-in-time
aggregation. The requested plan names counts, numeric sum/mean/min/max, distinct
counts, last values, and time since last over 7/30/90-day and all-history windows up
to `as_of`, with source/window provenance.

## `transform_required`

The planner never hides a data-shape change inside a feature plan. It returns
`status: "transform_required"` when the unit of analysis is wrong or a required
field is absent. Named corrections include:

- aggregate events to one point-in-time row per entity;
- pivot long measurement rows into feature columns;
- join a label table on the reported overlapping key;
- add an entity key, label, time, snapshot/`as_of`, treatment, or ranking group.

Where the SQL subset in [query.md](query.md) can express the correction, the result
includes `proposed_create_dataset_from_query_sql`. Event aggregation uses supported
`WHERE`, `GROUP BY`, `COUNT`, `SUM`, `AVG`, `MIN`, and `MAX`. Windowed aggregates,
`JOIN`, and pivoting are returned as an explicit `clean_step` because the current SQL
subset cannot express them. A caller must create and approve the transformed dataset,
then request a new analysis; nothing is applied automatically.

## Read and distribution boundaries

The implementation extends the canonical [`dataset_path`](dataset-path.md) resolver
with `sample_dataset`, so contract inference, analysis, and planning share catalog
lookup, degraded-table refusal, empty-table refusal, and sampling behavior. The
default sampler reads through `query::QueryService`; partition-only tables therefore
use distributed scatter/gather rather than a local partial read.

Sampling currently reads through the query layer and keeps the first requested rows
because the SQL subset has no `LIMIT`. Inference is evidence from that sample, not a
claim about unobserved rows. Analysis is a value result only: it creates no durable
entity, format, or model artifact.

## Phase B: the plan is a run precondition

Every runnable registry callback calls the library directly before creating a Run
or launching training. `ml::plan_run_features` analyzes the pipeline's landed table
through the distributed reader, then calls `data::plan_features`. A required pivot,
entity aggregation, missing role, or snapshot correction returns `invalid_input`
with `transform_required`, the transform name, and its reason. No correction is
executed automatically. M-1 now uses the Connect step's landed table when no
`dataset_path` override is supplied; its built-in demo and directory inputs record
the actual native account/event specification instead.

The same helper reuses `DataLeakageCheck` and `CausalUpliftCheck` for selected
sources. Explicitly selected leakage suspects, the label, and post-treatment uplift
inputs are refused, including graph annotations. Inferred unwanted columns are
excluded with reasons. M-3 retains its existing label/role gate. Sampling is still
evidence, not a guarantee about unseen rows or the causal availability of data.

The execution contract describes what the trainer can actually consume:

| Family | Planned execution |
|---|---|
| M-1, M-5, M-6, M-9 numeric table trainers | The accepted source list replaces the numeric dataset contract before training. Explicit pipeline selections are respected; labels, keys, leakage and unsupported inputs are excluded. Prepared numeric columns are carried through without claiming an unfitted encoder or scaler. |
| M-2 identity resolution | Native normalized pair similarities, missing indicators, and verified-identifier constraints. |
| M-3 segmentation | The existing global fitted numeric scaling/imputation and bounded one-hot encoder; reports use its fitted encoded names and exclusions. |
| M-4 forecasting | The pipeline's actual lag offsets and prior-only rolling window, evaluated at rolling origins; the raw current target is excluded. |
| M-7 anomaly | Native isolation-forest/MAD features or strictly prior KPI residuals. |
| M-8 recommendations | The pipeline's numeric propensity features, historical ALS affinity, and value/eligibility policy inputs; coordinated members retain the identical contract. |
| M-10, M-11, M-13 | Native text embeddings, document extraction, and image embeddings from explicitly named sources. |
| M-12 graph | Time-bounded weighted topology and embedding dimensions; endpoints remain structural keys, not scalar predictors. |
| M-14 speech | Consented WAV decoding, 16 kHz resampling, energy segmentation, and log-mel input to native Whisper. Recording IDs and reference transcripts are excluded as predictors; consent is a guard, not an audio feature. |
| M-15 bandits | Per-arm reward posterior statistics, plus the numeric context actually selected for LinUCB or linear Thompson. Context-free Thompson does not invent context features. Reward and decision ID remain excluded as predictors. |
| M-16 optimization | Named variables, objective coefficients, integrality, and present bounds, baseline, scenario/quantile, and constraint fields are passed unchanged to the native solver. Row IDs are excluded; `integer` is domain metadata, not a supervised label. |

M-1's native event specification also records aggregate names, source value/type
columns and window bounds. M-5 records its native person-period `interval` feature
and configured interval width; raw follow-up duration remains excluded as a predictor.

The read-only advisory plan's generic encoding suggestions are not represented as
executed transformations when a trainer has no such input contract. Unsupported
sources name the required clean/encoding step; a plan with no consumable features
requires `create_eligible_features`. This integration adds no new trainer, target
encoder, embedding runtime, or silent table writes. Native temporal featurizers
retain their existing cutoff/split checks instead of inventing a snapshot date.

Each accepted run stores its feature provenance in the replicated Run record.
`get_run_report` presents `features.used` and `features.excluded` with reasons for
every methodology, as described in [run-report.md](../pipeline/run-report.md).
Legacy records remain readable and only report what their fitted model recorded.

The action-journey tests verify reports across the runnable families, reject a
long-measurement table in every runnable callback before any Run is created, reject
event-grain M-1 inputs, and exercise selected leakage, clustering labels, and uplift
post-treatment refusal. Run tests verify provenance persistence and schema rejection.

Round 3 covers M-14–M-16 through their methodology-named pipelines as well as
direct callbacks. The long-measurement refusal runs before speech checkpoint or
audio access, bandit policy construction, and optimization solving. Contextual
bandit tests exercise inferred and explicit context lists and reject reward-copy,
raw reward, and decision-key context before a Run exists. Historical reward is a
source only for the native per-arm posterior, never a decision-time context input.
M-17–M-21 are outside this integration round.
