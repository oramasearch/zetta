# Promote a cached result set to durable

A [connector result set](connector-query.md) lands at **RF=1**: it is a cache,
always re-pullable from its source, and paying for three copies of it is paying
three times for something the source already holds. Everything else defaults to
**3** ([add-dataset.md](add-dataset.md)). `promote_dataset` is the deliberate act
that moves a dataset from the first world to the second — what makes "promote
this cache to permanent" a thing you can do rather than a thing you can describe.

```
promote_dataset { dataset: "accounts", database: "churn", replication_factor: 3 }
  -> { database, table, table_id, replication_factor, partitions, promoted,
       promotions: [ { partition_id, partition, previous_owner_nodes, owner_nodes } ] }
```

Engine entry point: `zetta::data::promote_dataset`
(`include/zetta/data/promote_dataset.hpp`). Capability: an Orchestration
operation under `pipelines:manage`, registered in
`src/core/api/capabilities.cpp`, so it is an MCP tool, `POST /v1/promote_dataset`,
and an OpenAPI operation like every other capability.

## The copy is the catalog write

There is no copy loop here. Ownership lives in the replicate-to-all
[partition catalog](dataset-partitions.md), and
[`ReplicatedBlobStore`](sharded-blob-placement.md) reads `owner_nodes` to decide
what a node fetches and what it refuses. So **adding an owner to a `part:` entry
is the whole operation**: `DatasetPartition::set_owner_nodes` captures the change,
it gossips to every node, and the new owner then lists the body in
`missing_blobs()` and pulls it over the existing large-object transport.

That is exactly the mechanism [partition-rebalance.md](partition-rebalance.md)
uses when membership changes — no new transport, no new verb.

## Promotion never removes an owner

The desired owner set for a partition is:

1. its **current owners** (they already hold the body), then
2. the **rendezvous order** for that partition id, until the target factor is
   reached.

A partition that already has at least the requested number of owners is left
untouched, so asking for a *lower* factor changes nothing. A promotion that
quietly dropped the only copy of a partition would be the exact opposite of what
it is for, and this shape makes that impossible rather than unlikely.

## The factor is clamped, and says so

The applied factor is the request clamped to the **live roster**
(`Node::cluster_members()`, falling back to this node alone). A single-node
cluster reports `replication_factor: 1` and `promoted: 0` rather than claiming
three copies that do not exist. The result is the honest record of what happened,
which is what an operator deciding whether the dataset is now safe actually
needs.

## Errors

Non-throwing; each returns a reason:

- an unknown database or table;
- a dataset with **no placed partitions** (nothing to replicate — a
  replicate-to-all table has no catalog entries);
- a factor of zero;
- through the capability, an ambiguous dataset name across several databases
  (pass `database`), the same rule `get_dataset` uses.

## Tests

`tests/data/test_promote_dataset.cpp` — a cache at RF=1 raised to 3 with every
partition keeping the owner it already had; a lower request changing nothing and
a huge one clamping to the roster; the error cases; and a two-node run where the
peer becomes an owner and **the body follows it** over the large-object
transport. `tests/api/test_registry.cpp` covers the capability surface, including
the honest clamp on a one-node cluster.

Related: [connector-query.md](connector-query.md),
[add-dataset.md](add-dataset.md), [partition-rebalance.md](partition-rebalance.md),
[scatter-gather-select.md](scatter-gather-select.md) (why a lost owner matters).
Issue: #833 section 9.
