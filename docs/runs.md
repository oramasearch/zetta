# Runs

A **Run** (`zetta::Run`) is the record of **one execution of a deployed graph** —
its status, logs, produced outputs (datasets / models / indexes), and lineage
(sources, steps, freshness, confidence). It is the keystone of FEAT-5 (Run +
monitor + lineage) and realizes the closed domain spec ENT-12.

This page documents the **storage + data model** only: the `Run` type and its
lifecycle (create / update / query) on a `Node`. Actually *executing* a graph
(the StartRun command) and the Runs / Outputs / Lineage UI panel are separate
work — a Run created here is the artifact they will write to and read from.

## The type

A `Run` (`include/zetta/run.hpp`) is a top-level entity with no parent, modeled
exactly like [`zetta::Project`](projects.md). Its value:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `project` | `std::string` | Owning project uuid. Optional (empty until wired). |
| `graph` | `std::string` | Executed graph id (free string). Optional. |
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

`project` / `graph` are **id-only references** (kept lightweight, like a
project's future references to its inputs / graphs / runs) and are fixed at
creation; the mutable fields change as the run progresses.

## CRUD on a Node

`Node` owns runs alongside databases + projects. Runs have **no human name** —
they are keyed by `id` (they are time-ordered execution records, not named
artifacts), so `find` / `drop` take the id:

| Method | Returns | Behavior |
|---|---|---|
| `create_run(project = {}, graph = {})` | `shared_ptr<Run>` | Stamps `created`/`updated`, starts `Pending`. Optional project + graph refs. |
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
a **projection** of it ([replication.md](replication.md)).

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

## Tests

`tests/test_run.cpp` covers create / update (status + logs + outputs + lineage) /
query, confidence clamping, survival across a restart (both disk-only and
CRDT-only), replication of a run + a later status change to a connected peer, the
JSON-Schema boundary rejecting malformed values, and a malformed `run.json` being
skipped on load.
