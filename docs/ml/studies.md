# AutoML studies

An AutoML **study** searches model families and hyperparameters against one
metric on a named validation split. A **trial** is exactly one durable
[`Run`](../pipeline/runs.md): its sampled parameters include `study.id` and
`study.seed`, intermediate objective values use [Run metrics](run-metrics.md),
and its terminal state is `pruned`, `failed`, or `complete`.

Studies are native C++ records. They do not load a foreign ML runtime or call a
hosted model service. A node-owned background driver discovers replicated
`running` studies during `Node::tick`. A stable hash of the Study id over the
replicated membership selects one driver node, so peers do not launch duplicate
trials. The driver and its training batches use work-stealing schedulers; all
entity mutations return through `Node::run_on_owner`, so training never blocks
the owner loop.

Every admitted trial creates a normal Run before training starts. The Run is
therefore visible through `list_runs` as well as `list_trials`, and it owns the
same logs, per-tree metrics, outputs, model reference, and lineage as a Run
started directly by a user. A restart reloads the Study and Runs from replicated
state and disk. The selected node resumes a `running` Study from those recorded
trials instead of beginning a new search. A retried Run replaces any model from
an interrupted attempt, so only its terminal model remains promotable.

## What a trial runs

A Pipeline-scoped Study supplies both `project` and `pipeline`. Each trial
copies that stored graph and executes it with sampled M-1 values overriding the
GBDT model controls. The resulting Run keeps the Project and Pipeline ids.

A dataset-level Study omits both fields and supplies `dataset` as
`database.table`. The driver resolves its dataset contract, including text
entity keys, numeric or boolean labels, and an `observed_at` column when one is
present. A bounded sample infers that contract only. Every point-in-time row in
the full table is then loaded for training and validation, including repeated
observations for one text entity key. When observation times are available, the
latest holdout fraction is reserved for validation; older rows train the model.
Without time, M-1 retains its content-hash holdout.

Both kinds of trial select their inputs through the same feature plan a Run of
that table uses (`ml::plan_table_features`, see
[featurization](../data/featurization.md#phase-b-the-plan-is-a-run-precondition)):
the resolved contract's feature columns are replaced by the plan's accepted
sources, the Pipeline's explicit feature roles and safety annotations apply, and
the trial's Run records the same `features` used/excluded provenance. A trial
whose plan has no consumable input fails with the same
`create_eligible_features` message as a Run
([#1216](https://github.com/oramasearch/zetta/issues/1216)).

Dataset-level execution currently supports M-1 natively. A workspace
methodology (`W-…`, Python run by Lake's runner) is studied too: the engine samples
its declared parameters and admits trials that wait for Lake's runner, which reports
each result back ([workspace methodologies](workspace-methodologies.md#experiments),
lake#689). `create_study` rejects any other methodology before storing the Study and
explains that it has no dataset-level driver.

M-1 search names are the canonical GBDT controls (`gbdt.num_trees`,
`gbdt.max_depth`, `gbdt.max_bins`, `learning_rate`, regularization and split
controls, and `holdout_fraction`). Unknown names fail during validation.
`gradient_boosting` in `model_family` choices or conditions is canonicalized to
`gbdt`; another model family is rejected.

## Published search space

`list_methodologies` publishes study support alongside each methodology. A
supported entry has `studies: true`, an empty `study_reason`, and a
`study_search_space` containing:

- parameters with canonical name, kind, bounds, step, sampling mode, default,
  choices, aliases, and conditions;
- supported objective metrics (canonical names), each one's accepted input
  aliases under `objective_metric_aliases`, and validation splits; and
- the dataset-path contract.

M-1 currently publishes `model_family` with the sole choice `gbdt`, the eight
GBDT and holdout controls accepted by the real driver,
`held_out_accuracy`, `held_out_auc`, and `held_out_log_loss`, and the
`validation` split. Dataset-level studies require an existing
`database.table`; directory paths are not accepted. Methodologies without a
study driver return `studies: false`, a reason, and a null
`study_search_space`.

The published defaults and the validator's accepted parameter names, kinds,
aliases, choices, metrics, and splits come from one methodology definition.
Callers can therefore construct a candidate from the published object and
preflight it before asking a person to confirm creation.

## Study definition

The objective has `metric`, `direction` (`minimize` or `maximize`), and
`validation_split`. `test` is rejected as a validation split, so model
selection cannot consume final test data. If a project Pipeline is supplied,
`create_study` runs the existing correctness gate before accepting it; this
reuses methodology leakage checks.

The real M-1 driver accepts the `validation` split with `held_out_auc`,
`held_out_accuracy`, or `held_out_log_loss`. Unsupported split and metric
names are rejected when the Study is created, and the reason lists the
supported metrics.

### One metric name

A metric has one name across the methodology (`list_methodologies` →
`metrics`), the run report (`get_run_report` → `model`), and the Study
(`objective.metric`, `list_studies` → `metric_summary.metric`): the run
report's key, such as `held_out_auc`
([#1217](https://github.com/oramasearch/zetta/issues/1217)). The spellings that
circulated before — `validation.auc`, `ROC-AUC`, `roc_auc`, `auc`,
`validation.log_loss`, `log loss`, `validation.accuracy` — are accepted on
input as aliases (case, spaces, dots and hyphens ignored) and normalized to the
canonical name, so a stored Study and `normalized_spec` always carry it. A
trial's objective is read from its model's report under that same name. The
table of names and aliases is `zetta::ml::metric_names()` and
[held-out metrics](held-out-metrics.md#metric-names).

Project and Pipeline refs supplied as ids are normalized to their display names
when the Study is created. Later renames update those stored names, and startup
normalizes older records against the loaded registries.

Search parameters are ordered and typed:

- `int` and `float` have minimum/maximum bounds, an optional step, and linear
  or logarithmic sampling.
- `categorical` has a closed list of scalar choices. The current real M-1 driver
  accepts canonical `gbdt` as `model_family`.
- A parameter is conditional when `condition_parameter` names an earlier
  parameter and `condition_values` lists the parent values that activate it.
- `from_defaults: true` supplies the selected methodology's model-family
  defaults. M-1 defaults contain executable GBDT controls. It also fills a
  missing objective (`held_out_auc`, `maximize`, the `validation` split) and
  missing budgets (10 trials, 2 in parallel, 10 minutes of wall time, no cost
  cap), field by field, so `name`, `methodology` and `dataset` are enough to
  start a Study. Without it each of those fields is required.

Samplers are `grid`, `random`, and TPE. Pruners are `median` and ASHA, plus
`none`. M-1 reports a normalized per-tree training-loss curve while the terminal
objective comes from the requested held-out validation metric. Median and ASHA
compare curve points with completed peers at the same tree and cooperatively end
a losing trial. Every seed is persisted.

Budgets contain `max_trials`, `max_wall_time_ms`, `max_parallel_trials`, and
`cost_cap`. The runner checks active limits at admission boundaries and the
wall-time deadline at every training round, reports consumed cost and its stop
reason on the Study, and never admits more than `max_parallel_trials` into one
scheduler batch.

Cost is measured in **trial-seconds**: wall-clock seconds from trial dataset
resolution through training and objective collection. Before launch, each trial
reserves the largest cost of a completed trial in the Study. Until a trial has
completed, the conservative reservation is 60 trial-seconds. A parallel batch
reserves that amount for every member, and a trial is not admitted when
`spent_cost + reservations` would exceed `cost_cap`. A zero cap is unlimited. A positive cap
below one first reservation (60 trial-seconds) could never admit a trial, so `validate_study` and
`create_study` reject it at `/budgets/cost_cap`.
ExperimentsAgent should propose `cost_cap: 300` for a small five-trial Study so
the initial conservative reservations fit in the same unit.

Natural completion records status `complete` with `max_trials`, `wall_time`, or
`budget` — provided a trial completed. A study whose trials all ended without a
result (at least one failed, none complete) records status `failed` with the
same `stop_reason` and a `failure_reason` naming the last failed trial and why
it failed (`no trial completed; trial 1 failed: ...`); it never reports
`complete` with an empty `failure_reason`
([#1217](https://github.com/oramasearch/zetta/issues/1217)). The
`study-events.<id>` topic carries `study_failed` instead of `study_complete`.
An explicit `stop_study` records `stopped` and cooperatively cancels active Run
training; `stop_trial` cancels only the named Run.

A trial whose training fails ends at once with the failure as its reason. A
metric batch that cannot be written stops training at the tree it was for and
says so (`run metric logging failed at tree N: <why>`); a stop the driver asks
for (wall time, pruning, cancellation) is reported as that stop, never as a
logging failure. A trial whose model has no held-out figure for the objective —
for example a tiny table whose validation split held no rows — fails with
`trial model did not report objective metric 'held_out_auc' ...: the
validation split held no rows`, which counts as a deterministic data error.

### Captures while a trial trains

A trial reports one metric batch and one intermediate value per boosting
round. The values change in memory at once and stream live on
`run-metrics.<run>` and `study-events.<id>`, but the Run's and the Study's
replicated captures are written at most once per second, and the tick writes
the last one ([run metrics](run-metrics.md#coalesced-trainer-captures)).
Capturing the whole Run and Study on every round kept every copy in the
replicated document's history, so the document — and the owner thread's
once-a-second save of it, which reads wait behind — grew with the square of
the rounds: a 12-row Study with two 200-tree trials grew it by 2.6 MB and each
save took 135 ms on a laptop. On a workspace whose document is already large,
that per-second save and the three owner-thread hops each round makes are the
explanation found for reads slowing from ~50 ms to 2–3 s while a trial ran in
the UX review. Coalesced, the same Study adds about 150 KB and a save takes
5 ms.

`validate_study` and the background driver's first step inspect the requested
split once before trial admission when the candidate names a dataset or its
stored Pipeline names a table source. A Pipeline with no resolvable table keeps
the existing structural validation path; its trials report the missing data.
An out-of-time split must contain both label classes in its training and
validation partitions. A failed check stores Study status `failed`,
`stop_reason: data`, and a `failure_reason` that names the split and its positive
and negative counts; when one class begins only after the cutoff, the reason
also names its first observation time. As a fallback for a deterministic data
problem that escapes preflight, two consecutive trials with the same data error
fail the Study instead of consuming the remaining budget.

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

Each trial's Model is named from its study: `<study name>-trial-<n>`, with the
name lowercased and other characters folded to `-` (`Churn tuning` trial 3 →
`churn-tuning-trial-3`; `experiment-<id8>-trial-<n>` when the name has no
letters or digits). A person reads a study as an *experiment*, so its models
are never named `study-<id>` (#1242).

`promote_trial` accepts only a complete trial whose Run has a registered Model.
It records that existing Model as the Study's promoted project model version;
it does not copy weights. The action requires the dedicated
`studies:promote` permission. `pipelines:manage` alone cannot promote.

## Deletion

`delete_study` refuses a study whose status is `running`; call `stop_study`
first. Deleting a stopped, complete, or failed study removes its replicated
record and each non-promoted trial Run. Run metrics are part of the Run record,
so its parameters, series, summaries, and metric artifacts are removed with it.

When a trial has been promoted, the promoted Model and the Run named by that
Model's lineage remain registered. Deletion does not remove any Model. The
result lists every removed Run and identifies the retained promoted Model and
Run. The action has its own `studies:delete` permission; neither
`pipelines:manage` nor `studies:promote` grants it.

## REST and MCP

Each operation is registered once. REST uses `/v1/<capability>` and MCP uses a
tool with the exact capability name and the same closed input/output schemas.
All examples below omit the common REST response envelope.

### `validate_study`

REST: `GET /v1/validate_study` with the candidate fields encoded as query
parameters. MCP tool: `validate_study`. Permission: `mcp:call` (`api:query`).

The input schema is identical to `create_study`. Validation performs the same
normalization, full-table split preflight, dataset lookup, Project/Pipeline
lookup and correctness gate, duplicate-name check, and study-spec checks as
creation, but stores nothing.
`create_study` calls this shared validation path before it creates the record.

```json
{
  "name": "churn-tuning",
  "dataset": "warehouse.accounts_snapshot",
  "methodology": "M-1",
  "objective": {
    "metric": "held_out_auc",
    "direction": "maximize",
    "validation_split": "validation"
  },
  "from_defaults": true,
  "budgets": {
    "max_trials": 5,
    "max_wall_time_ms": 3600000,
    "max_parallel_trials": 4,
    "cost_cap": 300.0
  }
}
```

```json
{
  "valid": true,
  "reason": "",
  "path": "",
  "normalized_spec": {
    "name": "churn-tuning",
    "methodology": "M-1",
    "sampler": "tpe",
    "pruner": "asha"
  }
}
```

The actual `normalized_spec` is complete, including the expanded search space
and budget defaults. An invalid candidate returns `valid: false`, a precise
`path`, a repair-oriented `reason`, and `normalized_spec: null`. Validation
failures remain successful read responses so clients can repair the candidate
without interpreting an action error.

`create_study` (Lake's `start_study`) fails with the same `reason` as an
`invalid_input` error whose `path` is the same field
([#1217](https://github.com/oramasearch/zetta/issues/1217)). That includes the
input-schema rejections the registry makes before the handler runs: a missing
required property is reported at the property (`/name`), a wrong type or enum
at the field (`/objective/direction`). An error under `/objective`,
`/budgets` or `/search_space`, and any schema rejection, ends with the offer
`set from_defaults: true to fill a missing objective (held_out_auc, maximize,
validation split), budgets and the M-1 search space` unless the input already
set it. The first error of a minimal `{name, methodology, dataset}` input is
therefore `/objective: required; set from_defaults: true ...`, and taking that
offer is the only change needed.

### `create_study`

REST: `POST /v1/create_study`. MCP tool: `create_study`. Permission:
`pipelines:manage` (`orchestration:create_study`).

```json
{
  "name": "churn-tuning",
  "project": "retention",
  "pipeline": "production",
  "dataset": "warehouse.accounts_snapshot",
  "methodology": "M-1",
  "objective": {
    "metric": "held_out_auc",
    "direction": "maximize",
    "validation_split": "validation"
  },
  "from_defaults": true,
  "sampler": "tpe",
  "pruner": "asha",
  "budgets": {
    "max_trials": 5,
    "max_wall_time_ms": 3600000,
    "max_parallel_trials": 4,
    "cost_cap": 300.0
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
  "failure_reason": "",
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
{
  "studies": [{
    "id": "study-uuid",
    "name": "churn-tuning",
    "project_name": "retention",
    "pipeline_name": "production",
    "status": "running",
    "created": "2026-09-29T12:00:00.000Z",
    "updated": "2026-09-29T12:05:00.000Z",
    "metric_summary": {
      "metric": "held_out_auc",
      "direction": "maximize",
      "completed_trials": 3,
      "best": 0.91
    }
  }]
}
```

Each entry also retains the complete Study record (`spec`, trials, budgets and
usage), preserving existing callers. The flattened names and objective summary
let an Experiments page render from this list call without Project lookups or
per-Run metric reads.

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
{ "study": "study-uuid", "status": "stopped", "reason": "stopped" }
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

### `delete_study`

REST: `POST /v1/delete_study`. MCP tool: `delete_study`. Permission:
`studies:delete` (`orchestration:delete_study`).

```json
{ "study": "study-uuid" }
```

```json
{
  "study": "study-uuid",
  "name": "churn-tuning",
  "removed": { "runs": ["non-promoted-run-uuid"] },
  "kept": {
    "model": "promoted-model-uuid",
    "run": "promoted-run-uuid"
  }
}
```

The `model` and `run` strings are empty when no trial was promoted.

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
`study-events.<study-id>`. The driver emits `trial_started`, `trial_completed`,
`trial_pruned`, `trial_failed`, `study_failed`, and `study_complete`; creation,
intermediate progress, stopping, and promotion use the same topic.
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

The replicated catalog key is `study:<uuid>`. A Study records the tables its
trials read in `reads` (`{database, table, table_id}`, zetta#1258) when it is
created: its `dataset`, or its Pipeline's connect steps. Each trial Run adds them
to its own `reads`, and [`delete_dataset`](../api/deletes.md#refusals) refuses a
dataset a running Study reads, even between trials. Its local projection is
`<data-dir>/<uuid>/study.json`; restart and reconciliation validate the closed
Study schema before materializing it. A running Study is eligible on exactly
one replicated member after reconciliation, including after a restart, and a node
cooperatively releases in-flight work if membership selects a new owner. The
schema is published as `study.schema.json`, and all ten capability schemas
appear in `schemas/openapi.json` and MCP `tools/list`.
