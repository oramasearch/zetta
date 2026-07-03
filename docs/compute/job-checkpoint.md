# Job checkpoint / restart (burst-job eviction tolerance)

The **checkpoint / restart** capability (`zetta::compute` —
`include/zetta/compute/checkpoint.hpp`) makes a long-running compute job
**eviction-tolerant**: it durably snapshots the job's progress and, after the
driving node dies, **resumes from the last snapshot** instead of starting over.
Blueprint **RULE-5 / RSK-9** ([#468](https://github.com/oramasearch/zetta/issues/468)),
part of the native distributed accelerated-compute pass
([#264](https://github.com/oramasearch/zetta/issues/264)).

## Why

zetta bursts compute onto **spot / preemptible** cloud nodes — every cloud
backend ([Azure](../cloud/azure-provisioner.md), [AWS](../cloud/aws-provisioner.md),
[GCP](../cloud/gcp-provisioner.md), [OCI](../cloud/oci-provisioner.md), [k8s](../cloud/k8s-provisioner.md),
[bare-metal](../cloud/bare-metal-provisioner.md)) requests spot capacity, because that is
where the all-cloud burst economics live. A spot node can be **reclaimed
mid-job** with only a short termination notice (AWS gives ~2 min, GCP ~30 s).

zetta already had **task-level** resilience: the cross-node
[work-stealing scheduler](work-stealing-scheduler.md) re-queues a lost peer's
in-flight *tasks* within a single `map()` call. But an **iterative SPMD job** —
the many boosting rounds of distributed [GBDT training](../ml/gbdt.md), say — has no way
to survive losing the node that drives it: its `coordinator.run(job, body)` call
dies, the surviving nodes' collectives time out, and the whole job restarts from
round 0. This module is the missing **job-level** piece.

```
include/zetta/compute/checkpoint.hpp   JobCheckpoint + CheckpointStore + FileCheckpointStore + run_resumable_rounds
src/core/compute/checkpoint.cpp        the value boundary, the file store, the resume driver
schemas/job_checkpoint.schema.json     generated from job_checkpoint_schema() (schema-as-code)
tests/compute/test_checkpoint.cpp              the value/boundary + store + resume/restart acceptance
```

It is a **standalone seam** with no `Node` / SQL / cluster wiring of its own (the
same altitude as [`Provisioner`](../cloud/provisioner.md) /
[`DeviceRegistry`](device-registry.md)) — the long-running job bodies and the
restart trigger compose it.

## The three pieces

### `JobCheckpoint` — the durable progress snapshot

Everything needed to resume a job without re-doing committed work:

| field | meaning |
|---|---|
| `job_id` | the logical job key (a ComputeJob id for a single-node job; the cross-node-agreed **`round_tag`** for a distributed job, since each node holds a distinct ComputeJob id but they must all resume from the same checkpoint) |
| `kind` | the [`JobKind`](compute-jobs.md) (featurize / train / explain / score / ddf) — for observability |
| `sequence` | units of progress **completed** = the resume point (e.g. boosting rounds / trees built) |
| `total` | total units, when known (0 = unknown); `progress()` is `sequence / total` |
| `state` | **opaque application bytes** — the job's resumable state, base64-encoded in JSON, fingerprinted but never interpreted by the store (like a [large-object body](../data/large-objects.md)) |
| `state_sha256` | lowercase-hex SHA-256 of `state` |
| `created` | ISO-8601 UTC stamp |

It is **boundary-validated** like the Run / Model / ComputeJob records: schema-as-code
(`job_checkpoint_schema()` is the source of truth, `schemas/job_checkpoint.schema.json`
is generated from it and drift-checked by `tests/test_schemas.cpp`), **plus** a
cross-field **integrity check** — `parse_job_checkpoint` decodes the body and rejects
the checkpoint unless `state_sha256` matches it, so a torn / corrupt checkpoint (an
eviction mid-write) is rejected at the boundary instead of resuming from garbage.

### `CheckpointStore` — the durability seam

The pluggable place a job's checkpoints live durably so they survive the node
dying. Non-throwing (every call returns a `CheckpointStatus`):

- `save(checkpoint)` — durably record it; must be crash-safe.
- `latest(job_id)` — the highest-`sequence`, schema-valid, hash-verified checkpoint,
  or `nullopt` (`NotFound` simply means "start fresh").
- `clear(job_id)` — discard a job's checkpoints (idempotent).
- `job_ids()` — the jobs that currently have a valid checkpoint (the source for an
  observability view).

### `FileCheckpointStore` — the durable reference backend

A `CheckpointStore` over the local filesystem under a root directory — the
durability that actually survives the process / node going away. One folder per
job (`<root>/<sha256(job_id)>/`, an opaque collision-free name; the original key
round-trips inside each checkpoint's `job_id` field), one file per checkpoint
(`<zero-padded-sequence>.json`). `save` writes to a temp file and **atomically
renames** it into place, so an interrupted write leaves the previous checkpoint
intact; `latest` returns the highest sequence that parses + validates + verifies,
**falling back past a corrupt head** (a torn write at the tip). Only the two newest
checkpoints are retained.

> A `FileCheckpointStore` is **local to one node's filesystem**. A single-node job
> (or a job whose driving node's process restarts in place) resumes directly. For a
> distributed job whose node is *replaced*, the surviving fleet must share /
> replicate the checkpoint storage — wiring this seam to a replicated store is a
> later slice; because the store is **injected**, that swap is transparent.

## `run_resumable_rounds` — the checkpoint / restart driver

The capability made concrete: drive an iterative job through its rounds with
checkpoint / restart.

```cpp
ResumableResult run_resumable_rounds(
    const std::string& job_id, JobKind kind, CheckpointStore& store,
    const ResumableConfig& config, const std::string& initial_state,
    const RoundStep& step, const StopRequested& stop_requested = {});
```

1. **Resume.** Load the latest checkpoint for `job_id`; if one exists, resume from
   its `sequence` with its `state` (the `initial_state` argument is then ignored).
   Otherwise start at round 0 with `initial_state`. A *corrupt* checkpoint fails the
   run hard rather than silently resuming from garbage or from scratch.
2. **Run + checkpoint.** Run each remaining round via `step(round, state)`,
   checkpointing the new state every `checkpoint_every` rounds (and always after the
   last).
3. **Drain.** When the optional `stop_requested()` returns true — the **spot-eviction
   pattern**: a node gets a termination notice, finishes the round in flight,
   checkpoints, and exits — the driver stops early with progress safely checkpointed.
   On full completion it optionally `clear()`s the checkpoints.

Calling it again over the **same store** (a fresh process / node after an eviction)
resumes where the last checkpoint left off, so the committed rounds are **never
recomputed**. The acceptance (`tests/compute/test_checkpoint.cpp`): a job of 10 rounds
evicted after 4 resumes at round 4, runs only rounds 4..9 (each round runs exactly
once across both attempts), and produces a result **identical** to an uninterrupted
run.

A long-running job body uses this by expressing its work as resumable rounds and
encoding its accumulated state into the opaque `state` bytes (for GBDT, the partial
ensemble — see below).

## How it composes (and what is a later slice)

- **GBDT training** ([gbdt.md](../ml/gbdt.md)) is the first real consumer: the distributed
  `train` job checkpoints its partial model after each boosting round and resumes
  from it, so an eviction costs at most the trees grown since the last checkpoint
  rather than the whole ensemble. The opaque `state` is the partial-model payload
  (the already-validated [`GbdtModel`](../ml/gbdt.md) JSON plus the running feature
  importances), so it rides existing validated boundaries.
- **Observability** ([RULE-7](../data/query.md)): a node owns a `FileCheckpointStore` under
  `<data_dir>/checkpoints`, surfaced as the read-only `system.checkpoints` table
  (one row per checkpointed job: kind / sequence / total / progress) — checkpoints
  are **local, ephemeral** durability artifacts, so the view has no CRDT backing
  (like `system.cloud_status`).
- **Later slices:** a **replicated / shared** checkpoint store so a *replacement*
  burst node can resume a distributed job; driving the restart **automatically**
  from the autoscaler / deployment controller on a *detected* eviction (today the
  capability is in place and the job bodies checkpoint — the auto-restart trigger is
  the remaining wiring); and a real spot-termination-notice source feeding
  `stop_requested`.

## See also

- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` lifecycle record a checkpoint tracks.
- [job-coordinator.md](job-coordinator.md) — the entry distributed jobs run through.
- [work-stealing-scheduler.md](work-stealing-scheduler.md) — the **task-level** eviction recovery this complements at the **job level**.
- [gbdt.md](../ml/gbdt.md) — the first consumer (per-round model checkpointing).
- [provisioner.md](../cloud/provisioner.md) — the spot/preemptible bursts this protects.
