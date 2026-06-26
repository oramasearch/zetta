# Job coordinator

The **job coordinator** (`zetta::compute::JobCoordinator`,
`include/zetta/compute/job_coordinator.hpp`) is the **single entry the native
distributed accelerated-compute algorithms submit work through** — blueprint
**ARC-11** ([#372](https://github.com/oramasearch/zetta/issues/372)), part of the
compute epic ([#264](https://github.com/oramasearch/zetta/issues/264)). It joins
the three pieces the compute pass for UC-1 churn was built from:

| piece | role in a job | built in |
|---|---|---|
| [work-stealing scheduler](work-stealing-scheduler.md) | split a job's work into tasks and run them across cores (intra-node) **and** nodes (cross-node steal) | [#366](https://github.com/oramasearch/zetta/issues/366) |
| [collectives](collectives.md) | combine per-node partials at a **round boundary** (allreduce / broadcast / barrier / gather) | [#365](https://github.com/oramasearch/zetta/issues/365) |
| [`ComputeJob`](compute-jobs.md) | the durable, replicated **lifecycle** record (`Queued → Running → Succeeded \| Failed`) | [#371-ish, ARC-11/ENT-31](https://github.com/oramasearch/zetta/issues/372) |

The future algorithms — DDF, GBDT, SHAP — don't touch the scheduler or
collectives directly. They hand the coordinator a **job body** and run it; the
coordinator drives the lifecycle and hands the body a small **work surface**
(`JobRound`) onto the scheduler + collectives. New algorithm = a new job body, not
a new orchestrator.

```
include/zetta/compute/job_coordinator.hpp   JobCoordinator + JobRound
src/core/compute/job_coordinator.cpp        lifecycle driving + handler wiring
tests/test_job_coordinator.cpp              the distributed-sum acceptance + edges
```

## The execution model: SPMD, with map as the alternative

A distributed compute job runs **SPMD** — *single program, multiple data* — the
way distributed GBDT / SHAP actually run:

1. **Every participating node runs the same job body** against its own data shard.
2. Within a node, the body fans its shard's work across the **local work-stealing
   pool** (`JobRound::parallel_map`) to compute a **per-node partial**.
3. At the **round boundary**, a **collective** (`JobRound::allreduce`, …) combines
   the per-node partials into the same global result on every node.
4. Repeat for the next round (a tree, an epoch); record the job's terminal status.

This is why collectives belong to the coordinator: a collective is inherently
SPMD (every node must call it), and a round boundary is exactly where per-node
partials become a cluster-wide answer.

For work that is **not pre-sharded** per node — batch scoring, a DDF map — the
coordinator also exposes the **cross-node `map`** (`JobRound::map`): one node owns
the work, splits it into tasks, and idle peers **steal** and run them (dynamic
load balancing over the [DistributedScheduler](work-stealing-scheduler.md)).
`map` is owner-centric (only the owner calls it); the SPMD path is symmetric
(every node calls the collectives). A job body picks whichever fits its data.

## Using it

Construct one `JobCoordinator` per node over that node's `Cluster`. It **owns**
the collectives + the distributed scheduler bound to the cluster (see
[Composition](#composition-one-handler-slot-shared) below), so it is the one
object the application holds.

```cpp
#include "zetta/compute/job_coordinator.hpp"

zetta::compute::JobCoordinator coordinator{cluster, /*worker_count=*/4};

// Only needed for the cross-node map() path — the executor that runs a stolen
// task, registered identically on EVERY node:
coordinator.set_executor([](std::string_view method, std::string_view payload) {
    return run_shard(method, payload);
});

coordinator.start();   // register the handler chain + start the steal loop

// Drive a ComputeJob (created on a Node) through its lifecycle:
auto job = node.create_job(zetta::JobKind::Train, project_id, graph_id);
zetta::JobStatus status = coordinator.run(*job, [&](zetta::compute::JobRound& round) {
    // 1. local partial: split this node's shard across the worker pool
    auto partial = compute_partial(round, my_shard);
    // 2. round boundary: combine partials across the cluster
    auto total = round.allreduce("epoch-1", partial, zetta::CollectiveOp::Sum);
    if (!total) return zetta::compute::JobCoordinator::Outcome{.ok = false,
                                                              .detail = "round failed"};
    return zetta::compute::JobCoordinator::Outcome{.ok = true, .detail = "done"};
});
```

### The job body (`Algorithm`) and `Outcome`

A job body is a `std::function<Outcome(JobRound&)>`. It does the compute and
reports an `Outcome`:

- `ok == true` → the coordinator sets the job **Succeeded**, recording `detail`
  (a result summary).
- `ok == false` → the coordinator sets the job **Failed**, recording `detail` (the
  failure reason).

`run` sets the job **Running** first, runs the body on the **calling thread**, and
sets the terminal status from the `Outcome` — so the coordinator never leaves a
job stuck in `Running`. The body **must not throw** across the seam (zetta's
no-exceptions-across-a-seam convention); a thrown exception would skip the
terminal transition. Each transition captures into the CRDT, persists `job.json`,
and — on the terminal transition — emits the `job_finished` [audit event](audit-trail.md)
through whatever `ReplicationManager` / `AuditSink` the job was created with (e.g.
via `Node::create_job`).

### `JobRound` — the work surface

The only thing a job body sees. It cannot reach past the coordinator's seams:

| method | backed by | use |
|---|---|---|
| `parallel_map(inputs, fn)` | local work-stealing pool | intra-node data parallelism (the per-node partial) |
| `map(method, inputs)` | cross-node DistributedScheduler | dynamic cross-node task stealing (not-pre-sharded work) |
| `allreduce` / `broadcast` / `barrier` / `gather` | collectives | round-boundary combine across nodes |
| `participants()` / `node_id()` / `is_root()` | cluster topology | who's in the round; am I the deterministic root |
| `job_id()` | the `ComputeJob` | the body's own logging / collective tags |

## Composition: one handler slot, shared

Both [`Collectives`](collectives.md) and the cross-node
[`DistributedScheduler`](work-stealing-scheduler.md) want the cluster's **single**
`set_signal_handler` slot. They are designed to chain — each forwards the signals
it doesn't own to a fallback/delegate. The coordinator owns both and wires the
chain so an incoming signal is routed:

```
signal ─▶ DistributedScheduler   (steal / steal_result)
            └─ else ─▶ Collectives   (collective.contribute / collective.result)
                         └─ else ─▶ application fallback (optional)
```

Member **declaration order** makes this correct without ceremony: the collectives
are declared first (so they register as the handler on construction and restore
the application fallback **last** on destruction); the scheduler is declared
second (it takes the handler slot on `start()` and restores the collectives on
`stop()` / destruction). This is the reason the coordinator **owns** these pieces
rather than taking references — it is the single object that holds the handler
slot for a node's compute plane.

`start()` is **required** before running jobs that use `map` (it starts the steal
loop). For SPMD / collective-only jobs it is optional but recommended for
uniformity — the collectives route correctly either way (directly as the handler
before `start()`, through the chain after).

## No new JSON boundary

The coordinator introduces **no wire/JSON format of its own**. It composes seams
that already [validate at their own boundaries](compute-jobs.md#boundary-validation-json-schema):

- `map` rides the schema-validated **steal messages**
  (`schemas/steal_{request,offer,result}.schema.json`).
- the collectives ride the schema-validated **`CollectiveMessage`**
  (`schemas/collective_message.schema.json`).
- the job record is validated against the **ComputeJob JSON Schema**
  (`schemas/compute_job.schema.json`) on every disk/CRDT read.

So malformed data still fails fast at each existing seam; the coordinator adds no
new place for it to enter.

## Acceptance: a distributed sum end-to-end

`tests/test_job_coordinator.cpp` runs the headline scenario: a trivial
distributed-sum job through the coordinator on a **multi-node loopback cluster**.
Three nodes each own a shard; each sums its shard across its local pool
(`parallel_map`), then `allreduce` combines the partials at the round boundary, so
every node ends with the same global sum (`666`) and **records its own
`ComputeJob` as `Succeeded`** (verified down to the `job_created` + `job_finished`
audit events). Further tests cover the cross-node `map` path through the
coordinator (an idle peer steals and runs every task of a zero-worker source), a
single-node cluster degenerating cleanly, and a failing job body marking the job
`Failed` with its reason.

## Limits / future

- **One round shape, driven by the body.** The coordinator drives whatever rounds
  the job body expresses; it does not yet own a round scheduler or a convergence
  criterion (that belongs to the algorithms — DDF / GBDT / SHAP — layered on top).
- **Outputs are a `detail` summary.** A `ComputeJob` has no `outputs` field (it
  carries a `run` ref fixed at creation); richer output capture — producing a
  [`Run`](runs.md) with datasets/models and linking it back — is a later slice.
- **No autoscaler.** Sizing the fleet from a job's [`ScalingPolicy`](scaling-policy.md)
  reward model (the autoscaler loop) is a separate, later slice; the coordinator
  runs against whatever cluster is present.

## See also

- [work-stealing-scheduler.md](work-stealing-scheduler.md) — the scheduler the
  coordinator submits tasks through (local + cross-node).
- [collectives.md](collectives.md) — the round-boundary combine.
- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` lifecycle record the
  coordinator drives.
- [signal-plane.md](signal-plane.md) — the ephemeral transport both the scheduler
  and the collectives ride on.
