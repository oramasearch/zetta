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
| [collectives](../cluster/collectives.md) | combine per-node partials at a **round boundary** (allreduce / broadcast / barrier / gather) | [#365](https://github.com/oramasearch/zetta/issues/365) |
| [`ComputeJob`](compute-jobs.md) | the durable, replicated **lifecycle** record (`Queued → Running → Succeeded \| Failed`) | [#371-ish, ARC-11/ENT-31](https://github.com/oramasearch/zetta/issues/372) |

The compute algorithms — DDF, GBDT, SHAP — don't touch the scheduler or
collectives directly. They hand the coordinator a **job body** and run it; the
coordinator drives the lifecycle and hands the body a small **work surface**
(`JobRound`) onto the scheduler + collectives. New algorithm = a new job body, not
a new orchestrator. The first such consumer is the DDF
[groupby-aggregation op](../ddf/ddf-ops.md) (`zetta::ddf::run_group_by_job`, #387): an
SPMD job body that computes each node's local partial and `gather`s them to the
root.

```
include/zetta/compute/job_coordinator.hpp   JobCoordinator + JobRound
src/core/compute/job_coordinator.cpp        lifecycle driving + handler wiring
include/zetta/node.hpp + src/core/node.cpp  Node::attach_cluster / job_coordinator (the node-owned spine, #579)
tests/compute/test_job_coordinator.cpp              the distributed-sum acceptance + edges
tests/compute/test_node_job_coordinator.cpp         the node-owned coordinator acceptance (#579)
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

### The node-owned coordinator (the long-lived spine, #579)

The example above constructs a coordinator **ad-hoc** — fine for a test or a
one-shot job. A **running [`Node`](../architecture/architecture.md) owns one long-lived
coordinator over its real `Cluster`**, so the distributed compute algorithms have
a standing place to submit work without each building (and tearing down) their own
coordinator. This is the "spine" of the compute epic
([#575](https://github.com/oramasearch/zetta/issues/575) / ARC-11):

```cpp
// main() builds the Node + the Cluster, then attaches the compute plane:
node.attach_cluster(cluster);            // construct + start the coordinator
…
zetta::compute::JobCoordinator* coordinator = node.job_coordinator();  // or nullptr
```

- **`Node::attach_cluster(Cluster&, unsigned worker_count = 0)`** constructs the
  coordinator over the cluster's signal plane and `start()`s it. It is called
  **after** both are up (in `main`, right after `cluster.start()` + the membership
  seed) because the Node is constructed and started *before* the Cluster exists —
  so the cluster can't be a constructor argument. `worker_count == 0` (the default)
  picks the local pool size from the hardware concurrency (one per core, leaving
  one for the node loop + the cluster's network thread); the SPMD `parallel_map`
  path always has at least one worker.
- **`Node::job_coordinator()`** returns the owned coordinator, or `nullptr` for a
  standalone node (no cluster attached) or before `attach_cluster`. A job submitted
  through it runs **SPMD across the live cluster membership**, exactly like an
  ad-hoc coordinator — it *is* the node's, over its real cluster.
- **Lifecycle.** `Node::stop()` tears the coordinator down (joins the steal loop,
  restores the cluster's signal handler) **before** the cluster shuts down, so the
  unwind never touches a dead cluster — which is why `main` keeps `cluster.stop()`
  then `node.stop()` and why the in-process test stops the node before the cluster.
  A `stop()` / `start()` cycle re-establishes the coordinator from the remembered
  attachment. It is **headless** (zetta-core, no SDL), so both `--headless` and the
  headed app get it for free. The coordinator's only standing background work is
  the scheduler's steal loop, a cheap ~25 ms poll (a no-op until a `map` executor
  is registered) that fits inside the node's tick cadence.

The GUI's churn demo (`App::run_churn_demo`) still spins up its **own** isolated
single-node loopback coordinator on purpose, so its SPMD collectives never block on
this node's real peers; routing that (and other in-process job submitters) through
the node-owned coordinator is follow-up work this spine enables.

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
and — on the terminal transition — emits the `job_finished` [audit event](../governance/audit-trail.md)
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

Both [`Collectives`](../cluster/collectives.md) and the cross-node
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

`tests/compute/test_job_coordinator.cpp` runs the headline scenario: a trivial
distributed-sum job through the coordinator on a **multi-node loopback cluster**.
Three nodes each own a shard; each sums its shard across its local pool
(`parallel_map`), then `allreduce` combines the partials at the round boundary, so
every node ends with the same global sum (`666`) and **records its own
`ComputeJob` as `Succeeded`** (verified down to the `job_created` + `job_finished`
audit events). Further tests cover the cross-node `map` path through the
coordinator (an idle peer steals and runs every task of a zero-worker source), a
single-node cluster degenerating cleanly, and a failing job body marking the job
`Failed` with its reason.

`tests/compute/test_node_job_coordinator.cpp` proves the **node-owned** path (#579): three
loopback nodes each `attach_cluster` and then run the same distributed sum
**through `Node::job_coordinator()`** — every node reaches the same global sum and
records its job `Succeeded` — plus the lifecycle edges (a single-node cluster, that
`stop()` releases the coordinator and a restart re-attaches it, and that an
unattached standalone node has no coordinator).

## Limits / future

- **One round shape, driven by the body.** The coordinator drives whatever rounds
  the job body expresses; it does not yet own a round scheduler or a convergence
  criterion (that belongs to the algorithms — DDF / GBDT / SHAP — layered on top).
- **Outputs are a `detail` summary.** A `ComputeJob` has no `outputs` field (it
  carries a `run` ref fixed at creation); richer output capture — producing a
  [`Run`](../pipeline/runs.md) with datasets/models and linking it back — is a later slice.
- **Autoscaler not yet wired in.** Sizing the fleet from a job's
  [`ScalingPolicy`](scaling-policy.md) reward model is the
  [job autoscaler](job-autoscaler.md) (`#371`) — a built, tick-driven control loop.
  Wiring its live observation source (round count / accumulated node-seconds) and
  its `tick()` into the coordinator for a live, end-to-end autoscaled job is the
  remaining, later slice; today the coordinator runs against whatever cluster is
  present.

## See also

- [ddf-ops.md](../ddf/ddf-ops.md) — the first consumer: the DDF groupby-aggregation op
  run as a distributed `ComputeJob` through this coordinator.
- [work-stealing-scheduler.md](work-stealing-scheduler.md) — the scheduler the
  coordinator submits tasks through (local + cross-node).
- [collectives.md](../cluster/collectives.md) — the round-boundary combine.
- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` lifecycle record the
  coordinator drives.
- [signal-plane.md](../cluster/signal-plane.md) — the ephemeral transport both the scheduler
  and the collectives ride on.
