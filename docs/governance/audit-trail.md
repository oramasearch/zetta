# Audit trail

Zetta records governed actions as immutable `AuditEvent` values. The trail is
replicated through the Automerge catalog, persisted in checksummed compressed
segments, queryable through `system.audit`, and shown in the governance and
activity views. It is the audit part of FEAT-9 and the record used by CTL-5.

## Audit policy

The default registry policy is one audit event per call. The policy changes only
for successful, authorised, high-frequency pub/sub reads:

| Call outcome or kind | Audit treatment |
|---|---|
| Write or any state-changing call | One event per call. |
| Denied call | One event per call, including every repeated denial. |
| Handler failure | One event per call. |
| Ordinary authorised read | One event per call. |
| Successful `receive_events`, `browse_topic`, `get_topic`, or `list_topics` | One session-start event, then a five-minute summary per actor, capability, and topic scope. |

The capability declaration selects `SummarizeAuthorizedReads`; handlers do not
implement audit policy themselves. The registry validates that this policy is
used only by non-mutating reads without consent gates. Adding it to a write is a
registration error.

A session-start event is a normal `data_access` row. Its target is
`api:<capability>`, and its detail names the scope. Each
`data_access_summary` row contains:

- `calls`: successful authorised reads in the window;
- `events_delivered`: pub/sub event envelopes returned by those reads;
- `scope`: the concrete topic, or `*` for `list_topics`;
- `window_started` and `window_ended`: the covered interval.

The summary is emitted every five minutes when the node ticks, when the next
call crosses the window boundary, and when the node stops. Both reads made by an
asynchronous long poll are counted: the initial empty read and its final read
after wake or timeout. A return with events contributes both one call and the
number delivered.

This remains complete for accountability. Actor, capability, scope, interval,
call count, and delivered-event count survive; an investigator can distinguish
idle polling from delivered traffic. Calls needing individual scrutiny—writes,
denials, and failures—remain separate immutable rows. The summaries remove
transport-loop noise without hiding an access decision or state change.

## Event value

`AuditEvent` (`include/zetta/governance/audit_event.hpp`) is a top-level value
with no mutator. Its common fields are:

| Field | Meaning |
|---|---|
| `id` | UUID; also the replicated catalog suffix. |
| `action` | Closed `AuditAction` vocabulary. |
| `actor` | Principal or access-key identity, when known. |
| `target` | Governed resource or `api:<capability>`. |
| `allowed` | Access decision or action outcome. |
| `detail` | Human-readable reason or summary. |
| `time` | ISO-8601 UTC record time. |
| `summary` | Optional structured read-window totals. |

The value is JSON-Schema validated at disk and replication boundaries. The
embedded schema in `src/core/governance/audit_event.cpp` is the source of truth;
`schemas/audit_event.schema.json` is its generated artifact.

Pub/sub administrative changes use `pubsub_topic_updated`,
`pubsub_event_updated`, and `pubsub_event_deleted`. Their details contain
sequence, revision, and before/after SHA-256 values. Payloads never enter the
audit trail. See [pub/sub CRUD](../pubsub/crud.md#audit-and-durability).

## Emitters

Events are emitted at governed boundaries, and reconciliation never emits a
second event for a peer's action. Important emitters include:

| Boundary | Actions |
|---|---|
| Capability registry | `data_access` and `data_access_summary` according to the policy above. |
| `Node::authorize` | Per-decision `data_access`. |
| Project graph CRUD and deployment | `graph_created`, `graph_dropped`, `graph_deployed`. |
| Compute jobs | `job_created`, `job_finished`. |
| Access-key and cloud governance | Key, provisioning, consent, inspection, and dataset actions. |
| Decision policy store | Policy creation/update and decision approval. |
| Pub/sub administration | Topic setting changes and event edits/deletions. |

`AuditSink::record_audit_event` decouples these emitters from the node-owned
store. `Node` assigns the id and time, captures the replicated value, and
appends its local projection.

## Replication and reconciliation

The catalog value is:

```text
audit:<uuid> -> { action, actor, target, allowed, detail, time, summary? }
```

Summary events use the same catalog prefix and reconciliation path as every
other audit event. Changed-key journals route only new `audit:` keys during a
tick. Startup still performs a full reconciliation. The local id index rejects
known records before parsing them.

Retention removes expired catalog keys through the replication manager. This
leaves CRDT removal metadata so an old peer cannot reintroduce an expired row.
The local segment rewrite and replicated removals converge through the normal
audit reconciliation path.

## Segment storage and bounded memory

The local projection lives under `<data_dir>/audit/`:

```text
audit/
  segment-00000000.audit.gz
  segment-00000001.audit.gz
```

Each segment is capped at 4 MiB of uncompressed records. A record frame has a
length, CRC-32, and compact JSON body containing the id and validated event
value. Gzip compression reduces disk use. A partial or corrupt trailing frame
stops recovery at the last valid frame instead of trusting damaged bytes.

Steady-state RAM holds a compact id-to-segment-position index and a bounded
recent-event window (2,048 by default), not every full event. Full historical
objects are inflated only for an explicit `audit_events()` or SQL read and are
released with that result. `recent_audit_events()` serves bounded UI views.

On first open after an upgrade, valid legacy `audit/<uuid>.json` files are
appended to the segment store, synced, and removed. Invalid legacy files remain
in place and are reported, preserving evidence for repair. Once migrated,
subsequent starts read the segments directly.

The storage regression appends one million records and requires less than
100 MiB additional resident memory and less than 50 MiB on disk.

## Retention

Ordinary audit rows have a 30-day default retention window. The node applies it
at startup and periodically while running. `--audit-retention-days N` or
`ZETTA_AUDIT_RETENTION_DAYS=N` changes the window; zero disables expiration for
an archival node.

Retention rewrites kept frames into a synced replacement directory, installs it
with directory renames, then records replicated removals. Every
`data_access_summary` is retained regardless of age. This preserves compact
accountability totals while bounding the detailed per-call projection.

## Querying

`system.audit` exposes the common fields plus nullable summary columns:
`calls`, `events_delivered`, `scope`, `window_started`, and `window_ended`.
For example:

```sql
SELECT action, actor, target, calls, events_delivered, scope, time
FROM system.audit
WHERE action = 'data_access_summary';
```

`GROUP BY` is not supported on system tables; aggregate returned rows in the
client when additional grouping is needed.

## Pointers

- `include/zetta/governance/audit_event.hpp` — event schema types and `AuditTrail`.
- `src/core/governance/audit_event.cpp` — codecs, segment store, migration, and retention.
- `include/zetta/api/registry.hpp` — per-capability `AuditPolicy`.
- `src/core/api/registry.cpp` — audit policy enforcement.
- `src/core/api/pubsub_capabilities.cpp` — summarised read declarations.
- `include/zetta/node.hpp` / `src/core/node.cpp` — windows, replication, and retention schedule.
