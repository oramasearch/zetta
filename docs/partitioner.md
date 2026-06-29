# Partitioner seam

The **`Partitioner`** seam (`include/zetta/partitioner.hpp`) is the pluggable
policy that decides **data placement** for a partitioned table. It realizes
**ADR-31** and entity **ENT-37** (`#512`). This slice is the **interface only** —
the seam plus a trivial baseline implementation and per-table configuration. The
concrete strategies and the persistence/replication that record where partitions
actually land are separate, dependent slices:

- **record → partition** — `HashPartitioner` (hash of a key column) and
  `RoundRobinPartitioner` (even balance) — `#513`.
- **partition → node(s)** — `RendezvousPlacement` (rendezvous / highest-random-weight
  hashing, so a join/leave moves only ~1/N of the partitions) — **built**, `#514`
  (see [below](#rendezvousplacement--partitionnodes-hrw)).
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

## `RendezvousPlacement` — partition→node(s) (HRW)

`RendezvousPlacement` (`include/zetta/rendezvous_placement.hpp`, `#514`) is the
first non-trivial **partition→node** strategy — **rendezvous hashing**, also
called **highest-random-weight (HRW)**. It answers `place_partition`: given a
partition and the current `members`, it scores each member with a deterministic
weight `hash(partition, member)` and keeps the `replication_factor` members with
the **highest** weight — the highest is the primary owner, the rest are replicas.

```cpp
RendezvousPlacement placement{/*partition_count=*/4096, /*replication_factor=*/2};
auto owners = placement.place_partition(partition, cluster.node_ids());
// owners[0] = primary, owners[1] = replica — the two highest-weight members.
```

### Why rendezvous hashing

The whole point of a fixed `P` is that a membership change should move only a
**small, bounded fraction** of partitions, not reshuffle everything. HRW gives
exactly that:

- **Adding a node** only steals a partition when the newcomer's weight for that
  partition is higher than the current owner's — which happens for ~`1/(N+1)` of
  partitions. Every other partition keeps its owner. So one node joining an
  `N`-node cluster moves only ~`1/(N+1)` of partitions, and they all move **to**
  the new node — nothing reshuffles between the pre-existing nodes.
- **Removing a node** only affects the partitions that node owned (~`1/N`); each
  falls through to the next-highest-weight surviving member. No partition owned
  by a surviving node moves.

This is the rebalancing guarantee in the [seam intro](#why-a-fixed-partition-count),
made concrete. Contrast a naive `member[hash(partition) % N]`: changing `N`
remaps almost every partition.

### Deterministic + leaderless

The weight is a pure function of `(partition, member)`, so `place_partition` is a
pure function of `(partition, members, replication_factor)` — **order-independent**
(it sorts by weight, so two nodes that observe the same member set in different
orders agree) and **deterministic**. That is what lets the leaderless cluster
agree on placement with **no election**: every node computes the same owners, the
same way the [collectives](collectives.md) pick a root by lexicographically-smallest
id. Ties in weight (astronomically unlikely with a 64-bit hash) break by node id
ascending, so even a collision is resolved deterministically.

The weight is **FNV-1a (64-bit)** — zetta-core's hashing idiom (the
[replication](replication.md) version fingerprint, the GBDT held-out split) — over
the partition's 4 bytes followed by the member id's bytes (hashing the fixed-width
partition first makes the encoding unambiguous, so two distinct
`(partition, member)` pairs can't collide by concatenation alone).

### Edge cases

- empty `members` → empty result (nowhere to place).
- `replication_factor == 0` → treated as `1` (a partition always has a primary
  owner when there is at least one member).
- `replication_factor > members.size()` → every member is returned (you cannot
  place more replicas than there are nodes); the returned owners are always
  distinct.

### A complete partitioner

The `Partitioner` seam bundles both halves, so `RendezvousPlacement` is a
**complete, directly-usable** partitioner — `place_partition` is the rendezvous
placement above, and `assign_partition` is a deterministic FNV-1a hash of the
record into `[0, partition_count())` so rows actually spread across the
partitions. That record→partition half is a documented **stand-in**: the
dedicated record→partition strategies (`HashPartitioner` over a key column,
`RoundRobinPartitioner` — `#513`) are a separate slice. The reusable core is the
free function

```cpp
std::vector<std::string> rendezvous_owners(
    PartitionId partition,
    const std::vector<std::string>& members,
    std::size_t replication_factor = 1);
```

so a `#513` strategy can give its rows a real key-column hash for assignment **and**
reuse `rendezvous_owners` for `place_partition`, without subclassing
`RendezvousPlacement`.

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

The seam — and `RendezvousPlacement` — introduce **no new JSON format**, so there
is no schema to validate: it is pure C++ (records are the already-validated row
documents a `StorageEngine` holds; node ids are plain strings), and a
partitioner's configuration (`P`, the replication factor) is constructor
arguments, not parsed input. The JSON boundary arrives with **persistence**: the
serialized form of *which* partitioner a table is configured with is inherently
**polymorphic** (it must round-trip `Single` vs. `Hash`/`RoundRobin` vs.
`Rendezvous` vs. composed strategies), so that discriminated-union boundary
belongs to the partitioned write path (`#511`) and the replicated partition
catalog (`#510`) — designed once, holistically — not to each strategy
piecemeal. Each is validated at its own boundary in the usual hand-written or
`pboettch` pattern (see [schemas.md](schemas.md)).

## Tests

`tests/test_partitioner.cpp` covers the trivial baseline (one partition,
everything→0, replicate-to-all placement, empty membership), a custom
`ModuloPartitioner` test double driving non-trivial logic through the base
pointer (assignment in range + deterministic, order-independent placement — proof
the seam is genuinely pluggable without the `#513` / `#514` strategies), and
**configuring a `Table`** with a partitioner (default none, set, swap, clear).

`tests/test_rendezvous_placement.cpp` covers `RendezvousPlacement`: assignment
spreads across partitions + is deterministic; placement is deterministic and
order-independent, returns distinct replicas with the primary first, clamps the
replication factor to the member count, and balances primaries roughly evenly
(~`1/N` each); and the **acceptance** — adding one node reassigns only ~`1/(N+1)`
of partitions (all moving *to* the newcomer, nothing reshuffled between existing
nodes) and removing one node reassigns only the partitions that node owned (~`1/N`),
leaving every surviving node's partitions in place.

## Pointers

- [storage.md](storage.md) — the on-disk layout + the `StorageEngine` seam a
  partition's rows are stored through.
- [replication.md](replication.md) — the CRDT data plane; the partition catalog
  (`#510`) and sharded replication strategy (`#509`) build on it.
- [cluster.md](cluster.md) — equal-peer membership; `Cluster::nodes()` is the
  source of the `members` `place_partition` is given.
- ADR-31 / ENT-37 — the blueprint artifacts this seam realizes.
