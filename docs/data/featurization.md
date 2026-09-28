# Data-first featurization

**Issue:** [#1062](https://github.com/oramasearch/zetta/issues/1062)
**Code:** [`include/zetta/data/featurize_plan.hpp`](../../include/zetta/data/featurize_plan.hpp),
[`src/core/data/analyze_dataset.cpp`](../../src/core/data/analyze_dataset.cpp),
[`src/core/data/featurize_plan.cpp`](../../src/core/data/featurize_plan.cpp)
**Tests:** `tests/data/test_featurize_plan.cpp`, `tests/api/test_registry.cpp`

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

Methodology names remain strings until the methodology registry lands. Their aliases
and behavior live in one rule table exposed through `methodology_rule`; trainers do
not carry copies of these rules.

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

## Deferred phase

This phase returns analysis and plans. Executing a plan inside every methodology's
featurize step, validating graph inputs against that plan, and listing the resulting
features and exclusions in `get_run_report` remain deferred until the methodology
registry from #1061 is available. Until then, callers must materialize any required
dataset correction explicitly and methodology trainers remain unchanged.
