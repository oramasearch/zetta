# Partitioner seam

The **`Partitioner`** seam (`include/zetta/partitioner.hpp`) is the pluggable
policy that decides **data placement** for a partitioned table. It realizes
**ADR-31** and entity **ENT-37** (`#512`). The seam plus a trivial baseline and
per-table configuration landed in `#512`; the concrete **record → partition**
strategies (`HashPartitioner` / `RoundRobinPartitioner`) landed in `#513` and are
documented [below](#the-recordpartition-strategies-513). The remaining
persistence/replication and the placement half are separate, dependent slices:

- **record → partition** — `HashPartitioner` (hash of key columns) and
  `RoundRobinPartitioner` (surrogate, for even balance) — **`#513`, built**
  (see [below](#the-recordpartition-strategies-513)).
- **partition → node(s)** — `RendezvousPlacement` (rendezvous / highest-random-weight
  hashing, so a join/leave moves only ~1/N of the partitions) — **built**, `#514`
  (see [below](#rendezvousplacement--partitionnodes-hrw)).
- **the partition catalog** — `system.dataset_partitions`, the replicated record
  of where each partition landed — `#510`.
- **the partitioned write path** — rows → partition → blob, placement recorded —
  **`#511`, built** (`zetta::write_partitioned`, see
  [partitioned-write-path.md](partitioned-write-path.md)): it drives both halves of
  this seam over a partitioned table's rows.
- **rebalance on membership change** — `#517`, **built**
  (`zetta::rebalance_partitions`, see
  [partition-rebalance.md](partition-rebalance.md)): on a join it re-runs the
  partition→node half over the new membership and moves only the changed partitions.
  **compute-near-data** (a DDF op runs over the node's local partitions) — `#516`.

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
the seam. The balanced / sharded record→partition strategies are below; sharded
placement (`partition → node(s)`) is `#514`.

## The record→partition strategies (`#513`)

Two concrete partitioners answer the **record → partition** half — the two ways a
row's partition is decided. Both take the **fixed partition count `P`** as their
first argument (a `0` is clamped up to `1` so `assign_partition` never divides by
zero) and, like every partitioner, keep `assign_partition` **deterministic +
side-effect-free** — the partition is a pure function of the *record*, with no
hidden per-call counter, so a row's partition is **recomputable** (which is what
locality-aware reads, the partition catalog, and rebalancing all rely on). For the
placement half both currently place **replicate-to-all** (every member owns every
partition, like `SinglePartitioner`): this slice's job is the record→partition
mapping. Composing the now-available
[`rendezvous_owners`](#rendezvousplacement--partitionnodes-hrw) (`#514`) for
sharded placement — using each table's [`ReplicationStrategy`](replication-strategy.md)
replication factor — is the partitioned write path's concern (`#511`), where
assignment and placement meet.

### `HashPartitioner` — `hash(key columns) % P`

The standard **shard-by-key** strategy: `assign_partition(record)` hashes the
named key column(s) and takes the result modulo `P`, so a row's partition is a
stable function of its **key**.

```cpp
HashPartitioner partitioner{/*P=*/256, /*key_columns=*/{"account_id"}};
PartitionId partition = partitioner.assign_partition(row);  // hash(row.account_id) % 256
```

- **Stable hash, not `std::hash`.** The partition a key maps to must be identical
  on **every node and across restarts** (durable placement), so the hash is a
  64-bit **FNV-1a** over the key columns' JSON text — the same small,
  deterministic, well-distributed hash zetta already uses for content fingerprints
  (`src/core/replication.cpp`, `gbdt_train_job.cpp`). `std::hash` is unusable
  here: its value is unspecified and varies across standard-library
  implementations and runs.
- **Balanced for a typical key.** A good hash spreads arbitrary keys roughly
  uniformly across the `P` partitions (the acceptance test hashes 10 000 distinct
  keys into 32 partitions and asserts each is within ±40 % of the mean). It does
  **not** *guarantee* even balance under a skewed key — that is what
  `RoundRobinPartitioner` is for.
- **Multi-column keys** are hashed as the ordered combination, joined by a unit
  separator so `("us","gold")` and `("usgo","ld")` don't collide. A **missing**
  column hashes as JSON `null` (a key-less record still lands in a deterministic
  partition rather than being dropped); an **empty** `key_columns` list (a
  degenerate config) falls back to hashing the whole record.

### `RoundRobinPartitioner` — `surrogate % P`

The strategy for **even balance regardless of key skew**: `assign_partition`
returns `surrogate % P`, where `surrogate` is a **monotonically-increasing
integer** the write path stamps on each row (the `surrogate_field`, default
`"_seq"`). A sequence `0, 1, 2, …` therefore cycles `0, 1, …, P-1, 0, 1, …`
across the partitions, so rows spread **perfectly evenly** no matter how their
keys are distributed (the acceptance test sends 800 sequential surrogates through
`P = 8` and asserts exactly 100 land in each partition).

```cpp
RoundRobinPartitioner partitioner{/*P=*/8};  // reads "_seq" by default
PartitionId partition = partitioner.assign_partition(row);  // row._seq % 8
```

The **surrogate** is the mechanism that makes round-robin *deterministic*: rather
than a hidden internal counter (which would break the "same record → same
partition" contract the seam requires), the cycle position rides on the row
itself. A missing / non-integer surrogate degrades to partition `0` (in range,
deterministic) rather than throwing; a negative surrogate is folded into
`[0, P)` with a sign-safe modulo. Stamping the monotonic surrogate is the
partitioned write path's job (`#511`); standalone, the caller supplies it.

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
remaps almost every partition. The between-jobs
[rebalance](partition-rebalance.md) (#517) acts on this guarantee — on a membership
change it re-runs `rendezvous_owners` per partition and updates only the changed
catalog entries.

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
configuration only**: persisting the selection (so a table is still partitioned
after restart) is still a later slice, but the resulting placements **are**
recorded — the [partitioned write path](partitioned-write-path.md) (`#511`) reads a
configured table's rows and lays them out as partitions (blobs + a
[`system.dataset_partitions`](dataset-partitions.md) catalog entry each). Setting a
partitioner writes nothing to disk or the CRDT here — but the partitioner's
*config* serializes (below), and the write path is what turns the configured
table into recorded partitions.

## JSON config boundary (`#513`)

The record→partition strategies carry serializable configuration (`P`, the hash
key columns, the round-robin surrogate field), so `#513` adds a JSON config
boundary for **them**. It is the **hand-written non-throwing pattern** of
[`ScalingPolicy`](scaling-policy.md) / `NodeCapability` (a value-type DTO
boundary, **not** the `pboettch` path reserved for replicated entities), mirroring
[`schemas/partitioner.schema.json`](../schemas/partitioner.schema.json):

```cpp
// Serialize a record→partition strategy's config.
HashPartitioner hash{/*P=*/256, /*key_columns=*/{"account_id"}};
nlohmann::json config = hash.to_json();
//   { "strategy": "hash", "partition_count": 256, "key_columns": ["account_id"] }

// Reconstruct it, validating at the boundary.
std::string error;
std::shared_ptr<Partitioner> p = partitioner_from_json(config, &error);  // null + reason on bad input
```

`SinglePartitioner` / `HashPartitioner` / `RoundRobinPartitioner` each expose
`to_json()`. `partitioner_from_json` is the **validated boundary**: it returns an
empty handle (and, given a non-null `error`, a path-qualified reason) on anything
malformed — not an object, an unknown `strategy`, a missing / non-integer / `< 1`
`partition_count`, a `hash` without a non-empty unique-string `key_columns`, a
`round_robin` with an empty `surrogate_field`, or a `single` with a
`partition_count ≠ 1` — so a table is never configured with a partitioner that
would mis-place rows. It round-trips with `to_json()`.

This is **only** the per-strategy config, not a universal serializer: the
**polymorphic** "which partitioner is this table using" form — a discriminated
union that must round-trip `single` / `hash` / `round_robin` **and**
`rendezvous` **and** composed strategies through a base pointer — is inherently
the partitioned write path's (`#511`) and partition catalog's (`#510`) holistic
concern, designed once rather than piecemeal, so `partitioner_from_json` does not
attempt `rendezvous` (its config — the replication factor — belongs to that
boundary alongside the [`ReplicationStrategy`](replication-strategy.md)). Each is
validated at its own boundary in the usual hand-written or `pboettch` pattern
(see [schemas.md](schemas.md)). The records a partitioner reads are the
already-validated row documents a `StorageEngine` holds.

## Tests

`tests/test_partitioner.cpp` covers the trivial baseline (one partition,
everything→0, replicate-to-all placement, empty membership), a custom
`ModuloPartitioner` test double driving non-trivial logic through the base
pointer, **configuring a `Table`** with a partitioner (default none, set, swap,
clear), and the `#513` strategies + boundary:

- **`HashPartitioner`** — deterministic for a given key (and stable across
  instances), always in range, **balanced** for a 10 000-key sample over `P = 32`
  (±40 % of the mean), multi-column keys, missing-column → `null`.
- **`RoundRobinPartitioner`** — **distributes evenly** across `P` (800 sequential
  surrogates → exactly 100 per partition over `P = 8`), deterministic /
  side-effect-free (interleaved calls never drift), a custom surrogate field, and
  negative / missing / non-integer surrogate handling.
- **the JSON config boundary** — `to_json` ↔ `partitioner_from_json` round-trips
  for all three strategies, the round-robin surrogate-field default, table
  configuration through the boundary, and rejection of every malformed-config
  shape above.

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
