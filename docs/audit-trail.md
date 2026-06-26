# Audit trail

An **audit trail** records an immutable, append-only `AuditEvent` for every
governed action — a **graph change**, a **deploy**, a **data-access** decision,
a **compute-job lifecycle** event, or an **access-key issue/revoke** (e.g. a
burst node's identity — see [burst-identity.md](burst-identity.md)). It is a
slice of **FEAT-9 (Governed self-service)** and realizes the
closed domain spec **ENT-23**; it relates to **CTL-5** (audit trail + approval
gates). It builds directly on the [key-scoped access model](access-control.md)
(`Principal` + `AccessKey`): a data-access event is exactly the outcome of a
`Node::authorize` decision.

> **Scope, deliberately.** This slice is the **record** — emit + list + replicate
> + validate. The first **approval gate** (blocking a risky action pending
> sign-off) is built on top of it — the
> [large-burst cost-cap approval gate](burst-approval-gate.md) (APR-3) records its
> block/approve decision here as a `provision_requested` event. Threading the
> login key through the whole request path (so *every* read/write is authorized
> and thereby audited) is a later slice; today the data-access seam is
> `Node::authorize` itself.

## The type

An `AuditEvent` (`include/zetta/audit_event.hpp`) is a top-level entity with no
parent, modeled like the other replicated entities — but, unlike them, it is
**immutable and append-only**: there is no mutator and no drop path. Its value:

| Field | Type | Notes |
|---|---|---|
| `id` | `std::string` | UUIDv4; also the on-disk file name. Never changes. |
| `action` | `AuditAction` | `graph_created` / `graph_dropped` / `graph_deployed` / `data_access` / `job_created` / `job_finished` / `key_issued` / `key_revoked` / `provision_requested`. |
| `actor` | `std::string` | Who acted — the login key id / principal, when known (else empty). |
| `target` | `std::string` | What was acted on — a graph name, a deploy graph name, or a `kind:name` resource. |
| `allowed` | `bool` | The outcome. For `data_access` it is the decision; for state changes it is `true`. |
| `detail` | `std::string` | Human-readable reason / summary (e.g. the access decision's reason). |
| `time` | `std::string` | ISO-8601 UTC stamp of when the action was recorded. |

Stamps come from `zetta::now_iso8601()` (fixed-width, so they order
chronologically as plain text); the trail is kept sorted by `(time, id)`.

## What emits an event

Events are emitted at the governed seams, and **only for local user actions** —
the reverse `materialize_*` paths used by reconciliation never emit (a peer's
action arrives already-audited as its own replicated `audit:` event, so each
action is recorded exactly once, on the node where it happened):

| Action | Seam | Event |
|---|---|---|
| **Graph change** | `Project::create_graph` / `drop_graph` | `graph_created` / `graph_dropped`, `target` = graph name, `actor` = the project's owner key. |
| **Deploy** | `Node::deploy_graph` | `graph_deployed` (on a successful placement), `target` = graph name, `detail` = node/edge counts + the placed node. |
| **Data access** | `Node::authorize` | `data_access`, `allowed` = the decision, `target` = `kind:name`, `actor` = the login key id, `detail` = the decision's reason. |
| **Compute job** | `Node::create_job` / `ComputeJob::set_status` | `job_created` (on create) / `job_finished` (on the first terminal transition), `target` = job id, `allowed` = success (for `job_finished`), `detail` = the job kind / note. See [compute-jobs.md](compute-jobs.md). |
| **Burst identity** | `BurstIdentityManager::mint` / `revoke` | `key_issued` (per minted identity) / `key_revoked` (per revoked one), `target` = the access key id, `detail` = the job + burst node. See [burst-identity.md](burst-identity.md). |
| **Provision burst** | `BurstApprovalGate::provision` | `provision_requested`, emitted **only** when a burst exceeds the cap, `allowed` = approved (vs. blocked pending approval), `target` = the burst size, `detail` = which cap axis was exceeded + the outcome. See [burst-approval-gate.md](burst-approval-gate.md). |

Because every `authorize` decision is recorded, `Node::authorize` is **non-const**
(it appends to the trail). `zetta::authorize(const AccessKey&, const Resource&)`
— the *pure* decision over a known key — stays const and is unaffected.

```cpp
auto project = node.create_project("churn", key->id());
project->create_graph("pipeline", graph);          // -> graph_created
node.deploy_graph(graph, DeploymentTarget::local(), "pipeline");  // -> graph_deployed
node.authorize(key->id(), {ResourceKind::Database, "churn"});     // -> data_access (allow/deny)

for (const auto& event : node.audit_events()) {    // listed, chronological
    // event->action(), event->target(), event->allowed(), event->detail(), ...
}
```

## The sink seam

A graph change happens on a `Project`, not on the `Node` that owns the trail, so
the two are decoupled by an **`AuditSink`** (`include/zetta/audit_event.hpp`):
a one-method seam (`record_audit_event`). `Node` *is* the sink (it implements
`AuditSink`), and a `Project` holds a non-owning `AuditSink*` the same way it
holds a `ReplicationManager*` — null means "not audited". This mirrors how the
[blob-store](large-objects.md) and storage-engine seams keep `zetta-core`
decoupled. `Node::record_audit_event` is also the **public append API**, so the
future request-path slice can record data accesses at real read/write points
without going through `authorize`.

## Replication + persistence

An audit event is **replicated state**, like everything else in zetta (the
Automerge CRDT document is the source of truth; the on-disk tree and in-memory
node are its projection). It uses a flat catalog key (`src/core/catalog.hpp`):

```
audit:<uuid>  ->  { action, actor, target, allowed, detail, time }
```

Because the trail is **append-only and immutable**, replication is especially
simple: the set of `audit:` keys only ever *grows* and no value is ever edited,
so the CRDT converges trivially and reconciliation only *adds* peer events (it
never removes or updates one). `record_audit_event` captures `audit:<uuid>` into
the document (when replicated) then materializes the on-disk file;
`migrate_to_replication` captures any local-only events on start, and
`reconcile_from_replication` materializes peer events merged in by gossip. The
distributed, convergent, tamper-evident trail every node agrees on is a natural
fit for the CRDT model.

On disk, audit events project to a **single shared `audit/` folder** — one file
per event — rather than a folder per event, the natural shape for a high-volume
append-only log:

```
data_dir/audit/
  <audit-uuid>.json   { id, action, actor, target, allowed, detail, time }
  ...
```

The file name *is* the id. The `audit/` folder sits alongside the per-entity
UUID folders; the other loaders skip it (it carries no `database.json` /
`project.json` marker).

## JSON + boundary validation

The value round-trips through JSON (`to_json` / `parse_audit_event_value`) and is
**schema-validated at every boundary** — both when read from `audit/<uuid>.json`
on disk and when decoded from an `audit:` CRDT value — against a draft-07 JSON
Schema, using the same `pboettch/json-schema-validator` path as the
[Run record](runs.md). `action` is a closed enum and the shape is sealed
(`additionalProperties: false`), so a malformed event (unknown action, missing
`action`/`time`, a wrong-typed field, a stray field) **fails fast with a clear
reason** instead of corrupting the trail — a bad file is skipped at load, a bad
CRDT value is ignored at reconcile.

The schema is the inline source of truth in `src/core/audit_event.cpp`, mirrored
for reference by
[`schemas/audit_event.schema.json`](../schemas/audit_event.schema.json) — keep
the two in sync.

```json
{
  "id": "f47ac10b-58cc-4372-a567-0e02b2c3d479",
  "action": "data_access",
  "actor": "a1b2c3d4-...",
  "target": "database:churn",
  "allowed": false,
  "detail": "resource not in key scope — denied",
  "time": "2026-06-25T14:03:21.123Z"
}
```

## Where it fits

This is the audit half of FEAT-9, on top of the
[key-scoped access model](access-control.md). The pieces still ahead: **approval
gates** (block a risky action pending sign-off), threading the login key through
the **request path** (so every query / mutation is authorized and audited, not
just the `authorize` seam), and a **`system.zetta_audit`** SQL table (the trail is
queryable via the C++ API today). The trail is now surfaced in the **GUI**: the
headed [Admin & Settings panel](admin-panel.md) (SCR-7 / CMP-5) renders it as a
read-only table.

## Pointers

- `include/zetta/audit_event.hpp` — `zetta::AuditEvent` / `AuditEventValue` /
  `AuditAction` (ENT-23) + the `AuditSink` seam + JSON schema/validate/parse.
- `src/core/audit_event.cpp` — the value, JSON form, and JSON-Schema boundary
  validation.
- `include/zetta/node.hpp` — `Node::record_audit_event` / `audit_events` and the
  emission seams (`deploy_graph`, `authorize`).
- [access-control.md](access-control.md) — the `Principal` + `AccessKey` model a
  `data_access` event records the decision of.
- [admin-panel.md](admin-panel.md) — the headed **Admin & Settings panel** that
  renders this trail (and principals/keys/grants) read-only.
- [runs.md](runs.md) — the sibling immutable record whose JSON-Schema boundary
  validation this mirrors.
- [replication.md](replication.md) — the CRDT model these events replicate through.
