# Work-stealing scheduler (`zetta::compute`)

The **work-stealing scheduler** is the task pool the native distributed
accelerated-compute pass for UC-1 churn runs on — blueprint **ARC-10**
([#366](https://github.com/oramasearch/zetta/issues/366)), part of the compute
epic ([#264](https://github.com/oramasearch/zetta/issues/264)). A job is split
into many small tasks; idle/faster workers **steal** pending tasks from busy
ones, so the work self-balances with **no fixed per-worker capability model** — a
faster worker (or, across nodes, a GPU node) naturally processes more tasks, a
slower one fewer. This is the "fluid, heterogeneity-tolerant" split the compute
pass is built around.

It has two halves, layered:

| Layer | Type | Steals across | Status |
|---|---|---|---|
| **Local** | `zetta::compute::WorkStealingScheduler` | cores (worker threads) | this page |
| **Cross-node** | `zetta::compute::DistributedScheduler` | nodes (over the signal plane) | this page |

The cross-node layer is built **on** the local one and reuses it as its
execution engine. It does **not** schedule a `ComputeJob` or run an algorithm —
that is the [job coordinator](compute-jobs.md)
([#372](https://github.com/oramasearch/zetta/issues/372)), a later slice that
*submits work through* this scheduler.

```
include/zetta/compute/work_stealing_scheduler.hpp   WorkStealingScheduler — local pool
src/core/compute/work_stealing_scheduler.cpp        per-worker deques + the steal loop
```

## Why a sibling to `zetta::runtime::thread_pool`

zetta's [async runtime seam](runtime.md) already exposes a `thread_pool`, backed
by Capy. That pool schedules off a **single shared queue**: it load-balances
(idle workers pull the next item, so faster workers do more), but it has **no
per-worker deque and so no observable steal** to count, test, or reason about.
The compute pass wants *genuine* work-stealing — per-worker deques with an
explicit steal operation — so this scheduler is a focused sibling, not a wrapper.
Task bodies may still enter the coroutine world (`zetta::runtime::block_on`); the
scheduler stays agnostic to what a task *does*.

## Local: `WorkStealingScheduler`

A fixed pool of worker threads, each owning a deque of pending tasks, with
stealing between them.

```cpp
#include "zetta/compute/work_stealing_scheduler.hpp"

zetta::compute::WorkStealingScheduler scheduler;          // hardware concurrency
zetta::compute::WorkStealingScheduler scheduler(4);       // explicit worker count

// Fire-and-forget a unit of work (runs on some worker, later):
scheduler.submit([] { /* heavy compute shard */ });

// Or map a function over N inputs across the pool and get results in order,
// blocking until every task completes:
std::vector<int> inputs = /* ... */;
std::vector<long> out = scheduler.parallel_map(inputs, [](int value) {
    return heavy(value);
});
```

### The deque discipline

The standard Chase-Lev split, made legible with a **per-deque mutex** rather than
a lock-free structure (correctness over cleverness — the tasks here are heavy
compute shards, so per-deque locking is never the bottleneck):

- The **owning** worker pushes and pops its **own deque's back** (LIFO — best
  cache locality: the most recently produced work is the freshest).
- A **thief** takes from a victim's **front** (FIFO — least contention with the
  owner, who is working the back).

A worker that runs dry scans the other deques round-robin and steals one task; if
every deque is empty it parks on a condition variable until work is submitted or
the scheduler stops (a short backstop timeout recovers any missed wakeup without
a hot spin).

### Submission routing

`submit()` from **one of this scheduler's own workers** lands on that worker's
deque (LIFO locality for recursively produced work); from **any other thread** it
is round-robined across the deques so a burst spreads out and every worker starts
busy. `parallel_map` is the common front door and submits its N tasks this way.

### `parallel_map` semantics

- Returns results in **input order**; each task writes its own slot, so there is
  no cross-task contention on the result vector.
- **Blocks the calling thread** on a `std::latch` until all tasks finish — so it
  must be called from a **non-worker** thread. Calling it from inside a task can
  deadlock the pool.
- `function` must **not throw** across the scheduler boundary (zetta's
  no-exceptions-across-the-seam convention).

### Zero workers

`WorkStealingScheduler(0)` is explicit and valid: a pure task **sink** that never
runs anything itself. (The cross-node layer uses this for a node that only
*sources* work for peers to steal.) `submit()` stays safe and `queued()` honest,
but `parallel_map` would block forever — don't call it on a zero-worker pool.

### Observability

`stats()` returns a `SchedulerStats` snapshot — `per_worker` execution counts, a
total `steals` count, and total `executed`. Under an imbalanced load the
fingerprint of stealing is `steals > 0` plus a lopsided `per_worker`
distribution. `current_worker_index()` tells a task which worker it is on (or an
out-of-range sentinel off-pool); `active_count()` / `queued()` report how many
workers are running a task and how many tasks are waiting.

## Cross-node: `DistributedScheduler`

_(documented when the cross-node layer lands in this PR)_

## Threading model

- **Worker threads** run task bodies. Keep a task self-contained; for async I/O a
  task may `zetta::runtime::block_on` its own coroutine.
- **`parallel_map` / `submit` callers** run on their own thread. `parallel_map`
  blocks; `submit` returns immediately.
- The scheduler owns its threads (`std::jthread`); its destructor requests stop,
  wakes every worker, and joins. Tasks still queued (not yet started) at
  destruction are dropped — `parallel_map` blocks to completion, so a finished map
  has no in-flight work.

## Limits / future

- **Local stealing is mutex-per-deque**, not lock-free. Fine for heavy compute
  shards; a lock-free deque is a future tuning step if task grain shrinks.
- **No priorities / fairness classes.** Every task is equal; a scheduling-group
  model (à la Seastar) is a later option if foreground/background isolation is
  needed.
- The [job coordinator](compute-jobs.md) (#372) — splitting a `ComputeJob` into
  tasks, driving collectives at round boundaries, updating job status — is the
  consumer built on top of this, not part of it.

## See also

- [runtime.md](runtime.md) — the async/coroutine seam (`zetta::runtime`) this
  complements.
- [signal-plane.md](signal-plane.md) — the ephemeral RPC/event/task layer the
  cross-node steal protocol rides on.
- [compute-device.md](compute-device.md) — the accelerator seam the stolen tasks
  ultimately target.
- [compute-jobs.md](compute-jobs.md) — the `ComputeJob` entity the coordinator
  schedules through this scheduler.
