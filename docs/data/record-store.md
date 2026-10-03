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
| Durable | The node that orders a write appends it to `records.journal` and fsyncs before it acknowledges. Startup replays the journal into the document. |
| Replicated | Before a write is acknowledged, every live peer has installed it. Peers that were down catch up by gossip, like every other key. |
| Linearizable per key | Every write of a key is ordered by one node, the key's **owner**, with an atomic read-check-write. `if_absent` and `if_revision` are decided there. |
| Read after write | A read on any live node after the acknowledgement returns the write or a newer one. |
| Reads | `get_record` is one ordered-map lookup; `list_records` walks one key range. Neither copies the catalogue (#1227). |
| Retention | A collection may declare `retention_ms`: a record expires that long after its last write. An expired record is invisible at once, and the owner removes it within ten seconds. |

What it does not promise, stated so a caller can plan for it:

- **During a membership change**, two nodes can briefly disagree about a key's owner. A forwarded
  write to a node that no longer considers itself the owner is refused with `busy` ("ownership of
  this key is moving; retry"). In the window before the views converge, two writes of one key on
  different owners can both succeed. The register then keeps the higher revision on every node, so
  the replicas agree, but one write is lost. A clean full mesh, which the operator's StatefulSet
  is, never enters that window.
- **The ownership view is the direct links.** A node that is not directly linked to every other
  durable node (a spanning cluster joined through a relay) sees a different owner set. Run the
  record store on a fully meshed set of permanent nodes.
- **A read concurrent with a write** may return the old value on one node and the new value on
  another until the write is acknowledged.
- **An acknowledged write whose fan-out timed out on a peer** reports it: `replicas` is less than
  `replicas_expected`. The write is still durable on its owner and reaches the peer by gossip.

## How it works

Every record, index entry and collection definition is a key of the replicated Automerge document
([replication.md](../cluster/replication.md)). Nothing is kept beside it.

| Key | Value |
|---|---|
| `record_collection:<name>` | `{name, indexes, retention_ms?, created}` |
| `record:<collection>:<key>` | `{rev, updated, value, index, order, expires?}`, or a deletion's `{rev, updated, deleted: true}` |
| `record_index:<collection>:<index>:<hex value>:<hex order>.<hex key>.<rev>` | `{rev}` |

**The owner.** A rendezvous hash of `(collection, key)` over this node and its live peers picks
the owner. Ephemeral nodes order writes only when no permanent node is reachable. A write that
arrives elsewhere is forwarded to the owner over the signal plane (`record.put`).

**The conditional write.** The owner runs `ReplicationManager::update`: under the replication
mutex it reads the record, checks `if_absent` / `if_revision`, and writes the record, its new
index entries and the removal of its old ones as one Automerge change. The new revision is
`max(previous + 1, now in microseconds)`, so revisions keep rising across a deletion and a purge
and a re-created key never reuses one a caller still holds.

**The fan-out.** The committed record goes to every live peer at once (`record.apply`, through
`Cluster::request_each`), and each peer installs it with the same `update`, only when its
revision is higher than what that peer holds. Every peer therefore writes its own copy of the
record, so a record key can hold several concurrent values after gossip. Record keys are
**revision registers**: the replicated document resolves concurrent values at a `record:` key to
the highest `rev` instead of Automerge's arbitrary winner, so every node agrees.

**Index entries** carry the record's revision in their key, so an entry is written once and only
ever removed. A list resolves each entry to its record and keeps it only when the revisions match
and the record is visible. A stale entry, left by a peer that missed a fan-out, is skipped and
then removed by the owner's sweep after the grace period.

**The journal.** The owner appends the committed record (a `record_signal` line, the same shape as
`record.apply`) and fsyncs before it acknowledges. `Node::persist_replication` takes the journal's
size before saving the document, fsyncs the saved document, and only then drops the journal up
to that size. A crash between the two leaves the record in the journal, never in neither. Replay
installs each line through the same revision check, so replaying twice is harmless.

**Retention and cleanup.** `Node::tick` sweeps every ten seconds. For the keys it owns, a node
removes expired records with their index entries, deletion tombstones older than ten minutes
(`kTombstoneGraceMs`, longer than a write can be in flight), and stale index entries of the same
age. Only the owner removes a key, so a removal never races a concurrent write of the same key
on another node.

Record keys have no on-disk projection. `Node::tick` drops them from its reconcile work, so a
burst of record writes never pushes a tick into a full reconcile.

## API

Every operation is one registry capability: REST route, MCP tool and OpenAPI operation. Writes
and reads run on the blocking pool after the owner-loop authorization gate (`blocks`).

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
log: 500 conversations of 100 messages, a 1 KiB body each, so 50,000 records and 50,000 index
entries. Measured on an Apple M3 Max:

| Read | Store | Registry call (validation, authorization, audit, handler) |
|---|---|---|
| `get_record`, median / p99 | 0.001 ms / 0.001 ms | 0.044 ms / 0.068 ms |
| `list_records`, 50 records, median / p99 | 0.066 ms / 0.093 ms | 0.190 ms / 0.442 ms |
| `list_records` 100 + `get_record`, worst of 4 concurrent readers | 0.144 ms | |

Both tests fail above 10 ms. A write costs one signal-plane round trip to each live peer (the
outbox flushes every 20 ms) plus one fsync on the owner.

## Tests

| Test | Covers |
|---|---|
| `StoreFixture.ConditionalWritesFollowTheLiveRevision` | `if_absent`, `if_revision`, one-use consumption, conditional delete, revisions rising across a re-create |
| `StoreFixture.RefusesBadInputAndUnknownCollectionsAndIndexes` | the validation boundary; idempotent and conflicting collection definitions |
| `StoreFixture.ListsOneIndexValueInOrderAPageAtATime` | order then key, both directions, cursors, a record moving between index values, the key listing |
| `StoreFixture.RetentionHidesThenSweepRemovesExpiredRecords`, `…DeletionTombstonesArePurgedAfterTheGracePeriod` | retention and cleanup |
| `StoreFixture.ConcurrentConditionalIncrementsNeverLoseAnUpdate` | 8 threads, 320 `if_revision` increments, no lost update, distinct revisions |
| `StoreFixture.TheJournalRestoresAcknowledgedWritesAfterACrash`, `RecordStoreNode.PersistingTheDocumentEmptiesTheJournalAndSurvivesRestart` | durability before the document is saved; trimming after it is |
| `RecordRevisionRegister.*` | the highest revision wins whatever the CRDT winner; `update` is atomic against concurrent writers |
| `RecordStoreCluster.ConditionalWritesAreLinearizableAcrossTwoNodes` | two real nodes, three threads each, increments of one key: none lost, and every acknowledged write is read on the other node at once |
| `RecordCapabilities.*`, `ReplicatedRestServerFixture.RecordCapabilitiesWorkOverRestAndMcp` | the registry journey, 409 details, the `ON COLLECTION` grant, REST and MCP |

## Later

- A `system.record_collections` read view.
- Ownership on a spanning (not fully meshed) cluster, through the leadership lease designed in
  [replication.md](../cluster/replication.md).
- A cheaper wake for the signal plane's outbox, which bounds write latency.
