# Replication strategy (per-table / per-database)

> **Status (this slice, ENT-35):** zetta **records, persists, and replicates** a
> per-table / per-database replication strategy. The default — `ReplicateToAll` —
> preserves today's behavior exactly. The **partitioned-placement engine** that
> *acts* on a non-default strategy (storing only a subset of shards/replicas on
> each node instead of a full copy everywhere) is being built incrementally: the
> first piece, **sharded blob placement** ([#515](https://github.com/oramasearch/zetta/issues/515)),
> already holds a partition's large-object **body** only on its owner node(s) per the
> [partition catalog](dataset-partitions.md) + replication factor (see
> [sharded-blob-placement.md](sharded-blob-placement.md)); the
> [partitioned write path](partitioned-write-path.md) ([#511](https://github.com/oramasearch/zetta/issues/511))
> and the [between-jobs rebalance](partition-rebalance.md)
> ([#517](https://github.com/oramasearch/zetta/issues/517)) build on it. This is the
> "record the choice now, drive it later" shape `DeploymentTarget` uses for its
> placement dial (R1 Local only) and `ScalingPolicy` uses for the reward model before
> the autoscaler that consumes it.

A `zetta::ReplicationStrategy` (`include/zetta/replication_strategy.hpp`) is the
per-entity counterpart to `ReplicationManager`'s **node-wide** `ReplicationPolicy`
(`include/zetta/replication.hpp`): the manager's policy is one knob for the whole
replicated document; the strategy is a choice **carried by each database and
table** describing *how that entity's replicated state is laid out across the
cluster*.

## The three strategies

| Kind | Stored name | Meaning |
| --- | --- | --- |
| `ReplicateToAll` | `replicate_to_all` | Every node holds a full copy (the **default** — identical to `ReplicationPolicy::ReplicateToAllNodes`, today's behavior). |
| `Partitioned` | `partitioned` | Sharded across nodes, a **single copy** per shard (no replicas). |
| `PartitionedWithReplicas` | `partitioned_with_replicas` | Sharded, with `replication_factor` **total copies** of each shard (defaults to **3**). |

`replication_factor` is the total number of nodes each shard is stored on (a
positive integer); it is only meaningful for — and only stored on —
`PartitionedWithReplicas`.

## Using it

The strategy is set at **create time** via the C++ API. With no explicit
strategy, a database defaults to `ReplicateToAll`, and a **table inherits its
database's strategy** unless it overrides:

```cpp
#include "zetta/replication_strategy.hpp"

// A database whose tables are partitioned with 3 replicas by default.
auto db = node.create_database(
    "events", zetta::ReplicationStrategy::partitioned_with_replicas());  // factor 3

auto a = db->create_table("a");  // inherits partitioned_with_replicas(3)
auto b = db->create_table(
    "b", zetta::ReplicationStrategy::replicate_to_all());  // overrides per table

db->replication_strategy();      // -> PartitionedWithReplicas, factor 3
a->replication_strategy();       // -> PartitionedWithReplicas, factor 3 (inherited)
b->replication_strategy();       // -> ReplicateToAll (overridden)
```

Factories: `replicate_to_all()`, `partitioned()`,
`partitioned_with_replicas(int = 3)` (a factor `< 1` is clamped to a single copy).

> **Scope note:** the strategy is fixed at create time in this slice. There is no
> "change a table's strategy" mutator yet (mirroring how a peer **rename**
> updates only the name on reconcile), and no SQL DDL surface
> (`CREATE TABLE ... WITH (...)`) — both are natural follow-ups.

## Persistence + replication

The choice rides everywhere the database/table catalog already does (see
[replication.md](../cluster/replication.md)):

- **On disk** — an optional `"replication"` field in `database.json` /
  `table.json` (the `ReplicationStrategy::to_json` shape).
- **In the CRDT** — the same `"replication"` field inside the existing
  `db:<uuid>` / `tbl:<uuid>` values; it gossips to peers and reconciles onto every
  node's disk like any other catalog state. No new key, no new wire format.

**The default is omitted.** A `ReplicateToAll` strategy writes no `"replication"`
field at all, so the on-disk files and CRDT values are **byte-identical** to
before databases/tables carried a strategy — which is exactly why the default
preserves current behavior, and why upgrading an existing data dir is a no-op.

## Boundary validation

The strategy is a **validated JSON boundary**, the hand-written non-throwing
pattern of `NodeCapability` / `ScalingPolicy` (not the `pboettch` path — that is
for top-level replicated *records*), mirroring
[`schemas/replication_strategy.schema.json`](../../schemas/replication_strategy.schema.json):

- `ReplicationStrategy::from_json` is **non-throwing** and returns `std::nullopt`
  on anything malformed — not an object, a missing/unknown `kind`, or a
  `replication_factor` that is not a positive integer. A
  `partitioned_with_replicas` value with no `replication_factor` defaults to 3.
- `replication_strategy_from_meta(value)` reads the optional `"replication"` field
  out of a `database.json` / `table.json` / CRDT value at **load/reconcile** time:
  absent → the default; **present-but-malformed → also the default** (full
  replication), because the strategy is advisory layout metadata and a clearly
  invalid value must fall back to safe full replication rather than strand the
  table/database on load. The strict, failing parse (`from_json`) is what guards
  any future API/DDL entry point where a user supplies a strategy.

## Where it lives

- `include/zetta/replication_strategy.hpp` / `src/core/replication_strategy.cpp`
  — the value type, kind names, JSON boundary, and the `replication_strategy_from_meta`
  reader.
- `include/zetta/database.hpp` / `include/zetta/table.hpp` — each carries a
  `replication_strategy()` accessor; `Database::create_table` takes an optional
  override (inherits the database's otherwise).
- `include/zetta/node.hpp` — `Node::create_database(name, strategy = {})`.
- `src/core/node.cpp` — capture into the CRDT (`create_database` /
  `migrate_to_replication`), decode from the CRDT and disk
  (`reconcile_databases` / `load_databases`).
- `schemas/replication_strategy.schema.json` — the mirrored schema.

See [replication.md](../cluster/replication.md) for the catalog/CRDT model this rides on.
