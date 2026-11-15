# Runs

A **Run** (`zetta::pipeline::Run`) is the record of **one execution of a deployed graph** —
its status, logs, produced outputs (datasets / models / indexes), and lineage
(sources, steps, freshness, confidence). It is the keystone of FEAT-5 (Run +
monitor + lineage) and realizes the closed domain spec ENT-12.

This page documents the **storage + data model** plus the **GUI viewer**: the
`Run` type and its lifecycle (create / update / query) on a `Node`, and the
read-only **Runs / Outputs / Lineage** panel that surfaces it in the headed app
(see [GUI panel](#gui-panel)). Actually *executing* a graph to produce a run is
its own slice — `zetta::pipeline::GraphExecutor` / `Node::run_graph`, see
[execution.md](execution.md); a Run created here is the artifact it writes to and
the panel reads from.

## The type

A `Run` (`include/zetta/pipeline/run.hpp`) is a top-level entity with no parent, modeled
exactly like [`zetta::pipeline::Project`](projects.md). Its value:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `project` | `std::string` | Owning project uuid. Optional (empty until wired). |
| `graph` | `std::string` | Executed graph id (free string). Optional. |
| `project_name` | `std::string` | Project display name captured at creation and refreshed on rename. |
| `pipeline_name` | `std::string` | Pipeline display name captured at creation and refreshed on rename. |
| `deployment` | `std::string` | Owning deployment uuid. Optional — set when the run is the execution of a deployed graph. |
| `status` | `RunStatus` | `Pending` → `Running` → `Succeeded` \| `Failed`. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation. |
| `updated` | `std::string` | ISO-8601 UTC stamp; bumped on every change. |
| `trace_id` | `std::string` | The run's trace (32-hex W3C id, #640) — stamped by `GraphExecutor`; `system.spans WHERE trace_id = …` / `get_trace` answer "what happened". Optional. |
| `logs` | `vector<RunLogEntry>` | Each `{ time, level, message }` — observability + diagnosis. |
| `outputs` | `vector<RunOutput>` | Each `{ kind, name, ref }`; `kind` ∈ `dataset` / `model` / `index`. |
| `lineage` | `RunLineage` | `{ sources[], steps[], freshness, confidence }`. |
| `published` | `optional<RunPublication>` | `{ version, caller }` when a published pipeline's served route started the run (zetta#1289): the exposure version it ran and the caller it was served to; the served status route answers the run only to that caller ([../api/serving.md](../api/serving.md#published-pipelines-zetta1289)). Absent otherwise. |
| `principal` | `optional<RunPrincipalRecord>` | `{ id, is_user }`: who the run runs as (#1322), recorded when a non-local principal starts it. A run executed again without `run_as` (a study trial, a retry) runs as this principal ([step authority](../governance/step-authority.md#the-principal-on-the-run-1322)). Absent for the trusted local session. |
| `reads` | `vector<TableRead>` | The tables the run reads, each `{ database, table, table_id }`, recorded **when the run is created**, before it reads anything (zetta#1258): `Node::create_run` records its pipeline's connect steps that name an existing table, and `run` adds the `dataset_path` it was handed (`Run::add_reads`); an AutoML trial adds its Study's. Every other path that creates a run records what it reads right after `create_run` (zetta#1272): each methodology runner the resolved dataset it was handed (so a solution's stages, which are handed the solution snapshot, record it), `run_solution` its snapshot's source table and then the snapshot table, `GraphExecutor` the connect steps of the graph it executes (which need not be stored), a deployment's run the deployment's `reads`, `record_methodology_run` its `dataset_path`, and an object dataset load the table it creates. Testkit case runs read no table of the live node. Lineage says where a finished run's results came from; `reads` says what an unfinished one is reading, and [`delete_dataset`](../api/deletes.md#refusals) refuses a table a pending or running run reads. Absent when the run reads no table, so older records and peers keep their shape. |
| `features` | JSON object | Accepted feature provenance: `used[{name, source[], transform, why}]` and `excluded[{name, why}]`; absent on old records means empty arrays (#1062). |
| `metrics` | JSON object | Column-oriented step series, one-time flattened params, final/best summaries, and typed JSON artifacts. See [run metrics](../ml/run-metrics.md) (#1137). |
| `entity_key_mapping` | JSON object | Present for a text-keyed dataset: the entity column plus collision-checked `{derived_id, original_value}` entries used to restore business ids in every output (#957). `get_run` answers `{column, entry_count}` in its place unless `include_entity_keys: true` (#1202): the entries grow with the row count (222 KB on 1,500 rows). The record stores a reference `{column, entry_count, table, owner, chunks}` to its rows of the `_entity_keys` table instead of the entries; records written before #1255, and runs on a standalone node, keep them inline ([entity-key-table.md](../data/entity-key-table.md)). |

`status` is grounded in the run events: a run starts `Pending`, moves to
`Running` (RunStarted), and ends `Succeeded` (RunSucceeded, with outputs +
lineage) or `Failed` (RunFailed; the failing step + reason live in `logs`).
`lineage.confidence` is a 0..1 score, **clamped into range on write** so stored
values always satisfy the schema. Stamps come from `zetta::util::now_iso8601()`
(fixed-width, so they order chronologically as plain text).

`project` / `graph` / `deployment` are stable references fixed at creation.
`project_name` and `pipeline_name` are denormalized presentation values, so list
clients do not need one Project lookup per row. Project and Pipeline renames
refresh those names on every matching Run. `deployment` ties a run
to the placement the [deployment controller](deployment-controller.md) is keeping
alive — the link the reconcile loop follows from *desired* (the deployment) to
*actual* (this run).

## CRUD on a Node

`Node` owns runs alongside databases + projects. Runs have **no human name** —
they are keyed by `id` (they are time-ordered execution records, not named
artifacts), so `find` / `drop` take the id:

| Method | Returns | Behavior |
|---|---|---|
| `create_run(project = {}, graph = {}, deployment = {})` | `shared_ptr<Run>` | Stamps `created`/`updated`, starts `Pending`, and resolves Project/Pipeline display names when their refs exist. |
| `find_run(id)` | `shared_ptr<Run>` | Empty handle on miss. |
| `drop_run(id)` | `bool` | `true` if one was removed. |
| `runs()` | `vector<shared_ptr<Run>>` | Stable order: by `created`, then `id`. |
| `run_count()` | `size_t` | — |

The **update** half of the slice is the mutators on the `Run` handle. Each bumps
`updated`, captures `run:<id>` into the CRDT (when replicated), then rewrites
`run.json`:

`set_features` validates the complete candidate record before persisting it. A
malformed feature or missing reason returns false and leaves the Run unchanged.
Methodology callbacks supply this provenance after their data-first preflight;
[run-report.md](run-report.md) documents its read surface.

```cpp
auto run = node.create_run("project-uuid", "graph-1");   // Pending
run->set_status(zetta::pipeline::RunStatus::Running);
run->add_log({zetta::util::now_iso8601(), "info", "connect: 1.2M rows"});
run->set_status(zetta::pipeline::RunStatus::Succeeded);
run->set_outputs({{"dataset", "churn_features", "tbl:abc"},
                  {"index",   "churn_index",    "idx:def"}});
zetta::pipeline::RunLineage lineage;
lineage.sources    = {"events.jsonl", "crm.csv"};
lineage.steps      = {"connect", "clean", "model", "index"};
lineage.freshness  = zetta::util::now_iso8601();
lineage.confidence = 0.87;
run->set_lineage(lineage);
```

## Persistence & replication

Runs follow zetta's **"EVERYTHING is replicated state"** rule: the Automerge
CRDT document is the source of truth, and the in-memory list + on-disk folder are
a **projection** of it ([replication.md](../cluster/replication.md)).

- **Replicated state.** Each run is one flat key in the document:
  `run:<uuid> → { project, graph, project_name, pipeline_name, status, created, updated, logs, outputs,
  lineage, metrics, entity_key_mapping }` (`src/core/catalog.hpp`). The user-facing mutators capture into the
  document; on each `tick()` the node **reconciles** the projection to match the
  document, so a peer's run (or a status / output update from a peer)
  materializes locally with **no re-capture** (`apply_value`, so no feedback
  loop). Versions converge conflict-free.
- **On-disk projection.** A run is a folder named by its UUID under the data dir,
  holding `run.json` (the value above plus `id`):

  ```
  data_dir/
    <run-uuid>/
      run.json
  ```

  `Node::start()` rebuilds the in-memory list by scanning the data dir
  (`load_runs()`) and — when replicated — reconciling from the CRDT document, so
  a run **survives restart** in both standalone and replicated nodes.

Records written before `project_name` and `pipeline_name` existed remain valid.
Startup resolves missing names from the loaded Project and Pipeline registries,
persists only changed records, and then establishes the replication baseline.

## List API

`list_runs` returns each Run's stable refs, `project_name`, `pipeline_name`,
status, `metric_summary`, privacy-suppression reason, creation/update stamps,
and log/output counts. The summary is the stored final/best summary from the Run
record. REST `GET /v1/list_runs` and the MCP `list_runs` tool use the same
registry declaration and return the same value. A Runs page can therefore render
its rows from this one call without joining Projects or fetching metrics per Run.

## Boundary validation (JSON Schema)

Run is the first zetta record validated against a **formal JSON Schema** (draft-07)
rather than ad-hoc shape checks. The schema is the single source of truth for the
value shape — `status` and output `kind` are closed enums, `confidence` is bounded
to `[0, 1]`, and `additionalProperties` is `false` — and it is exposed via
`zetta::pipeline::run_value_schema()`.

Every value that **enters** the system passes through it:

- `validate_run_value(json, &error)` — checks a value against the schema, filling
  `error` with a human-readable reason on failure.
- `parse_run_value(json, &error)` — validate **then** decode into a `RunValue`,
  returning `nullopt` when validation fails.

Both the disk-load path (`Node::load_runs`, reading `run.json`) and the
CRDT-reconcile path (`Node::reconcile_from_replication`, reading a `run:` value)
go through `parse_run_value`, so a malformed record (bad status, out-of-range
confidence, wrong types, stray fields) is **rejected at the boundary with a clear
reason** instead of corrupting the in-memory tree — fail fast, don't propagate.

The validator is the `pboettch/json-schema-validator` library, built on the
nlohmann/json zetta already uses and linked **PRIVATE** to `zetta-core` (it stays
out of the public header surface). Validation failures are routed through the
library's error-handler callback (not exceptions), so they never cross the
library boundary as a thrown type.

## AutoML trials

Every [AutoML study](../ml/studies.md) trial is one Run. The Run carries the
sampled parameters, study id, seed, intermediate objective series, logs, final
status, and produced Model lineage. This keeps pruning, comparison, audit, and
promotion on the existing execution record instead of creating a second trial
recording system (#1138).

## GUI panel

The headed app (`src/gui/app.cpp`) shows runs in a collapsing **Runs / Outputs /
Lineage** panel (`App::draw_runs`), the GUI slice of FEAT-5 — screen SCR-6 /
component CMP-4. It is a **read-only viewer**: it renders whatever runs the node
holds (created via the API / tests, or replicated from a peer), so a fresh
standalone node simply shows "(no runs)" until something creates one. It adds no
mutators of its own — driving a run (StartRun) is a later slice.

Layout renders through the
[master-detail scaffold](../gui/master-detail.md) (#634):

- **Run list** (left) — one row per `node_.runs()` entry, a standard
  `MasterRow`. Runs have no human name, so the title is the first segment of
  the run's UUID, with a **status-toned dot** (pending neutral, running amber,
  succeeded green, failed red — `tone_of`) and the status text as the mono
  meta. Selecting a row sets the active run
  (held by id and re-resolved each frame, like the rest of the UI, so a dropped
  run can't leave a dangling handle).
- **Detail** (right) — for the selected run:
  - **identity + status** — full id, the color-coded status, optional
    `project` / `graph` / `deploy` refs, and `created` / `updated` stamps.
    The refs are one-click [entity links](../gui/cross-entity-links.md)
    (#633) when they resolve against the live node — project → Explorer,
    graph → Graph workspace, deployment → Deploy;
  - **Compute jobs** — the jobs this run spawned (resolved by the job's
    `run` back-ref), each linking into the Jobs panel; shown only when any
    exist;
  - **Logs** — each entry's time, a **severity-colored** level (error red, warn
    amber, else neutral), and the message (the failing step + reason of a failed
    run surface here);
  - **Outputs** — the produced datasets / models / indexes in a `Kind / Name /
    Ref` table (a model ref that resolves to a real Model links into the
    Models panel);
  - **Trained models** — the model(s) this run produced, with their metrics and
    ranked drivers (see below); shown only when the run has produced a model;
  - **Lineage** — `sources` and `steps` as joined refs, `freshness`, and
    `confidence` rendered as a 0..1 progress bar.

### Trained models / metrics / ranked drivers

The FEAT-5 "surface the trained **Model** + metrics + ranked drivers" slice. For
the selected run, the detail pane resolves the [`Model`](../ml/models.md)(s) the run
produced and renders each (`render_model_card`):

- **identity + weights** — the model's name / kind / short id, and its weights
  fingerprint (`size`, flagged when the body is still in flight to this node);
- **Metrics** — the model's evaluation scores (`metrics`, a flat `name → number`
  map, e.g. `auc` / `f1`) in a `Metric / Value` table; a value in `[0, 1]` is drawn
  as a bar so a score reads at a glance;
- **Ranked drivers** — the model's feature importances, **strongest |influence|
  first**, each as a normalized bar with its signed value (`+/-`, since a feature
  can push a prediction up or down). Read from the model's free-form
  `feature_schema.drivers` via [`zetta::ml::ranked_drivers`](../ml/models.md#ranked-drivers-explainability);
  absent until an `Explain` job records them ("(no ranked drivers recorded)").

The run↔model link is the **durable reverse edge** — a model whose producing-run
ref (`Model::run()`) is this run's id — not the executor's logical model `output.ref`
(a placeholder handle in R1). `draw_run_detail` filters `node_.models()` by that
ref; each card carries an "open in models" [entity link](../gui/cross-entity-links.md)
into the standalone Models panel (#633), and the Models panel's `run:` ref links
back — the edge is walkable in both directions.

`render_model_card` is **shared** with the standalone
[Models panel](../gui/models-panel.md) (`App::draw_models`, **Operations → Models**), the
model-first view over the whole [`system.models`](../data/query.md) registry — so a
model reads identically whether seen alongside its run here or browsed on its own
there.

The panel reads already-validated `Run` **and** `Model` handles in memory, so it
introduces **no new JSON boundary** — `parse_run_value` / `parse_model_value` have
already run when each record entered from disk or the CRDT, and the ranked drivers
are read *tolerantly* from the already-validated, free-form `feature_schema` (not a
second validation gate).

The sibling FEAT-5 GUI slice — the live **Compute Jobs** panel + per-job
objective/scaling controls — is the [Jobs panel](../gui/jobs-panel.md).

## Tests

`tests/pipeline/test_run.cpp` covers create / update (status + logs + outputs + lineage) /
query, confidence clamping, survival across a restart (both disk-only and
CRDT-only), replication of a run + a later status change to a connected peer, the
JSON-Schema boundary rejecting malformed values, and a malformed `run.json` being
skipped on load. `tests/pipeline/test_project_graph.cpp` covers name capture,
Project/Pipeline rename propagation, and startup backfill. API tests cover list
summaries and exact REST/MCP parity.
