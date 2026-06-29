# Partitioner seam

The **`Partitioner`** seam (`include/zetta/partitioner.hpp`) is the pluggable
policy that decides **data placement** for a partitioned table. It realizes
**ADR-31** and entity **ENT-37** (`#512`). This slice is the **interface only** —
the seam plus a trivial baseline implementation and per-table configuration. The
concrete strategies and the persistence/replication that record where partitions
actually land are separate, dependent slices:

- **record → partition** — `HashPartitioner` (hash of a key column) and
  `RoundRobinPartitioner` (even balance) — `#513`.
- **partition → node(s)** — `RendezvousPlacement` (consistent / rendezvous
  hashing, so a join/leave moves only ~1/N of the partitions) — `#514`.
- **the partition catalog** — `system.dataset_partitions`, the replicated record
  of where each partition landed — `#510`.
- **the partitioned write path** — rows → partition → blob, placement recorded —
  `#511`.
- **rebalance on membership change** — `#517`; **compute-near-data** (a DDF op
  runs over the node's local partitions) — `#516`.

A **learned model-as-partitioner** (a trained model performs the
record→partition / partition→node assignment) is a documented future direction
behind this same seam.

## Why a fixed partition count

A `partitioned` table is split into a **fixed number of partitions `P`**, chosen
`P >> nodes`. A partition — not a row — is the **unit of placement and
rebalancing**: when membership changes, whole partitions (as blobs) move between
nodes, rather than every row being rehashed. Holding `P` fixed for the life of a
partitioner is exactly what makes that possible — the record→partition mapping is
stable, so only the partition→node mapping shifts, and a good placement function
(rendezvous / consistent hashing) moves only ~1/N of the partitions on a
join/leave.

This mirrors the seam pattern zetta uses everywhere: like `StorageEngine` answers
"how are rows stored" and `ComputeDevice` answers "where does this op run", the
`Partitioner` answers "where does this data live".

## The seam

```cpp
// A partition index in [0, partition_count).
using PartitionId = std::uint32_t;

class Partitioner {
public:
    virtual ~Partitioner() = default;

    // Short name for logs / UI (e.g. "single", "hash").
    virtual const char* name() const = 0;

    // The fixed number of partitions P (chosen P >> nodes).
    virtual PartitionId partition_count() const = 0;

    // record → partition, always in [0, partition_count()).
    virtual PartitionId assign_partition(const nlohmann::json& record) const = 0;

    // partition → node(s): the owner node id(s), given the current members.
    // The first entry is the primary owner; further entries are replicas.
    virtual std::vector<std::string> place_partition(
        PartitionId partition,
        const std::vector<std::string>& members) const = 0;
};
```

It has two responsibilities, the two halves of "data placement":

1. **`assign_partition(record) -> partition`** — which of the `P` partitions a
   row belongs to. A `record` is a row document (`nlohmann::json`, the `doc` a
   `StorageEngine` stores). The result is always in `[0, partition_count())`.
2. **`place_partition(partition, members) -> owner_node(s)`** — which cluster
   node(s) own a partition, given the current membership. Each `member` and each
   returned owner is a **node id** — the stable cluster / CRDT actor UUID, e.g.
   from `Cluster::nodes()`. The first owner is the primary; any further entries
   are replicas. An empty `members` places nowhere (empty result).

Both methods are **pure**: deterministic functions of their inputs with no side
effects, and **non-throwing** (zetta-core is built `-fexperimental-library`; the
seam never unwinds). Determinism is what lets a leaderless cluster agree on
placement — every node computes the same `place_partition` for a given
`(partition, members)` with no election, the same way the
[collectives](collectives.md) pick a root by lexicographically-smallest id.

## `SinglePartitioner` — the trivial baseline

`SinglePartitioner` is the identity, "not really partitioned yet" default:

- `partition_count()` → `1`.
- `assign_partition(record)` → `0` (every row lands in the one partition).
- `place_partition(0, members)` → `members` (every node owns the single
  partition — replicate-to-all).

Configuring a table with it therefore leaves today's behaviour unchanged (every
node holds all the data, as under the
[`ReplicateToAllNodes`](replication.md) policy) while exercising both halves of
the seam. The balanced / sharded strategies replace it in `#513` / `#514`.

## Selected per table

A `Table` holds the partitioner it is configured with:

```cpp
void Table::set_partitioner(std::shared_ptr<Partitioner> partitioner);
std::shared_ptr<Partitioner> Table::partitioner() const;  // empty = not partitioned
```

The default is an **empty handle** — a table is *not partitioned* until one is
set, and `set_partitioner(nullptr)` un-partitions it. This is **in-memory
configuration only** in this slice: persisting the selection (so a table is still
partitioned after restart) and recording the resulting placements are the
partition catalog (`#510`) and the partitioned write path (`#511`). Nothing is
written to disk or the CRDT here.

## No new JSON boundary (yet)

This slice introduces **no new JSON format**, so there is no schema to validate:
the seam is pure C++ (records are the already-validated row documents a
`StorageEngine` holds; node ids are plain strings), and the trivial
`SinglePartitioner` carries no serializable configuration. The JSON boundary
arrives with the concrete partitioners that *do* carry config (the hash key
column, `P`, the replication factor — `#513` / `#514`) and with the replicated
partition catalog (`#510`), each validated at its own boundary in the usual
hand-written or `pboettch` pattern (see [schemas.md](schemas.md)).

## Tests

`tests/test_partitioner.cpp` covers the trivial baseline (one partition,
everything→0, replicate-to-all placement, empty membership), a custom
`ModuloPartitioner` test double driving non-trivial logic through the base
pointer (assignment in range + deterministic, order-independent placement — proof
the seam is genuinely pluggable without the `#513` / `#514` strategies), and
**configuring a `Table`** with a partitioner (default none, set, swap, clear).

## Pointers

- [storage.md](storage.md) — the on-disk layout + the `StorageEngine` seam a
  partition's rows are stored through.
- [replication.md](replication.md) — the CRDT data plane; the partition catalog
  (`#510`) and sharded replication strategy (`#509`) build on it.
- [cluster.md](cluster.md) — equal-peer membership; `Cluster::nodes()` is the
  source of the `members` `place_partition` is given.
- ADR-31 / ENT-37 — the blueprint artifacts this seam realizes.
