# The run report — what a run found

**Issues:** [#958](https://github.com/oramasearch/zetta/issues/958),
[#1055](https://github.com/oramasearch/zetta/issues/1055)
([#948](https://github.com/oramasearch/zetta/issues/948) step 4) ·
**Capability:** `get_run_report` ·
**Code:** `src/core/api/capabilities.cpp`, `src/core/usecases/churn/churn_step_executor.cpp`,
`src/core/ml/rank_runner.cpp` ·
**Tests:** `tests/usecases/churn/test_churn_solution.cpp`,
`tests/ml/test_streaming.cpp`, `tests/api/test_action_journeys.cpp`

A run's outputs say what it *produced* — a dataset, a model, an index. A run's
**report** says what it *found*: how good the model is, what drives the outcome, and
which entities are most at risk. The prose is written from it by the
[run-report prompt](run-report-prompt.md); this page is the artifact underneath.

## Reading it

```jsonc
GET /v1/get_run_report?run_id=<uuid>&limit=20
```

The run itself names the report among its outputs:

```jsonc
{ "kind": "report", "name": "run report", "ref": "<the run's own id>" }
```

## Assembled on read, not stored

For [M-7 anomaly runs](../ml/anomaly.md), the same capability returns
`methodology: "M-7"`, native `model.metrics` (threshold, budget, alert rate, optional
precision@k), ranked `anomalies` capped by `limit`, and both `output_tables`.
`dataset` records the actual source, rows read/excluded, feature roles, and excluded
columns with reasons. Its source is the anomaly Model plus this Run's anomaly table;
it does not invent supervised quality metrics or holdout advice. A missing evaluation
label is reported in `missing`. The remaining sections describe the M-1 report.

There is no report document on disk. Every number in it already lives in replicated
state, and `get_run_report` puts them together at query time:

| Part | Source |
|---|---|
| model quality or solver facts, dataset facts | the **Model record's** `metrics` |
| drivers | the Model's `feature_schema` (`ranked_drivers`) |
| the ranked rows | the run's result table in `churn_results` or `rank_results` |
| survival quality | the survival Model's concordance and IPCW Brier metrics |
| survival outputs | the run's survival and interval-calibration table refs |
| M-8 recommendation rows and policy facts | the run's tables in `recommendation_results` |
| used features and exclusions with reasons | the Run's accepted feature plan, plus fitted segmentation encoding metadata |
| decisions + constraint checks | an optimization run's plan and constraint-trace tables in `optimization_results` |
| M-17 uncertainty metrics | the Bayesian Model's numeric `metrics` |
| M-17 source and output tables | the Run's lineage and dataset outputs |
| M-18 lift and base weights | the ensemble Model's metrics and drivers |
| queryable result tables | the Run's dataset outputs |
| streaming prequential metrics and drift | the streaming Model record's `metrics` |
| streaming source, exclusions, topics, and score table | the run's lineage, logs, Model feature contract, and outputs |

For an M-2 entity-resolution run, the same capability returns the registered
model's native metrics and scoring method, the stated match/review/non-match
thresholds, rows read and excluded, and both output-table references. Labelled
runs include blocking recall, pairwise precision/recall/F1, and B-cubed
precision/recall. Unlabelled runs omit those label-dependent measurements.

That is deliberate. A stored copy is a second set of the same numbers, and two copies
of a number drift. It also means a report generated a month later reflects the same
facts as one generated the moment the run ended, and that anything querying
`churn_results` directly cannot disagree with the report about who is at risk.

It follows RULE-7: observable state is a view computed from the live source, never a
hand-synced copy.

For a `next_best_action` Model, the report returns the Model's hit-rate and IPS/DR
metrics, the selected and excluded input columns, every output reference, ranked
entity-action rows with expected value and `why`, and the policy report's clipping
diagnostics. It always includes the warning that response propensity is predictive,
not causal lift, and directs treat/no-treat decisions to M-6.

M-15 bandit reports use the same on-read rule. Their Model record supplies
`cumulative_reward`, fixed-baseline reward, regret, exploration share, decision
count, and rows read/excluded. Run lineage supplies the source table, while Run
outputs name the allocation-policy and propensity-log tables. The report exposes
those as `metrics`, `dataset`, `policy_table`, and `decisions_table`; it does not
manufacture churn drivers or an at-risk ranking for a policy run.

## Optimization reports

For an M-16 run, `model` carries `solver_status`, objective value, current-plan
objective, objective delta, feasibility, MIP optimality gap, constraint
violations, simplex iterations, and explored branch nodes. `dataset` names the
table actually read, the row count, and excluded fields recorded by dataset
resolution. `plan` contains one row per decision variable and `constraints`
contains the independently recomputed activity, bounds, residual, and satisfied
flag for every constraint.

Infeasible or unbounded problems still return status and report metrics, but
the run has no plan output. A feasible plan is published only after the
independent verifier confirms every variable bound, integrality requirement,
and constraint within the declared tolerance.

**It always answers (#1007).** A run with no resolvable model output answers
`not_found` (404) saying there are no measured numbers — it does not wait for one.
The at-risk rows are read without typed accessors that throw: a `rank` or `risk`
that is not a number reads as `0`, and text entity values remain the original
business ids after their internal derived keys are resolved (#957). A dataset-less churn run (the built-in demo data) is
covered end to end over REST in `tests/api/test_rest_server.cpp`.

## What the run has to write down

### Features used and excluded (#1062)

All 21 runnable methodologies include the same provenance shape:

```json
{
  "features": {
    "used": [{
      "name": "lag_7",
      "source": ["value", "time"],
      "transform": "strictly past lag 7",
      "why": "forecast history is ordered within series and evaluated with rolling origins"
    }],
    "excluded": [{
      "name": "value",
      "why": "the label is a target or evaluation role, never a contemporaneous feature"
    }]
  }
}
```

`source` is an array because native transformations can read several columns. The
label and entity key are reserved roles, not scalar predictors. A structural key
or historical target can therefore appear as a transformation source while also
being excluded as a direct feature. Leakage suspects and post-treatment uplift
columns have explicit exclusion reasons; selecting unsafe inputs refuses the run.

The accepted choices are stored on the Run through the schema-validated
`set_features` mutator and survive persistence and replication. The report is still
assembled on read, without a second report document. Segmentation expands the
record using its fitted encoder's actual names, scaling, and exclusions. Other
native featurizers record their own executed transforms, not generic suggestions
that their trainers do not implement. See [featurization.md](../data/featurization.md).

M-14 records `audio_log_mel` from the configured audio-reference column; recording
IDs, consent metadata, and reference transcripts are not prediction features.
M-15 records `arm_policy_statistics` sourced from arm and historical reward, plus
the numeric context columns actually passed to the policy. Reward is not allowed
as decision-time context, and decision ID remains a reserved role. M-16 records the
solver's objective, variable domain, and available constraint/scenario inputs,
with row ID excluded. Its `integer` column is used integrality metadata, not a
classification label. All three reject a required data transform before creating
the Run, so a rejected plan cannot produce a trained-model report.

M-17 records `hierarchical_group_statistics` with group and observation sources;
the observation label is excluded as a direct predictor. M-18 records accepted
numeric base inputs and `oof_<base-name>` predictions for the native stacker, using
the fitted model's base names. M-19 records normalized numeric inputs and named
`weak_vote_<function-name>` threshold votes. M-20 records empirical-marginal/copula
inputs and the configured clipping bounds used for the private model. Their label
and entity roles are excluded with reasons.

M-21 records `window_count` and `window_sum_<source>` with entity, event-time and
numeric-value provenance. The transform states the configured window duration and
allowed lateness, including the current accepted event; prediction precedes its
label update. Raw event time and delayed-label availability are excluded as scalar
predictors. All five callbacks reject transform-first data and selected leakage
before creating a Run or launching work. Provenance uses the existing replicated
Run field and report schema; it adds no new report format.

Older/direct-trainer Runs without provenance remain readable. Their report uses
the fitted Model's feature-name list when present, explicitly saying detailed
provenance was not recorded; it does not reconstruct historical exclusions from
today's mutable table.

### Measured outcomes

The feature table is in memory and gone when the run ends, so the facts a report needs
are recorded as the run goes:

- **On the Model record** (`run_model`, root only): `dataset_accounts`,
  `dataset_events`, `dataset_churn_rate`, `dataset_majority_class_accuracy`, beside the
  `in_sample_*` / `held_out_*` metrics from training
  ([held-out metrics](../ml/held-out-metrics.md)).
- **In the at-risk table** (`run_act`): each row's values for the population's three
  strongest drivers, alongside `rank` / `account` / `risk` / `dominant_driver`.
- **For M-21 streaming runs:** prequential loss and accuracy, drift delta,
  event/exclusion counts and watermark on the Model; resolved table and input
  topic in lineage; excluded columns in logs; and model, score-table, and report
  outputs on the Run.

**Every value on the Model's `metrics` must be a number.** `model_schema()` declares it
as `additionalProperties: {"type": "number"}`, and the rejection happens at the
*receiving* peer — a non-number registers on the root and then fails to materialize on
the cluster (#953).

**The evidence columns are the same on every row**, chosen by the population driver
ranking rather than per entity. Picking each account's worst-looking numbers
individually would make rows incomparable, and is a way of lying with true figures.

**`dataset_events` is cluster-wide.** The featurize job allreduces it; a node only sees
its own shard, and a report that says how much data the model was built from cannot be
assembled from one shard's view.

## `missing` — what it says it cannot tell you

Anything a report would want and this run cannot supply is listed with a reason rather
than left blank:

```jsonc
"missing": [
  { "field": "held_out_auc",
    "why": "this run set no holdout_fraction, so nothing was measured on rows the
            model had not seen. Every figure below is in-sample and flatters…" }
]
```

A blank field invites the writer to substitute its own number, which is the one thing
[the report rule](run-report-prompt.md) forbids. A stated absence does not.

## Where it is rendered

Both Studio surfaces read this capability — **one behavior path**, so the GUI, the web
app, MCP and chat cannot disagree about what a run found:

| Surface | Where |
|---|---|
| Web app | `?view=reports` — index, then `?view=reports&run=<id>` (`web_ui_run_report`) |
| Desktop GUI | Data library → Reports (`src/gui/app_reports.cpp`) |

Reports is **one item under Data library** on both, matching the design
(`oramasearch/orama-design@4621d87`); the import is recorded in
[`docs/product/experience/design-sync.md`](../product/experience/design-sync.md).

## Bayesian uncertainty reports

An M-17 report returns an `uncertainty` object instead of churn ranking fields.
It carries nominal and empirical hold-out coverage, mean interval width,
hold-out row count, and the exchangeability assumption. `dataset` names the
table actually read, its excluded-row count, and any columns not used.
`output_tables` names the materialized per-group posterior table. Every numeric
value comes from the replicated Bayesian Model record; the assumption and table
refs come from the Run.

**The report's discipline is part of the surface, not the prose.** A screen that shows
the figures without it undoes the work that produced them, so both renderers lead with
the held-out figure and say what it was measured on, never print accuracy without the
accuracy a constant answer would score, and **draw `missing` rather than skipping it**.

## Boundaries

Feature provenance extends `run.schema.json` with an optional, strictly validated
`features` object; older records default to empty arrays. The output `kind` enum
includes `"report"`. `get_run_report` remains a read capability (`GET`), on the REST
surface, the MCP tool list, and the agent's read-tool allowlist. Its OpenAPI response
requires the same feature-provenance shape.

## Learning-to-rank reports

For a model whose kind is `rank`, `get_run_report` reports `ndcg_at_k`,
`yield_at_k`, the cutoff, and train/held-out row and group counts. Its result
table is returned as `ranked_queue` rows with group, entity, score, rank, and
relevance. Classification-only AUC fields and at-risk wording are not applied
to these runs.

M-18 reports add `best_single_held_out_auc`,
`held_out_auc_lift_over_best_single`, `meta_model_weights`, and `output_tables`.
Its blended-score table uses the same `rank` / `account` / `risk` columns, so the
ranked rows remain available through the common report shape. The dataset section
names the pipeline table from lineage and lists columns excluded during contract
resolution.

## Later slices

- M-3 segmentation also emits a `report` output. Its typed report contains
  `model.metrics`, `model.chosen_k_reason`, `model.k_candidates`, input table and
  row counts, fitted features, excluded columns, `output_tables` and a bounded
  `segments` preview. It does not invent churn accuracy or an at-risk list for
  unsupervised clusters. See [segmentation.md](../ml/segmentation.md).
- Other methodology engines can add their own model-kind report view without storing
  a duplicate document.
- A `system.run_reports` read view, if listing them ever beats reading them one by one.

## Forecast reports (M-4)

The native [forecast runner](../ml/forecast.md) also emits a report reference.
For `kind: forecast`, this capability returns `model.metrics` (per-fold and mean
MAE, MASE, sMAPE, seasonal-naive MAE, FVA and interval coverage), the explicit
`baseline_comparison`, `dataset` read/exclusion facts, and `outputs` references.
It does not substitute classification metrics or request probability calibration.

`forecast` rows come from the output table and are sorted by numeric series key,
then timestamp, **before** `limit` is applied. Replicated table storage order is
not chronological, so limiting in storage order would return different periods
on different members. The multi-member forecast launch test checks identical
timestamps and predictions against the single-member oracle, plus the limited
prefix. Tables not yet replicated are identified under `missing`.
