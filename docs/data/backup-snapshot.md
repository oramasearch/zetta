# Backup snapshots

A consistent point-in-time copy of a Zetta's data as one archive, and the restore of that
archive into an empty Zetta, checked against the counts the archive carries
([zetta#1518](https://github.com/oramasearch/zetta/issues/1518)). The Orama operator's backup
system calls these routes ([orama-operator#732](https://github.com/oramasearch/orama-operator/issues/732),
`internal/backup/zetta.go` in [orama-operator#1183](https://github.com/oramasearch/orama-operator/pull/1183)):
its Job asks for a snapshot, streams the archive to the bucket it names, and releases the
snapshot. The engine never holds the bucket's credentials.

Code: `include/zetta/data/backup_snapshot.hpp`, `src/core/data/backup_snapshot.cpp` (the
engine), `include/zetta/data/write_gate.hpp` (the freeze), `src/core/api/backup_routes.hpp/.cpp`
(authorization, audit, result mapping) and the routes in `src/core/api/http_server.cpp`.

## The routes

Every route needs **`cluster:admin`** and is audited twice: the authorization decision, then
what the call did (`api:<route>` in `system.audit`; a restore or a discard that wrote is a
`data_changed` row). Answers use the usual envelope (`{"ok", "value"|"error", ...}`); a
success also carries `snapshot` and `summary` at the top level, which is what the operator
reads.

| Route | Audit target | What it does |
|---|---|---|
| `POST /v1/backup_snapshot` | `api:backup_snapshot` | Take a snapshot. Answers `{"snapshot": "<id>", "summary": {...}}`. 503 `busy` while another snapshot is being captured, or when writes in flight do not finish within 10 seconds. |
| `GET /v1/backup_snapshot/<id>` | `api:get_backup_snapshot` | Stream the snapshot's archive (`application/octet-stream`, any size, a 1 MiB slice at a time). 404 once released or expired. |
| `DELETE /v1/backup_snapshot/<id>` | `api:release_backup_snapshot` | Release it. 404 once released or expired. |
| `POST /v1/restore_snapshot` | `api:restore_snapshot` | The body is an archive. Restore it into this node, which must hold no user data (409 `conflict` otherwise). Answers `{"summary": {...}}`, counted from the node after the restore, and `value.details.node_id`, the node a discard names. |
| `POST /v1/restore_snapshot:discard` | `api:discard_restored_snapshot` | Empty a restored scratch target. Body `{"node_id": "<this node>", "snapshot": "<the snapshot it was restored from>"}`. See [Emptying a scratch target](#emptying-a-scratch-target). |

A snapshot is kept under `<data_dir>/backups/<id>/` (`archive`, and `snapshot.json` once it
is complete) until it is released or 6 hours after it was taken (`BackupSnapshots::kRetention`).
The archive is written whole when the snapshot is taken, so a snapshot needs free disk about
the size of the data it copies.

These routes move raw archives, so the HTTP adapter serves them rather than the registry,
like the object byte routes. Their OpenAPI rendering is `backup_route_paths()`
(`src/core/api/backup_routes.cpp`), merged into the document beside the object routes
(zetta#1520).

## What a snapshot holds

- **The catalogue** as of one moment: every family except the source cluster's membership
  (`node:`), the object leases (`object_lease:`), retired audit (`audit:`) and the identity
  records (`user:`, `role:`, `token:`, `user_role:`, `policy:`, `principal:`, `key:`). Identity
  records are sealed under the source cluster's identity root and stay with it: a restored
  Zetta keeps its own owner, tokens, roles and row-security policies. Saved connections are
  copied still sealed under the source cluster's join token.
- **Every record** of every record collection, the engine-internal ones too (`_entity_keys`,
  each metric's `_metric.<name>`), with its revision, indexes and expiry.
- **Every object version** of every bucket.
- **Every body** the catalogue names: large-object bodies (dataset partitions included) and
  model weights.

Left out: a partition-only table whose load had not finished (it has no partition manifest
yet), with its partitions and their bodies, so a table is never restored torn; the
manifest lists it in `skipped_tables`. Pub/sub topics' messages, the audit trail, the
ledger and the node's security files are not in a snapshot.

## Consistency

While it captures, the snapshot freezes the record store's and the object store's write
gates (`WriteGate`, `include/zetta/data/write_gate.hpp`) on this node and on every other
member of the cluster (`BackupFreeze`, zetta#1520). A record write or an object
mutation decided on this node then answers `Unavailable` ("a backup snapshot is being
captured; retry"), and a replica install is refused (anti-entropy brings it later). The gate
never blocks a writer, so a freeze can never hold a pool thread or the owner loop. The
snapshot waits for the writes already past the gates to finish (polled from a coroutine
timer), then:

1. reads the catalogue on the node's owner loop, between two owner tasks, so no statement
   is half applied;
2. writes every record into the archive;
3. pins a reader on every object version (a reader holds the segments it reads, so a version
   replaced afterwards still reads whole);

and thaws. The object and body bytes are copied after the thaw from what it pinned; bodies
are immutable once written. Writes are refused only for steps 1 to 3.

Engine-internal rows (`put_internal`, metric folds) are not gated: an entity-key row is
written once, before the run or model that names it, so a captured reference always finds
its rows.

### The pause across the cluster (zetta#1520)

The pause covers every member. The answering node freezes its own gates, then sends
`backup.gate` `freeze` to every member it knows (`BackupPeer`, on every node's signal plane
after the object store). Each member freezes its two gates for the snapshot's id and answers
whether its writes in flight have drained. The answering node polls `status` (every 20 ms,
from a coroutine timer) until every member has drained, for at most 10 seconds.

- **Refused, never blocked.** On every member a write that meets the pause answers
  `Unavailable` at once, exactly as on the answering node.
- **Bounded.** Every gate is frozen for a lease of 120 seconds (`BackupFreeze::kLease`); a
  member never takes a longer one than it is asked for. After it the gate lapses by itself,
  so a dead coordinator or a lost thaw cannot leave a member refusing writes.
- **Released on any failure.** If any member refuses (it holds another snapshot's pause) or
  does not answer (it is unreachable, or runs an engine without `backup.gate`), every gate
  already frozen is released at once and the snapshot answers 503 `busy`. The thaw goes to
  each member as a fire-and-forget event, so releasing never waits on a member; one lost is
  covered by the lease.
- **Still one cut.** After the capture, the answering node checks that every gate, its own
  and each member's, is still held by this snapshot (`BackupFreeze::held`). If a lease lapsed
  during a long capture, the snapshot is refused (503) rather than written as something that
  is not one moment.

A gate holds one snapshot at a time: a second snapshot anywhere in the cluster is refused
while the first holds any member.

### Assembling the cluster's data

The answering node assembles the whole cluster's data:

- the catalogue is replicated to every node;
- records are on every live node;
- an object it does not hold is read from its owner;
- **a body it lacks is fetched** over `backup.body` (zetta#1520). That is a partition's body
  from the partition's owners, or any other body from any member. It is fetched a 4 MiB
  slice at a time and verified against its catalogue entry's size and SHA-256 before it
  enters the archive.

A partition body that none of its owners gives whole refuses the snapshot with 409
`conflict`, naming the body and its owners. The archive is never written short.

## The archive (`zetta-backup/1`)

A sequence of frames. Each frame is one JSON header line, `{"kind": ..., "bytes": n, ...}\n`,
followed by `n` bytes of payload:

| Kind | Header fields | Payload |
|---|---|---|
| `header` | `format`, `snapshot`, `node`, `taken_ms` | none |
| `catalogue` | | the catalogue entries, one JSON object |
| `records` | `collection`, `count` | at most 1,000 records, one JSON object a line: `key`, `value`, `revision`, `updated_ms`, `index`, `order`, `expires_ms` |
| `object` | `bucket`, `key`, `generation`, `updated_ms` | the object's bytes |
| `body` | `id`, `sha256` | the body's bytes |
| `manifest` | | `format`, `snapshot`, `node`, `taken_ms`, `summary`, `missing_bodies`, `skipped_tables`, `excluded` |
| `end` | `sha256` | none: the SHA-256 of every byte before this line |

A restore refuses an archive that does not start with its header, names another format, has
a frame kind it does not know, or whose trailer is missing or does not match.

## The summary

The counts a snapshot takes and a restore takes again, by names that are the same on every
node:

| Name | Counts |
|---|---|
| `table:<database>.<table>` | rows: those in the catalogue, plus the rows of every partition whose body is present |
| `bucket:<name>` | objects |
| `collection:<name>` | records, for each collection that holds any |
| `catalogue:<family>` | live catalogue entries of each family copied (record collections, metrics and buckets are counted by what they hold) |
| `bodies` | large-object and weights bodies present |
| `bodies:missing` | bodies the catalogue names that no node holds (absent when none); listed in the manifest's `missing_bodies` |

## Restore

`POST /v1/restore_snapshot` restores into a node whose own summary counts nothing (409
`conflict` otherwise, with the node's summary in `error.details`). One restore runs at a time
on a node. The archive is applied a frame at a time as it arrives, so no request holds a
whole archive: the catalogue in batches of 512 entries, each one replicated change (a
partition's `owner_nodes` become this node); records as stored, with their revisions; objects
as whole puts, or as uploads past 4 MiB; bodies verified against their catalogue entry's size
and SHA-256. At the end the trailer is checked, the buckets are synced, and the node is counted
the same way. **The restore passes only when that summary equals the manifest's**; otherwise it
answers 422 `verification_failed` with `expected`, `observed` and `differences` in
`error.details`, and leaves the node as restored for inspection. A record past its retention
when restored is not written; `expired_records` names them by collection and the engine's
comparison accounts for them.

The node records the restore in `<data_dir>/backups/restore.json`: the snapshot, its source
node, the state (`restoring`, `restored`, `unverified`, `failed`) and the summary counted when
it finished. A restore that fails part way says so, and leaves the node holding part of the
archive: discard it before restoring again.

## Emptying a scratch target

A weekly restore rehearsal restores into the same scratch Zetta each time, so it must be
emptied after each one. `POST /v1/restore_snapshot:discard` does that, and is refused unless
**every one** of these holds:

1. the caller holds `cluster:admin`;
2. the node was started as a restore target: `--restore-target`, or `ZETTA_RESTORE_TARGET=1`
   (off by default; the environment can only turn it on). Never set it on a live Zetta;
3. the node holds a restore record, so its data came from a restore;
4. the body names this node (`node_id`) and the snapshot it was restored from (`snapshot`);
5. the node still holds exactly what the restore counted: anything written since refuses it,
   so data written after the restore is never wiped.

It then removes every object, every record and every catalogue entry a restore writes, and
the buckets' leases; record collection and metric definitions stay (the engine declares some
itself). Bodies leave with their catalogue entries as the node reconciles. The restore record
is removed, so a second discard is refused until the next restore.

## Verify

Tests (written; not yet run, they run at review and release time):

- `tests/data/test_backup_snapshot.cpp`: `RestoresIntoAnEmptyNodeWithTheSameSummary` (every
  family, a 2,500-record collection over several frames, an object past one slice, a finished
  and an unfinished partition-only table, identity and membership left out, partitions owned
  by the target); `ASnapshotTakenUnderWritesIsOneCut` (a writer of record i then object i,
  five snapshots, the records never fewer than the objects nor more than one past them);
  `TheWriteGateRefusesWritesOnlyWhileFrozen`; `ANodeHoldingUserDataIsRefusedAsATarget`;
  `ADamagedOrTruncatedArchiveIsRefused`; `DiscardEmptiesOnlyARestoredRestoreTargetLeftAsRestored`;
  `ABodyHeldOnlyByAnotherMemberRefusesTheSnapshot`; `SnapshotsAreKeptUntilReleasedOrExpired`;
  `TheSummaryNamesWhatItCounts`.
- `tests/data/test_backup_cluster.cpp` (two real loopback members, zetta#1520):
  - `ThePauseCoversEveryMembersWritesAndIsReleased`: writes on both members are refused
    within moments, never blocked, and succeed after the thaw;
  - `AMemberHoldingAnotherSnapshotRefusesAndNothingStaysFrozen`;
  - `APauseThatLapsedDuringTheCaptureRefusesTheCut`;
  - `APartitionBodyHeldOnlyByAnotherMemberIsFetched`: a body past one slice, then refused
    once no member holds it;
  - `TheGateLapsesByItself`.
- `tests/api/test_backup_routes.cpp`: the operator's calls over loopback HTTP,
  `TheOperatorsCallsBackUpAndRestoreIntoAFreshNode`, `EveryRouteNeedsClusterAdmin`,
  `DiscardEmptiesOnlyARestoreTarget`, `ATruncatedArchiveIsRefused`.

## Not yet

- A rehearsal on the nonprod cluster: a scheduled `Backup` landing in the bucket and a
  `RestoreRehearsal` restoring it into a scratch `OramaZettaCluster`, which must be started
  with `ZETTA_RESTORE_TARGET=1` for the operator to empty it.
