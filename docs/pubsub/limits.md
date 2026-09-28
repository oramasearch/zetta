# Limits, detected defaults and spilling

**Issue:** [#1030](https://github.com/oramasearch/zetta/issues/1030) · **Decisions:** D10,
D11, D12 · **Code:** `include/zetta/pubsub/resources.hpp`, `src/core/pubsub/resources.cpp`,
`src/core/pubsub/topic.cpp` · **Tests:** `tests/pubsub/test_dispatch_and_resources.cpp`,
`tests/pubsub/test_pubsub.cpp`

## Detection

`detect_resources(probe, data_dir, overrides)` never guesses. Memory: configuration wins;
otherwise the tightest of the cgroup limit (v2 `memory.max` or v1
`memory.limit_in_bytes`, walking every ancestor up to the mount point found in
`/proc/self/mountinfo`), the Downward API limit `ZETTA_POD_MEMORY_LIMIT`, and the host
(`hw.memsize` / `MemTotal`). `ZETTA_POD_MEMORY_REQUEST` is logged when present. CPU:
`cpu.max` / `cfs_quota_us`, then `ZETTA_POD_CPU_LIMIT` (millicores), then hardware
threads. Disk: `statvfs` on the data directory. The `ResourceProbe` makes all of it
injectable; tests fake a pod.

**For the operator:** a Zetta pod only exposes its requests if the pod spec sets the
three `ZETTA_POD_*` variables through `resourceFieldRef`. Limits are read from cgroups
without it.

`derive_limits` takes 25% of the memory budget for pub/sub and 50% of free disk for
spill files. Dispatch defaults to one shared worker per 250 active pool keys, with a
minimum of one worker and a maximum of the detected CPU count. Each value can be
overridden (`LimitOverrides`). The node CLI and environment settings are listed in
[dispatch.md](dispatch.md). Detection logs each input and result:

```
pubsub: INFO memory budget 4.0 GiB (cgroup v2 memory.max /sys/fs/cgroup/kubepods/pod1; host 256.0 GiB ignored)
pubsub: INFO memory for topics 1.0 GiB (25% of budget; override: pubsub.memory)
pubsub: INFO spill disk 180.0 GiB (50% of 360.0 GiB free; statvfs /var/lib/zetta; override: pubsub.spill_disk)
pubsub: INFO cpu 1.5 (cgroup v2 cpu.max 150000 100000); dispatch pool one thread per 250 topics, minimum 1, maximum 2 threads
```

Topic-count growth and shrink are runtime decisions rather than resource detection.
The pool grows immediately at each multiple of 250 and shrinks after the count is
ten keys below the previous boundary. Resizes are logged and exposed in
`system.node_metrics`.

## Overflow

A topic is over its limit when its memory exceeds its cap (default: the node's pub/sub
memory) or all topics together exceed the node's. It first releases what every reader
has passed, then applies its policy (default by guarantee: at-most-once skips ahead,
otherwise spills):

| Policy | Effect |
|---|---|
| `SkipAhead` | release the oldest ring; readers behind it get a gap notice |
| `Disconnect` | close the slowest subscription |
| `HoldPublisher` | the publisher waits up to `hold_publisher` (5 s), then fails |
| `RejectNew` | the publish fails |
| `SpillToDisk` | spill the oldest ring; when disk is at its cap, hold, then fail |
| `Unbounded` | nothing |

A subscription can also set `max_lag_bytes` with its own `lag_policy` (skip ahead or
disconnect), applied by the maintenance pass. Ephemeral topics never spill.

## Spilling (D12)

A full ring is written to `<data_dir>/pubsub/spill/<topic>/level-N/ring-<position>.frames`
and read back when a subscriber reaches it. It is logged when a topic starts spilling,
every 30 s while it continues (`spill_log_interval`), and when it ends:

```
pubsub: WARN topic:orders spilling to disk: memory 1.0 GiB full; slowest subscriber agent:billing-sync at position 88121004; expect higher latency
pubsub: WARN topic:orders still spilling (2m30s): 6.2 GiB on disk; subscribers: agent:billing-sync
pubsub: INFO topic:orders no longer spilling after 4m10s; 9.8 GiB spilled in total
```
