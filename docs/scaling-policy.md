# Job objective / reward model (ScalingPolicy)

The **`ScalingPolicy`** (`include/zetta/scaling_policy.hpp`) is the per-job
**reward objective an autoscaler optimizes** — part of the **native distributed
accelerated compute** pass for UC-1 churn (`#264`). It realizes **ADR-29** and
entity **ENT-34**, and sits beside the sibling slices in the same pass: the
[ComputeJob](compute-jobs.md) record (ENT-31, which carries a policy by id), the
[Provisioner seam + node capability model](provisioner.md) (ENT-32), and the
[ComputeDevice](compute-device.md) accelerator seam (ARC-9).

A policy answers one question for a running job: **what should the autoscaler do
next?** Grow the fleet, shrink it, or hold — to satisfy one of three objectives,
always inside a hard **bounds** envelope:

- **minimize cost** — run as cheaply as possible (favor the node floor),
- **runtime target** — finish by a runtime/latency deadline (`target_seconds`),
- **data shape** — size the fleet to the shape/size of the work.

This slice is a **pure value type + boundary validation + the objective
evaluation** — no autoscaler loop, no replication, no `Node` CRUD. It is modeled
exactly like its sibling value types `NodeCapability` / `ProvisionedNode`
([provisioner.md](provisioner.md)): a JSON `to_json` / non-throwing `from_json`
boundary plus a pure `evaluate()`. A `ComputeJob` references a `ScalingPolicy` by
id (its `scaling_policy` field) — persisting/replicating policies as their own
entity and wiring `evaluate()` into the live job coordinator are **later slices**.

## The value type

```cpp
struct ScalingPolicy {
    ScalingObjective objective = ScalingObjective::MinimizeCost;
    ScalingBounds    bounds;
    double           target_seconds = 0.0;  // RuntimeTarget runtime/latency target

    std::uint32_t  clamp_nodes(std::uint32_t nodes) const;
    ScalingDecision evaluate(const ScalingSample& sample) const;

    nlohmann::json to_json() const;
    static std::optional<ScalingPolicy> from_json(const nlohmann::json&);
};
```

### Objective — `ScalingObjective`

| Value | Stored name | Meaning |
|---|---|---|
| `MinimizeCost` | `minimize_cost` | run as cheaply as possible (favor `min_nodes`) |
| `RuntimeTarget` | `runtime_target` | finish by `target_seconds` (runtime/latency) |
| `DataShape` | `data_shape` | size the fleet to the work/data shape |

An **append-only** enum whose lowercase name is the stored identifier (so never
reorder it). `runtime/latency` share the one `target_seconds` field: for a batch
compute job it is the total wall-clock to finish; richer per-request latency
targets reuse the same field.

### Bounds — `ScalingBounds`

| Field | Type | Meaning |
|---|---|---|
| `min_nodes` | `std::uint32_t` | floor on the node count (`>= 1` — a job needs a node) |
| `max_nodes` | `std::uint32_t` | ceiling (`>= min_nodes`) |
| `cost_cap` | `double` | spend ceiling in abstract cost units; `0` = **unlimited** |

The **cost cap is a hard constraint**: in `evaluate()`, a breached cap sheds the
fleet back to `min_nodes` no matter what the objective would otherwise want.

## Objective evaluation — `evaluate(sample)`

`evaluate()` is **pure and deterministic** (same policy + sample → same
decision). A `ScalingSample` is a live snapshot of the job — the three signals
the acceptance criterion names plus the current allocation:

```cpp
struct ScalingSample {
    double        progress = 0.0;          // fraction complete, [0, 1]
    double        elapsed_seconds = 0.0;   // wall-clock since the job started
    double        cost = 0.0;              // accumulated cost so far
    std::uint32_t nodes = 0;               // nodes currently allocated
};
```

It returns a `ScalingDecision`:

| Field | Meaning |
|---|---|
| `reward` | the scalar objective value — **higher is better**; the autoscaler maximizes it |
| `recommended_nodes` | the next node count, always inside `[min_nodes, max_nodes]` |
| `action` | `scale_up` / `hold` / `scale_down`, relative to `sample.nodes` |
| `within_cost_cap` | `false` once `cost` exceeds a set cap |
| `reason` | a human-readable explanation |

The decision rules, in order:

1. **Cost cap (hard constraint, all objectives).** If a cap is set and
   `cost > cost_cap`, recommend `min_nodes`, set `within_cost_cap = false`, and
   `reward = -cost`. This overrides the objective.
2. **`MinimizeCost`.** Recommend `min_nodes` (cheapest viable); `reward = -cost`
   (a pricier sample scores strictly worse).
3. **`RuntimeTarget`.** Project total runtime as `elapsed / progress` (a
   linear-throughput estimate), then scale the fleet linearly toward hitting the
   target: behind (`projected > target`) → add nodes, ahead → release them.
   `reward = -|projected - target|`, so reward **peaks at 0 exactly on target**
   and falls off in either direction (late *or* wastefully early). Before the
   first progress sample (`progress <= 0`) there is no throughput signal, so it
   **holds** at the current allocation with neutral reward.
4. **`DataShape`.** Size the fleet to the remaining work `(1 - progress)` across
   the node envelope: lots left → toward `max_nodes`, nearly done → toward
   `min_nodes`. `reward = -|nodes - desired|` (peaks at 0 when the current
   allocation already matches). In this slice "data shape" is approximated by
   remaining work; a richer data-size signal plugs in behind the same
   `evaluate()` later.

## JSON boundary + schema

`ScalingPolicy` has a **validated JSON boundary** (`to_json` / non-throwing
`from_json`), the same pattern as `NodeCapability` and
[`DeploymentTarget`](deployments.md). The JSON shape:

```json
{
  "objective": "runtime_target",
  "bounds": { "min_nodes": 2, "max_nodes": 12, "cost_cap": 250.0 },
  "target_seconds": 300.0
}
```

`cost_cap` is omitted when `0` (unlimited); `target_seconds` is omitted when `0`
(unset). `from_json` returns `std::nullopt` on anything malformed so a bad policy
fails fast instead of silently widening what the autoscaler will do:

- not an object, a missing/wrong-typed field, or an unknown objective,
- `min_nodes < 1`, or `max_nodes < min_nodes`,
- a negative `cost_cap` or `target_seconds`,
- a `runtime_target` objective with no positive `target_seconds` (a runtime
  target with no target is meaningless).

The JSON shape is documented by
[`schemas/scaling_policy.schema.json`](../schemas/scaling_policy.schema.json),
which the `from_json` boundary checks **mirror** — keep the two in sync. The
parser accepts an integer whether nlohmann stored it signed or unsigned, so a
hand-written `{"min_nodes": 2}` and a round-tripped value both parse.

## Tests

`tests/test_scaling_policy.cpp` covers the enum names, `clamp_nodes`, the
objective evaluation for each of the three objectives over `(progress, elapsed,
cost)` samples (scale up when behind, down when ahead, reward peaks on target /
on a matched allocation), the cost cap as a hard constraint, and the JSON
round-trip + boundary rejection.
