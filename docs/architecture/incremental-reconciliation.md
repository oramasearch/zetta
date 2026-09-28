# Incremental replication reconciliation

Issue [#1079](https://github.com/oramasearch/zetta/issues/1079) makes the common
changed tick proportional to the effective keys changed since the preceding
tick, rather than proportional to every key in the replicated document.

## Read path

`ReplicationManager` owns a parsed JSON cache alongside the Automerge document.
Local `set()` and `remove()` calls update that cache and append the affected key
to a bounded change journal. `merge()` compares effective encoded values after
the Automerge merge and is the only path that scans the complete document.

`changes_since(token)` returns an immutable shared snapshot, the deduplicated
changed keys, the next token, and a full-resync flag. A held snapshot is stable:
a concurrent writer uses copy-on-write, while a writer with no active reader
updates the cache in place. Tombstones stay in the encoded cache but are absent
from the shared live snapshot, preserving the removal-wins rule from #838.

## Tick routing

`Node::tick()` reads the CRDT version before it asks for changes. That ordering
preserves the #837 watermark rule: a merge that races the read can cause an
extra idempotent tick, but cannot be credited without being projected.

The changed keys are bucketed by the `catalog::k*Prefix` constants, then the
nonempty buckets run in the same parent-before-child order as a full reconcile:

| Prefixes | Reconciler |
|---|---|
| `db:` `tbl:` `row:` `lo:` `mfst:` | databases, tables, rows, large objects, manifests |
| `project:` then `input:` `graph:` | projects before their children |
| `deployment:` `run:` `model:` `profile:` `plan:` `index:` `job:` `part:` | top-level entity handlers |
| `principal:` then `key:` | principals before access keys |
| `user:` `role:` then `token:` `user_role:` `policy:` | credential parents before children |
| `audit:` | append-only audit materialization |
| `node:` | incremental membership update and partition rebalance |

Each bucket contains only its changed live values. Its changed-key list scopes
removals, so an absent tombstoned key removes only that projected entity. Table
and manifest changes also fetch their paired current value because applying a
manifest depends on the table parent being visible in the same pass.

`agent:` and `vtable:` keys have no materialized `Node` entity or reconcile
handler: their consumers read the replication manager directly. A changed tick
therefore advances their change token without creating a projection pass.

## Full reconcile fallbacks

A complete reconcile still runs:

- on startup;
- when the caller's token is absent or older than the bounded journal;
- when changed keys exceed one quarter of the current live keys.

Partition rebalancing has no other global dependency. On an incremental tick it
updates the prior membership set only from changed `node:` keys; on a full tick
it rebuilds membership from the snapshot.

## Persistence choice

The vendored Automerge implementation has no incremental-save API. Zetta keeps
that dependency unchanged and debounces the existing full save to at most once
per second. Startup writes the migrated document, and `stop()` always writes the
latest document, so a clean restart retains every accepted change.

This changes no entity JSON, gossip bytes, capability, audit meaning, or public
control surface. The only new C++ API is the in-process change-token read on
`ReplicationManager`.
