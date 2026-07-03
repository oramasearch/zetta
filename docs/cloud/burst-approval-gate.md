# Large-burst cost-cap approval gate

The **`BurstApprovalGate`** (`include/zetta/cloud/burst_approval_gate.hpp`) guards the
[`Provisioner`](provisioner.md) seam against runaway burst cost. It realizes
**APR-3** and protects against **RSK-9** ("runaway burst cost") in the native
distributed accelerated-compute pass for UC-1 churn (`#264` / ARC-13, `#379`).

Before a provision request that exceeds a configured **node/cost cap** proceeds,
it must pass an **approval gate**, and the gate's decision is recorded in the
[audit trail](../governance/audit-trail.md). A request **within** the cap proceeds untouched.

## How it fits

The gate is a **decorator** over the `Provisioner` interface — it *is* a
`Provisioner` that wraps another one. So it drops in transparently wherever the
autoscaler / job coordinator already holds a `Provisioner&`: wrap the real
backend (Local `#374`, AWS `#377`, GCP `#378`) once, and every `provision()` call
is gated without the caller changing.

```cpp
LocalProvisioner backend{...};                       // the real backend
BurstApprovalGate gate{backend,
                       BurstCap{/*max_nodes=*/16, /*max_cost=*/5000.0},
                       node /*AuditSink*/,            // records over-cap decisions
                       my_approver};                  // decides over-cap bursts
autoscaler.use(gate);                                 // sees a plain Provisioner&
```

- **`provision`** is the only gated call.
  - **Within the cap** → delegate straight to the inner backend. Unaudited: the
    gate governs only the exception, so a steady stream of small autoscaler
    provisions does not flood the audit trail.
  - **Over the cap** → consult the **approver** (deny-by-default), record the
    decision in the audit trail, then either delegate (approved) or return a
    blocked, unsatisfied `ProvisionResult` **without touching the inner backend**
    — so nothing comes up and no cost is incurred.
- **`teardown`** / **`provisioned`** are **not** gated (they reduce or merely
  report cost) — they delegate straight to the inner backend.

## The cap (`BurstCap`)

A configured ceiling with two **independent, optional** dimensions; `0` on an
axis means *unlimited* on that axis. A request trips the cap when it exceeds
**either** active axis.

| Field | Type | Meaning |
|---|---|---|
| `max_nodes` | `std::size_t` | how many nodes one provision call may bring up (`0` = unlimited) |
| `max_cost` | `double` | ceiling on the burst's estimated cost (`0` = unlimited) |

`caps_nodes()` / `caps_cost()` report whether each axis is active.

### Cost estimation (`CostEstimator` + `default_burst_cost`)

The cost axis needs a number to compare against `max_cost`. That comes from a
pluggable **`CostEstimator`** (`std::function<double(const ProvisionRequest&)>`).
The built-in **`default_burst_cost`** is a coarse, transparent estimate in
**relative compute units** (not dollars):

```
cost = count × (spec.cores + spec.gpu.throughput_tflops)
```

So a GPU burst weighs far heavier than a CPU burst — a 2-node A100 burst (8 cores
+ 312 TFLOPS each) is `2 × 320 = 640` units, while a 10-node CPU burst (8 cores
each) is `80`. That matches the real shape of RSK-9: the runaway cost is the
*GPU* fleet. It is an honest placeholder — pass a custom `CostEstimator` (e.g.
real $/node-hour cloud pricing) when the AWS/GCP backends land.

## The approval seam (`BurstApprover`)

When (and only when) a request is over the cap, the gate calls the
**`BurstApprover`**:

```cpp
using BurstApprover =
    std::function<bool(const ProvisionRequest&, const BurstEstimate&)>;
```

It returns `true` to approve (the burst proceeds) or `false` to block it (pending
approval). The `BurstEstimate` it receives explains *why* the burst tripped the
cap (the node count, the estimated cost, and which axis/axes were exceeded), so a
human or policy can decide with the numbers in hand.

There is **no default approver**: an over-cap burst with no approver wired is
**blocked** — *deny-by-default*, mirroring the
[key-scoped access model](../governance/access-control.md). A production deployment wires the
approver to a human approval workflow; whichever way it decides, the gate audits
the outcome.

> This slice's approver is a **synchronous policy** — it decides in-line. A
> durable "pending approval" queue (a replicated entity an operator approves
> out-of-band) is a natural follow-on, but the gate above it stays the same: an
> over-cap burst is held until the approver says yes.

## Audit trail

Every time the gate **engages** (an over-cap burst) it records one
[`AuditEvent`](../governance/audit-trail.md) via the `AuditSink` it was constructed with:

| Field | Value |
|---|---|
| `action` | `provision_requested` (a new `AuditAction`) |
| `allowed` | the outcome — `true` if approved, `false` if blocked |
| `target` | the burst size, e.g. `"10 nodes"` |
| `detail` | which axis/axes were exceeded + the outcome, e.g. `"burst of 10 node(s), nodes 10 > cap 4 — blocked pending approval"` |

`provision_requested` is added to the closed `AuditAction` enum and to the
AuditEvent JSON Schema (`schemas/audit_event.schema.json` + the embedded mirror
in `src/core/governance/audit_event.cpp`), so the new action validates at the storage
boundary like every other audit value. A null `AuditSink` means "not audited"
(e.g. a gate used in isolation in a test).

## JSON boundary + schema

`BurstCap` has a **validated JSON boundary** (`to_json` / non-throwing
`from_json`), following the same pattern as
[`NodeCapability`](provisioner.md): `from_json` returns `std::nullopt` on
anything malformed (not an object, a wrong-typed field, a negative number), so a
malformed cap fails fast instead of silently disabling the gate. The shape is
documented by [`schemas/burst_cap.schema.json`](../../schemas/burst_cap.schema.json),
which the `from_json` boundary checks **mirror** — keep the two in sync. The JSON
form exists for forward use (configuring / replicating a cap); this slice keeps
the gate in-memory and does not yet persist it.

## Tests

`tests/cloud/test_burst_approval_gate.cpp` covers the acceptance criteria and the value
types:

- **below the cap proceeds** — a within-cap (and at-cap) burst passes straight
  through to the inner backend and is **not** audited.
- **above the cap is blocked + audited** — an over-cap burst with no approver is
  blocked (inner backend never called, empty unsatisfied result) and records a
  `provision_requested` audit event with `allowed == false`.
- **approved over-cap proceeds + is audited** — with an approver that says yes,
  the burst reaches the inner backend and records `allowed == true`.
- **the cost dimension** — an expensive GPU burst trips the cost cap while a cheap
  CPU burst of the same node count does not; either axis can trip the cap
  independently; a custom `CostEstimator` overrides the default.
- **teardown / listing delegate** — they are not gated.
- **unlimited cap** — a `BurstCap{0, 0}` never engages, even for a 1000-node GPU
  burst.
- **`BurstCap` JSON boundary** — round-trips through `to_json`/`from_json`
  (including single-axis and empty/unlimited docs), and rejects malformed
  documents.
- **default cost model** — `default_burst_cost` weights GPU throughput heavily
  and is zero for a zero-count burst.

## Composed in the engine (TC-12, #502)

The gate's first end-to-end consumer is the **cost-cap-gated add-nodes binding**:
when a lowered sim-Python `cluster.add_nodes(…, cost_cap=…)` step runs through
the Node-aware [`run_add_nodes_step`](add-nodes.md), the binding wraps the
injected provisioner in a `BurstApprovalGate` (`max_cost` = the script's cap, the
Node as the `AuditSink`, deny-by-default) for that call — so the cap holds
identically for a cloud backend and the hermetic `LocalProvisioner`. The
`cluster.uc1_at_scale` testkit case
([uc1-at-scale-e2e.md](../use-cases/uc1-at-scale-e2e.md)) drives it end to end:
the canonical program's within-cap burst passes, an over-cap probe is blocked +
audited, and the whole burst lifecycle (rebalance on, train, drain back, revoke)
runs behind it.

See also [provisioner.md](provisioner.md) (the seam this gate wraps) and
[audit-trail.md](../governance/audit-trail.md) (where its decisions are recorded).
