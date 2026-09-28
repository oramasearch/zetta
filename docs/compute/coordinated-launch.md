# Coordinated cross-process SPMD launch (`run.launch`)

The **co-launch** half of the distributed compute story (#601, follow-up to
#580/#600, part of #575). The [collectives](../cluster/collectives.md) already
handle the **combine** — allreduce / gather / broadcast at every round boundary
— but an SPMD job only converges when **every participating node runs the same
job body concurrently**: each collective round waits for *all* connected peers
(a bounded timeout), so a single node submitting an interactive run over a
multi-node cluster whose peers are not co-running fails those rounds. Until
this slice the co-launch existed only *in-process* (the testkit spawning one
`std::thread` per loopback node — `cases_uc1_scale.cpp`); this seam makes it
work **across processes**, over the live membership.

```
include/zetta/compute/coordinated_launch.hpp   CoordinatedLaunchRequest + the envelope
                                               boundary + CoordinatedLaunchListener
src/core/compute/coordinated_launch.cpp        the embedded schema + listener impl
include/zetta/node.hpp + src/core/node.cpp     Node::coordinated_launch (wired by
                                               attach_cluster; drained by tick)
schemas/coordinated_launch_request.schema.json the generated envelope schema
schemas/churn_coordinated_run.schema.json      the generated churn payload schema
tests/compute/test_coordinated_launch.cpp              the seam's unit suite
tests/usecases/churn/test_churn_coordinated_launch.cpp the multi-node convergence proof
```

## The mechanism

A launch is a **`run.launch` Task signal** — the fire-and-forget work envelope
the [signal plane](../cluster/signal-plane.md) purpose-built as "the substrate
for job dispatch" (`SignalKind::Task` had no consumer until now):

1. **Fan-out (the initiator).** Snapshot the roster — the collective
   participant set (this node + every connected peer, node-id ascending,
   identical on every node) — build a `CoordinatedLaunchRequest` naming the
   roster + a kind-specific payload, and `fan_out()` it: one `run.launch` task
   per roster peer. Then run your own shard of the work on the calling thread.
2. **Validate + queue (each peer, network thread).** The peer's
   `CoordinatedLaunchListener` receives the task on the cluster's network
   thread, where it only **schema-validates the envelope and enqueues** it (the
   steal-handler discipline). Running the SPMD body there would deadlock the
   very collective rounds it participates in.
3. **Execute (each peer, owner thread).** `Node::tick()` drains the queue and
   runs the registered runner for the request's `kind` on the node-owning
   thread — the same "synchronous on the thread that owns the Node" pattern
   every interactive submitter uses. The co-run's collectives then block an
   application thread on every member, and the round combines.

The listener sits in the coordinator's signal-handler chain as the
**application fallback**, delegating everything that is not a `run.launch`
task onward (the telemetry exchange today):

```
signal ─▶ DistributedScheduler   (steal / steal_result)
            └─ else ─▶ Collectives   (collective.contribute / collective.result)
                         └─ else ─▶ CoordinatedLaunchListener   (run.launch)
                                      └─ else ─▶ NodeTelemetryExchange (node.metrics)
```

`Node::attach_cluster` wires all of it (and registers the built-in launch
kinds) on **every clustered node** — that is what lets any member co-run a
peer's interactive run; `Node::coordinated_launch()` reaches the listener
(nullptr before attach, like `job_coordinator()`).

## Ephemeral by design

A launch is a *message*, not durable state — the signal-plane carve-out from
the "everything is replicated" rule. Delivery is best-effort, at-most-once: a
peer that misses the launch simply doesn't co-run, and the initiator's
collective rounds surface the absence with their bounded timeout (a failed
run with the engine's reason — observable, never silent). The durable
artifacts of the launched work — the `ComputeJob`s, the `Run`, the `Model` —
replicate through their own CRDT records exactly as always. A **replicated
run-request table** (the durable alternative the issue names) stays a later
slice if at-least-once launch semantics are ever needed.

## The boundary (new JSON, schema-validated)

The launch envelope is a new wire format, so it gets the full treatment
([schemas-as-code](../architecture/schemas.md)): `id` + `kind` + `initiator` +
`participants` (the roster) + `payload`, pboettch-validated against the
embedded draft-07 `coordinated_launch_request_schema()` in
`decode_coordinated_launch` **before anything reaches the queue** — a
malformed or hostile envelope is counted (`rejected()`), recorded as a failed
`run.launch:reject` span, and never reaches a runner. The kind-specific
`payload` is validated by its kind's runner at *its* boundary.

## Sharding: the launch kind decides

New launch kind = a new `set_runner(kind, runner)` registration — never a
bespoke fan-out path. The runner contract: derive this node's rank via
`participant_rank(request, node_id)`, run **only that shard**, return a
`LaunchOutcome` (recorded on the drain span). The first registered kind is the
churn solution (`usecases::churn::install_churn_launch_runner`):

- **Centrally-originated interactive data** (the churn case): the initiator
  encodes the whole dataset once (global entity ids) and ships the **encoded
  numeric shards** in the payload; every participant keeps the slice
  `entity % roster_size == rank`. Slices are disjoint and their union is the
  whole, so the allreduce combines to the global model **without
  double-counting** — proven single-node == multi-node in
  `tests/usecases/churn/test_churn_coordinated_launch.cpp`. The peer **re-runs
  the correctness gate** before co-running (RULE-1 holds on every member).
  See [churn-solution.md](../use-cases/churn-solution.md).
- **A cross-node connector ingest** (`connect::install_connector_fetch_launch_runner`,
  #833): the payload carries **no data at all** — a statement handle, the result
  set's per-chunk row counts, the target table's ids, and a *saved-connection
  name*. Each participant derives the same tiling from the roster, pulls only its
  own tile straight from the source, and lands it as partitions it owns. The
  participants then agree with one `allreduce` over the launch id, so a node that
  stops mid-fetch fails the whole ingest. See
  [../data/distributed-connector-ingest.md](../data/distributed-connector-ingest.md).
- **Already-partitioned tables** (the #531/#533 near-data path): the natural
  second consumer — a launch kind whose runner calls
  `run_near_data_featurize_train_job` over the partitions the node already
  owns (no data in the payload at all). Today that path still co-launches
  in-process (the testkit threads); moving it onto this seam is the
  UC-1-at-scale follow-on.

## Limits / later slices

M-3 adds the `segmentation` launch kind (#1042). Its validated payload carries
the resolved table identity and feature contract, never feature rows. Every peer
trains over its owned partitions with the same config and unique Run round tag.
The initiator returns a Run immediately while a runtime task drives its shard;
peers use the existing owner-loop launch listener. The initiator writes both
output tables even when it is not the collective root. Final assignments are
gathered and broadcast for that single-writer step; training stays distributed.
Missing owners, changed tables or roster disagreements fail rather than train a
partial dataset. See [segmentation.md](../ml/segmentation.md).

- **Membership races.** The roster is snapshotted at launch; a node that joins
  between fan-out and the first round makes the collectives wait on a
  non-participant — the run fails with the bounded timeout (the same window
  the near-data jobs already live with). Retry is the caller's choice.
- **At-most-once.** No launch acks or retries; see "Ephemeral by design".
- **No traceparent yet.** Each member's co-run records its own trace; carrying
  the initiator's `traceparent` in the envelope (the steal-offer pattern) so
  one trace spans the whole coordinated run is a natural next step.
- **Payload size.** Shipping encoded rows suits interactive-scale data; at
  scale, use a launch kind over already-partitioned tables (above).

## See also

- [job-coordinator.md](job-coordinator.md) — the SPMD execution model + the
  signal-handler chain this listener joins.
- [../cluster/signal-plane.md](../cluster/signal-plane.md) — the Task envelope
  + threading contract.
- [../cluster/collectives.md](../cluster/collectives.md) — the combine this
  launch exists to feed.
- [../use-cases/churn-solution.md](../use-cases/churn-solution.md) — the first
  launch kind end to end.
