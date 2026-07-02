# Runs

A **Run** (`zetta::Run`) is the record of **one execution of a deployed graph** —
its status, logs, produced outputs (datasets / models / indexes), and lineage
(sources, steps, freshness, confidence). It is the keystone of FEAT-5 (Run +
monitor + lineage) and realizes the closed domain spec ENT-12.

This page documents the **storage + data model** plus the **GUI viewer**: the
`Run` type and its lifecycle (create / update / query) on a `Node`, and the
read-only **Runs / Outputs / Lineage** panel that surfaces it in the headed app
(see [GUI panel](#gui-panel)). Actually *executing* a graph to produce a run is
its own slice — `zetta::GraphExecutor` / `Node::run_graph`, see
[execution.md](execution.md); a Run created here is the artifact it writes to and
the panel reads from.

## The type

A `Run` (`include/zetta/run.hpp`) is a top-level entity with no parent, modeled
exactly like [`zetta::Project`](projects.md). Its value:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `project` | `std::string` | Owning project uuid. Optional (empty until wired). |
| `graph` | `std::string` | Executed graph id (free string). Optional. |
| `deployment` | `std::string` | Owning deployment uuid. Optional — set when the run is the execution of a deployed graph. |
| `status` | `RunStatus` | `Pending` → `Running` → `Succeeded` \| `Failed`. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation. |
| `updated` | `std::string` | ISO-8601 UTC stamp; bumped on every change. |
| `logs` | `vector<RunLogEntry>` | Each `{ time, level, message }` — observability + diagnosis. |
| `outputs` | `vector<RunOutput>` | Each `{ kind, name, ref }`; `kind` ∈ `dataset` / `model` / `index`. |
| `lineage` | `RunLineage` | `{ sources[], steps[], freshness, confidence }`. |

`status` is grounded in the run events: a run starts `Pending`, moves to
`Running` (RunStarted), and ends `Succeeded` (RunSucceeded, with outputs +
lineage) or `Failed` (RunFailed; the failing step + reason live in `logs`).
`lineage.confidence` is a 0..1 score, **clamped into range on write** so stored
values always satisfy the schema. Stamps come from `zetta::now_iso8601()`
(fixed-width, so they order chronologically as plain text).

`project` / `graph` / `deployment` are **id-only references** (kept lightweight,
like a project's future references to its inputs / graphs / runs) and are fixed at
creation; the mutable fields change as the run progresses. `deployment` ties a run
to the placement the [deployment controller](deployment-controller.md) is keeping
alive — the link the reconcile loop follows from *desired* (the deployment) to
*actual* (this run).

## CRUD on a Node

`Node` owns runs alongside databases + projects. Runs have **no human name** —
they are keyed by `id` (they are time-ordered execution records, not named
artifacts), so `find` / `drop` take the id:

| Method | Returns | Behavior |
|---|---|---|
| `create_run(project = {}, graph = {}, deployment = {})` | `shared_ptr<Run>` | Stamps `created`/`updated`, starts `Pending`. Optional project + graph + deployment refs. |
| `find_run(id)` | `shared_ptr<Run>` | Empty handle on miss. |
| `drop_run(id)` | `bool` | `true` if one was removed. |
| `runs()` | `vector<shared_ptr<Run>>` | Stable order: by `created`, then `id`. |
| `run_count()` | `size_t` | — |

The **update** half of the slice is the mutators on the `Run` handle. Each bumps
`updated`, captures `run:<id>` into the CRDT (when replicated), then rewrites
`run.json`:

```cpp
auto run = node.create_run("project-uuid", "graph-1");   // Pending
run->set_status(zetta::RunStatus::Running);
run->add_log({zetta::now_iso8601(), "info", "connect: 1.2M rows"});
run->set_status(zetta::RunStatus::Succeeded);
run->set_outputs({{"dataset", "churn_features", "tbl:abc"},
                  {"index",   "churn_index",    "idx:def"}});
zetta::RunLineage lineage;
lineage.sources    = {"events.jsonl", "crm.csv"};
lineage.steps      = {"connect", "clean", "model", "index"};
lineage.freshness  = zetta::now_iso8601();
lineage.confidence = 0.87;
run->set_lineage(lineage);
```

## Persistence & replication

Runs follow zetta's **"EVERYTHING is replicated state"** rule: the Automerge
CRDT document is the source of truth, and the in-memory list + on-disk folder are
a **projection** of it ([replication.md](../cluster/replication.md)).

- **Replicated state.** Each run is one flat key in the document:
  `run:<uuid> → { project, graph, status, created, updated, logs, outputs,
  lineage }` (`src/core/catalog.hpp`). The user-facing mutators capture into the
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

## Boundary validation (JSON Schema)

Run is the first zetta record validated against a **formal JSON Schema** (draft-07)
rather than ad-hoc shape checks. The schema is the single source of truth for the
value shape — `status` and output `kind` are closed enums, `confidence` is bounded
to `[0, 1]`, and `additionalProperties` is `false` — and it is exposed via
`zetta::run_value_schema()`.

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

## GUI panel

The headed app (`src/gui/app.cpp`) shows runs in a collapsing **Runs / Outputs /
Lineage** panel (`App::draw_runs`), the GUI slice of FEAT-5 — screen SCR-6 /
component CMP-4. It is a **read-only viewer**: it renders whatever runs the node
holds (created via the API / tests, or replicated from a peer), so a fresh
standalone node simply shows "(no runs)" until something creates one. It adds no
mutators of its own — driving a run (StartRun) is a later slice.

Layout is a list + detail, mirroring the explorer's "select on the left, inspect
on the right" shape:

- **Run list** (left) — one row per `node_.runs()` entry. Runs have no human
  name, so each row is a **color-coded status badge** (pending neutral, running
  amber, succeeded green, failed red — the same palette as the cluster panel)
  plus the first segment of the run's UUID. Selecting a row sets the active run
  (held by id and re-resolved each frame, like the rest of the UI, so a dropped
  run can't leave a dangling handle).
- **Detail** (right) — for the selected run:
  - **identity + status** — full id, the color-coded status, optional
    `project` / `graph` refs, and `created` / `updated` stamps;
  - **Logs** — each entry's time, a **severity-colored** level (error red, warn
    amber, else neutral), and the message (the failing step + reason of a failed
    run surface here);
  - **Outputs** — the produced datasets / models / indexes in a `Kind / Name /
    Ref` table;
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
  `feature_schema.drivers` via [`zetta::ranked_drivers`](../ml/models.md#ranked-drivers-explainability);
  absent until an `Explain` job records them ("(no ranked drivers recorded)").

The run↔model link is the **durable reverse edge** — a model whose producing-run
ref (`Model::run()`) is this run's id — not the executor's logical model `output.ref`
(a placeholder handle in R1). `draw_runs` filters `node_.models()` by that ref and
passes the matches to the detail renderer.

`render_model_card` is **shared** with the standalone
[Models panel](../gui/models-panel.md) (`App::draw_models`, **Operations → Models**), the
model-first view over the whole [`system.zetta_models`](../data/query.md) registry — so a
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

`tests/test_run.cpp` covers create / update (status + logs + outputs + lineage) /
query, confidence clamping, survival across a restart (both disk-only and
CRDT-only), replication of a run + a later status change to a connected peer, the
JSON-Schema boundary rejecting malformed values, and a malformed `run.json` being
skipped on load.
