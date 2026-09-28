# AutoML studies

An AutoML **study** searches model families and hyperparameters against one
metric on a named validation split. A **trial** is exactly one durable
[`Run`](../pipeline/runs.md): its sampled parameters include `study.id` and
`study.seed`, intermediate objective values use [Run metrics](run-metrics.md),
and its terminal state is `pruned`, `failed`, or `complete`.

Studies are native C++ records. They do not load a foreign ML runtime or call a
hosted model service. Trial batches use the local work-stealing scheduler, and
the study runner accepts the same native evaluator on every worker.

## Study definition

The objective has `metric`, `direction` (`minimize` or `maximize`), and
`validation_split`. `test` is rejected as a validation split, so model
selection cannot consume final test data. If a project Pipeline is supplied,
`create_study` runs the existing correctness gate before accepting it; this
reuses methodology leakage checks.

Search parameters are ordered and typed:

- `int` and `float` have minimum/maximum bounds, an optional step, and linear
  or logarithmic sampling.
- `categorical` has a closed list of scalar choices. `model_family` may select
  GBDT, linear, or a methodology learner.
- A parameter is conditional when `condition_parameter` names an earlier
  parameter and `condition_values` lists the parent values that activate it.
- `from_defaults: true` supplies the selected methodology's model-family
  defaults. M-1 defaults include conditional GBDT and linear controls.

Samplers are `grid`, `random`, and TPE. Pruners are `median` and ASHA, plus
`none`. Median and ASHA compare the current Run metric with completed peers at
the same step and cooperatively end a losing trial. Every seed is persisted.

Budgets contain `max_trials`, `max_wall_time_ms`, `max_parallel_trials`, and
`cost_cap`. The runner checks every active limit at admission boundaries,
reports consumed cost and its stop reason on the Study, and never admits more
than `max_parallel_trials` into one scheduler batch. A finite cost cap uses
one-trial admission so reported cost is checked before another trial starts.

For a differentially private trainer or dataset, the objective metric must be
listed in `released_metrics`. Private per-step curves are suppressed from Run
metrics and never reach a pruner; only a released terminal metric may guide the
study.

## Analysis

`get_study_analysis` returns:

- the best complete trial;
- incumbent optimization history;
- normalized parameter importances estimated by objective association;
- complete-trial rows for a parallel-coordinates plot; and
- a ranked leaderboard with a 95% confidence interval when intermediate
  measurements allow one.

`promote_trial` accepts only a complete trial whose Run has a registered Model.
It records that existing Model as the Study's promoted project model version;
it does not copy weights. The action requires the dedicated
`studies:promote` permission. `pipelines:manage` alone cannot promote.

## REST and MCP

Each operation is registered once. REST uses `/v1/<capability>` and MCP uses a
tool with the exact capability name and the same closed input/output schemas.
All examples below omit the common REST response envelope.

### `create_study`

REST: `POST /v1/create_study`. MCP tool: `create_study`. Permission:
`pipelines:manage` (`orchestration:create_study`).

```json
{
  "name": "churn-tuning",
  "project": "retention",
  "pipeline": "production",
  "dataset": "accounts-snapshot",
  "methodology": "M-1",
  "objective": {
    "metric": "validation.auc",
    "direction": "maximize",
    "validation_split": "validation"
  },
  "from_defaults": true,
  "sampler": "tpe",
  "pruner": "asha",
  "budgets": {
    "max_trials": 40,
    "max_wall_time_ms": 3600000,
    "max_parallel_trials": 4,
    "cost_cap": 25.0
  },
  "seed": 1138
}
```

```json
{
  "id": "study-uuid",
  "spec": { "name": "churn-tuning", "sampler": "tpe", "pruner": "asha" },
  "status": "running",
  "trials": [],
  "spent_cost": 0.0,
  "stop_reason": "",
  "promoted_model": ""
}
```

The actual response's `spec` is complete; the shortened object above highlights
the selected controls.

### `get_study`

REST: `GET /v1/get_study?study=study-uuid`. MCP tool: `get_study`.
Permission: `mcp:call` (`api:query`).

```json
{ "study": "study-uuid" }
```

```json
{ "id": "study-uuid", "status": "running", "spent_cost": 3.0, "trials": [] }
```

### `list_studies`

REST: `GET /v1/list_studies`. MCP tool: `list_studies`. Permission:
`mcp:call` (`api:query`).

```json
{}
```

```json
{ "studies": [{ "id": "study-uuid", "status": "running" }] }
```

### `list_trials`

REST: `GET /v1/list_trials?study=study-uuid&state=complete`. MCP tool:
`list_trials`. Permission: `mcp:call` (`api:query`).

```json
{ "study": "study-uuid", "state": "complete" }
```

```json
{
  "study": "study-uuid",
  "trials": [{
    "number": 7,
    "run": "run-uuid",
    "params": { "model_family": "gbdt", "learning_rate": 0.05 },
    "state": "complete",
    "objective": 0.91,
    "seed": 1145,
    "cost": 0.7,
    "intermediate": [{ "step": 10, "value": 0.86 }],
    "reason": "",
    "model": "model-uuid"
  }]
}
```

### `stop_study`

REST: `POST /v1/stop_study`. MCP tool: `stop_study`. Permission:
`pipelines:manage` (`orchestration:stop_study`). Running trial Runs are failed
cooperatively.

```json
{ "study": "study-uuid" }
```

```json
{ "study": "study-uuid", "status": "stopped", "reason": "stopped by caller" }
```

### `stop_trial`

REST: `POST /v1/stop_trial`. MCP tool: `stop_trial`. Permission:
`pipelines:manage` (`orchestration:stop_trial`).

```json
{ "study": "study-uuid", "run": "run-uuid" }
```

```json
{ "study": "study-uuid", "run": "run-uuid", "state": "failed" }
```

### `promote_trial`

REST: `POST /v1/promote_trial`. MCP tool: `promote_trial`. Permission:
`studies:promote` (`orchestration:promote_trial`).

```json
{ "study": "study-uuid", "run": "run-uuid" }
```

```json
{
  "study": "study-uuid",
  "run": "run-uuid",
  "model": "model-uuid",
  "project": "project-uuid"
}
```

### `get_study_analysis`

REST: `GET /v1/get_study_analysis?study=study-uuid`. MCP tool:
`get_study_analysis`. Permission: `mcp:call` (`api:query`).

```json
{ "study": "study-uuid" }
```

```json
{
  "best_trial": { "run": "run-uuid", "number": 7, "value": 0.91, "params": { "learning_rate": 0.05 } },
  "optimization_history": [{ "number": 7, "run": "run-uuid", "value": 0.91, "best": 0.91 }],
  "parameter_importances": [{ "parameter": "learning_rate", "importance": 1.0 }],
  "parallel_coordinates": [{ "run": "run-uuid", "params": { "learning_rate": 0.05 }, "value": 0.91 }],
  "leaderboard": [{ "rank": 1, "run": "run-uuid", "value": 0.91, "confidence_low": 0.89, "confidence_high": 0.93 }]
}
```

## Live progress

Progress is published as JSON to the exposed ephemeral topic
`study-events.<study-id>`. Events cover study lifecycle, trial start,
intermediate steps, pruning, completion, failure, stopping, and promotion.
REST and MCP both use the existing bounded `receive_events` capability:

```json
{
  "topic": "study-events.study-uuid",
  "from_sequence": 1,
  "max": 50,
  "wait_ms": 20000
}
```

This is an asynchronous long poll; a waiting client does not occupy one thread
per request.

## Persistence and schemas

The replicated catalog key is `study:<uuid>`. Its local projection is
`<data-dir>/<uuid>/study.json`; restart and reconciliation validate the closed
Study schema before materializing it. The schema is published as
`study.schema.json`, and all eight capability schemas appear in
`schemas/openapi.json` and MCP `tools/list`.
