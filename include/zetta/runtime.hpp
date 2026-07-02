#pragma once

#include <cstddef>
#include <functional>
#include <optional>
#include <type_traits>
#include <utility>

#include <boost/capy.hpp>
#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/test/run_blocking.hpp>

// zetta's async runtime seam.
//
// zetta code uses `zetta::runtime::*` and never names `boost::capy` /
// `boost::corosio` directly. The coroutine/IO/networking backend is the C++
// Alliance stack (Capy + Corosio, vendored under modules/), which is beta and
// may churn — so confining it to this one header means a backend swap (or a
// move to Boost.Asio) is a change here, not across the codebase.
//
// Today: Capy provides the coroutine foundation (`task<Value>`, executors,
// thread pools). Corosio (networking) is wired in here when zetta grows a server.
namespace zetta::runtime {

// A coroutine returning Value (void by default).
template <class Value = void>
using task = boost::capy::task<Value>;

// A pool of worker threads exposing an executor (`pool.get_executor()`).
using thread_pool = boost::capy::thread_pool;

// A coroutine-native one-shot event: awaiters suspend on `co_await
// event.wait()` and are all resumed when any thread calls `set()` (each on
// its own executor). The coroutine-first alternative to a condition
// variable — the primitive cross-thread completion signaling goes through
// (e.g. the API surface's MainThreadBridge).
using async_event = boost::capy::async_event;

// Synchronous bridge: run `coroutine` to completion on the calling thread. Use
// from non-coroutine code (main(), tests) to enter the async world. Backed by
// Capy's blocking runner.
inline void block_on(task<> coroutine) {
    boost::capy::test::run_blocking()(std::move(coroutine));
}

// Value-returning overload: run `coroutine` and return its result.
template <class Value>
    requires(!std::is_void_v<Value>)
Value block_on(task<Value> coroutine) {
    std::optional<Value> result;
    boost::capy::test::run_blocking(
        [&](Value value) { result.emplace(std::move(value)); })(std::move(coroutine));
    return std::move(*result);
}

// --- Non-blocking spawn: run a coroutine off the calling thread ---------------
//
// Schedule `coroutine` on the shared runtime pool and **return immediately** — the
// coroutine runs concurrently while the caller keeps going. `on_complete` is
// invoked once, when the coroutine finishes. This is the seam's off-the-caller-
// thread primitive: it is how any long-running work (a provider CLI subprocess, a
// network round-trip) is run without blocking a latency path such as the GUI
// frame loop, WITHOUT any caller spawning a raw std::thread — the runtime seam is
// that interface (see CLAUDE.md "async is coroutines, through the seam").
//
// `on_complete` runs on a **pool worker thread**, so it must only touch
// thread-safe state (atomics / a mutex) — never the UI or the Node directly. The
// canonical pattern: the coroutine writes its result into shared state, then
// `on_complete` flips a done flag the caller polls each frame, and the caller
// applies the result on its own thread.
//
// Prefer a coroutine **function** whose inputs are taken by value (they live in
// the coroutine frame) over a capturing lambda — a capturing-lambda coroutine
// dangles once the temporary lambda is destroyed (same caveat as parallel_for).
void spawn(task<> coroutine, std::function<void()> on_complete);

// --- Data parallelism: parallel_for over the shared runtime thread pool -------
//
// A blocking data-parallel primitive for CPU-bound work — the seam's answer to
// "spread this loop across cores" without any caller naming `boost::capy`. The
// heavy compute backend (zetta::compute::CpuDevice) and, later, the scheduler /
// collectives are its consumers. It fans out onto one shared, process-wide
// thread pool (the runtime thread pool) and joins before returning.

// The number of contiguous chunks parallel_for splits `count` items into. It is
// a pure function of `count` and the pool's worker count, so two calls with the
// same `count` see an identical (chunk_index -> [begin, end)) tiling — a
// guarantee multi-pass algorithms (e.g. a parallel prefix scan) rely on to line
// up their phases. Returns 0 for count == 0, and 1 for any count small enough
// that parallelism is not worth the dispatch (the body then runs inline).
std::size_t parallel_chunks(std::size_t count) noexcept;

// Run `body(chunk_index, begin, end)` over the parallel_chunks(count) disjoint
// contiguous sub-ranges that tile [0, count), concurrently on the shared runtime
// thread pool, and block until every sub-range has completed. The body is called
// once per chunk with a half-open range and the chunk's index in
// [0, parallel_chunks(count)); it should loop over [begin, end) itself (chunked,
// not per-element dispatch — the grain stays cache-friendly and the pool overhead
// is amortized). When there is a single chunk the body runs inline on the calling
// thread with no pool dispatch.
//
// `body` must not throw. zetta-core compiles with -fexperimental-library and must
// never unwind across a seam (the same discipline that makes compute operators
// return ComputeStatus rather than throwing), so a body that raises an exception
// is a contract violation and aborts the process rather than unwinding into the
// coroutine backend.
void parallel_for(
    std::size_t count,
    const std::function<void(std::size_t chunk_index, std::size_t begin,
                             std::size_t end)>& body);

}  // namespace zetta::runtime
