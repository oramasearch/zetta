# The keyed record store

Refs [#1231](https://github.com/oramasearch/zetta/issues/1231). Built for lake's workspace records
([lake `docs/architecture/workspace-records.md`](https://github.com/oramasearch/lake/blob/issue-370-workspace-store/docs/architecture/workspace-records.md),
the "Zetta capability required" section): mutable rows, one-use codes, sessions, task claims and
unique names that need conditional writes, and collections large enough that a full table scan
breaks a page budget.

`zetta::data::RecordStore` (`include/zetta/data/record_store.hpp`,
`src/core/data/record_store.cpp`) keeps records addressed by `(collection, key)`. A record's value
is **opaque bytes**, base64 on the wire; the engine never reads it, and lake seals it before it
arrives. A record also carries its value in each declared secondary index, an `order` string that
orders it within an index value, and a revision.

## Contract

| Property | What holds |
|---|---|
| Durable | The node that orders a write fsyncs it to its record log before it acknowledges. |
| Replicated | Before a write is acknowledged, every live peer has installed it. A node that was down or partitioned catches up by anti-entropy within a few seconds of rejoining. |
| Linearizable per key | Every write of a key is ordered by one node, the key's **owner**, with an atomic read-check-write. `if_absent` and `if_revision` are decided there. |
| Read after write | A read on any live node after the acknowledgement returns the write or a newer one. |
| Reads | `get_record` is one key-directory lookup and one `pread`; `list_records` walks one in-memory index range. Each reads its collection's definition from the catalogue's shared view, which never waits for a catalogue save (#1248). |
| Retention | A collection may declare `retention_ms`: a record expires that long after its last write. An expired record is invisible at once, and every node removes its copy within a second. |
| Bounded | Disk and memory follow the live set, not the write history: the soak below holds 1,000,000 writes at a flat footprint. |

What it does not promise, stated so a caller can plan for it:

- **During a membership change**, two nodes can briefly disagree about a key's owner. A forwarded
  write to a node that no longer considers itself the owner is refused with `busy` ("ownership of
  this key is moving; retry"). In the window before the views converge, two writes of one key on
  different owners can both succeed. Every node keeps the higher revision, so the copies agree,
  but one write is lost. A clean full mesh, which the operator's StatefulSet is, never enters that
  window.
- **The ownership view is the direct links.** A node that is not directly linked to every other
  durable node (a spanning cluster joined through a relay) sees a different owner set. Run the
  record store on a fully meshed set of permanent nodes.
- **A read concurrent with a write** may return the old value on one node and the new value on
  another until the write is acknowledged. A write is visible on its owner a moment before its
  fsync completes, so a crash in that moment can lose a write a concurrent reader saw; the writer
  itself never got an acknowledgement.
- **An acknowledged write whose fan-out timed out on a peer** reports it: `replicas` is less than
  `replicas_expected`. The write is durable on its owner and reaches the peer by anti-entropy.
- **A node away for longer than the deletion grace** (`kTombstoneGraceMs`, 24 hours) can bring a
  record deleted meanwhile back when it rejoins: the other nodes have forgotten the deletion.
  Expired records never come back, since expiry travels with the record.

## How it works

### Storage: a node-local log, not the replicated catalogue

The first version kept records as keys of the replicated Automerge document. A soak (below) showed
why that cannot hold lake's load: Automerge keeps every write in its history, a deletion is one
more write, and the document is saved whole once a second. After 200,000 puts with a three-second
retention, three keys were live but the saved document was 141 MiB and growing linearly, the
process held 5.2 GB, saves took 1.5 s (p50) to 7.6 s (p99), and reads overlapping a save waited up
to 437 ms.

Now the replicated catalogue holds only each collection's definition
(`record_collection:<name>`, replicated like any catalogue key). Each node keeps its copy of every
record in `zetta::data::RecordLog` (`src/core/data/record_log.hpp`), under `<data_dir>/records/`,
in Bitcask's design:

- **Segments.** `segment-NNNNNNNN.log` files of checksummed frames (`u32 length`, `u32 crc32`,
  JSON `{c, k, r}` where `r` is a `stored_record`). The active segment rolls at 32 MiB.
- **Key directory.** In memory, per collection: each key's metadata (revision, index values,
  order, expiry, deletion) and the location of its frame. Values stay on disk and are read with
  one `pread`. Each declared index is an ordered set of `(order, key)` per index value, so a list
  walks exactly its range.
- **Writes.** One frame appended under an exclusive lock; the fsync runs after the lock is
  released, so a read never waits for the disk.
- **Purges** (expiry, and deletions past their grace) append a small marker, so a restart never
  revives an older frame of the key.
- **Compaction.** Every tick, `maintain()` deletes dead sealed segments oldest-first (oldest-first
  keeps every purge marker until no older frame of its key remains) and, from a mostly-dead oldest
  segment, moves the live frames forward, one megabyte per step, reading outside the lock.
- **Startup** scans the segments in order and rebuilds the directory, the indexes, the expiry
  queue and the anti-entropy digests. A torn frame at the end of the last segment is truncated.

### Ordering, replication and catch-up

**The owner.** A rendezvous hash of `(collection, key)` over this node and its live peers picks
the owner. Ephemeral nodes order writes only when no permanent node is reachable. A write that
arrives elsewhere is forwarded to the owner over the signal plane (`record.put`).

**The conditional write.** The owner's log decides under its lock: it reads the key's current
revision, checks `if_absent` / `if_revision`, and appends the new record. The new revision is
`max(previous + 1, now in microseconds)`, so revisions keep rising across a deletion and a purge
and a re-created key never reuses one a caller still holds.

**The fan-out.** The committed record goes to every live peer at once (`record.apply`, through
`Cluster::request_each`), and each peer keeps it only when its revision is higher than the one it
holds.

**Anti-entropy.** Every five seconds, on the blocking pool, each node pulls from each live peer:
it compares 256 bucket digests per collection (an XOR of `(key, revision, deleted)` hashes kept
current on every write; `record.digest`), lists the versions in differing buckets (`record.keys`),
and fetches the records that are newer than its own (`record.fetch`), skipping expired records
and deletions past their grace. A node that missed writes, or a new node, converges without any
gossip of record bodies.

**Retention and cleanup.** Expiry and the deletion grace are properties of the record, the same on
every node, so each node removes its own copies from an expiry queue every second; a revision
check makes a removal lose to a write that landed since the key was queued.

**Engine-internal collections.** A collection whose name starts with `_` belongs to the engine;
the API refuses such names, so no caller can read or write one. The time series folds its buckets
into `_metric.<name>` with `fold` (this node only, replicated by anti-entropy). The text entity-key
table writes `_entity_keys` rows with `put_internal` and `remove_internal`: fsynced on the writing
node and sent to every live peer at once, with anti-entropy for a peer that missed them. Each row
has one writer, so no owner orders them ([entity-key-table.md](entity-key-table.md), #1255).

## API

Every operation is one registry capability: REST route, MCP tool and OpenAPI operation. Writes
and reads run on the blocking pool after the owner-loop authorization gate (`blocks`). A `value`
that is not base64 is refused as `invalid_input` at `/value`.

| Capability | Kind | Right | Input → output |
|---|---|---|---|
| `create_collection` | action | `records:manage` | `{collection, indexes?, retention_ms?}` → `{collection, created}`. Idempotent for an identical definition; `409 conflict` for a different one. |
| `put_record` | action | `records:write` | `{collection, key, value, index?, order?, if_absent? \| if_revision?}` → `{collection, key, revision, updated_ms, expires_ms?, owner, replicas, replicas_expected}` |
| `get_record` | read | `records:read` | `{collection, key}` → `{record: {collection, key, value, revision, index, order, updated_ms, expires_ms?}}` |
| `delete_record` | action | `records:write` | `{collection, key, if_revision?}` → the same shape as `put_record` |
| `list_records` | read | `records:read` | `{collection, index?, value?, after?, limit?, direction?}` → `{records, next_after?}` |

`get_record` and `list_records` also accept a JSON-body POST. A failed condition answers HTTP 409
(`conflict`) with `error.details.revision`, the live revision, when there is one. An owner that
does not answer, or ownership that is moving, answers 503 (`busy`): retry.

`list_records` with an index lists that index value's records ordered by `order`, then key,
`direction` `asc` (default) or `desc`, `limit` 1–1000 (default 100). Without an index it lists
every record of the collection by key. `next_after` is an opaque cursor; pass it as `after`.

Limits: collection names `^[a-z0-9][a-z0-9_.-]{0,95}$`, index names `^[a-z0-9][a-z0-9_]{0,63}$`,
at most 8 indexes; keys 1–512 bytes without control characters; values at most 1 MiB of base64;
index values 512 bytes; `order` 256 bytes.

### Granting a collection namespace

`records:read`, `records:write` and `records:manage` are object-scopable by collection name
prefix, like topics (#1029):

```sql
CREATE ROLE lake_service;
GRANT mcp:call TO ROLE lake_service;
GRANT records:read, records:write, records:manage ON COLLECTION lake_ TO ROLE lake_service;
```

A user holding that role reads and writes every collection whose name starts with `lake_` and no
other. None of the built-in roles except `admin` holds a records right, so with lake's service user
as the only holder of the `lake_` grant, only lake writes lake's collections.

## Latency

`RecordStoreBenchmark.GetAndListStayInsideTheTenMillisecondBudget` and
`RecordCapabilities.GetAndListCallsStayInsideTheTenMillisecondBudget` load a workspace's message
log: 500 conversations of 100 messages, a 1 KiB body each, so 50,000 records. Measured on an Apple
M3 Max:

| Read | Store | Registry call (validation, authorization, audit, handler) |
|---|---|---|
| `get_record`, median / p99 | 0.005 ms / 0.008 ms | 0.051 ms / 0.072 ms |
| `list_records`, 50 records, median / p99 | 0.262 ms / 0.311 ms | 0.404 ms / 0.640 ms |
| `list_records` 100 + `get_record`, worst of 4 concurrent readers | 0.624 ms | |

Both tests fail above 10 ms. A write costs one fsync on the owner plus one signal-plane round trip
to each live peer (the outbox flushes every 20 ms).

## Soak

`RecordStoreSoak.HistoryStaysBoundedAndReadsStayFastDuringSaves` (`tests/data/test_record_soak.cpp`)
runs a node with four writers and a reader: puts of 1 KiB values across three collections
(`agent_task_status`, `ask_messages`, `webhook_events`) with a three-second retention, 20% of them
`if_revision` updates of recent keys, while the owner loop ticks, saves, sweeps and compacts. It
fails if the record log passes 160 MiB, the live set 50,000 records, the RSS 1 GiB, the catalogue
32 keys or a 256 KiB save, or if the median read overlapping a save costs more than three times
the median read in the same run. It asserts no wall-clock budget: on a loaded host (load average
13 to 60) a fixed one failed while measuring the scheduler, not the store (#1248). It prints the
save and read timings for a reviewer. `ZETTA_RECORD_SOAK_PUTS` sets its length; the suite runs
200,000. One million, on an Apple M3 Max:

| | Records in the replicated document (first version, stopped at 200,000 puts) | Node-local record log (1,000,000 puts) |
|---|---|---|
| Throughput | 1,085 puts/s | 11,640 puts/s |
| Catalogue on disk | 141 MiB at the end, growing linearly | under 0.1 MiB (4 keys) |
| Catalogue save, p50 / p99 | 1,528 ms / 7,647 ms | 0.5 ms / 0.8 ms (74 saves, one forced each second) |
| Record storage on disk | (in the catalogue) | 76–120 MiB throughout; 27 MiB once the last records expired |
| Live records | | 30,000–43,000 throughout (three seconds of writes) |
| Key directory and indexes in memory | | 17–24 MiB |
| `records.journal` (first version) | 0–12 MiB | (no journal: the log is the durable store) |
| Process RSS | 5,175 MiB, growing about 20 MiB per 1,000 puts | 121 MiB peak, flat after 200,000 puts |
| Read p99 (`get` + `list` of 50) overlapping a save | up to 437 ms | 6.8 ms |
| Read p99 otherwise | 2.5 ms | 2.4 ms |

## Tests

| Test | Covers |
|---|---|
| `StoreFixture.ConditionalWritesFollowTheLiveRevision` | `if_absent`, `if_revision`, one-use consumption, conditional delete, revisions rising across a re-create |
| `StoreFixture.RefusesBadInputAndUnknownCollectionsAndIndexes` | the validation boundary; idempotent and conflicting collection definitions |
| `StoreFixture.ListsOneIndexValueInOrderAPageAtATime` | order then key, both directions, cursors, a record moving between index values, the key listing |
| `StoreFixture.RetentionHidesThenSweepRemovesExpiredRecords`, `…DeletionTombstonesArePurgedAfterTheGracePeriod` | retention, cleanup, records never in the catalogue, purges surviving a restart |
| `StoreFixture.ConcurrentConditionalIncrementsNeverLoseAnUpdate` | 8 threads, 320 `if_revision` increments, no lost update, distinct revisions |
| `StoreFixture.TheLogRestoresAcknowledgedWritesAfterARestartAndATornTail`, `RecordStoreNode.RecordsSurviveANodeRestartOutsideTheCatalogue` | restart, a torn final frame, values kept out of the catalogue file |
| `RecordStoreCompaction.DiskFollowsTheLiveSet` | dead segments deleted, live frames moved forward, both readable after a restart |
| `RecordStoreCluster.ConditionalWritesAreLinearizableAcrossTwoNodes` | two real nodes, three threads each, increments of one key: none lost, and every acknowledged write is read on the other node at once |
| `RecordStoreCluster.ANodeThatJoinsLaterCatchesUpByAntiEntropy` | a node joining after 600 writes, a deletion and a metric point pulls all of them, then nothing |
| `RecordStoreSoak.*` | the soak above |
| `RecordStoreLatency.ReadsDoNotWaitForTheCatalogueDocument` | 200 record reads complete while another thread holds the catalogue document's mutex (as `save()` and `merge()` do), inside an `update()` that waits. It counts reads instead of timing them, so a loaded host cannot fail it; before #1248 none completed until the holder's five-second deadline |
| `RecordCapabilities.*`, `ReplicatedRestServerFixture.RecordCapabilitiesWorkOverRestAndMcp` | the registry journey, 409 details, the `ON COLLECTION` grant, REST and MCP |

## Later

- A `system.record_collections` read view.
- Ownership on a spanning (not fully meshed) cluster, through the leadership lease designed in
  [replication.md](../cluster/replication.md).
- A cheaper wake for the signal plane's outbox, which bounds write latency.
- Anti-entropy lists a differing bucket's versions by walking the collection; a per-bucket key
  set would make that proportional to the bucket for very large collections.
