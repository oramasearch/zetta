# Audit trail

Zetta records governed actions as immutable `AuditEvent` values. The trail is
replicated through per-origin logs, persisted in checksummed compressed
segments, queryable through `system.audit`, and shown in the governance and
activity views. It is the audit part of FEAT-9 and the record used by CTL-5.

## Audit policy

The default registry policy is one audit event per call. The policy changes only
for successful, authorised, high-frequency pub/sub reads:

| Call outcome or kind | Audit treatment |
|---|---|
| Write or any state-changing call | One event per call. |
| Denied call | One event per call, including every repeated denial. |
| Handler failure | The authorization row, then a second row whose detail starts `failed: <code>` for any call that is not a plain read (#847). `allowed` stays true unless the handler itself refused (`unauthorized`). Before #847 a per-call action that failed left only the authorization row, which read exactly like a success. |
| Ordinary authorised read | One event per call. |
| Successful `receive_events`, `browse_topic`, `get_topic`, or `list_topics` | One session-start event, then a five-minute summary per actor, capability, and topic scope only when the window returned data. |

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
number delivered. A window in which every authorised read delivered zero events
and returned no other data produces no summary row. `get_topic` metadata and
non-empty topic lists still count as returned data even though they contain no
event envelopes.

This remains complete for accountability. The session-start row records which
principal began polling which capability and scope. A summary then preserves
actor, capability, scope, interval, call count, and delivered-event count for
every window that exposed data. Calls needing individual scrutiny—writes,
denials, and failures—remain separate immutable rows. Suppressing later empty
windows removes idle transport-loop noise without hiding an access decision,
delivered result, or state change.

## Event value

`AuditEvent` (`include/zetta/governance/audit_event.hpp`) is a top-level value
with no mutator. Its common fields are:

| Field | Meaning |
|---|---|
| `id` | UUID; also the replicated catalog suffix. |
| `action` | Closed `AuditAction` vocabulary. |
| `actor` | Principal or access-key identity: the caller the entry point declared (see [attribution](#attribution)), `local` for the unauthenticated local session. |
| `target` | Governed resource or `api:<capability>`. |
| `allowed` | Access decision or action outcome. |
| `detail` | Human-readable reason or summary. |
| `time` | ISO-8601 UTC record time. |
| `summary` | Optional structured read-window totals. |

The value is JSON-Schema validated at disk and replication boundaries. The
embedded schema in `src/core/governance/audit_event.cpp` is the source of truth;
`schemas/audit_event.schema.json` is its generated artifact.

A SQL statement that changes data, schema or governance — `INSERT`, `UPDATE`,
`DELETE`, `TRUNCATE`, `CREATE`/`DROP`, user, token, role and policy statements —
records `data_changed` from the executor, whatever surface ran it. Its target is
`sql:<kind>:<object>` (for example `sql:table:app.docs`), `allowed` is false for
a refusal, and the detail is the statement's status line (`UPDATE 3`) or its
error — never a value it wrote, so `CREATE TOKEN`'s secret is not in it. The
`sql` capability's own per-call `data_access` row is recorded as before. A
`SELECT` the executor refuses (no `sql:read` on the table, or a filter, grouping,
aggregate or ordering over a masked column) records `data_access` with
`allowed: false` and the target `sql:table:<db>.<table>` (#252). See
[per-statement enforcement](access-control.md#per-statement-enforcement).
A node that predates this action does not accept it in an audit range, which is
one more reason the upgrade policy below upgrades all nodes together.

Changes made through the Node's own API are recorded by the Node as
`data_changed` (#847), because the headed app calls these methods directly and
no capability or SQL statement sees them:

| Call | Target | Detail |
|---|---|---|
| `create_project`, `rename_project`, `reveal_project`, `drop_project` | `project:<name>` (the new name on a rename) | `project created`, `internal project created`, `project renamed from '<old>'`, `internal project '<old>' revealed`, `project dropped` |
| `create_database`, `rename_database`, `drop_database` | `database:<name>` | `database created`, `database renamed from '<old>'`, `database dropped` |
| `create_model`, `drop_model` | `model:<id>` | `model '<kind>' registered [in project '<name>']`, `model dropped` |
| `run_graph` | `run:<id>` | `graph run succeeded`, or `graph run failed [at step '<id>']: <error>` |

A call that changes nothing (a name clash, nothing to drop) records nothing. A
change made over a capability or SQL therefore has two rows: the
authorization or statement row, and the Node's row for its effect. A peer's
change arrives through reconcile and is not recorded a second time.

## Attribution

Every row names who acted (#847). An entry point that knows the caller opens a
`zetta::governance::AuditActorScope` for the work it does:

| Entry point | Actor |
|---|---|
| `Registry::call` | the calling user or access key; `local` when none |
| `Registry::decide_consent` (approved execution) | the requester, as before |
| `query::Executor::execute` (every SQL surface) | the session's acting key; `local` when none |
| `repl::GovernedConsole::run` | the console's session key; `local` when none |
| The headed app's frame (`App::render_frame`) | `local`, the person at the desktop |

`Node::record_audit_event` stamps that actor on any event recorded without one,
so a deploy (`graph_deployed`), a queued job (`job_created`) and the Node's own
`data_changed` rows name the caller instead of being blank. A project's graph
changes (`graph_created`, `graph_approved`, `graph_dropped`) name the caller
rather than the project's owner, and fall back to the owner only when no caller
was declared. The scope is thread-local; `Node::run_on_owner` carries the
caller's actor to the owner thread with the work it hands over, so work queued
from a background worker is attributed to whoever queued it, not to what the
owner thread was doing.

### Work that finishes later, on another thread (#1327)

Work handed off by an entry point keeps its caller, and the surface it came
through, as a `zetta::governance::AuditOrigin` captured where the work is
launched and reinstated where it runs:

| Launcher | How the origin travels |
|---|---|
| `Node::create_job` | stored on the job as `submitted_by` / `submitted_via` (replicated, optional); `ComputeJob::set_status` puts them on `job_finished`, whichever thread finishes the job |
| `kind: job` workers on their own thread (`AsyncChurnRun`, rank, survival, document extraction, label efficiency, streaming, testkit cases, the cloud actions) | captured in the thread's lambda when it is started, inside the handler, and opened as an `AuditOriginScope` at the top of the thread |
| `kind: job` coroutine workers (solution runs, anomaly, segmentation, forecast, next best action) | captured in the work's state at launch and passed to `Node::run_on_owner(origin, work)` for every owner hop; each synchronous engine call between hops runs under an `AuditOriginScope` that closes before the next suspension |

A scope is never held across a `co_await`: the coroutine may resume on another
thread, and the scope would restore that thread's actor to the wrong value.

Not covered yet: the deployment controller's self-heal runs (started from
`Node::tick`) and any other work the node starts on its own name no automated
principal; the engine has no scheduler that fires retrains today
(`set_training_schedule` only records the schedule). Both stay open on #1327.

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
| Node catalogue changes | `data_changed` for projects, databases, models and graph runs (#847). |
| Compute jobs | `job_created`, `job_finished`. |
| Access-key and cloud governance | Key, provisioning, consent, inspection, and dataset actions. |
| Decision policy store | Policy creation/update and decision approval. |
| Pub/sub administration | Topic setting changes and event edits/deletions. |
| Audit administration | `audit_purged`, recorded after an explicit purge. |
| SQL executor | `data_changed` for every statement that changes data, schema or governance, allowed or refused (#1229); `data_access` with `allowed: false` for a refused `SELECT` (#252). |
| Outbound HTTP client | `egress_denied` for every connection the egress policy refuses, with target `egress:<host>:<port>` and actor `node` ([egress-policy.md](egress-policy.md), #75). |

`AuditSink::record_audit_event` decouples these emitters from the node-owned
store. `Node` assigns the id and time and appends the event to its origin log
for bounded range replication.

## Replication and upgrade policy

Audit events are **not catalog entries** (#1164). Each newly recorded event has
an immutable origin (the recording node id) and monotonically increasing local
sequence. Peers advertise per-origin high/low watermarks and pull missing
checksummed frames in bounded ranges. A relay preserves the original origin,
sequence and id. Duplicate ids use the existing 96-bit fingerprint index.
There is no dual writing, catalog reconciliation, or per-event CRDT tombstone.

The peer hello advertises audit protocol version `3` (`kAuditProtocolVersion`;
`3` since #1328 added `surface`, `2` before it); a missing field means the
old version `0`. Mismatched peers log an explicit refusal and exchange no audit
status, requests or responses. The rest of the catalog continues to replicate.
The upgraded catalog adapter strips `audit:` operations from incoming documents,
including removed values, before merging, and rejects local writes with that
prefix. `system.nodes.audit_protocol_version` and `audit_compatible` expose the
handshake result. Unknown/unconnected peer versions are null.

**Upgrade all nodes together.** Mixed versions deliberately do not converge
audit history. Stop old writers, upgrade every node, and restart the cluster;
do not roll one node at a time expecting audit delivery during the transition.
The AKS cluster is development, not a production compatibility commitment.

On first open, every live `audit:<uuid>` key held locally is validated and
appended to that node's segments under reserved origin `legacy`, deduplicating
records already present. Legacy sequences are local and **never shipped**: old
peers already held the replicated catalog. After the segments are synced, the
catalog adapter removes all audit operations from its serialized history and
rebuilds the document. It preserves other operation identities, predecessor
sets, removal markers and actor clocks. No per-audit-key tombstone remains.
Migration is restartable; a failure leaves the old persisted catalog available.
An unreadable store/catalog or failed migration prevents startup.

Network coroutines only queue bounded audit messages. The owner-thread tick
reads/appends segments and sends replies; no thread or public capability is
created per request. Status is periodic, requests retry after a lost reply, and
range validation rejects corrupt frames, wrong origins, out-of-order sequences
and gaps not covered by either of the origin's class-specific retention
low-water marks.

## Segment storage and bounded memory

The local projection lives under `<data_dir>/audit/`:

```text
audit/
  segment-00000000.audit.gz
  segment-00000001.audit.gz
  index.bin
  watermarks.bin
```

Each segment is capped at 4 MiB of uncompressed records. A record frame has a
length, CRC-32, and compact JSON body containing the id, origin, sequence and
validated event value. Gzip compression reduces disk use. A partial trailing frame
stops recovery at the last valid frame instead of trusting damaged bytes.

Steady-state RAM holds a flat open-addressed 96-bit id fingerprint index
and chunked per-origin sequence/offset indexes, one reusable segment cache, and a bounded
recent-event window (2,048 by default), not every full event. The checksummed
sidecar rebuilds these indexes without inflating historical events at startup;
only the active tail and bounded recent window are decoded. A missing or damaged
sidecar is rebuilt by streaming segments. Full historical objects are
inflated only for an explicit `audit_events()` or SQL read and are released with
that result. `recent_audit_events()` serves bounded UI views.

Index records referencing a missing segment, or durable watermarks ahead of
available unexpired frames, cause startup to fail rather than crediting lost data.

The fingerprint index stores no duplicate frame locations and allocates no
per-event map nodes. Each seek entry is 16 bytes: sequence, segment, and offset;
frame length is read from the existing checksummed frame envelope. The seek
index grows in chunks without reallocating a whole origin's history. Normal
range pages are 64 KiB (a single larger validated frame is allowed). Segment
inflation reuses its buffer in place and only invalidates the cached segment
when that segment changes; bounded wire/range buffers reserve their capacity.
After migration and a completed bulk catch-up, the node requests allocator
pressure relief (`malloc_zone_pressure_relief` on macOS, `malloc_trim` on glibc).
This is not a substitute for bounded live allocations; proof records RSS both
before and after an additional relief request. Allocator-retained pages are
measured, not subtracted from the RSS budget.

On first open after an upgrade, valid legacy `audit/<uuid>.json` files are
appended to the segment store, synced in 64-record durability batches, and then
removed. Invalid legacy files remain in place and are reported, preserving
evidence for repair. Existing frames without origin metadata are indexed under
`legacy` as well. Subsequent starts use the sidecar, not a full reconciliation.

The storage regression appends one million records and requires less than
100 MiB additional resident memory and less than 120 MiB on disk, including the
new seek index. The replicated proof uses two independent native processes,
500,000 writes on each, convergence to one million records per node, and fresh
processes for restart. Each node must stay below 100 MiB added steady-state RSS
and 150 MiB added startup peak RSS. See [the proof runbook](../testing/audit-replication.md).

## Retention

Ordinary audit rows have a 30-day default retention window. The node applies it
periodically while running, with the first pass after startup rather than a
historical scan during open. `--audit-retention-days N` or
`ZETTA_AUDIT_RETENTION_DAYS=N` changes the window; zero disables expiration for
an archival node.

Retention rewrites kept frames into a synced replacement directory, installs it
with directory renames, then advertises monotone ordinary and summary low-water
marks per origin. Only an expired prefix advances its class mark; a clock-skewed
older event after a newer retained event waits for a later pass. High-water
marks never decrease, even when every event is removed, so sequences are not
reused. Peers accept expired gaps and trim their own segments when a newer mark
arrives. Replayed old ranges cannot resurrect expired rows.

Summaries have an independent 365-day default. Set
`--audit-summary-retention-days N` or
`ZETTA_AUDIT_SUMMARY_RETENTION_DAYS=N`; zero keeps summaries forever for an
archival node. The two cursors advance independently, so a short ordinary
window does not force a retained summary out and an expired summary cannot
return from a peer.

Records from either class that remain above their own low-water mark are still
included when a new peer pulls a range from zero. No catalog removal is written
for retention.

## Explicit purge

`purge_audit {"before":"<UTC ISO-8601 time>"}` is an admin-only registry
action exposed with REST and MCP parity. It requires the dedicated
`audit:purge` permission (`orchestration:purge_audit`); the built-in `admin`
role has it, while `writer` and `reader` do not.

The action runs the same segment rewrite and replicated low-water-mark path as
scheduled retention, using `before` for both ordinary rows and summaries.
Records strictly before the cutoff are removed on the caller and peers trim
when they receive the new cursors. A future cutoff is refused. On success the
node appends one `audit_purged` row after the rewrite, naming the actor, cutoff,
and number of removed rows. Appending after the rewrite guarantees the row for
the current purge is never one of the records it removes.

REST callers use `POST /v1/purge_audit`; MCP callers invoke the `purge_audit`
tool. The result is `{"before":"...","removed":N}`. Segment rewriting is
offloaded through the runtime's blocking-work path while authorization and
activity notification remain on the node owner thread.

## Which surface an action came through (#1328)

Each row may carry `surface`: `mcp` (MCP `tools/call`), `rest` (`/v1/<name>`
with a bearer, and the governed serving routes), `web` (the Studio web app and
dashboard, authenticated by the `zetta_session` cookie, and the web console),
`desktop` (the headed app), `console` (the governed console run directly),
`pgwire` (reserved for a Postgres wire listener; zetta has none today) or
`internal` (work the node starts itself). The entry point declares it the way
it declares the actor: the server layers set `ApiContext::surface` and
`Registry::call` opens a `zetta::governance::AuditSurfaceScope`, the headed
app's frame and `GovernedConsole::run` open one themselves, and
`Node::record_audit_event` stamps it on every row recorded without one.
`Node::run_on_owner` carries it (with the actor, as an `AuditOrigin`) to the
owner thread. The same capability called over MCP and over REST by two
identities therefore gives two rows with different actors and surfaces.

`surface` is optional: a row from before #1328, or one recorded where no entry
point declared a surface, has none (`system.audit.surface` is null). The schema
accepts only the values above. Adding the field changed the audit protocol to
`3`; a node at `2` rejects these records (its schema is sealed), so upgrade all
nodes together, as below.

Not decided yet: whether a call `Registry::call` refuses as `invalid_input`
before its authorization step (so it leaves no row today) should be recorded
for a mutating capability. That question stays open on #1328.

## Querying

`system.audit` exposes the common fields, `surface` (#1328), plus nullable summary columns:
`calls`, `events_delivered`, `scope`, `window_started`, and `window_ended`.
For example:

```sql
SELECT action, actor, target, calls, events_delivered, scope, time
FROM system.audit
WHERE action = 'data_access_summary';
```

`GROUP BY` is not supported on system tables; aggregate returned rows in the
client when additional grouping is needed.

The existing `sql` capability serves these same rows over REST and MCP; no
capability or event-value schema changes. `audit_events()` and
`recent_audit_events()` retain their existing contracts. `system.node_metrics`
adds `audit_protocol_version`, `audit_watermarks` (JSON text keyed by origin,
with `high`, `low`, and `summary_low`) and estimated `audit_memory_bytes`.
The `system.memory` audit row (#1163) uses that same segment-index/cache/window
estimate and counts retained ids without loading historical events. It no longer
includes per-event catalog values. See [memory accounting](../ops/memory.md).

## Pointers

- `include/zetta/governance/audit_event.hpp` — event schema types and `AuditTrail`.
- `src/core/governance/audit_event.cpp` — codecs, segment store, migration, and retention.
- `include/zetta/api/registry.hpp` — per-capability `AuditPolicy`.
- `src/core/api/registry.cpp` — audit policy enforcement.
- `src/core/api/pubsub_capabilities.cpp` — summarised read declarations.
- `include/zetta/node.hpp` / `src/core/node.cpp` — windows, replication, and retention schedule.
