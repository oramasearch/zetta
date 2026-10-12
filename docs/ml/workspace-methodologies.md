# Workspace methodologies

Implemented for [#1261](https://github.com/oramasearch/zetta/issues/1261), part of
[lake#655](https://github.com/oramasearch/lake/issues/655). Not deployed.

The [methodology registry](methodologies.md) holds Orama's catalogue, M-1 through
M-21, and it is read-only. A workspace can also have its **own** methodologies:
Python code in one of its Lake repositories that implements one documented
interface (`fit`, `predict`, `score`, optionally `explain`). The engine registers
them next to the catalogue, lists them, compiles pipelines that use them, refuses to
verify one that has not been validated, and records what a run produced.

**The engine never runs the Python** (ADR-17 stands). The code runs in Lake's
sandboxed runner, in its own container with locked packages and no credentials, the
way a Python agent runs outside the engine. Lake reports each finished run back with
`record_methodology_run`.

## The entry

A workspace methodology is a replicated value at `methodology:<id>`
(`src/core/catalog.hpp`), id `W-` followed by eight lowercase hex digits. Its schema
is `workspace_methodology_value_schema()` (`include/zetta/ml/workspace_methodology.hpp`):

| Field | Meaning |
|---|---|
| `name`, `description`, `family` | what it is; `family` defaults to `Workspace` |
| `input_kinds` | at least one of `tabular`, `text`, `time_series`, `graph`, `image`, `audio`, `document` |
| `target` | `{required, kind}`: `kind` is `binary`, `multiclass`, `regression` or `none`; `required` is false exactly when the kind is `none` |
| `required_features` | column names it needs |
| `parameters` | `{name, kind: int\|float\|categorical\|bool, default, minimum?, maximum?, choices?, log?, description?}`; the default must sit inside its bounds or choices, a categorical names its choices, a log-scaled parameter's minimum is above zero, names are unique |
| `metrics` | base names (`auc`), reported by a run as `in_sample_<m>` and `held_out_<m>` |
| `explain` | whether its code implements `explain` |
| `code` | `{repository_id, revision, path}`: where the Python lives |
| `image` | `{build_id, reference}`: the image Lake built from that code |
| `validation` | `{status: not_validated\|running\|passed\|failed, findings[{check, ok, message}], run_id, revision, validated_at}` |
| `network` | whether the person granted it the internet |
| `resources` | `{cpu, memory, gpu, timeout_seconds}` |
| `duplicated_from`, `created_by`, `created_at`, `updated_at` | provenance |

An entry is **runnable** only when `validation.status` is `passed` and
`validation.revision` equals `code.revision`: new code is not the code that was
validated.

## Capabilities

| Capability | What it does |
|---|---|
| `register_methodology` | input: every field above except `id`, `image`, `validation` and the timestamps; required `name`, `input_kinds`, `target`, `metrics`, `code`. Assigns the `W-` id, starts `not_validated`. A `duplicated_from` must name a catalogue id or an existing entry. Answers `{methodology}` |
| `update_methodology` | input: `id` plus any field, `image` and `validation` included. A changed `code.revision` resets `validation` to `not_validated` unless the same call sets it. An `M-n` id answers `forbidden` (403) saying to duplicate it instead. Answers `{methodology}` |
| `delete_methodology` | `{id}` → `{removed, id}`, idempotent; an `M-n` id answers `forbidden` |
| `get_methodology` | `{id}` → `{methodology}`: a catalogue entry or a workspace entry |
| `list_methodologies` | the 21 catalogue entries, then the workspace's own, id-ordered |
| `record_methodology_run` | below |
| `append_methodology_artifact` | below: a large artifact, in chunks (lake#690) |
| `read_model_weights` | a model's weights read back in chunks (lake#690) |

Every listed entry carries `source` (`orama` or `workspace`) and `editable`. A
workspace entry has the catalogue's fields, derived:

- `model_kinds` is `["python:<id>"]`;
- `inputs` are roles: `label` (required, when it has a target) and `feature` (required when it names required features);
- `outputs` are `model`, `predictions`, `metrics`;
- `metrics` are the `held_out_<m>` names, as the catalogue lists held-out keys;
- `studies` is false, with a `study_reason`: Lake's runner does not run experiment trials yet;
- `solutions` is empty and `issue` is empty.

It also carries `description`, `interface` (`input_kinds`, `target`,
`required_features`, `parameters`, `metrics` as base names, `explain`), `code`,
`image`, `validation`, `network`, `resources`, `duplicated_from`, `created_by`,
`created_at` and `updated_at`.

## A pipeline step

`compile_intent` with `methodology: "W-…"` builds the workspace methodology's own
shape (`build_workspace_methodology_shape`):

1. `read_methodology_rows` (Connect): the table from `hints.source`/`hints.table` or `hints.dataset_path`;
2. `python_methodology` (Model): `model_kind: python:<id>`, `methodology: <id>`, `parameters` (the declared defaults, overridden by `hints.params`), `target` (`hints.label` or `hints.target`), `holdout_fraction` (default 0.2), `runner: lake`;
3. `register_methodology_model` (Serve): the fitted artifact becomes a Model.

The template is named "the W-… <name> workspace methodology template". An unknown
`W-` id is refused.

**Verification.** The API's `gate` and `run` add a `methodology` check from the
entry (`gate_with_workspace_methodologies`): it passes only when the entry is
runnable, and otherwise says to validate it. The pure `MethodologyCheck` never treats
a `python:W-…` kind as implemented. `deploy` refuses the graph with `not_supported`:
serving a workspace methodology's model is not built.

**Run.** The engine does not run the code. Once Verification passes, `run` answers
the error `runner_required` (HTTP 409) with `error.details`:

```json
{"methodology": "W-0123abcd", "dataset_path": "shop.customers",
 "params": {"max_depth": 4, "learning_rate": 0.1}, "target": "churned",
 "holdout_fraction": 0.2, "project": "<uuid>", "project_name": "churn",
 "graph": "<uuid>", "graph_name": "churn with my code",
 "code": {"repository_id": "…", "revision": "…", "path": "."}}
```

`dataset_path` is the run's own `dataset_path` input when it has one, otherwise the
connect step's table. Lake's `start_run` dispatches the code to its runner and
reports back with `record_methodology_run`, naming the project and graph.

## record_methodology_run

Input: `methodology` (a `W-` id), `run_id` (Lake's run id), `mode`
(`validate` or `train`), and optionally `dataset_path`, `params`, `metrics` (numbers
only), `rows {train, held_out}`, `artifact_base64`, `artifact_format`
(`bytes` or `pickle`), `features`, `project` and `graph` (both or neither).

- `validate` registers nothing: Lake sets the outcome with `update_methodology`'s `validation`. Answers `{model_id: null, run_id: null}`.
- `train` needs the artifact: `artifact_base64`, strict base64 of at most **24 MiB** decoded (the request must stay under the HTTP adapter's 33 MiB body limit), or, for a larger one, `artifact_staged {bytes, sha256}` naming what `append_methodology_artifact` staged for the same `methodology` and `run_id` (below). Not both. It creates a Run (on the project and graph for a pipeline step; the graph must run this methodology), records its parameters, its metrics as step 0, a log line naming Lake's run, the model output and the lineage, and sets it Succeeded. It registers a Model: kind `python:<id>`, the metrics, the artifact as its weights, and in `feature_schema` the methodology, Lake's run id, the code, the parameters, the features, the dataset, the row counts and the artifact format. Answers `{model_id, run_id}`.

## Artifacts over 24 MiB (lake#690)

`append_methodology_artifact {methodology, run_id, offset, chunk_base64}` stages a
run's artifact on the node's disk (`<data-dir>/methodology-artifacts/<sha256 of
methodology and run>.part`), in order, at most **8 MiB** decoded per call and
**1 GiB** in all. It answers `{received}`, the bytes staged. A chunk at an offset
below `received` replaces what was staged from there, so a sender that starts over
is safe; an offset past it is `conflict` naming where to resume.
`record_methodology_run` with `artifact_staged {bytes, sha256}` checks the staged
file's size and SHA-256 (hashed as it is read, `zetta::util::Sha256`), refuses a
mismatch as `conflict`, and moves the file into place as the Model's weights
(`Model::adopt_weights_file`) without reading it into memory.

`read_model_weights {model_id, offset?, limit?}` answers up to 8 MiB of a model's
weights from `offset` as `chunk_base64`, with `next_offset` (null at the end),
`total_bytes` and `sha256`. Lake reads a served model's artifact this way.

The Model appears in `list_models` and `system.models` like any other. Its weights
are the methodology's own artifact; only Lake's runner can load them.

## Verify

```sh
./build/tests/zetta-tests --gtest_filter='WorkspaceMethodology*:Sha256*'
```

`OramasMethodologiesAreReadOnly`, `RegisterGetUpdateDeleteAndList`,
`APipelineStepIsVerifiedOnlyOnceValidatedAndRunsInLakesRunner`,
`RecordingARunChecksWhatItIsGiven`, `AnArtifactMovesInChunksAndReadsBackInChunks`
and `ItsPipelineIsNotDeployedForServing`; `Sha256.InPiecesEqualsAtOnce`.

## Serving, and what is not built

- Experiments (studies) over a workspace methodology's parameters: the parameters are declared, but trials would run in Lake's runner.
- Serving a workspace methodology's model is Lake's, not the engine's (lake#690): Ship → Models
  publishes it, Lake has the Orama operator run the methodology's image in `serve` mode, and
  Lake answers `predict` from it. The engine's `deploy` of such a pipeline still answers
  `not_supported`, since the engine never runs the Python.
