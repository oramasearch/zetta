# Replication

A node's **replicated state** — DDL, metadata, and rows — is backed by an
**Automerge CRDT** (`modules/automerge-cpp`, a from-scratch C++23 CRDT, no Rust).
Nodes exchange document blobs and merge them **conflict-free**: concurrent
changes on different nodes all survive, with no coordination or central leader.

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

It is thread-safe (the cluster's network thread and local callers both touch it).

## Status / next steps

- **Done + tested:** the CRDT core — `set`/`snapshot`/`save`/`merge`, distinct
  per-node actor ids, and conflict-free convergence of concurrent edits
  (`tests/test_replication.cpp`).
- **Next:** gossip `save()`/`merge()` over the [cluster](cluster.md)'s peer
  connections so state replicates automatically across running nodes; then hook
  the live `Node`'s DDL / metadata / row mutations into the replicated document
  (so real database operations replicate).
