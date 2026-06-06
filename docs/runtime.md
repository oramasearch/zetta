# Async runtime (`zetta::runtime`)

zetta is moving to a **coroutine-native** async model. All of it sits behind one
seam — `include/zetta/runtime.hpp`, namespace `zetta::runtime` — so the rest of the
codebase is decoupled from the (beta, fast-moving) backend.

## The backend: C++ Alliance Capy + Corosio

The backend is the C++ Alliance's coroutine-native stack, vendored as git
submodules under [`modules/`](../modules/):

- **Capy** (`modules/capy`) — the foundation: `task<T>`, executors, strands,
  thread pools, `when_all`/`when_any`. No I/O of its own. **In use.**
- **Corosio** (`modules/corosio`) — coroutine-native networking (TCP sockets,
  acceptors, timers, TLS); the declared successor to Boost.Asio. **Vendored;
  wired into the build when zetta grows a server.**

These are beta (≈2026) and require **Clang 17+**, which is why zetta builds with
Homebrew Clang (see [build.md](build.md)). Background and the design rationale
for choosing them are in [research/coroutines.md](research/coroutines.md).

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

```cpp
#include "zetta/runtime.hpp"

zetta::runtime::task<int> inner() { co_return 20; }
zetta::runtime::task<int> outer() { co_return co_await inner() + 22; }

int main() {
    int answer = zetta::runtime::block_on(outer());   // 42
}
```

## Migration plan

Per [research/coroutines.md](research/coroutines.md) §9, the rollout is
incremental:

1. **Vocabulary in place** *(done)* — `zetta::runtime::task` / `block_on`, Capy
   vendored and building, covered by `tests/test_runtime.cpp`.
2. **Make the call graph coroutine-shaped** — wrap existing synchronous core
   operations (query execution, storage reads/writes) as `zetta::runtime::task`,
   sequencing with `co_await`; `block_on` at the entry points.
3. **Async file I/O** — replace blocking reads/writes via Corosio (or a kqueue
   wrapper), keeping the call graph from step 2.
4. **Networking** *(started)* — Corosio is wired in; the cluster
   (`src/core/cluster.cpp`, see [cluster.md](cluster.md)) is the first user — a
   leader accept-loop + per-connection coroutines, behind `zetta::runtime::net`.
5. **Structured concurrency / cancellation** — `when_all` for parallel
   sub-queries, stop-tokens for timeouts.

Keep `zetta-core` free of SDL/ImGui (the async layer is core, not GUI), and keep
the backend confined to `runtime.hpp`.
