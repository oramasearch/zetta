# Node metrics — live per-node resource telemetry (`system.node_metrics`)

`system.node_metrics` is the engine source for the **animated per-node gauges**
of the cluster observability epic ([#575](https://github.com/oramasearch/zetta/issues/575)
area 2): each node box shows live CPU / memory / GPU + VRAM / storage / network
utilization. The GUI is a thin renderer — it polls this table; all the telemetry
logic lives in **`zetta-core`** (`include/zetta/node_metrics.hpp` /
`src/core/node_metrics.cpp`), so it is headless-testable and carries no SDL.

## What it is

A node samples **its own host's** OS counters at query time and emits one row:

```sql
SELECT * FROM system.node_metrics;
SELECT node_id, cpu_percent, memory_percent FROM system.node_metrics;
```

Like [`system.zetta_cloud_status`](../cloud/cloud-inspector.md) and
[`system.checkpoints`](../compute/job-checkpoint.md), it is an **ephemeral, observed** read
view — **no CRDT backing, never replicated**. A query reads live counters here
and now; nothing is stored or gossiped. This is the deliberate exception to
"EVERYTHING is replicated state": telemetry is a measurement, not durable
agreement.

A node can only measure the host it runs on, so the table returns **exactly one
row — the node the query hits**. A cluster's *full* picture is gathered separately,
over the [signal plane](signal-plane.md): the **ephemeral telemetry exchange**
(`zetta::NodeTelemetryExchange`, #584) has each node broadcast its own
`NodeMetrics` to its peers as a `node.metrics` event and every node cache what it
receives, so `Node::telemetry_snapshot()` returns this node + every fresh peer.
That is what the headed cluster view's [animated per-node gauges](../gui/cluster-node-gauges.md)
animate. It is deliberately **not** routed through `system.node_metrics` (which
stays the single-host read view) nor the CRDT — telemetry is a measurement, not
durable agreement, so it rides the ephemeral signal plane and goes stale in
seconds.

## Columns

| Column | Meaning |
| --- | --- |
| `node_id` | this node's id (`Node::node_id()`) |
| `cpu_cores` | logical cores (`std::thread::hardware_concurrency()`) |
| `cpu_percent` | CPU utilization 0..100 across all cores |
| `memory_used_bytes` / `memory_total_bytes` | physical RAM used / total |
| `memory_percent` | RAM utilization 0..100 |
| `gpu_present` | whether a GPU gauge is available (false in a CPU-only core build) |
| `gpu_percent` | GPU utilization 0..100 |
| `vram_used_bytes` / `vram_total_bytes` | VRAM used / total |
| `storage_used_bytes` / `storage_total_bytes` | the `data_dir` filesystem used / total |
| `storage_percent` | storage utilization 0..100 |
| `net_rx_bytes_per_sec` / `net_tx_bytes_per_sec` | network receive / transmit rate |
| `net_percent` | link utilization 0..100 |
| `sampled_at` | ISO-8601 UTC stamp of the sample |

## Graceful degradation

A counter a host can't report is emitted as **`null`** rather than a misleading
zero. Each gauge group carries an availability flag in `NodeMetrics`; the
`system.node_metrics` projection maps an unavailable group's values to `null`:

- **GPU + VRAM** are **absent in `zetta-core`** — no CUDA/NVML may leak into the
  core (the same discipline that keeps SDL/CUDA out of the core headers), so
  `gpu_present` is false and the GPU/VRAM columns are `null`. A future
  accelerator-telemetry binding fills them behind the same `NodeMetrics` struct.
- **Network utilization** (`net_percent`) needs a link-capacity reference that
  isn't reliably available cross-platform, so it degrades to `null`; the rx/tx
  **rates** are still reported when the host exposes interface counters.
- **CPU utilization** is a delta measurement — the sampler seeds a baseline on
  construction, so the first sample after any real elapsed time has an interval
  to measure over; if none has elapsed yet, `cpu_percent` is `null`.

## How the sampler works

`zetta::NodeMetricsSampler` is a small, **stateful, thread-safe** per-node object
(`Node` owns one; `Node::sample_node_metrics()` is the accessor). Absolute gauges
(cores, memory, storage) need no history; the **CPU and network gauges are
rates** computed from the delta between consecutive samples, so the sampler holds
the previous reading. The OS-specific reads are confined to the `.cpp`:

- **macOS** — `host_statistics`/`host_statistics64` (CPU ticks + VM stats),
  `sysctlbyname("hw.memsize")`, `getifaddrs` (`AF_LINK` `if_data` rx/tx).
- **Linux** — `/proc/stat` (CPU), `/proc/meminfo` (RAM), `/proc/net/dev`
  (network).
- **Both** — `statvfs` over the `data_dir` for storage.

The header holds **no platform types**, so it is safe to include from `Node`.
Multiple nodes in one process (e.g. a loopback test cluster) each sample the same
shared host, so they report identical host-wide gauges.

## No new JSON boundary

The telemetry originates from **OS syscalls, not parsed JSON**, and the row JSON
is *emitted* (computed at query time) — like every other `system.<table>`
projection. There is no untrusted JSON entering the engine here, so there is no
schema boundary to validate (and `test_schemas` confirms none was added).

## Tests

- `tests/test_node_metrics.cpp` — the sampler: timestamp + cores present, the
  memory/storage gauges are consistent (total > 0, used ≤ total, percent in
  [0,100]), a missing storage path degrades gracefully, CPU% becomes available
  across an interval, GPU is absent in the core build, and network utilization
  degrades.
- `tests/test_query.cpp` (`QueryService.SelectsNodeMetricsSystemTable`) — the SQL
  read view: `SELECT * FROM system.node_metrics` returns one row for this node,
  GPU/`net_percent` are `null`, storage is populated, and projection works.

## See also

- [query.md](../data/query.md) — the `system` database + the other read views.
- [cloud-inspector.md](../cloud/cloud-inspector.md) / [job-checkpoint.md](../compute/job-checkpoint.md)
  — the sibling ephemeral, non-replicated read views.
