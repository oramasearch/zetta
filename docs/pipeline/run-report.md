# The run report — what a run found

**Issue:** [#958](https://github.com/oramasearch/zetta/issues/958)
([#948](https://github.com/oramasearch/zetta/issues/948) step 4) ·
**Capability:** `get_run_report` ·
**Code:** `src/core/api/capabilities.cpp`, `src/core/usecases/churn/churn_step_executor.cpp` ·
**Tests:** `tests/usecases/churn/test_churn_solution.cpp`

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

There is no report document on disk. Every number in it already lives in replicated
state, and `get_run_report` puts them together at query time:

| Part | Source |
|---|---|
| model quality, dataset facts | the **Model record's** `metrics` |
| drivers | the Model's `feature_schema` (`ranked_drivers`) |
| the ranked list + evidence | the run's **at-risk table** in `churn_results` |
| M-8 recommendation rows and policy facts | the run's tables in `recommendation_results` |

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

**It always answers (#1007).** A run with no resolvable model output answers
`not_found` (404) saying there are no measured numbers — it does not wait for one.
The at-risk rows are read without typed accessors that throw: a `rank` or `risk`
that is not a number reads as `0`, and an `account` that is not an integer (a
business id never derived to a key, #957) is reported verbatim as `entity` rather
than failing the whole report. A dataset-less churn run (the built-in demo data) is
covered end to end over REST in `tests/api/test_rest_server.cpp`.

## What the run has to write down

The feature table is in memory and gone when the run ends, so the facts a report needs
are recorded as the run goes:

- **On the Model record** (`run_model`, root only): `dataset_accounts`,
  `dataset_events`, `dataset_churn_rate`, `dataset_majority_class_accuracy`, beside the
  `in_sample_*` / `held_out_*` metrics from training
  ([held-out metrics](../ml/held-out-metrics.md)).
- **In the at-risk table** (`run_act`): each row's values for the population's three
  strongest drivers, alongside `rank` / `account` / `risk` / `dominant_driver`.

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

**The report's discipline is part of the surface, not the prose.** A screen that shows
the figures without it undoes the work that produced them, so both renderers lead with
the held-out figure and say what it was measured on, never print accuracy without the
accuracy a constant answer would score, and **draw `missing` rather than skipping it**.

## Boundaries

No new JSON format and no new storage. `run.schema.json`'s output `kind` enum gains
`"report"`; `get_run_report` is a read capability (`GET`), on the REST surface, the MCP
tool list, and the agent's read-tool allowlist.

## Later slices

- Other methodology engines can add their own model-kind report view without storing
  a duplicate document.
- A `system.run_reports` read view, if listing them ever beats reading them one by one.
