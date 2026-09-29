# Memory accounting

Zetta reports what owns process memory through `system.memory`,
`system.node_metrics`, `GET /v1/node_memory`, and the matching `node_memory` MCP
tool. The registry is local to each node and sampled on demand; it is observed
state, not CRDT state.

## Reading `system.memory`

```sql
SELECT subsystem, bytes, items, share_of_in_use
FROM system.memory
ORDER BY bytes DESC;
```

Each probe returns resident bytes it owns, an item count, and optional reserved
bytes. `share_of_in_use` compares owned bytes with allocator in-use bytes. The
same row also carries process `rss_bytes`, allocator `allocator_in_use_bytes`,
allocator `allocator_free_held_bytes`, anonymous bytes, and the sample time.
The synthetic `unattributed` row is allocator in-use minus the sum of probes,
clamped at zero. A large unattributed value means an allocation has no probe yet
or an estimate is low. If probes exceed allocator in-use, the API reports the
excess as `over_attributed_bytes` instead of wrapping the subtraction.

`reserved_bytes` is address space that should not be compared with allocator
in-use. Runtime thread stacks are reported there because an operating system
stack reservation is not fully resident or allocated from the heap.

On Linux, allocator truth comes from `mallinfo2()`, RSS and peak RSS from
`/proc/self/status`, and anonymous memory from `/proc/self/smaps_rollup`. On
macOS, Zetta sums malloc-zone statistics and reads resident size through Mach.
Availability flags in `node_memory` distinguish an unsupported measurement from
a real zero.

## Subsystems

| Row | What it owns | Growth and bound |
| --- | --- | --- |
| `audit` | flat fingerprint and per-origin seek indexes, reusable segment cache, bounded recent window, pending persistence estimate, and read-summary windows | Index memory grows with retained event count. The recent window is bounded by `audit_recent_events`; retention bounds ordinary event age. No per-event catalog values or tombstones remain (#1164). |
| `pubsub` | topic and inbox event stores | Governed by the detected/configured pub/sub memory budget. Inspect `system.pubsub_topics` for the topic-level breakdown. |
| `replication` | Automerge document estimate, encoded-value cache, parsed snapshot, and bounded change journal, excluding audit values | Current replicated metadata grows with live and tombstoned keys. Cache rebuilds and legacy-audit retirement remeasure counters from current entries; subtraction clamps at zero. The pending journal is capped at 65,536 changes. |
| `datasets_tables` | database/table objects, resident JSONL rows, Parquet id indexes, and decoded page caches | JSONL rows are fully resident. Parquet decoded pages follow the configured cache budget; positional ids avoid a per-row id table. |
| `registry_sessions_connections` | node registries, queued owner work, capability declarations, active HTTP parsers/sessions, idle connection cache, requests, and long-poll waiters | Entity registries follow their durable catalogs. HTTP connections and idle objects follow server admission/cache limits; long polls are coroutine waiters, not threads. |
| `runtime` | coroutine frames and blocking-pool work | Work counts should fall after calls finish. Worker thread stack reservations appear in `reserved_bytes`. |
| `models` | registered model metadata plus loaded native weights and tokenizers | Loaded weight tensors dominate this row. `ModelCache` capacity bounds resident checkpoints. |
| `runs_jobs` | in-memory Run/ComputeJob projections and bounded pipeline instrumentation | Durable registries grow with retained records; pipeline instrumentation keeps the latest 512 entries. Checkpoint bodies stay on disk and are not counted as resident memory. |

Probe byte values are inexpensive ownership estimates, not object-layout proofs.
Allocator and RSS fields remain the authority for total memory. The difference is
kept visible instead of being assigned to an arbitrary owner.
The replication estimate is re-baselined whenever a document is restored or
legacy audit history is removed. This prevents retired catalog entries from
leaving stale counters that wrap the unsigned `bytes` or `items` fields (#1167).
The audit row counts retained segment ids without decoding historical events.
`system.node_metrics.audit_memory_bytes` uses the same ownership calculation;
the [replicated million-event proof](../testing/audit-replication.md) also checks
that this row counts one million events on each node after convergence/restart.

## Logs and status

At startup, every five minutes, and whenever RSS moves into another 256 MiB
band, the node writes one INFO line:

```text
zetta: INFO memory: rss 812M in-use 790M audit 610M pubsub 1M unattributed 40M free-held 22M
```

The headless heartbeat status ends with the current RSS and largest owner:

```text
| mem rss 812M top audit 610M
```

## Allocator knobs

For glibc nodes with many runtime and dispatch threads, set `MALLOC_ARENA_MAX`
in the process environment when many arenas retain more free memory than the
workload needs. Start conservatively and validate throughput and
`allocator_free_held_bytes`; fewer arenas may increase allocator contention.

`ZETTA_MALLOC_TRIM_THRESHOLD_BYTES` opts a Linux node into periodic
`malloc_trim(0)`. When free-held bytes meet the positive threshold, Zetta tries
the trim no more than once per five-minute reporting interval and logs whether
the allocator reported a release. Other platforms report the operation as
unsupported. Trimming is off by default because it can trade resident memory for
later page faults.

## API parity

`GET /v1/node_memory` and MCP `tools/call` with `name: "node_memory"` use the
same capability declaration, output schema, authorization, audit, and handler.
The returned subsystem list is ordered by descending bytes. The parity regression
also verifies identical replication byte/item values on both transports. OpenAPI
is generated from that declaration.
