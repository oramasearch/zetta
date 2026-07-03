# Autoscaler control loop (JobAutoscaler)

The **`JobAutoscaler`** (`include/zetta/compute/job_autoscaler.hpp`) is the control loop
that finally *acts on* a job's reward objective. The
[`ScalingPolicy`](scaling-policy.md) slice computes a `ScalingDecision`; this loop
**closes the loop**: each tick it observes a running
[`ComputeJob`](compute-jobs.md)'s live signals, scores them with the policy, and
**adds/removes nodes through the [`Provisioner`](../cloud/provisioner.md) seam** — inside
the policy's bounds, with hysteresis so it does not thrash. It realizes
**ADR-29 / ARC-11** in the native distributed accelerated-compute pass for UC-1
churn (`#264`, `#371`).

It is the consumer the sibling slices forward-reference: the
[scaling-policy](scaling-policy.md) reward model, the
[provisioner](../cloud/provisioner.md) seam (Local `#374` + the cloud backends), the
[job-coordinator](job-coordinator.md) (which runs the job this loop sizes the
fleet for), and the work-stealing scheduler's [elastic
membership](work-stealing-scheduler.md) (which keeps an in-flight job correct
while this loop changes the fleet).

## How it fits

```
                observe (progress / elapsed / cost)
                          │
   ┌──────────────────────▼───────────────────────┐
   │  JobAutoscaler::tick()                         │
   │    sample = { observation, nodes() }           │   nodes() ← provisioner.provisioned()
   │    decision = ScalingPolicy::evaluate(sample)  │
   │    hysteresis (confirm_ticks consecutive)      │
   └──────────┬──────────────────────┬─────────────┘
        scale up │              scale down │
   provision(count=target)   teardown(node_id) ×N
              └────────► Provisioner ◄────────┘
```

The loop is **tick-driven and owns no thread** — exactly like the
[deployment controller](../pipeline/deployment-controller.md)'s `reconcile()`, which `Node`
drives from `tick()`. The caller (the job coordinator, the headless/headed loop)
calls `tick()` each heartbeat. That keeps it **deterministic** and unit-testable:
a test ticks it by hand and asserts the fleet.

```cpp
JobAutoscaler autoscaler{
    job,                                   // const ComputeJob& — steered only while Running
    policy,                                // the resolved ScalingPolicy (reward objective)
    provisioner,                           // any Provisioner (Local, a cloud backend, the cost-cap gate)
    [&] { return meter.observe(); },       // ObservationSource: progress / elapsed / cost
    AutoscalerConfig{/*node_spec=*/spec, /*confirm_ticks=*/2, /*shed_on_cost_breach=*/true}};

for (/* each heartbeat */) {
    const AutoscaleTick result = autoscaler.tick();
    // result.confirmed_action / acted / nodes_before → nodes_after / detail
}
```

Because it programs against the abstract `Provisioner&`, the
[`BurstApprovalGate`](../cloud/burst-approval-gate.md) drops in transparently: wrap the
real backend once, and every scale-up the loop issues is cost-capped without the
loop changing.

## One tick

`tick()` does five things, in order:

1. **Read the allocation.** `nodes()` returns `provisioner.provisioned().size()`.
   Reading it **every tick** is what makes the loop reflect live, **elastic
   membership** for free: a node that left the cluster simply stops appearing, and
   the loop re-provisions toward the target on its own (no extra wiring). A
   non-`Running` job is a no-op — the loop holds and resets its hysteresis state.
2. **Observe.** Call the `ObservationSource` for the job's live `progress`,
   `elapsed_seconds`, and `cost`. (The current node count is *not* observed — it
   is read from the provisioner in step 1 and filled into the sample.) These three
   signals plus the node count are the `ScalingSample`.
3. **Score.** `policy.evaluate(sample)` → a `ScalingDecision` (recommended nodes,
   a `scale_up`/`hold`/`scale_down` action, the reward, `within_cost_cap`).
4. **Apply hysteresis** (below).
5. **Act**, if a move is confirmed: grow with `provision(count = target)` (the
   "ensure `target` nodes total" semantic every backend implements), or shrink by
   tearing down the surplus nodes (LIFO, from the tail of the fleet). The
   recommendation is already clamped to `[min_nodes, max_nodes]` by the policy.

It returns an `AutoscaleTick` recording what happened: `job_running`, the
`sample`, the raw `decision`, the `confirmed_action` actually taken, whether it
`acted`, `nodes_before` → `nodes_after`, and a human-readable `detail`.

## Hysteresis — the anti-thrash guard

A naive loop that acted on every recommendation would flap: one noisy progress
sample nudges the projection, the fleet grows, the next sample nudges it back, and
so on. The loop requires a direction to be recommended on **`confirm_ticks`
consecutive ticks** before it moves the fleet (default **2**). A `hold`
recommendation — or a flip to the opposite direction — **resets** the streak, so
an alternating up/down signal never acts. After a confirmed move the streak resets
so the next move must re-confirm.

`confirm_ticks = 1` disables the multi-tick confirmation (act on the first
recommendation), but the loop still never moves the fleet more than once per tick.

**The cost cap is the exception.** When the policy reports the cap breached
(`within_cost_cap == false`), the loop **sheds immediately**, bypassing the
confirmation window (`shed_on_cost_breach`, default `true`) — the cost cap is a
hard safety constraint (it already overrides the objective inside `evaluate()`), so
the loop does not wait out hysteresis before cutting spend.

## Given hardware degrades to best-effort

With the given-hardware [`LocalProvisioner`](../cloud/provisioner.md), `provision` is
bounded by the existing cluster and `teardown` is a no-op. So a **scale-up** is
best-effort (it uses whatever nodes are available) and a **scale-down** keeps the
nodes — **max utilization of fixed hardware**. The loop does not fail in that
case: it records the shortfall in the tick `detail` (e.g. *"best-effort: released
0 of 2 node(s) (given hardware cannot release)"*) and `acted` is `false`. The
cumulative counters (`total_scale_ups()` / `total_scale_downs()`) count
**effective** fleet changes, so a best-effort no-op is not miscounted.

## Configuration + JSON boundary

`AutoscalerConfig` carries a **validated JSON boundary** (`to_json` /
non-throwing `from_json`), the same pattern as
[`NodeCapability`](../cloud/provisioner.md) and [`BurstCap`](../cloud/burst-approval-gate.md), so a
later slice can persist / replicate / GUI-edit it. The JSON shape:

```json
{
  "node_spec": { "cores": 8, "memory_bytes": 17179869184, "gpu": { "kind": "none" } },
  "confirm_ticks": 2,
  "shed_on_cost_breach": true
}
```

| Field | Meaning | Default |
|---|---|---|
| `node_spec` | the per-node [capability](../cloud/provisioner.md) floor for provisioned nodes | no floor (any node) |
| `confirm_ticks` | consecutive same-direction ticks before acting (`>= 1`) | `2` |
| `shed_on_cost_breach` | a cost-cap breach sheds immediately, bypassing `confirm_ticks` | `true` |

`from_json` returns `std::nullopt` on anything malformed (not an object, an
invalid `node_spec`, a `confirm_ticks` that is not an integer `>= 1`, a non-boolean
`shed_on_cost_breach`) so a bad config fails fast instead of silently changing how
the loop behaves. Omitted fields take their defaults. The shape is documented by
[`schemas/autoscaler_config.schema.json`](../../schemas/autoscaler_config.schema.json),
which the `from_json` boundary checks **mirror** — keep the two in sync.

## Scope / later slices

- **No new replicated state.** The policy is passed in by value (a `ComputeJob`
  still references it by id only; persisting policies as their own replicated
  entity is a later slice), the observation rides a callable, and the only JSON
  boundary is the `AutoscalerConfig` DTO.
- **The observation source is injected.** Wiring the loop to a real progress/cost
  meter inside the [job coordinator](job-coordinator.md) (round count / total,
  accumulated node-seconds) is a follow-up; this slice ships the steering loop and
  the seam.
- **Driven by the caller.** Mounting the loop into `Node::tick()` / the coordinator
  for a live, end-to-end autoscaled job is a later wiring slice; the loop and its
  deterministic test land here.

## Tests

`tests/compute/test_job_autoscaler.cpp` is the deterministic acceptance suite (a simulated
job + a fake provisioner): it scales **up under a runtime target** and **down
under a cost objective**, sheds immediately on a **cost-cap breach**, holds the
fleet on an **alternating signal** (hysteresis), degrades to **best-effort** under
the given-hardware `LocalProvisioner`, **re-provisions after a membership loss**
(elastic membership), is a no-op for a **non-running** job, and round-trips +
rejects the `AutoscalerConfig` JSON boundary.

## See also

- [scaling-policy.md](scaling-policy.md) — the reward model this loop reads.
- [provisioner.md](../cloud/provisioner.md) — the seam this loop acts through (+ the
  given-hardware `LocalProvisioner`).
- [burst-approval-gate.md](../cloud/burst-approval-gate.md) — the cost-cap decorator that
  drops in front of the provisioner this loop holds.
- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` lifecycle record this loop
  steers while it is `Running`.
- [job-coordinator.md](job-coordinator.md) — runs the job this loop sizes the
  fleet for; the future home of the live observation source + the tick driver.
- [work-stealing-scheduler.md](work-stealing-scheduler.md) — elastic mid-job
  membership keeps an in-flight job correct while this loop changes the fleet.
