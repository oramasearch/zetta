# Compute jobs

A **ComputeJob** (`zetta::ComputeJob`) is the record of **one unit of native
distributed accelerated compute** — featurize / train / explain / score / a DDF
op — with a lifecycle (`Queued → Running → Succeeded | Failed`). It is the
foundational entity of the *native distributed accelerated compute* pass for UC-1
churn (epic [#264](https://github.com/oramasearch/zetta/issues/264)) — it
realizes **ARC-11** and the entity spec **ENT-31**.

This page documents the **storage + data model** only: the `ComputeJob` type and
its lifecycle (create / update / query) on a `Node`, its persistence +
replication, the audit events it emits, its JSON-Schema boundary, and the
read-only `system.zetta_jobs` SQL registry that exposes it (below). The
[work-stealing scheduler](work-stealing-scheduler.md) that *runs* a job's tasks
across cores + nodes is built ([#366](https://github.com/oramasearch/zetta/issues/366)),
as is the [`ScalingPolicy`](scaling-policy.md) reward model the autoscaler reads
(built in [#370](https://github.com/oramasearch/zetta/issues/370)). The
[job coordinator](job-coordinator.md) that *schedules* a job onto the scheduler +
collectives and drives its lifecycle is also built
([#372](https://github.com/oramasearch/zetta/issues/372)); the
[**autoscaler** control loop](job-autoscaler.md) that sizes the fleet from a job's
`ScalingPolicy` via the `Provisioner` is built too
([#371](https://github.com/oramasearch/zetta/issues/371)) — wiring its live
observation source into the coordinator is the remaining slice. A job created here
is the durable, replicated record the coordinator drives.

It is modeled **exactly like** [`zetta::Run`](../pipeline/runs.md): a top-level entity keyed
by id, a flat CRDT key + on-disk JSON projection, mutators on the handle, and
boundary validation against a formal JSON Schema. If you know `Run`, you know
`ComputeJob`.

## The type

A `ComputeJob` (`include/zetta/compute_job.hpp`) is a top-level entity with no
parent. Its value:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk folder name. Never changes. |
| `kind` | `JobKind` | `featurize` / `train` / `explain` / `score` / `ddf`. Fixed at creation. |
| `status` | `JobStatus` | `Queued` → `Running` → `Succeeded` \| `Failed`. |
| `project` | `std::string` | Owning project uuid. Optional ref. |
| `graph` | `std::string` | Source graph id. Optional ref. |
| `run` | `std::string` | Produced run uuid. Optional ref. |
| `scaling_policy` | `std::string` | `ScalingPolicy` (reward) id. Optional ref. |
| `created` | `std::string` | ISO-8601 UTC stamp at creation. |
| `updated` | `std::string` | ISO-8601 UTC stamp; bumped on every change. |
| `detail` | `std::string` | Human-readable note / failure reason. Optional. |

`status` is grounded in the job events: a job starts `Queued`, moves to `Running`
when it is scheduled onto compute, and ends `Succeeded` or `Failed` (`detail`
surfaces the reason on failure). `Succeeded` / `Failed` are **terminal** —
`is_terminal(status)` is the predicate the finish-audit emission hinges on.
Stamps come from `zetta::now_iso8601()` (fixed-width, so they order
chronologically as plain text).

`kind` and the four refs (`project` / `graph` / `run` / `scaling_policy`) are
**id-only references** fixed at creation, kept lightweight exactly like a `Run`'s
references. The `scaling_policy` ref points at a
[`ScalingPolicy`](scaling-policy.md) reward model that drives the autoscaler; the
policy is a **separate value type** (built in #370) — here it is only referenced
by id, never embedded.

## CRUD on a Node

`Node` owns compute jobs alongside databases + projects + runs. Like runs, jobs
have **no human name** — they are time-ordered execution records keyed by `id`,
so `find` / `drop` take the id:

| Method | Returns | Behavior |
|---|---|---|
| `create_job(kind, project = {}, graph = {}, run = {}, scaling_policy = {})` | `shared_ptr<ComputeJob>` | Stamps `created`/`updated`, starts `Queued`, records a `job_created` audit event. |
| `find_job(id)` | `shared_ptr<ComputeJob>` | Empty handle on miss. |
| `drop_job(id)` | `bool` | `true` if one was removed. |
| `jobs()` | `vector<shared_ptr<ComputeJob>>` | Stable order: by `created`, then `id`. |
| `job_count()` | `size_t` | — |

The **update** half of the slice is the `set_status` mutator on the handle. It
bumps `updated`, captures `job:<id>` into the CRDT (when replicated), then
rewrites `job.json`; an optional `detail` records a note (non-empty replaces):

```cpp
auto job = node.create_job(zetta::JobKind::Train, "project-uuid", "graph-1");
job->set_status(zetta::JobStatus::Running);
// ... compute happens ...
job->set_status(zetta::JobStatus::Succeeded, "12 nodes, 4.2s");
```

## Queryable via SQL (`system.zetta_jobs`)

The compute-job registry is exposed as a virtual, read-only system table —
**`system.zetta_jobs`** — the same pattern as `system.zetta_nodes` /
`system.zetta_deployments` / `system.zetta_models` ([query.md](../data/query.md)). It is
**computed at query time** from `Node::jobs()` (nothing extra is stored or
replicated), so a `SELECT` always reflects the live set:

```sql
SELECT * FROM system.zetta_jobs;
SELECT job_id, kind, status FROM system.zetta_jobs WHERE status = 'running';
```

| Column | Source |
|---|---|
| `job_id` | `id` |
| `kind` | `kind` (lowercase wire name) |
| `status` | `status` (lowercase wire name) |
| `project` / `graph` / `run` | the optional id references |
| `scaling_policy` | the optional reward-model reference |
| `created` / `updated` / `detail` | as stored |
| `node` | the **observing** node (`Node::node_id()`) |

`SELECT *`, projection, and a single `WHERE` equality work exactly as for a
regular table. Qualified name only (no bare alias), like
`system.zetta_deployments`. The `node` column is the node whose replicated view
the rows are read from — jobs replicate, so every node lists the same set;
recording which node actually *runs* a job is a later slice (the scheduler), so
until then `node` answers "which node's registry you queried", not a placement
assignment.

## Audit (create / finish)

Queueing a job and a job finishing are both **governed actions**, so they each
append an immutable event to the [audit trail](../governance/audit-trail.md) (FEAT-9) through
the existing `AuditSink` — the same seam a `Project`'s graph CRUD uses. Two
actions extend the audit taxonomy:

- **`job_created`** — emitted by `Node::create_job` once the job is materialized.
  `target` is the job id; `allowed` is `true`.
- **`job_finished`** — emitted by `ComputeJob::set_status` on the **first**
  transition into a terminal state (`Succeeded` / `Failed`), once and only once.
  `target` is the job id; `allowed` mirrors success (`true` for `Succeeded`,
  `false` for `Failed`); `detail` carries the note.

The reconcile paths (`materialize_job` / `apply_value`) **never** emit — a peer's
create/finish arrives already-audited as its own replicated `audit:` event, the
same rule the rest of the tree follows. A `ComputeJob` holds a non-owning
`AuditSink*` (its `Node`) alongside its `ReplicationManager*`, exactly like a
`Project`.

## Persistence & replication

Compute jobs follow zetta's **"EVERYTHING is replicated state"** rule: the
Automerge CRDT document is the source of truth, and the in-memory list + on-disk
folder are a **projection** of it ([replication.md](../cluster/replication.md)).

- **Replicated state.** Each job is one flat key in the document:
  `job:<uuid> → { kind, status, project, graph, run, scaling_policy, created,
  updated, detail }` (`src/core/catalog.hpp`). The user-facing mutator captures
  into the document; on each `tick()` the node **reconciles** the projection to
  match the document, so a peer's job (or a status update from a peer)
  materializes locally with **no re-capture and no audit** (`apply_value`, so no
  feedback loop). Versions converge conflict-free.
- **On-disk projection.** A job is a folder named by its UUID under the data dir,
  holding `job.json` (the value above plus `id`):

  ```
  data_dir/
    <job-uuid>/
      job.json
  ```

  `Node::start()` rebuilds the in-memory list by scanning the data dir
  (`load_jobs()`) and — when replicated — reconciling from the CRDT document, so
  a job **survives restart** in both standalone and replicated nodes.

## Boundary validation (JSON Schema)

Like [`Run`](../pipeline/runs.md) and the [audit event](../governance/audit-trail.md), a `ComputeJob` value
is validated against a **formal JSON Schema** (draft-07) rather than ad-hoc shape
checks. The schema is the single source of truth for the value shape — `kind` and
`status` are closed enums and `additionalProperties` is `false` — and is exposed
via `zetta::compute_job_value_schema()`. It lives inline in
`src/core/compute_job.cpp` and is mirrored, for reference, in
[`schemas/compute_job.schema.json`](../../schemas/compute_job.schema.json) (keep the
two in sync).

Every value that **enters** the system passes through it:

- `validate_compute_job_value(json, &error)` — checks a value against the schema,
  filling `error` with a human-readable reason on failure.
- `parse_compute_job_value(json, &error)` — validate **then** decode into a
  `ComputeJobValue`, returning `nullopt` when validation fails.

Both the disk-load path (`Node::load_jobs`, reading `job.json`) and the
CRDT-reconcile path (`Node::reconcile_from_replication`, reading a `job:` value)
go through `parse_compute_job_value`, so a malformed record (bad kind/status,
wrong types, stray fields) is **rejected at the boundary with a clear reason**
instead of corrupting the in-memory tree — fail fast, don't propagate.

The validator is the `pboettch/json-schema-validator` library, linked **PRIVATE**
to `zetta-core` (it stays out of the public header surface); validation failures
are routed through the library's error-handler callback (not exceptions), so they
never cross the library boundary as a thrown type.

## Tests

`tests/test_compute_job.cpp` covers create / update (lifecycle) / query, the
`job_created` + `job_finished` audit emission (including that non-terminal
transitions don't audit and a terminal state audits exactly once), survival
across a restart (both disk-only and CRDT-only), replication of a job + a later
status change to a connected peer, the JSON-Schema boundary rejecting malformed
values, and a malformed `job.json` being skipped on load.
