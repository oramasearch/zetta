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

## Consistency model: multi-leader by default, leadership only where needed

Because the base is a CRDT, **zetta is multi-leader (multi-writer) by default**:
every node accepts writes, and merges converge conflict-free. There is **no
global leader and no global election** — the cluster is equal peers (see
[cluster.md](cluster.md)). This is the right default for the overwhelming
majority of data (DDL, rows, metadata): it just merges.

A leader is needed **only for structures that require exclusivity** — locks,
sequences, and other single-writer resources where "two holders at once" is
incorrect. Those are the exception, not the rule, and they get a scoped
mechanism rather than making the whole system single-leader:

- **A replicated `zetta_leadership` system table** (planned — not built yet)
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

So `ClusterRole` (today always `Follower`) is best understood as **per-resource
leadership**, not a node-wide title — a node can be the leader for resource A
and a follower for resource B at the same time.

> **Status:** leadership is **documented design only** for now. Nothing in this
> section is implemented yet; it's recorded so the build stays pointed the right
> way. The current work is the multi-leader data plane (below).

## Policies

`ReplicationManager` takes a `ReplicationPolicy`. The first (and currently only)
policy is **`ReplicateToAllNodes`** — every node keeps a full copy of all state.
Future policies (sharded, quorum, …) plug in behind the same enum/seam.

## Actor identity

Each node's **UUIDv4 is its Automerge actor id** (a UUID is 16 bytes — exactly
the size of an actor id; `uuid_to_bytes` does the conversion). Distinct actor
ids are what make concurrent edits merge conflict-free — two documents sharing
an actor id would collide on the same actor+sequence and silently drop changes.
The node's UUID is also its cluster node id: one identity per node.

## API (a facade over Automerge)

`zetta::ReplicationManager` (`include/zetta/replication.hpp`) exposes only JSON +
bytes; Automerge is hidden in the adapter `src/core/replication.cpp`, so the rest
of zetta stays decoupled from the backend.

- `set(key, json)` — a local change (any JSON value, stored as JSON text in the CRDT).
- `snapshot()` — the merged document as JSON.
- `save()` → bytes; `merge(bytes)` — exchange + merge with a peer, conflict-free.
- `entry_count()` — number of top-level keys (zetta seeds one per node).
- `version()` — a short fingerprint of the document's Automerge **heads** (the
  change-DAG leaves). Two nodes hold identical state exactly when their versions
  match — this is the "in sync" check. `"empty"` for an empty document.

It is thread-safe (the cluster's network thread and local callers both touch it).

## Gossip over the cluster

The [cluster](cluster.md) carries replication. When a node is built with a
`ReplicationManager`, each peer connection runs a gossip exchange on a ~½s timer
(adapter: `src/core/cluster.cpp`):

- `ZETTA/1 SYNC <base64 of save()>` — the whole document; the receiver `merge()`s
  it (conflict-free, so order and duplicates don't matter).
- `ZETTA/1 STATUS <entries> <version>` — the sender's status, recorded against
  that peer so `Cluster::nodes()` can report each node's entries, version, and
  whether it's **in sync** (its version equals ours) and how long ago it last
  synced.

Each node seeds its own entry — `node_id → { address, databases }` — so it writes
only its own key; the merge unions every node's entry into one shared map and the
documents converge to the same `version()`.

## Seeing it: all nodes + replication status

`Cluster::nodes()` returns this node plus every connected peer with its
replication status. It surfaces three ways:

- **Headed** — the Cluster panel's *Nodes & replication* table (id, address,
  role, entries, version, in-sync/syncing + age).
- **Headless** — the heartbeat logs a per-node table whenever it changes.
- **SQL** — `SELECT * FROM zetta_nodes;` (see [query.md](query.md)).

## Data replication: CRDT is the source of truth, disk follows (design)

This is the active build — making real database data (DDL **and** rows) replicate
so every node converges on the same data, in memory and **on disk**.

**The CRDT document is authoritative; the on-disk store + in-memory `Node` tree
are a materialized *projection* of it.** Concretely:

- **Document model — flat keys, structured values** (so concurrent edits to
  different things never conflict; each row owns its own key by UUID):
  - `node:<node_id>` → `{ address }` — membership (the node list / status view).
  - `db:<db_uuid>` → `{ name }` — a database.
  - `tbl:<tbl_uuid>` → `{ db: <db_uuid>, name }` — a table.
  - `row:<row_uuid>` → `{ tbl: <tbl_uuid>, seq, data: {…} }` — a row. The UUID key
    means two nodes inserting concurrently both survive (no collision); `seq`
    gives a stable-ish order. Deletes remove the key (`remove()`).
- **Capture (local write):** every mutation writes the CRDT **first** (the source
  of truth), then materializes to disk/memory. Capture lives at the data-model
  layer (`Node`/`Database`/`Table`), so *all* paths replicate — query executor,
  file import, and GUI buttons alike — not just one entry point. DB/table UUIDs
  are the CRDT keys *and* the on-disk folder names, identical on every node.
- **Gossip:** unchanged — peers exchange `save()`/`merge()` blobs and converge.
- **Apply (remote → local):** on the **main/Node thread** (the network thread only
  merges into the CRDT, which is thread-safe), `Node::tick()` notices the CRDT
  `version()` changed and **reconciles** the on-disk projection to match the
  document — creating missing databases/tables/rows (and dropping removed ones).
  Reconcile reads the CRDT and writes only disk, so it never loops back into the
  CRDT, and it is idempotent.
- **Persistence:** the CRDT document is saved to `data_dir/replication.automerge`
  (Automerge `save()`), and `merge()`d back on startup, so replicated state
  survives restarts. On first run with pre-existing on-disk data, that data is
  captured into the document (migrated) so nothing is lost.

## Status / next steps

- **Done + tested:** the CRDT core — `set`/`snapshot`/`save`/`merge`, distinct
  per-node actor ids, conflict-free convergence (`tests/test_replication.cpp`);
  and **gossip over the cluster** — peers exchange documents and converge
  automatically, with per-node status in `nodes()` (`tests/test_cluster.cpp`).
- **In progress:** the data-replication design above (DDL + rows → CRDT → every
  node's disk, persisted).
- **Documented, not built:** the leadership / `zetta_leadership` table and Raft
  (see *Consistency model* above) — design only for now.
