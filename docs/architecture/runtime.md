# Async runtime (`zetta::runtime`)

zetta is moving to a **coroutine-native** async model. All of it sits behind one
seam — `include/zetta/runtime.hpp`, namespace `zetta::runtime` — so the rest of the
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

## The seam

Code uses `zetta::runtime::*` and **never names `boost::capy` / `boost::corosio`
directly**. A backend swap — or a fallback to Boost.Asio if the beta APIs churn
— is then a change to `runtime.hpp` alone, not across the codebase.

Today the seam exposes:

| `zetta::runtime` | Backed by | Purpose |
|---|---|---|
| `task<T>` (default `task<>`) | `boost::capy::task<T>` | a coroutine returning `T` |
| `thread_pool` | `boost::capy::thread_pool` | worker threads + an executor |
| `block_on(task)` / `block_on(task<T>)` | Capy blocking runner | synchronous bridge: run a task to completion from non-coroutine code (e.g. `main()`, tests) and get its result |
| `parallel_for(count, body)` | Capy `thread_pool` + `run_async` | blocking data-parallel fan-out: split `[0, count)` into chunks, run each on the shared pool, join before returning |
| `parallel_chunks(count)` | — | the number of chunks `parallel_for` would tile `count` into (for sizing per-chunk state) |

```cpp
#include "zetta/runtime.hpp"

zetta::runtime::task<int> inner() { co_return 20; }
zetta::runtime::task<int> outer() { co_return co_await inner() + 22; }

int main() {
    int answer = zetta::runtime::block_on(outer());   // 42
}
```

## Data parallelism: `parallel_for`

For CPU-bound work that wants to spread a loop across cores, the seam exposes a
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
  and must never unwind across a seam (the same discipline behind
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
the backend confined to `runtime.hpp`.
