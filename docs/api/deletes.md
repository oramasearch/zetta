# Deleting datasets, models, runs and indexes

Four actions remove what a workspace accumulates (requested by
[lake#645](https://github.com/oramasearch/lake/issues/645)). Before them a dataset could only be
dropped with SQL `DROP TABLE` (which left its partition catalog entries, their bodies and the
indexes built over it behind), a model or an index could only be unexposed, and a run stayed
forever. Each is a registry action (REST `POST /v1/<name>`, the MCP tool of the same name),
authorized by its own right `orchestration:<name>`, granted with `pipelines:manage` beside
`delete_project`, and each goes through the replicated drop path (`Node::drop_*`,
`Database::drop_table`), so every peer converges. None is undoable.

Every one takes `dry_run` (default `false`). With `dry_run: true` nothing changes and the answer is
the same document a real delete returns, so a caller can show exactly what goes before asking for
confirmation. A missing entity is `not_found`.

| Capability | Input | Removes | Keeps |
|---|---|---|---|
| `delete_dataset` | `dataset` (the name `list_datasets` reports, `database.table`, or the table id), `database` (only when the name exists in several databases), `dry_run` | the table and its rows, its partition catalog entries and their large-object bodies, and every index whose `table` is the dataset's name or `database.table` | other datasets and their indexes |
| `delete_model` | `model_id` (the uuid, or a display name naming exactly one model), `dry_run` | the model and its weights; a served model is unexposed first, so its endpoint stops answering | the run that trained it (`kept.run`) |
| `delete_run` | `run_id`, `dry_run` | the run with its logs, metrics, parameters and artifacts | the models it trained (`kept.models`, ids) |
| `delete_index` | `index_id`, `dry_run` | the index record and its folder; a served index is unexposed first | the indexed dataset |

## Answers

- `delete_dataset` → `{dataset, name, database, table_id, dry_run, removed: {rows, size_bytes,
  partitions, indexes: [index ids]}}`.
- `delete_model` → `{model_id, name, dry_run, removed: {model: 1, was_exposed, standalone_project?},
  kept: {run}}`. `standalone_project: 1` says the model was a standalone model's last version and its
  internal project goes too ([../ml/standalone-models.md](../ml/standalone-models.md)); it stays while
  one of that project's runs is pending or running.
- `delete_run` → `{run_id, name (its pipeline name, or empty), dry_run, removed: {run: 1, status},
  kept: {models: [model ids]}}`.
- `delete_index` → `{index_id, name (the indexed table: indexes have no name of their own), table,
  dry_run, removed: {index: 1, was_exposed}}`.

## Refusals

- `delete_run` refuses a pending or running run (`engine_error`, naming its status); wait for it to
  finish (`get_run`) and delete it then, as `delete_project` does.
- `delete_model` with a display name that several models share is `engine_error`: delete one by its
  `model_id`. A delete never guesses.
- `delete_dataset` with a name that exists in several databases is `engine_error` (pass `database`);
  the reserved connection store (`zetta_connect`) is `forbidden` (use `delete_connection`).
- `delete_dataset` refuses a dataset that a pending or running run, or a running study, reads
  (`engine_error`, naming each: `run <id> (running)`, `study '<name>' (running)`); a `dry_run`
  refuses the same way, so a preview never promises a delete that would be refused. Wait for the
  runs to finish (`get_run`) and stop the study (`stop_study`), then delete it. A run and a study
  record the tables they read when they are created (`reads`, zetta#1258): a run its pipeline's
  connect steps and the `dataset_path` it was handed, a study its dataset. Runs created outside
  `run` record theirs too (zetta#1272): methodology runners and solution stages the dataset they
  were handed, `run_solution` its snapshot tables, a deployment's run its graph's connect steps,
  `GraphExecutor` the graph it executes
  ([../pipeline/runs.md](../pipeline/runs.md), [../ml/studies.md](../ml/studies.md)).

## How to verify

`tests/api/test_registry.cpp`: `DeleteDatasetRemovesItsPartitionsAndIndexes`,
`DeleteDatasetRefusesWhileARunOrAStudyReadsIt`, `DeleteDatasetRefusesWhileADeploymentRunReadsIt`,
`DeleteDatasetRefusesWhileAGraphExecutorRunReadsIt`, `DeleteModelStopsServingItAndKeepsItsRun`, `DeleteModelByNameRefusesAnAmbiguousName`,
`DeleteRunRefusesWhileInFlightAndKeepsItsModels`, `DeleteIndexStopsServingItAndKeepsTheDataset`;
`tests/api/test_action_journeys.cpp`: `SolutionJourney.DemandForecastBuildsActsAndReportsFvaLimitsHonestly`
(the solution Run and its stage Runs); `tests/pipeline/test_deployment.cpp`:
`Deployment.KeepsTheTablesItsGraphReadsAcrossRestart`; `tests/ml/test_bandit.cpp`:
`BanditState.PolicyTableSurvivesNodeRestartAndFeedsTheNextRun` (a runner handed a dataset).
