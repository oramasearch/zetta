# Replication

> **Core rule: EVERYTHING is replicated state.** Replication is the single source
> of truth for *all* distributed state in zetta — **schemas (databases + tables /
> DDL), rows, cluster membership (the node list), metadata, everything.** Any
> state more than one node must agree on goes through `ReplicationManager` (the
> Automerge CRDT). Never maintain a parallel copy of that state outside the
> replicated document and sync it by hand. In-memory structures (the `Node`'s
> database tree, the `Cluster`'s peer table) are **derived from / kept in lockstep
> with** the document, not authoritative on their own. New replicated state = a
> new key/shape in the document; new kinds of agreement = a new
> `ReplicationPolicy` — never a bespoke sync path.

A node's **replicated state** — DDL, metadata, rows, and node membership — is
backed by an **Automerge CRDT** (`modules/automerge-cpp`, a from-scratch C++23
CRDT, no Rust). Nodes exchange document blobs and merge them **conflict-free**:
concurrent changes on different nodes all survive, with no coordination or
central leader.

Audit history is the approved durable-log exception (#1164): per-origin
checksummed segment ranges replace `audit:` catalog entries. The peer hello
advertises an audit protocol version; incompatible peers still exchange ordinary
catalog state but refuse audit, and incoming legacy audit operations are removed
before merge. Migration compacts audit history without changing non-audit
operation identities or actor clocks. See [audit replication and upgrades](../governance/audit-trail.md).

## Consistency model: multi-leader by default, leadership only where needed

Because the base is a CRDT, **zetta is multi-leader (multi-writer) by default**:
every node accepts writes, and merges converge conflict-free. There is **no
global leader and no global election** — the cluster is equal peers (see
[cluster.md](cluster.md)). This is the right default for the overwhelming
majority of data (DDL, rows, metadata): it just merges.

**That promise is asserted with the transport removed.**
`Replication.AMeshOfThreeConvergesAfterConcurrentWrites`
(`tests/cluster/test_replication.cpp`) drives three `ReplicationManager`s through
300 rounds of concurrent local writes interleaved with one-way whole-document
merges in arbitrary directions — the shape the cluster's 500 ms `SYNC` produces,
including the partial views a mesh routinely delivers — then gossips to a fixed
point and requires identical versions *and* identical content. It is
deterministic (fixed seed), has no sockets, no threads and no clocks, so a
failure there is unambiguously a merge bug. That matters because the equivalent
property over real mTLS
([`ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad`](security.md))
does intermittently fail, and this test is what separates "the CRDT lost a
change" from "the link stopped carrying it" — currently the latter, see
[#835](https://github.com/oramasearch/zetta/issues/835).

A leader is needed **only for structures that require exclusivity** — locks,
sequences, and other single-writer resources where "two holders at once" is
incorrect. Those are the exception, not the rule, and they get a scoped
mechanism rather than making the whole system single-leader:

- **A replicated `system.leadership` table** (planned — not built yet)
  tracks, per exclusive resource, the current holder and a **lease with a
  timeout**. Leadership is acquired by *ascension*: if a resource is unheld or
  its lease has expired, a node claims it; concurrent claims are resolved
  deterministically (e.g. lowest node-id wins) and the loser backs off. The
  holder renews before expiry; if it dies, the lease lapses and another node
  ascends. This is **lease/advisory** exclusivity built on the CRDT — good
  enough for most locks.
- **Raft (future) for true exclusivity.** Where lease-based leadership isn't
  strong enough (no two holders *ever*, even under a partition), we can run
  Raft **scoped to just those exclusive structures** — not the whole database.
  The CRDT data plane stays multi-leader; only the few exclusive resources pay
  the consensus cost.

So leadership is **per-(node, resource)**, never a single node-wide title: a node
holds **zero to many** leadership roles at once — leader for resource A, follower
for B, uninvolved in C — all simultaneously. `ClusterRole` (today a single value,
always `Follower`) is therefore a placeholder; the real shape is a *set* of roles
the node currently holds, keyed by resource.

> **Status:** leadership is **documented design only** for now. Nothing in this
> section is implemented yet; it's recorded so the build stays pointed the right
> way. The current work is the multi-leader data plane (below).

## Policies

`ReplicationManager` takes a `ReplicationPolicy`. The first (and currently only)
policy is **`ReplicateToAllNodes`** — every node keeps a full copy of all state.
Future policies (sharded, quorum, …) extend the same policy enum.

### Per-table / per-database replication strategy (ENT-35)

Alongside the node-wide policy, each **database and table** carries a
**`zetta::data::ReplicationStrategy`** describing how *its* replicated state is laid out:
`ReplicateToAll` (the default — a full copy everywhere), `Partitioned` (sharded,
one copy per shard), or `PartitionedWithReplicas(replication_factor)` (sharded with
N total copies, default 3). The choice is **recorded, persisted, and replicated**
(an optional `"replication"` field on `database.json` / `table.json` and the
`db:` / `tbl:` CRDT values), and the default is **omitted** so the representation
stays byte-identical to before — the default preserves current behavior exactly.
This slice records the choice; the partitioned-placement engine that *acts* on a
non-default strategy is a later slice. See
[replication-strategy.md](../data/replication-strategy.md).

## Actor identity

Each node's **UUIDv4 is its Automerge actor id** (a UUID is 16 bytes — exactly
the size of an actor id; `uuid_to_bytes` does the conversion). Distinct actor
ids are what make concurrent edits merge conflict-free — two documents sharing
an actor id would collide on the same actor+sequence and silently drop changes.
The node's UUID is also its cluster node id: one identity per node.

It is **stable across restarts** — persisted in `data_dir/node_id` and reused
(`load_or_create_node_id` in `src/main.cpp`). A fresh id each run would fragment
the Automerge change history across many actors and leave a stale `node:`
membership key behind every restart, growing the document for no reason. (Copying
a node's `data_dir` to another machine would duplicate the id — delete `node_id`
there to re-issue one.)

## API (a facade over Automerge)

`zetta::cluster::ReplicationManager` (`include/zetta/cluster/replication.hpp`) exposes only JSON +
bytes; Automerge is hidden in the adapter `src/core/cluster/replication.cpp`, so the rest
of zetta stays decoupled from the backend.

- `set(key, json)` — a local change (any JSON value, stored as JSON text in the CRDT).
- `remove(key)` — a local removal. Writes a **tombstone**, see below.
- `snapshot()` — a compatibility copy of the merged document as JSON, **live
  entries only**.
- `shared_snapshot()` — a shared immutable view of the same parsed cache. A
  writer updates the cache in place when no reader holds it and uses
  copy-on-write otherwise.
- `changes_since(token)` — the shared snapshot, the effective keys changed
  since the caller's token, the next token, and whether the caller must perform
  a full resync.
- `knows_key(key)` — whether the document holds the key at all, *including* as a
  tombstone. "Was this ever replicated?", as opposed to `snapshot()`'s "is it live
  now?".
- `save()` → bytes; `merge(bytes)` — exchange + merge with a peer, conflict-free.
- `entry_count()` — number of **live** top-level keys (zetta seeds one per node).
- `version()` — a short fingerprint of the document's Automerge **heads** (the
  change-DAG leaves). Two nodes hold identical state exactly when their versions
  match — this is the "in sync" check. `"empty"` for an empty document.

It is thread-safe (the cluster's network thread and local callers both touch it).

## Parsed cache and incremental reads (#1079)

`ReplicationManager` keeps each key's encoded value and parsed JSON current at
the point of mutation. `set()` replaces the cached live value, while `remove()`
keeps the encoded tombstone and removes the key from the live snapshot. The
cache therefore has the same removal-wins behavior as the document: a
tombstoned key remains known to `knows_key()` but is absent from snapshots.

Each effective value change also advances a monotonic token and appends its key
to a bounded 65,536-record journal. `changes_since(token)` deduplicates the
records after that token. Token zero, a future token, or a token older than the
journal produces `full_resync = true`; the returned snapshot is still the
current immutable view. This bounds bookkeeping while making the common read
cost proportional to the changes since the last tick.

A peer merge is the exception: Automerge does not expose the affected top-level
keys cheaply. After `Document::merge`, the manager makes one pass over the
merged document, compares each effective encoded value with the cache, updates
only differences, and records those keys. Concurrent tombstones participate in
that comparison, so a merge that changes a live key to removal is reported even
when Automerge's selected scalar is unchanged.

This adds no JSON or wire format. Gossip and the persisted Automerge bytes keep
their existing representation.

## Removal replicates as a tombstone

**`remove(key)` writes a tombstone value; it does not delete the key from the CRDT
map.** The difference is load-bearing, and getting it wrong cost a real bug
([#838](https://github.com/oramasearch/zetta/issues/838)).

A CRDT map delete carries the set of predecessor writes the deleting node could see
*at that moment*. Any write it had not yet merged is not named, so that write
survives the delete verbatim — and the key comes back on every node the moment the
write arrives, **including on the node that removed it**. That was observed
directly: a key absent from a node's own snapshot immediately after the removal,
present again milliseconds later, before any `tick()`.

It is not a corner case. A coordinator cleaning up after a failed distributed
ingest removes `part:` and `lo:` keys while participants are still writing their
own, so the removal is *necessarily* concurrent with writes it cannot see. The same
exposure applied to `drop_table`, `drop_job`, `drop_index`, `drop_database`,
`drop_project` — every path that removed a key and relied on absence.

The rules now:

- A removal is an ordinary **value change** (`{"_removed": true}`), so it merges
  like every other value change. Nothing about it depends on what the remover had
  seen.
- **Removal wins over a concurrent write.** `snapshot()` inspects *all* concurrent
  values at a key (`Document::get_all`), not just the one Automerge picks as the
  winner, and treats the key as absent if any of them is a tombstone. Asking only
  the winner would be deterministic but arbitrary — and a half-removed entity is
  exactly what #838 produced.
- **A write causally after the removal re-creates the key**, normally. The writer
  had already seen the tombstone, so this is a deliberate re-create; without it a
  tombstone would make a name unusable forever.
- `snapshot()` omits tombstoned keys, so a removal still looks like plain absence
  to all 20-odd reconcile passes — they needed no change.
- **`knows_key()` is how a caller sees through the tombstone.**
  `Node::migrate_to_replication()` must: it re-captures local projections the
  document does not hold, and a removed key is absent from the snapshot by
  construction, so asking the snapshot alone would read a deliberate drop as
  local-only data predating replication and put it straight back on the next start.

`row:` keeps its own tombstone shape (`{tbl, deleted:true}`) and does **not** use
`remove()`. A row tombstone is an entry reconcile has to *see* — it routes by the
`tbl` field to find the table whose projection to update — whereas `_removed` means
"treat this key as absent" and is filtered out. Distinct markers, so neither
mechanism swallows the other.

The cost is that a removed key stays in the document as a tombstone forever. That
is the standard CRDT trade, it is already what `row:` deletes do, and it is small
next to the change history the document carries anyway. Compaction is a separate
question ([Status](#status--next-steps)).

Covered by `tests/cluster/test_drop_replication.cpp`: a drop reaching a peer and
staying gone through further gossip; a removal winning over a write it never saw; a
causally-later write re-creating the key; and a restart not re-capturing a dropped
entity. The middle two and the last fail without the tombstone.

## Gossip over the cluster

The [cluster](cluster.md) carries replication. When a node is built with a
`ReplicationManager`, each peer connection runs a gossip exchange on a ~½s timer
(adapter: `src/core/cluster/cluster.cpp`):

- `ZETTA/1 SYNC <base64 of save()>` — the whole document; the receiver `merge()`s
  it (conflict-free, so order and duplicates don't matter).
- `ZETTA/1 STATUS <entries> <version>` — the sender's status, recorded against
  that peer so `Cluster::nodes()` can report each node's entries, version, and
  whether it's **in sync** (its version equals ours) and how long ago it last
  synced.
- `ZETTA/1 LOREQ <lo-uuid>` / `ZETTA/1 LORES <lo-uuid> <base64-body>` —
  large-object **body** transfer (the metadata rode in via SYNC). A node requests
  the bodies it's missing (`BlobStore::missing_blobs`) and serves the ones it has
  (`read_blob`); the receiver verifies size + SHA-256 (`store_blob`) before
  writing. Bodies never enter the document.

Each node seeds its own membership entry — `node:<node_id> → { address, reachability, node_type }` — under
its own key, so the merge unions every node's entry conflict-free. The database
catalog (DDL + rows) lives under separate keys (see *Data replication* below);
once two nodes have exchanged everything, their documents share a `version()`.

## Seeing it: all nodes + replication status

`Cluster::nodes()` returns this node plus every connected peer with its
replication status. It surfaces three ways:

- **Headed** — the Cluster panel's *Nodes & replication* table (id, address,
  role, entries, version, in-sync/syncing + age).
- **Headless** — the heartbeat logs a per-node table whenever it changes.
- **SQL** — `SELECT * FROM system.nodes;` (see [query.md](../data/query.md)).

`Cluster::nodes()` is the DIRECT-peer view (this node + its live links) — the source of
the replication-status tier above. The FULL membership — every node the replicated
node-list knows, including ones this node has no direct link to — is surfaced by the
unified **`system.nodes`** virtual table (#809): a query-time composition of the durable
`node:<id>` node-list entries (converging on all N members from any node, so no "5 not 9"
undercount) overlaid with this node's live liveness + replication status. See
[../data/query.md](../data/query.md).

## Data replication: CRDT is the source of truth, disk follows

Real database data (DDL **and** rows) replicates: every node converges on the
same data, in memory and **on disk**. (Done + tested — see
`tests/cluster/test_data_replication.cpp`.)

**The CRDT document is authoritative; the on-disk store + in-memory `Node` tree
are a materialized *projection* of it.** Concretely:

- **Document model — flat keys, structured values** (so concurrent edits to
  different things never conflict; each row owns its own key by UUID):
  - `node:<node_id>` → `{ address, reachability, node_type }` — membership (the node-list
    entry; the durable tier the unified `system.nodes` view reads). See [../data/query.md](../data/query.md).
  - `db:<db_uuid>` → `{ name }` — a database.
  - `tbl:<tbl_uuid>` → `{ db: <db_uuid>, name }` — a table.
  - `row:<row_uuid>` → `{ tbl: <tbl_uuid>, seq, data: {…} }` — a row. The UUID key
    means two nodes inserting concurrently both survive (no collision); `seq`
    gives a stable-ish order. A row delete writes its own tombstone value
    (`{tbl, deleted:true}`) rather than calling `remove()` — reconcile has to
    *see* it, because it routes by the `tbl` field to find the table whose
    projection to update. See *Removal replicates as a tombstone* below.
  - `lo:<lo_uuid>` → `{ db, name, size, sha256 }` — large-object **metadata**
    only. The body is **not** in the document (it would bloat every node); it
    transfers peer-to-peer and is verified against `size` + `sha256`. See
    [large-objects.md](../data/large-objects.md).
  - `project:<project_uuid>` → `{ name, created, updated, owner }` — an AI
    project (a top-level entity with no parent). See [projects.md](../pipeline/projects.md).
  - `graph:<graph_uuid>` → `{ project: <project_uuid>, name, graph: { nodes,
    edges } }` — an `ExecutionGraph` IR owned by a project. The whole serialized
    graph rides as one value (a graph is authored + stored as a unit, unlike a
    row); the on-disk projection is `<project-uuid>/graphs/<graph-uuid>.json`. See
    [projects.md](../pipeline/projects.md) and [execution-graph.md](../pipeline/execution-graph.md).
  - `deployment:<deployment_uuid>` → `{ graph_name, target: {…}, node,
    node_count, edge_count, created }` — a graph placed onto a target (a
    top-level entity with no parent). See [deployments.md](../pipeline/deployments.md).
  - `run:<run_uuid>` → `{ project, graph, status, created, updated, logs,
    outputs, lineage }` — one execution of a deployed graph (a top-level entity).
    The value is **validated against a JSON Schema** before it is trusted, at
    both entry points (disk + a peer's CRDT value). See [runs.md](../pipeline/runs.md).
  - `ledger:<entry_uuid>` → one immutable action or outcome record. Actions
    preserve score, decision, actor, model/policy/agent versions, experiment
    cell, cost, and evidence timestamps; outcomes reference their action. Each
    value is schema-validated before materialization under
    `<data_dir>/ledger/`. See [audit-ledger.md](../solutions/audit-ledger.md).
  - `model:<model_uuid>` → `{ name, kind, project, run, feature_schema, metrics,
    created, weights: { size, sha256 } }` — a trained model (a top-level entity).
    Like a large object, only the **metadata** rides in the document; the
    serialized **weights body** stays on disk and transfers peer-to-peer over the
    LO body transfer path, verified against `weights.size` + `weights.sha256`. The value is
    **JSON-Schema validated** at both entry points. See [models.md](../ml/models.md).
  - `experiment:<experiment_uuid>` → `{ name, seed, allocations, created }` and
    `experiment_assignment:<experiment_uuid>:<entity_digest>` → the entity's
    allocation, tier, propensity, hash bucket, exception fields, and assignment
    time. Each assignment owns a key, so different peers retain concurrent
    assignments. Both values are JSON-Schema validated. See
    [experiments.md](../solutions/experiments.md).
  - `part:<part_uuid>` → `{ table, blob_ref, row_range: { start, end }, stats,
    owner_nodes, created }` — a dataset partition: where a slice of a table's data
    lives (a top-level entity). The catalog is **replicate-to-all**, so every node
    materializes the same partition set and placement is globally visible. The
    value is **JSON-Schema validated** at both entry points. See
    [dataset-partitions.md](../data/dataset-partitions.md).
- **Capture (local write):** every mutation writes the CRDT **first** (the source
  of truth), then materializes to disk/memory. Capture lives at the data-model
  layer (`Node`/`Database`/`Table`), so *all* paths replicate — query executor,
  file import, and GUI buttons alike — not just one entry point. DB/table UUIDs
  are the CRDT keys *and* the on-disk folder names, identical on every node.
- **Gossip:** unchanged — peers exchange `save()`/`merge()` blobs and converge.
- **Apply (remote → local):** on the **main/Node thread** (the network thread only
  merges into the CRDT, which is thread-safe), `Node::tick()` notices the CRDT
  `version()` changed and **reconciles** changed prefix buckets into the on-disk
  projection — creating missing databases/tables/rows and dropping removed ones.
  Startup, journal gaps, and batches larger than one quarter of live keys use the
  complete snapshot. Reconcile reads the CRDT and writes only disk, so it never
  loops back into the CRDT, and it is idempotent.

  Solution monitors, experiments/assignments, and ledger additions participate
  in these changed-key buckets too. Definition changes revisit dependent
  assignment records; the policy/decision and ROI stores remain direct CRDT
  reads. See [solution-reconciliation.md](solution-reconciliation.md) (#1128).

  **The watermark is the version read at the TOP of the tick, never the current
  one.** Peers merge on the network thread, so a merge can land at any moment
  during a tick. `tick()` used to read `version()` again at the end and record
  *that* as reflected — claiming credit for a merge the reconcile had never seen.
  The projection then skipped it and, with no later write to move the version
  again, **never revisited it**: the on-disk/in-memory view stalled permanently
  short of the document. That is silent data loss at the projection layer, and it
  is what made a peer see 8 of 12 partitions forever
  ([#837](https://github.com/oramasearch/zetta/issues/837)). Recording the
  earlier version is the conservative direction — at worst the next tick
  reconciles a snapshot it has already applied, which is idempotent.
- **Persistence:** the CRDT document is saved to `data_dir/replication.automerge`
  (Automerge `save()`), and `merge()`d back on startup, so replicated state
  survives restarts. A dirty document is saved at most once per second and is
  always saved by `stop()`. On first run with pre-existing on-disk data, that
  data is captured into the document (migrated) so nothing is lost.

## Status / next steps

- **Done + tested:** the CRDT core (`tests/cluster/test_replication.cpp`); **gossip over
  the cluster** with per-node status in `nodes()` (`tests/cluster/test_cluster.cpp`); and
  **data replication** — DDL + rows captured into the document, reconciled onto
  every node's disk, and persisted across restart (`tests/cluster/test_data_replication.cpp`).
- **Documented, not built:** the leadership / `leadership` table and Raft
  (see *Consistency model* above) — design only for now.
- **Known limits (v1):** persistence still serializes the whole document, but
  no more than once per second plus `stop()`; peer merge still makes one full
  comparison pass because Automerge exposes no changed top-level keys.
  **The document keeps full change history** (a CRDT keeps it so merges stay
  conflict-free), so `save()` grows
  with every change — it does *not* grow while idle, but a write-heavy or
  long-lived document will accumulate. **History compaction** (dropping changes
  all peers already have) is the future mitigation; for now, deleting
  `replication.automerge` and restarting rebuilds a clean, minimal document from
  the on-disk data (the `migrate` step re-captures it). Row ordering across nodes
  is by arrival, not a global sequence. Dropping a table/database leaves its
  child row/table keys as harmless orphans in the document (reconcile ignores
  them; the preflight check counts them — see [storage.md](../data/storage.md)).
