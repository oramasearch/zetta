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

## Status / next steps

- **Done + tested:** the CRDT core — `set`/`snapshot`/`save`/`merge`, distinct
  per-node actor ids, conflict-free convergence (`tests/test_replication.cpp`);
  and **gossip over the cluster** — peers exchange documents and converge
  automatically, with per-node status in `nodes()` (`tests/test_cluster.cpp`).
- **Next (toward "everything is replicated"):**
  - Route the live `Node`'s DDL / metadata / **row** mutations into the document
    so real database operations replicate (today each node only *seeds* a schema
    snapshot at startup; ongoing mutations aren't yet streamed in).
  - Make cluster **membership** a projection of the replicated node map rather
    than a separately-maintained `Cluster` peer table.
  - In both cases the in-memory structures become **views derived from** the
    document — the document stays authoritative (see the Core rule above).
