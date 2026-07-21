# The partition scheduler — how many partitions, how big, and where

`zetta::data::PartitionScheduler` / `DefaultPartitionScheduler` ·
[`include/zetta/data/partition_scheduler.hpp`](../../include/zetta/data/partition_scheduler.hpp) ·
[`src/core/data/partition_scheduler.cpp`](../../src/core/data/partition_scheduler.cpp) ·
[#833](https://github.com/oramasearch/zetta/issues/833)

## The problem with eight

Partition count was `AddDatasetOptions::partition_count = 8`. Eight because
eight. That is wrong in both directions at once:

- **Eight partitions of a 4 KB dataset** are eight Parquet footers, eight
  large-object entries, and eight `part:` catalog rows — and the catalog is
  **replicate-to-all**, so each one gossips to every node in the cluster, forever,
  to describe a handful of rows.
- **Eight partitions of a 200 GB dataset** are 25 GB blobs. A partition cannot be
  split across nodes, and a Parquet row group is decoded whole, so that is a
  granularity no rebalance can spread and no node can page.

The count is a **consequence** of a target partition *size* and of how many nodes
must be given work. It is never an input — except as a deliberate override.

## The shape

```
PartitionRequest  ──▶  PartitionScheduler  ──▶  PartitionPlan  ──▶  Partitioner
 rows, bytes,          (policy)                 count, targets,     (execution)
 members, RF,                                   owners, rationale
 cache?, source,
 manual count?,
 telemetry
```

The scheduler sits **above** [`Partitioner`](partitioner.md): it decides the
plan, a `Partitioner` executes it. [Fetch affinity](fetch-affinity-placement.md)
stops being a special case inside the ingest and becomes "a plan the scheduler
emitted". One place makes these decisions, and every write path —
`add_dataset_file`, `add_dataset_rows`, the connector ingest — goes through it.

`PartitionPlan::rationale` carries **why**, in words, and is returned on
`AddDatasetResult::plan` / `DistributedIngestResult::plan`. A plan nobody can
second-guess is a plan nobody will trust the first time a dataset lands lopsided.

## Choosing the count

**A manual `partition_count` is honoured exactly.** A caller that says 37 gets
37. `nullopt` — the default — means auto-detect:

```
bytes    = approx_bytes, or rows × 256 when the caller cannot say
by_size  = ceil(bytes / target_bytes_per_partition)      # ≥ 1
floor    = partitions_per_node × |members|               # every node gets work
count    = clamp(max(by_size, floor), 1, max_partitions)
count    = min(count, row_count)                         # never a partition per row
```

### The target size: 64 MiB, and why

- **Too small pays per-blob overhead.** Every Parquet body carries a footer
  (Thrift metadata for every column of every row group) and at least one row
  group; every partition additionally costs a `lo:` entry and a replicate-to-all
  `part:` entry. At 1 MiB bodies a 64 GiB dataset puts ~65 000 catalog entries on
  the replication plane, and the metadata becomes the bottleneck, not the data.
- **Too large loses parallelism and costs decode memory.** A partition is the
  unit of scatter-gather parallelism, of ownership, and of movement on a
  rebalance — it cannot be spread over two nodes. A row group is decoded whole,
  so a 1 GiB partition is a 1 GiB decode buffer.
- **64 MiB sits between them.** Parquet's own row-group guidance is ~128 MiB
  (sized against an HDFS block); a zetta partition is *also* a replication and
  movement unit, so a notch below that is the right side of the trade. It keeps a
  realistic cluster at hundreds of catalog entries rather than millions, while
  leaving every node several partitions to work on at once.

`PartitionSchedulerConfig` makes it configurable —
`target_bytes_per_partition`, `partitions_per_node` (default 2, the "P >> nodes"
floor of ADR-31), `max_partitions` (4096, matching the API's cap), and the
storage thresholds.

## Choosing the owners — capacity-aware, from live telemetry

Each member gets a weight from the [node telemetry exchange](../cluster/README.md)
(`Node::telemetry_snapshot()`):

| factor | rule | why |
|---|---|---|
| **storage** | neutral below 75% full; tapers to 0.25 across [75%, 90%); collapses to a token weight at ≥90% | A node 77% full with hundreds of GB free is a fine target. Storage is a **gate with a taper**, not a gradient of used-ness — weighting by the free fraction would rank a healthy node at 0.23. |
| **bandwidth** | `1 − net_percent`, when the host can report link utilization | The goal is the **wall-clock fetch time** of the ingest, which is set by the slowest uplink — not row fairness. A saturated link should pull less even if it has disk to spare. A raw rx/tx *rate* says nothing about headroom, so it is ignored. |
| **CPU** | `1 − cpu_percent/200`, floored at 0.5 | Decode + Parquet encode are real work, but not the bottleneck a link is. Deliberately mild. |

Primaries go to the node with the lowest load-to-weight ratio (deterministic,
ties on roster order). Replicas walk the weight order from a per-partition
rotation, so they spread rather than piling onto the strongest node.

### Degrading gracefully

The telemetry exchange is ephemeral and TTL'd: a peer that has not gossiped
recently simply is not in the snapshot. Such a member is weighted like a
**typical** member — the mean of the measured weights — never like a perfect one.

That is not a detail. A measured node can only ever score *at or below* neutral,
so handing an unknown node 1.0 makes it beat every node that actually reported,
and an entire dataset lands on the one node the coordinator knows nothing about.
(It did, before this rule: a 3-node cluster planned four of four partitions onto
the single member with no sample.) With nothing measured at all the mean is 1.0
and the whole cluster is neutral — an even spread, the documented degrade, never
a failure.

A nearly-full node is **avoided, not banned**: it keeps a token weight so it is
still a legal replica target, because banning it could leave a plan with nowhere
to put a replica.

## The plan travels now

`plan_partition_assignments` was a pure function of (roster, row counts), so
every node derived the identical tiling and nothing crossed the wire. **That
property is gone, deliberately**: telemetry differs per node by construction, so
a plan each participant derived for itself would not agree with any other's.

So the **coordinator computes the plan once and ships it**, in the existing
`run.launch` payload (`connector_fetch`,
[`schemas/connector_fetch_launch.schema.json`](../../schemas/connector_fetch_launch.schema.json),
whose `plan` property embeds `partition_plan_schema()` rather than restating it).
Participants re-validate it at the boundary — a launch envelope is untrusted
input like any other — and then **use** it. Nobody recomputes. `partition_count`
and `replication_factor` are no longer separate payload fields; both live in the
plan, which is the single place those decisions are made.

`partition_plan_from_json` also checks the one cross-field invariant a JSON
Schema cannot state: `owners.size() == partition_count`. A plan whose owner list
does not describe every partition would silently under-place a dataset.

## Right-sizing the fetch — and when a rebalance is earned

**Fetch parallelism and resting storage layout are different decisions.** A
HuggingFace dataset published as 12 files fixes the *downloader* count at 12;
Snowflake's `partitionInfo[]` chunks are freely assignable. Neither dictates a
good resting layout. `SourceLayout` records which case a source is:

```
1. the scheduler computes the TARGET layout
2. source FLEXIBLE  (Snowflake: result chunks assignable)
     -> fetch DIRECTLY into the target shape. NO rebalance, ever.
   source FIXED      (HuggingFace: 12 files)
     -> fetch source-shaped (max download parallelism), then rebalance to target
```

For a flexible source the ingest passes the plan's per-node shares to
`plan_fetch_assignments`, so a node that will own a fifth of the partitions pulls
a fifth of the rows and writes exactly the partitions the plan gave it. The
bodies come out the size the scheduler targeted, and **there is nothing to move
afterwards** — which is the point: most ingests need no rebalance at all, and the
automatic one only fires when the source genuinely forced a mismatch.

**Implemented now: `SourceLayout` and the Snowflake (flexible) case.** The
fixed-source path is designed for, not built: `SourceShape::Fixed` exists as the
input, and `rebalance_partitions(..., include_fetch_affinity=true)` is the call
it drops into.

### The conflict that had to be resolved

Phase 4 made [`rebalance_partitions`](partition-rebalance.md) **skip
fetch-affinity partitions**, because a membership change was rehashing exactly
what fetch affinity had placed to avoid a shuffle. That skip is right for the
**opportunistic** trigger (the membership-change rebalance in `Node::tick`, and
`add_nodes`) and stays. But if it also swallowed an ingest's own post-download
rebalance, the two features would cancel out and the fixed-source path could
never work.

So the skip is now scoped to the opportunistic trigger.
`include_fetch_affinity` — false by default, so nothing background ever moves a
fetch-affinity partition — makes an ingest's explicit call actually run.

## Tests

[`tests/data/test_partition_scheduler.cpp`](../../tests/data/test_partition_scheduler.cpp)

| property | test |
|---|---|
| manual override honoured exactly | `ManualOverrideIsHonouredExactly`, `AddDatasetUsesThePlanAndReportsIt` |
| sensible counts tiny / medium / huge | `AutoDetectTargetsASizeAcrossTinyMediumAndHuge` |
| every node gets work; the ceiling holds | `EveryNodeGetsWorkAndTheCeilingHolds` |
| a nearly-full node is avoided (not banned) | `ANearlyFullNodeIsAvoided` |
| two healthy-but-different disks stay spread | `HealthyNodesWithADisagreeingStorageReadingStaySpread` |
| stale / missing telemetry ⇒ even distribution | `MissingTelemetryFallsBackToAnEvenDistribution` |
| an unmeasured node does not out-rank measured ones | `AnUnmeasuredNodeDoesNotOutrankEveryMeasuredOne` |
| a cache is planned at RF=1 | `ACacheIsPlannedAtReplicationFactorOne` |
| the target size is configurable | `ThePlanIsConfigurable` |
| the plan round-trips its JSON boundary | `ThePlanRoundTripsThroughItsJsonBoundary`, `AMalformedPlanIsRejectedAtTheBoundary` |
| a flexible source is tiled into the plan's shape | `AFlexibleSourceIsTiledIntoThePlansShape` |
| the count survives a restart via the manifest | `ThePlansCountSurvivesARestartThroughTheManifest` |

[`tests/connect/test_distributed_ingest.cpp`](../../tests/connect/test_distributed_ingest.cpp)
— `CoLaunchFetchesEachChunkOnceAndLandsWhereItPulled` additionally asserts that
the plan round-trips through the **launch payload** (every node lands within its
planned share), that every entry is still `fetch_affinity`-placed, and that an
opportunistic rebalance over the live membership moves **nothing**.

[`tests/data/test_partition_rebalance.cpp`](../../tests/data/test_partition_rebalance.cpp)
— `FetchAffinityIsSkippedOpportunisticallyButMovesWhenAsked`.

## Related

[partitioner.md](partitioner.md) ·
[fetch-affinity-placement.md](fetch-affinity-placement.md) ·
[partition-rebalance.md](partition-rebalance.md) ·
[partition-manifest.md](partition-manifest.md) ·
[distributed-connector-ingest.md](distributed-connector-ingest.md) ·
[add-dataset.md](add-dataset.md)
