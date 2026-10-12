# Dispatch

**Issues:** [#1026](https://github.com/oramasearch/zetta/issues/1026),
[#1157](https://github.com/oramasearch/zetta/issues/1157) · **Decision:** D24 ·
**Code:** `include/zetta/pubsub/dispatcher.hpp`, `src/core/pubsub/dispatcher.cpp` ·
**Tests:** `tests/pubsub/test_dispatch_and_resources.cpp`,
`tests/pubsub/test_node_pubsub.cpp`

The `Dispatcher` runs pub/sub work that should not run on the publisher's thread:
inbox drains and appends for topics with `producer_queue`. Work is submitted under
a topic or inbox key. Work for one key always runs in submission order.

## Default pool and sizing

`Pool` is the node default. It starts with one worker and grows at one worker per
250 active pool keys:

```
threads = clamp(ceil(active_pool_keys / topics_per_thread), minimum, maximum)
```

The defaults are 250 keys per thread, a minimum of one thread, and a maximum of
the detected CPU count. Thus 1–250 topics use one worker, 300 use two, and 1,000
use four when the maximum permits it. The minimum and maximum make resource use
predictable while allowing an operator to scale dispatch for the workload.

Growth happens as soon as a key crosses a multiple of `topics_per_thread`.
Shrink uses a ten-key margin: after a two-worker pool has grown, it returns to one
at 240 keys, not while the count moves between 250 and 251. Every resize is logged:

```
pubsub: INFO dispatch: 251 topics; pool 1 → 2 threads
pubsub: INFO dispatch: 240 topics; pool 2 → 1 threads
```

Keys are assigned to workers by a stable hash. A resize reassigns queued keys,
but a key that is running stays on its current worker until that job finishes.
Its next job then runs on the new worker, preserving per-key order.

## Modes

| Mode | What runs the work |
|---|---|
| `Pool` | The default shared pool. Each key stays on one worker; workers visit ready keys round robin and run at most `per_turn_cap` jobs for a key per turn. |
| `PerKey` | One thread per key. Available only when explicitly selected. |
| `Inline` | The submitting thread. Available only when explicitly selected. |
| `Auto` | The legacy explicit choice: `PerKey` below `topics_per_thread`, then `Pool`, with hysteresis on the return. It is never selected automatically. |

`TopicSettings::dispatch` overrides the node mode for one topic. This is how a
topic opts into `PerKey`, `Pool`, or `Inline` without changing the node default.

## Configuration and observability

| CLI | Environment | Default | Meaning |
|---|---|---:|---|
| `--pubsub-dispatch-mode` | `ZETTA_PUBSUB_DISPATCH_MODE` | `pool` | `pool`, `auto`, `per_key`, or `inline` |
| `--pubsub-topics-per-thread` | `ZETTA_PUBSUB_TOPICS_PER_THREAD` | 250 | active pool keys per worker |
| `--pubsub-dispatch-threads` | `ZETTA_PUBSUB_DISPATCH_THREADS` | 1 | minimum pool size |
| `--pubsub-dispatch-max-threads` | `ZETTA_PUBSUB_DISPATCH_MAX_THREADS` | detected CPUs | maximum pool size |

`system.pubsub_topics` reports `dispatch_mode`, `dispatch_threads`,
`dispatch_keys`, `topics_per_dispatch_thread`, `dispatch_min_threads`, and
`dispatch_max_threads`. `system.node_metrics` reports the same node-wide values
with a `pubsub_` prefix. The `sql` registry capability exposes both views through
REST and MCP, and the transport-parity test compares their results.

The bus and workspace images compile this same node binary, so they inherit the
pool default and accept the same CLI and environment settings; there is no
image-specific dispatch implementation.

## Blocking rule

A dispatch job must never wait on a condition variable or long poll. The worker
sets a thread-local dispatch marker around each job; blocking topic paths assert
in debug builds when that marker is present and decline the wait in release
builds. REST and MCP long polls use the coroutine event path and do not occupy a
dispatch worker. This is required for a one-worker node: one blocking job would
otherwise stop every topic assigned to that worker.

## Throughput snapshot

The disabled benchmark queues 1,000,000 no-op jobs across 1,000 keys and drains
them. On the 16-logical-CPU macOS proof host on 2026-09-28, a release build
reported:

| Pool size | Elapsed | Jobs/second |
|---:|---:|---:|
| 1 | 3.25975 s | 306,772 |
| 4 | 3.74609 s | 266,945 |

Command:

```bash
./build/tests/zetta-tests --gtest_also_run_disabled_tests \
  --gtest_filter='PubSubDispatcherBenchmark.DISABLED_ThroughputOneAndFourThreads'
```

The jobs deliberately do almost no work, so this records dispatcher overhead,
not application throughput. The four-worker result includes synchronization and
queue-submission contention from the single benchmark producer.
