# Distributed-algorithm-at-work animation (#590)

The headed **Cluster** panel **animates the distributed pipeline over the node-box
canvas as it runs** — partitioning (rows → partitions → owner nodes), near-data
loads, scatter/gather collective rounds, train rounds, and rebalance-on-join
(partitions sliding onto a new node), plus the **wire-bytes-vs-local-bytes** flow
contrast. This is the "fun, high-fidelity" payoff of the cluster observability epic
([#575](https://github.com/oramasearch/zetta/issues/575) area 5), building on the
[spatial node-box canvas](../cluster/cluster.md#spatial-node-box-view-the-headed-canvas)
(#582), the [animated per-node gauges](cluster-node-gauges.md) (#584), and the
[live pipeline instrumentation read views](../compute/pipeline-instrumentation.md) (#589).

It is a **thin renderer**: every flow is born from a real change in the live
instrumentation, and no number is invented. The data sources are exactly the three
ephemeral read views the prior slice exposed — the same source the SQL `system.*`
views project from:

| Animation | Source (read view) | `Node` accessor |
| --- | --- | --- |
| partition placement + rebalance moves | [`system.dataset_partitions`](../data/dataset-partitions.md) | `Node::dataset_partitions()` |
| near-data local read + wire partials | [`system.pipeline_jobs`](../compute/pipeline-instrumentation.md) | `Node::pipeline_jobs()` |
| scatter/gather collective rounds | [`system.collective_rounds`](../compute/pipeline-instrumentation.md) | `Node::job_coordinator()->collectives()` |

## What you see

While a distributed run is in flight, the canvas (`App::draw_cluster`,
`src/gui/app_operations.cpp`) comes alive:

- **Partition placement** — a new [partition catalog](../data/dataset-partitions.md) entry
  (e.g. from [Add dataset](../data/add-dataset.md) or the [partitioned write
  path](../data/partitioned-write-path.md)) drops a **green ring pulse** onto each owner
  node box, captioned with its row count. Data landing on the nodes that own it.
- **Rebalance-on-join** — when a partition's `owner_nodes` change (the
  [between-jobs rebalance](../data/partition-rebalance.md) after [add nodes](../cloud/add-nodes.md)),
  an **ink chip slides** from the old owner's box onto the new owner's — the
  "partitions sliding onto a new node" the epic calls out.
- **Near-data load** — a newly-recorded near-data job
  ([group-by](../compute/compute-near-data.md) / [featurize](../ddf/ddf-featurize.md) /
  [train](../use-cases/uc1-train-over-partitions.md)) pulses a **green ring on the node that ran
  it**, captioned `kind · <local bytes>` — the bytes that **stayed home** (read near
  data, never moved).
- **Scatter / gather rounds** — each tick of the live coordinator's
  `rounds_completed` sends a paced **ink comet** from each participant to the
  leaderless root (gather) and back (scatter) — the collective heartbeat of a live
  train. On a single-node cluster (no peer to flow to) a round is a self pulse on
  the lone node.
- **Wire flow** — a job that moved partials sends an **ink comet** from its node
  toward the root, sized by the bytes moved.
- **Working-node glow** — a node doing pipeline work wears a soft **green halo**
  (eased up when a flow touches it, decayed when the work stops), so the active
  nodes light up.

Below the canvas a **live activity strip** echoes the real numbers verbatim —
`rounds`, `wire moved`, `near-data jobs`, `partitions`, and the
placed / rebalanced tallies witnessed since the view opened — with a legend tying
the flow colours to **near data** (green, stays home) vs **over the wire** (ink).
When no compute plane is attached (a standalone node) the strip says so and the
canvas stays quiet.

### Colours (Orama design system)

The flow vocabulary makes "wire-bytes-vs-local-bytes" legible at a glance:
**near-data / local work reads green** (`kDotActive` — it stays home, the near-data
win the instrumentation makes checkable), **wire / collective movement reads ink**
(`kInkMuted` / `kInkSecondary` — it crosses the network). **Coral is never used** —
it stays the surgical "you are here" selection accent (the node-box border); the
semantic green/amber/red stays the [resource gauges'](cluster-node-gauges.md).

## How it works

The overlay is the same **engine-owns-the-data, GUI-animates-it** split as the
per-node gauges:

- **`src/gui/cluster_pipeline.hpp`** — pure data (no ImGui/SDL), so `app.hpp` can
  include it without pulling the toolkit into every TU. It declares a
  `PipelineFlow` (one short-lived eased flow — kind, `from`/`to` node ids, age,
  lifetime, weight, label) and the `ClusterPipelineState` animator (the last-seen
  instrumentation baseline to diff against, the bounded in-flight flow list, and a
  per-node "activity" level). `PipelineInputs` is the per-frame snapshot the
  animator diffs — a plain value type built from the `Node` accessors, so the header
  carries **no zetta-core dependency**.
- **`src/gui/app_operations.cpp`** — `update_cluster_pipeline()` snapshots the three
  read views **once per frame** in `draw_cluster`, **diffs** the snapshot against the
  baseline (a new partition id → placement; changed owners → a move; a new job id →
  a near-data read + wire flow; an advanced `rounds_completed` → a scatter/gather
  set), and advances + culls the in-flight flows. `draw_pipeline_flow()` positions
  each flow from the **live box centres** (rebuilt each frame, so the animation
  tracks a window resize / reflow) and glides it over the boxes.

### Staying within the render cadence

The constraint is that the overlay **must not starve a live resize** — it runs
inside the same `render_frame` / `SDL_WaitEventTimeout` (~30 fps idle) cadence the
[gauges](cluster-node-gauges.md) rely on. So it is deliberately cheap and bounded:

- the per-frame reads are in-memory snapshots (the partition catalog, the bounded
  near-data ring, three atomic collective counters) — no I/O, no allocation storm;
- steady state is a couple of map lookups + a bounded flow list (≤ 64 in flight);
- **at most a handful of new flows spawn per update** (`kMaxSpawnPerUpdate`), so a
  burst — e.g. returning to the Cluster view after a long run, where the baseline is
  stale — **folds into the baseline instead of replaying as a storm**;
- the diff maps (`partition_owners`, `seen_jobs`) are pruned to the current snapshot
  each update, so a long-lived session can't grow them without limit;
- the collective pulses are paced to a legible ~2 Hz by a cooldown, however fast
  `rounds_completed` actually ticks (the strip still shows the true count).

The **first update only primes the baseline** (spawns nothing), so opening the view
in the middle of history doesn't replay it — only changes witnessed live animate.

## No new JSON boundary

The overlay parses no JSON. It reads the same typed, already-validated
instrumentation the read views project — `DatasetPartition` (CRDT-backed, schema-
validated at its own boundary), the typed `PipelineJobInstrumentation` records, and
atomic collective counters — and emits nothing. Like the read views it animates, it
introduces **no new format to validate** (see
[pipeline-instrumentation.md](../compute/pipeline-instrumentation.md)).

## Where it's wired

- `src/gui/cluster_pipeline.hpp` — `PipelineFlow` / `ClusterPipelineState` /
  `PipelineInputs` (pure data).
- `src/gui/app.hpp` — the `ClusterPipelineState cluster_pipeline_` member +
  `App::draw_pipeline_activity`.
- `src/gui/app_operations.cpp` — `update_cluster_pipeline()` (the diff),
  `draw_pipeline_flow()` (one flow), the integration in `App::draw_cluster` (snapshot
  → update → box-centre capture + activity halo → flow overlay), and
  `App::draw_pipeline_activity()` (the live strip + legend).

## See also

- [pipeline-instrumentation.md](../compute/pipeline-instrumentation.md) — the engine source
  (`system.pipeline_jobs` / `system.collective_rounds`) this animates.
- [cluster.md](../cluster/cluster.md) — the cluster + the spatial node-box canvas (#582).
- [cluster-node-gauges.md](cluster-node-gauges.md) — the sibling animated overlay
  (per-node resource gauges, #584).
- [compute-near-data.md](../compute/compute-near-data.md) — where `local_bytes_read` vs
  `wire_bytes_moved` (the green-vs-ink contrast) comes from.
- [dataset-partitions.md](../data/dataset-partitions.md) /
  [partition-rebalance.md](../data/partition-rebalance.md) — the placement + rebalance the
  partition flows animate.
