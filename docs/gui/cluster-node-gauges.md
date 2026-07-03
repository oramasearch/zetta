# Animated per-node resource gauges (#584)

The headed **Cluster** panel renders **live, animated resource gauges + sparklines
on each node box** — CPU / memory / GPU + VRAM / storage / network in-out, with %
utilization — so the cluster's load reads at a glance as moving bars, not static
numbers. This is area 2 of the cluster observability epic
([#575](https://github.com/oramasearch/zetta/issues/575)), building on the
[spatial node-box canvas](../cluster/cluster.md#spatial-node-box-view-the-headed-canvas)
(#582) and the [per-node telemetry source](../cluster/node-metrics.md) (#581).

## What you see

Each node box (in `App::draw_cluster`, `src/gui/app_operations.cpp`) carries, below
its identity header (health dot + short id + address + sync state), a compact
**gauge strip** of five rows — `CPU` / `MEM` / `GPU` / `DSK` / `NET`. Each row is a
rounded meter with:

- an **eased fill bar** to the current utilization — it *glides* toward the live
  value every frame, so the bar moves smoothly between the ~1 Hz telemetry samples
  (the animation);
- a faint **sparkline** of the recent history drawn over the track (one point per
  sample), so the trend reads at a glance;
- a compact **right-hand read-out** (a `%` for the load gauges, a short rate like
  `334k` for network).

Selecting a node shows the same five gauges in the **detail pane** under the
canvas (the `LIVE TELEMETRY` section) with fuller read-outs — core count, byte
totals, and in/out rates (e.g. `16 cores · 9%`, `38.5 GB / 64.0 GB · 60%`,
`1.7 TB / 1.8 TB · 92%`, `in 312.7 KB/s · out 13.9 KB/s`).

### Colours (Orama design system)

The load gauges (CPU / memory / GPU / storage) use the **semantic status palette**
— green under moderate load, **amber** as it climbs (≥ 60%), **red** when saturated
(≥ 85%) — never coral (coral stays the surgical "you are here" accent). Network
throughput is a *flow*, not a good/bad status, so it uses a neutral ink fill and a
relative bar normalized against a decaying rolling peak (no reliable link-capacity
reference exists cross-platform).

### Graceful degradation

A counter the host can't report renders as a faint **n/a** track, never a
misleading 0 — so a CPU-only core build shows `GPU · no GPU on this host`, and a
node before its first CPU%-delta interval shows `CPU · n/a` until a sample
elapses. This mirrors the `null`-for-unavailable rule of
[`system.node_metrics`](../cluster/node-metrics.md#graceful-degradation).

## Where the data comes from — cluster-wide telemetry over the signal plane

`system.node_metrics` measures only **the host the query hits**, so a node can't
see its peers' utilization directly. The gauges are fed by a small **ephemeral
telemetry exchange over the [signal plane](../cluster/signal-plane.md)** — the "future
ephemeral telemetry exchange" the node-metrics doc anticipated — so every node box
(self **and** peers) animates real per-node data:

```
each node, every ~1s (Node::tick):
   sample its own NodeMetrics  →  broadcast a `node.metrics` EVENT to all peers
each node, on receipt:
   cache the peer's sample, keyed by the transport-verified sender id
the GUI, every frame:
   Node::telemetry_snapshot()  →  self + every fresh peer  →  animate the gauges
```

- The engine half is **`zetta::NodeTelemetryExchange`**
  (`include/zetta/node_telemetry.hpp`, `src/core/node_telemetry.cpp`): a
  `SignalHandler` wired as the [`JobCoordinator`](../compute/job-coordinator.md)'s **fallback**
  (so it coexists with the steal / collective signals the coordinator already
  routes), caching each peer's `node.metrics` event and the local node's own
  sample. `Node` drives the throttled sample + `broadcast_event` from `tick()` and
  exposes the cache via **`Node::telemetry_snapshot()`** (this node + every peer
  within the freshness window, node-id ascending). All of this lives in
  **zetta-core** (no SDL) — so it is headless-testable and a headless node
  participates too (it broadcasts + caches, so a headed node sees headless peers).
- Like everything on the signal plane it is **deliberately NOT replicated** (a
  signal is a message, not durable state): telemetry goes stale in seconds, so the
  cache **drops a peer whose last sample aged past the TTL** (default 8s) and
  nothing is gossiped through the CRDT. It is the same *ephemeral, observed*
  altitude as [`system.zetta_cloud_status`](../cloud/cloud-inspector.md) /
  [`system.checkpoints`](../compute/job-checkpoint.md).
- The GUI is a **thin renderer over `telemetry_snapshot()`**: it holds only the
  animation state (`zetta::gui::NodeGaugeState` in `src/gui/cluster_telemetry.hpp`
  — the eased value + sparkline ring per gauge, keyed by node id), updated each
  frame and pruned of nodes that have left the cluster.

### The wire boundary (schema-validated)

A `node.metrics` event's payload is JSON, so it rides a **schema-validated
boundary** like the [`CollectiveMessage`](../cluster/collectives.md) that shares this
transport: `encode_node_metrics` / `decode_node_metrics` round-trip the sample, and
`decode` validates against `node_metrics_wire_schema()` (draft-07, the **code-owned
source of truth**; `schemas/node_metrics.schema.json` is generated from it and
drift-checked by `test_schemas`) before any field is read — so a malformed or
hostile payload fails fast (with a reason) instead of corrupting the cache. An
unavailable gauge group is carried by **omission**, so graceful degradation
round-trips as field presence. The cache keys by the **transport-verified sender
id** (`Signal::from`), not the payload's self-reported `node_id`, so a peer can't
masquerade as another node.

## Performance / cadence

The animation stays within the render cadence and never starves a live resize:

- The headed loop already wakes every ~33 ms (`SDL_WaitEventTimeout`), so the eased
  bars glide at ~30 fps with no extra timer; the per-frame cost is one cheap pass
  (ease + draw), the same budget as the node-box canvas it extends.
- Telemetry **sampling** (the OS-counter reads) is **throttled to ~1 Hz** in
  `Node::tick()` — not per frame — and the CPU / network gauges are deltas measured
  over that interval. The broadcast payload is a few hundred bytes per node per
  second.

## Scope / later slices

- Peer gauges are real now (cross-node telemetry over the signal plane). A node
  that just joined shows an `awaiting telemetry` note until its first broadcast
  arrives (~1s).
- A true network **% of link capacity** still needs a per-interface capacity
  reference that isn't reliably available cross-platform; the network gauge is a
  relative flow until then. GPU/VRAM gauges fill in behind the same `NodeMetrics`
  struct once an accelerator-telemetry binding lands (absent in the CPU-only core
  build today).

## Tests

- `tests/test_node_telemetry.cpp` — the wire boundary (encode/decode round-trip,
  graceful-degradation by omission, malformed-payload rejection at the schema
  boundary) and the exchange cache (record local, receive peers keyed by
  `Signal::from`, ignore other events, expire stale entries).
- `tests/test_schemas.cpp` — drift-checks `schemas/node_metrics.schema.json`
  against `node_metrics_wire_schema()`.
- The gauge rendering itself is a thin GUI renderer (no SDL in tests), verified
  visually like the [node-box canvas](../cluster/cluster.md) (#582).

## See also

- [node-metrics.md](../cluster/node-metrics.md) — the per-node telemetry source
  (`system.node_metrics`).
- [cluster.md](../cluster/cluster.md) — the spatial node-box canvas the gauges render on.
- [signal-plane.md](../cluster/signal-plane.md) — the ephemeral RPC / event transport the
  telemetry exchange rides.
- [collectives.md](../cluster/collectives.md) — the sibling schema-validated payload over the
  same transport.
