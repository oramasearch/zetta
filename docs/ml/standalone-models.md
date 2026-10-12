# Standalone models

A **standalone model** is a model made on its own, not as one step of a project's pipeline
([lake#654](https://github.com/oramasearch/lake/issues/654)). A person trains one from a dataset, a
target column and a methodology, or brings one trained elsewhere from its file. It is a model like
any other: `list_models` lists its versions with their held-out scores and features, and `score`,
`explain`, `expose_model`, `predict` and `delete_model` take it as they take a project's.

## Can a model be trained without a project?

Not by the engine as it is built: every methodology runner takes a project and a pipeline
(`zetta::ml::RunRequest` carries both), and the Run and the Model it registers record them as
lineage. So a standalone model gets an **internal project** of its own:

- `train_model` creates it, holding the one pipeline the chosen methodology composes for the dataset
  (the same compile path as `compile_intent` with `hints.source`, `hints.table` and
  `hints.label_column`), approves it, and runs it.
- It is a `Project` with `internal: true` in its replicated value and its `project.json`; every other
  project's value is unchanged. `Node::project_names()` leaves it out, so `list_projects`, the GUI,
  the web UI and every count of projects never show it. It is found by its id (`get_project`, `run`,
  `get_run_report`), which is how a model's lineage points at it.
- Its name is `<model name> (standalone model)`, so a second standalone model of the same name is
  refused, and a person's own project of that name is left alone.
- `list_models` answers `standalone: true` for each version of the model.

Each training is a run of that pipeline, so the versions of a standalone model are its records
exactly as a project's model's are: the same name from the same pipeline of the same project.

## The capabilities

| Capability | Kind | What it does |
|---|---|---|
| `train_model` | job | `name`, `methodology` (an id from `list_methodologies` or one of its model kinds), `dataset_path` (`database.table`), `target` (required by a methodology whose `label` input is required; omit it for one that learns without), `entity_column?`, `holdout_fraction?`. Creates the internal project, composes and approves the pipeline, starts the run, answers `{run_id, name, project_id, pipeline, methodology, standalone: true, …}` (the `run` answer's fields too). The model appears when the run finishes. A refusal leaves nothing behind. |
| `retrain_model` | job | `model_id` (any version, or a name one model has), and optionally a new `dataset_path`, `target`, `entity_column` or `holdout_fraction`. Runs the model's pipeline again. A standalone model keeps the new settings for its next retrain; a project's model uses them for this run only, since its pipeline says what it trains on. A brought model has no training to repeat and is `not_supported`. |
| `register_model` | action | `weights` (a text file's contents) or `weights_base64`, `format?`, `feature_names?`, `name` (or `version_of`, a brought standalone model's id, for a new version of it), `kind?`, `metrics?`, `source?`. Keeps a model trained elsewhere so it is scored, served and compared like a trained one. |
| `move_model_to_project` | action | `model_id`, `project_name?` (default the model's name). Reveals the internal project under that name, with its pipeline and every version, so a pipeline can grow around the model. Replicates. A project's model is `invalid_input`. |
| `delete_model` | action | Unchanged for a project's model. Deleting a standalone model's last version also drops its internal project (`removed.standalone_project: 1`), unless one of its runs is still training. `dry_run` says so first. |

Each mutating capability requires `orchestration:<name>`, held by `pipelines:manage`.

## What training records

The pipeline's model steps carry `standalone_training`: `{model_name, methodology, dataset_path,
target?, entity_column?, holdout_fraction?}`, and its first connect step names the dataset
(`source`, `table`). Both ride in the replicated graph, so any node can retrain the model, and a
pipeline moved into a project already says what it reads.

## What a brought model can be

The engine scores, serves and explains a gradient-boosted (or other) tree model. `register_model`
takes the file as `weights` (text) or `weights_base64` (any file, and every binary one) in any of:

- **ONNX**: `TreeEnsembleRegressor` or `TreeEnsembleClassifier`, as scikit-learn (skl2onnx), XGBoost
  and LightGBM (onnxmltools) export them;
- **XGBoost's own model**, JSON or UBJSON (`save_model`);
- **LightGBM's text model** (`save_model("model.txt")`);
- **the engine's own JSON weights** (`objective`, `base_score`, `feature_names`, `trees`, optional
  `calibration`), validated by `GbdtModel::from_json`. Every model trained here is kept in this
  format.

Each is converted exactly into the engine's model ([model-import.md](model-import.md), lake#681):

- **What is refused.** Anything else answers `not_supported`, names the reason, and nothing is kept:
  a pickle, a neural network, preprocessing inside the graph, a multiclass or random-forest
  classifier, or categorical splits.
- **Foreign runtimes.** None is loaded to read a file (ADR-17); every reader is the engine's own.
- **`format`** is read from the file. Pass it explicitly to skip detection.
- **`feature_names`** names the columns. ONNX does not carry column names, so this is needed for an
  ONNX file.
- **Kind.** It defaults from the converted objective: a logistic link is a classifier, a sum is a
  regressor.
- **Metrics.** `metrics` are kept as given (numbers only), so a brought model compares with trained
  ones on the figures measured where it was trained.
- **What `feature_schema` records:** `features`, `objective`, `trees`, `brought: true`, `format` and
  `source`.

`export_model` downloads any model version's weights: as kept, or a tree model as ONNX.

## Tests

`tests/api/test_standalone_models.cpp`: training one without a listed project and scoring it, the
refusals (no target, no dataset, unknown methodology, a name taken) leaving nothing, a retrain that
adds a version and keeps a new held-out share, bringing one (scored, exposed, a new version, other
formats refused), bringing ONNX, LightGBM and XGBoost UBJSON files and downloading weights as kept and
as ONNX, moving one into a project, deleting the last version removing the internal
project, and the flag surviving a restart and replicating.
