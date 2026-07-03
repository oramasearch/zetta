# Live pipeline instrumentation (`system.pipeline_jobs` / `system.collective_rounds`)

The live distributed-compute pipeline's instrumentation — the per-job
**near-data** data-movement counters and the live **collective-plane** progress —
is queryable as two read-only `system.<table>` views, so the GUI's
**algorithm-at-work animation** (the cluster observability epic,
[#575](https://github.com/oramasearch/zetta/issues/575) area 5) reads from system
tables rather than bespoke per-panel hooks. This is the **engine source** for that
animation; the GUI consumer is a separate slice.

Both views live entirely in **`zetta-core`** (no SDL, headless-testable) and are
**ephemeral, observed** — **no CRDT backing, never replicated** — exactly like
[`system.node_metrics`](../cluster/node-metrics.md), [`system.checkpoints`](job-checkpoint.md),
and [`system.cloud_status`](../cloud/cloud-inspector.md). They are a *measurement* of
what this node did / is doing, not durable agreement, so they live only in memory
on the node the query hits. This is the deliberate exception to "EVERYTHING is
replicated state".

## What already existed vs. what this adds

- The near-data jobs ([`run_near_data_group_by_job`](compute-near-data.md) /
  [`run_near_data_featurize_job`](../ddf/ddf-featurize.md) /
  [`run_near_data_train_job`](../use-cases/uc1-train-over-partitions.md)) already **compute**
  `local_bytes_read` vs `wire_bytes_moved` per job — but returned them by value, so
  nothing held onto them and they weren't queryable.
- The [partition catalog](../data/dataset-partitions.md) (`system.dataset_partitions`) and
  the [`ComputeJob`](compute-jobs.md) lifecycle (`system.jobs`) were already
  queryable; the [collectives](../cluster/collectives.md) had a cumulative `wire_bytes()`
  meter but no exposed round count.
- This slice **records** the per-job near-data counters into a small ephemeral
  per-node registry and **exposes** the live coordinator's collective-plane meters,
  surfacing both as `system.<table>` views.

The **partition-placement timeline** the issue also lists is already queryable via
[`system.dataset_partitions`](../data/dataset-partitions.md) (each row carries `created` +
`owner_nodes`), so no new table is added for it — order by `created` for the
placement-over-time view.

## `system.pipeline_jobs` — per-job near-data counters

One row per recorded near-data job on this node (empty until a near-data job runs,
like `system.checkpoints`). It exposes the near-data acceptance directly:
`local_bytes_read` (partition body bytes read **near data**, not moved) vs
`wire_bytes_moved` (the collective-plane partials), so a query confirms
bytes-moved ≈ partials, not ≈ dataset.

```sql
SELECT * FROM system.pipeline_jobs;
SELECT job_id, kind, local_bytes_read, wire_bytes_moved
  FROM system.pipeline_jobs WHERE kind = 'train';
```

| Column | Meaning |
| --- | --- |
| `job_id` | the [`ComputeJob`](compute-jobs.md) id this came from |
| `kind` | the near-data op — `group_by` / `featurize` / `train` |
| `node` | the node that ran + recorded it (`Node::node_id()`) |
| `partitions_read` | owned partitions of the table materialized |
| `rows_read` | rows in this node's local shard |
| `local_bytes_read` | partition-body bytes read **near data** (not moved) |
| `wire_bytes_moved` | collective-plane bytes moved running the op (the partials) |
| `status` | the job's terminal status (`succeeded` / `failed` / …) |
| `recorded_at` | ISO-8601 UTC stamp at record time |

**Source.** `Node::pipeline_jobs()` returns a snapshot of a **bounded, in-memory
ring** (`PipelineJobInstrumentation`, `include/zetta/compute/pipeline_instrumentation.hpp`)
the near-data runners append to via `Node::record_pipeline_job(...)` once a job
completes. The ring keeps the most-recent `Node::kMaxPipelineJobs` (512) records
(oldest dropped) so a long-lived node can't grow it without limit. It is guarded by
its own mutex — a background interactive run records while the GUI / query thread
reads.

The composed [featurize → train runner](../use-cases/uc1-featurize-train-over-partitions.md)
(#530) records **two rows per run**: a `featurize` row carrying its one shard read
(`partitions_read` / `rows_read` / `local_bytes_read`) + the featurization partials,
and a `train` row carrying the feature rows this node trained on + the
train/explain/score partials — so summing the view never double-counts the shared
read.

## `system.collective_rounds` — live collective-plane progress

Always **exactly one row** (this node), like `system.node_metrics`. It reflects the
live [job coordinator](job-coordinator.md)'s [collectives](../cluster/collectives.md) (the
[#579 spine](job-coordinator.md) — `Node::attach_cluster` /
`Node::job_coordinator`). `rounds_completed` and `wire_bytes_moved` both tick up
during **any** distributed run — including the interactive
[churn run](../use-cases/churn-solution.md) — so the GUI can watch a run advance.

```sql
SELECT * FROM system.collective_rounds;
```

| Column | Meaning |
| --- | --- |
| `node` | this node's id |
| `attached` | whether a coordinator is attached (`false` on a standalone node) |
| `participants` | participants in this node's collective rounds (self + connected peers) |
| `rounds_completed` | collective rounds (allreduce/broadcast/barrier/gather) that succeeded |
| `wire_bytes_moved` | cumulative collective-plane payload bytes this node moved |

A node with **no attached coordinator** (a standalone node, or before
`attach_cluster`) reports `attached=false` with zeroed meters — still exactly one
row, so the view is uniform.

**The round counter.** `Collectives::rounds_completed()` is a new cumulative atomic
incremented once per successfully-completed round (a timed-out / unreachable-peer
round does not count) on the application call path — a single relaxed increment, so
it's safe on the hot collective path. It pairs with the existing cumulative
`Collectives::wire_bytes()` to make "round progress" concrete.

## No new JSON boundary

Neither view parses untrusted JSON. The per-job records are constructed from the
runners' **typed result fields** (no JSON in, no schema to validate), and the
collective meters are **atomic counters**. The row JSON is *emitted* (computed at
query time), like every other `system.<table>` projection. `test_schemas` confirms
no schema was added.

## Where it's wired

- `include/zetta/compute/pipeline_instrumentation.hpp` — the `PipelineJobInstrumentation`
  record + `NearDataJobKind`.
- `include/zetta/node.hpp` / `src/core/node.cpp` — `Node::record_pipeline_job()` /
  `Node::pipeline_jobs()` (the bounded ring).
- `src/core/ddf/local_group_by_job.cpp`, `src/core/ddf/local_featurize_job.cpp`,
  `src/core/ml/local_train_job.cpp` — the near-data runners record their counters
  before returning.
- `include/zetta/cluster/collectives.hpp` / `src/core/cluster/collectives.cpp` —
  `Collectives::rounds_completed()` + the per-round increment.
- `src/core/query/executor.cpp` — the `system.pipeline_jobs` /
  `system.collective_rounds` row builders + dispatch.

## Tests

- `tests/compute/test_pipeline_instrumentation.cpp` — the views: `system.pipeline_jobs`
  exposes recorded counters (+ projection / WHERE), the ring is bounded, an empty
  registry yields no rows; `system.collective_rounds` reports `attached=false` on a
  standalone node and tracks the live coordinator (rounds advance) after a job runs
  an allreduce + barrier through the node-owned coordinator.
- `tests/ddf/test_ddf_local_partitions.cpp`
  (`NearDataGroupByMatchesOracleAndMovesOnlyPartials`) — the **live interactive
  run** acceptance: across a real loopback cluster the near-data group-by's
  per-job counters are queryable on every node via `system.pipeline_jobs`, matching
  the values the run reported.

## See also

- [query.md](../data/query.md) — the `system` database + the other read views.
- [node-metrics.md](../cluster/node-metrics.md) / [job-checkpoint.md](job-checkpoint.md) /
  [cloud-inspector.md](../cloud/cloud-inspector.md) — the sibling ephemeral,
  non-replicated read views.
- [compute-near-data.md](compute-near-data.md) — where the `local_bytes_read` /
  `wire_bytes_moved` counters come from.
- [collectives.md](../cluster/collectives.md) / [job-coordinator.md](job-coordinator.md) —
  the live collective plane the round meter reads.
