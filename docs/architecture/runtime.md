# Async runtime (`zetta::runtime`)

zetta is moving to a **coroutine-native** async model. All of it sits behind one
interface — `include/zetta/runtime.hpp`, namespace `zetta::runtime` — so the rest of the
codebase is decoupled from the (beta, fast-moving) backend.

## The backend: C++ Alliance Capy + Corosio

The backend is the C++ Alliance's coroutine-native stack, vendored as git
submodules under [`modules/`](../../modules):

- **Capy** (`modules/capy`) — the foundation: `task<T>`, executors, strands,
  thread pools, `when_all`/`when_any`. No I/O of its own. **In use.**
- **Corosio** (`modules/corosio`) — coroutine-native networking (TCP sockets,
  acceptors, timers, TLS); the declared successor to Boost.Asio. **Vendored;
  wired into the build when zetta grows a server.**

These are beta (≈2026) and require **Clang 17+**, which is why zetta builds with
Homebrew Clang (see [build.md](../build.md)). Background and the design rationale
for choosing them are in [research/coroutines.md](../research/coroutines.md).

## The interface

Code uses `zetta::runtime::*` and **never names `boost::capy` / `boost::corosio`
directly**. A backend swap — or a fallback to Boost.Asio if the beta APIs churn
— is then a change to `runtime.hpp` and its `src/core/runtime.cpp` adapter,
not across the codebase.

Today the interface exposes:

| `zetta::runtime` | Backed by | Purpose |
|---|---|---|
| `task<T>` (default `task<>`) | `boost::capy::task<T>` | a coroutine returning `T` |
| `with_stop_token(task, token)` | Capy task runner | inherit the executor while overriding cancellation; an empty token protects completion waits over borrowed state (#1077) |
| `thread_pool` | `boost::capy::thread_pool` | worker threads + an executor |
| `async_event` | serialized Capy event adapter | suspend without holding a thread; publish/close wakes all waiters on their executors (#1076) |
| `block_on(task)` / `block_on(task<T>)` | Capy blocking runner | synchronous bridge: run a task to completion from non-coroutine code (e.g. `main()`, tests) and get its result |
| `parallel_for(count, body)` | Capy `thread_pool` + `run_async` | blocking data-parallel fan-out: split `[0, count)` into chunks, run each on the shared pool, join before returning |
| `parallel_chunks(count)` | — | the number of chunks `parallel_for` would tile `count` into (for sizing per-chunk state) |
| `offload(callable) -> task<T>` | `spawn` + `async_event` | run a **blocking** callable on a pool worker and resume the awaiting coroutine with its result — `co_await runtime::offload([]{ return blocking(); })`. The bridge for a backend adapter that can only block (a vendored C driver like **libpq**): the blocking call runs off every zetta network/main thread. Confine such a callable to the one adapter TU that names the backend library. |

```cpp
#include "zetta/runtime.hpp"

zetta::runtime::task<int> inner() { co_return 20; }
zetta::runtime::task<int> outer() { co_return co_await inner() + 22; }

int main() {
    int answer = zetta::runtime::block_on(outer());   // 42
}
```

## Cross-thread event waits

`async_event` is a runtime adapter, not an alias of the backend event: Capy's
event requires one executor, while topic publishers and REST waiters run on
different threads. The adapter in `src/core/runtime.cpp` holds a short mutex
around signal checks, registration, publication and cancellation cleanup. The
check and registration happen in one critical section so a publish cannot fall
between them. The lock is released before suspension; waiting uses a coroutine
frame and never parks a thread. `runtime::timeout(event.wait(), duration)`
cancels and removes a waiter when its deadline expires. Shared event state
outlives pending waits (#1076, [pub/sub exposure](../pubsub/exposure.md)).

Cancellation is still enabled for topic waits, including their timeout. A queued
owner-thread closure is different: it can borrow data from the awaiting request
frame, so `MainThreadBridge` waits for its completion under
`with_stop_token(completed->wait(), std::stop_token{})`. Transport cancellation
cannot release that frame until the owner has executed the closure (#1077).
The shutdown owner loop must continue draining during that completion wait.

No JSON or wire format changes. Coverage includes signals before registration,
500 waiters across pool threads, and publication racing timeout cleanup.

## Data parallelism: `parallel_for`

For CPU-bound work that wants to spread a loop across cores, the interface exposes a
blocking data-parallel primitive so callers never name `boost::capy` to fan work
out. It runs over one **shared, process-wide thread pool** (created on first use,
one worker per hardware thread) and **joins before returning** — `parallel_for`
is synchronous: the calling thread blocks until every chunk has run.

```cpp
#include "zetta/runtime.hpp"

// Square a large array across all cores.
zetta::runtime::parallel_for(values.size(),
    [&](std::size_t /*chunk*/, std::size_t begin, std::size_t end) {
        for (std::size_t i = begin; i < end; ++i) values[i] *= values[i];
    });
```

- `parallel_for(count, body)` tiles `[0, count)` into `parallel_chunks(count)`
  disjoint, contiguous sub-ranges and calls `body(chunk_index, begin, end)` once
  per chunk on a pool worker. The body loops over `[begin, end)` itself — the
  grain stays cache-friendly and the pool overhead is amortized.
- The tiling is a **pure function of `count`** (and the worker count), so two
  calls with the same `count` see an identical `chunk_index → [begin, end)`
  mapping — multi-pass algorithms (e.g. a parallel prefix scan) rely on this to
  line their phases up.
- Small ranges (below a grain threshold) run **inline** on the calling thread as
  a single chunk, with no pool dispatch.
- `body` **must not throw**: zetta-core compiles with `-fexperimental-library`
  and must never unwind across a public boundary (the same discipline behind
  [`ComputeStatus`](../compute/compute-device.md#error-model-computestatus)). A throwing body
  is a contract violation and aborts the process rather than unwinding into the
  coroutine backend.

The first consumer is the CPU compute backend
([`zetta::compute::CpuDevice`](../compute/cpu-device.md)); the work-stealing job scheduler and
collectives of the accelerated-compute pass will build on the same primitive.

## Migration plan

Per [research/coroutines.md](../research/coroutines.md) §9, the rollout is
incremental:

1. **Vocabulary in place** *(done)* — `zetta::runtime::task` / `block_on`, Capy
   vendored and building, covered by `tests/test_runtime.cpp`.
2. **Make the call graph coroutine-shaped** — wrap existing synchronous core
   operations (query execution, storage reads/writes) as `zetta::runtime::task`,
   sequencing with `co_await`; `block_on` at the entry points.
3. **Async file I/O** — replace blocking reads/writes via Corosio (or a kqueue
   wrapper), keeping the call graph from step 2.
4. **Networking** *(started)* — Corosio is wired in; the cluster
   (`src/core/cluster/cluster.cpp`, see [cluster.md](../cluster/cluster.md)) is the first user — an
   accept loop + per-peer dial coroutines (equal peers), behind
   `zetta::runtime::net`.
5. **Structured concurrency / cancellation** — `when_all` for parallel
   sub-queries, stop-tokens for timeouts.

Keep `zetta-core` free of SDL/ImGui (the async layer is core, not GUI), and keep
the backend confined to `runtime.hpp` and `src/core/runtime.cpp`.
