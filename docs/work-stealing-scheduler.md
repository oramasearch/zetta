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
include/zetta/compute/distributed_scheduler.hpp     DistributedScheduler — cross-node steal
src/core/compute/distributed_scheduler.cpp          the steal protocol over the signal plane
schemas/steal_request.schema.json                   the "give me N tasks" message
schemas/steal_offer.schema.json                     the "here are the tasks" reply
schemas/steal_result.schema.json                    the "here are the results" return
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

The same idea, one level up: idle/faster **nodes** steal pending tasks from busy
peers over the [signal plane](signal-plane.md). It is layered on a local
`WorkStealingScheduler` (its execution engine) and the cluster's RPC.

```cpp
#include "zetta/compute/distributed_scheduler.hpp"

zetta::compute::DistributedScheduler scheduler(cluster, /*worker_count=*/4);

// One executor, registered identically on EVERY node, so a stolen task runs the
// same wherever it lands. payload + return are raw application bytes.
scheduler.set_executor([](std::string_view method, std::string_view payload) {
    return run_shard(method, payload);
});

scheduler.start();   // register as the cluster's signal handler + start stealing

// Run `method` over every input across the local pool AND idle peers:
std::vector<std::string> outputs = scheduler.map("featurize", inputs);
```

### The protocol (pull-based, single-hop)

The node that calls `map` **owns** the job and holds the authoritative queue of
not-yet-started tasks. Two things drain it, both pulling from the same queue so a
task is run exactly once:

1. **Local workers.** One *drainer* per worker pops tasks and runs the executor.
2. **Idle peers.** A node with a free worker (`active_count() < worker_count()`)
   sends a `steal` RPC to a peer; the owner replies with an **offer** of up to
   *N* pending tasks (leasing each to the requester); the stealer runs them on
   its own pool and returns a `steal_result` to the owner, who stores them.

```
   idle node B                         owner node A (running map())
   ───────────                         ────────────────────────────
   request "steal" {count:N}  ───────▶ pop N from pending, lease to B
                              ◀─────── offer {tasks:[{job,index,method,payload}]}
   run tasks on local pool
   request "steal_result" ────────────▶ store results, mark done, notify map()
     {job, results:[{index,payload}]}  ◀─ ok
```

Stealing is **single-hop**: a node only hands out tasks from jobs *it* owns,
never tasks it has itself stolen — so there is no re-steal loop. It is also the
"faster nodes process more" property in the large: a fast/idle node keeps pulling
batches and returning them until the owner's backlog drains, while a slow node
pulls fewer.

### Robustness: leases + re-queue

Each handed-out task is **leased** to the stealer with a deadline
(`Config::steal_lease`, default 3 s). If the stealer never returns it — it died,
or the link dropped — the owner's steal loop **re-queues** the expired
assignment, so a local drainer or another peer finishes it and `map` never hangs
on a lost helper (as long as *some* compute capacity remains). Completion is
**idempotent** (a per-index `done` flag), so a slow helper's late result for an
already-re-queued task is harmlessly ignored rather than double-counted.

A `worker_count == 0` node is a valid **pure source**: it runs no tasks itself
and exists only to hand its work to peers (the deterministic cross-node test
drives exactly this — a zero-worker source whose every task is stolen and run by
an idle helper).

### The schema boundary

The three messages are JSON over the signal plane and are **validated at the
boundary** on receipt, exactly like the [Run record](runs.md) and the
[signal envelope](signal-plane.md) — against a draft-07 **JSON Schema**
(`schemas/steal_request.schema.json`, `steal_offer.schema.json`,
`steal_result.schema.json`, mirrored embedded in
`src/core/compute/distributed_scheduler.cpp`, via `pboettch/json-schema-validator`).
A malformed message fails fast with a path-qualified reason and never mutates a
job's state. Each task's raw payload is base64-encoded inside the JSON
(binary-safe), the same way the signal payload is.

### Sharing the cluster's signal handler

The cluster has a single `set_signal_handler` slot. `DistributedScheduler` takes
it (to field `steal` / `steal_result`) and forwards **every other** signal to a
`set_fallback_handler(...)` delegate, so it can coexist with another consumer
until the cluster grows a method router (a later slice — the job coordinator).

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
- **Cross-node stealing is pull-based + polled.** An idle node polls peers on a
  short interval (`Config::steal_interval`, default 25 ms) rather than being
  pushed work; this matches the signal plane's best-effort, at-most-once delivery.
  A push/notify path is the natural next step if steal latency matters.
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
