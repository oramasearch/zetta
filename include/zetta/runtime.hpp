#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <type_traits>
#include <utility>

#include <boost/capy.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/test/run_blocking.hpp>

// zetta's async runtime interface.
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

// Run on the caller's executor with an explicit cancellation token. An empty
// token defers cancellation for completion waits that protect borrowed state
// (#1077; docs/architecture/runtime.md).
template <class Value>
task<Value> with_stop_token(task<Value> coroutine, std::stop_token token) {
    co_return co_await boost::capy::run(std::move(token))(std::move(coroutine));
}

// The `(error_code, values…)` pair every I/O awaitable in the stack yields, so a
// caller writes `auto [error, value] = co_await …` and never throws across the
// boundary — the async mirror of zetta-core's non-throwing status returns.
template <class... Values>
using io_result = boost::capy::io_result<Values...>;

// A coroutine yielding an `io_result<Values…>` (i.e. `task<io_result<Values…>>`).
// This is the shape the composition primitives below require, so an async interface
// meant to be fanned out (connect::AsyncHttpTransport) returns an io_task rather
// than a bare task.
template <class... Values>
using io_task = boost::capy::io_task<Values...>;

// --- Structured concurrency: fan out, then join -------------------------------
//
// Run every awaitable CONCURRENTLY and resume once they have all finished. The
// range overload takes a `std::vector<io_task<Value>>` and yields
// `io_result<std::vector<Value>>` (results in submission order); the variadic
// overload takes heterogeneous awaitables and yields a matching tuple. The first
// error wins and requests stop on its siblings.
//
// This is how zetta gets *aggregate* bandwidth out of one node: N in-flight
// requests on one thread of control instead of N sequential round-trips. Callers
// must still BOUND the fan-out (a slice at a time) — an unbounded when_all over
// thousands of awaitables opens thousands of sockets.
using boost::capy::when_all;

// Race an awaitable against a wall-clock deadline. The awaitable's own result is
// returned when it wins; otherwise the result carries `error::timeout` and a
// default-constructed payload, and the loser is cancelled. Every outbound
// exchange zetta makes should be wrapped in one, so a hung peer cannot pin a
// coroutine (and, transitively, a fan-out slot) forever.
using boost::capy::timeout;

// Suspend the awaiting coroutine for `duration`, then resume it — a timer, not a
// sleep. **This is what a wait costs nothing means**: the coroutine's frame sits
// on a shared timer service and its thread goes back to running other work,
// where `std::this_thread::sleep_for` would have pinned the thread doing
// nothing (the anti-pattern CLAUDE.md names by hand). Any poll of a remote
// operation — a warehouse statement still executing, a peer not yet ready —
// composes this instead of sleeping. `co_await runtime::delay(500ms)`.
using boost::capy::delay;

// A pool of worker threads exposing an executor (`pool.get_executor()`).
using thread_pool = boost::capy::thread_pool;

// Launch on a specified executor (network adapters), retaining its work until
// the coroutine completes. Unlike spawn(), this does not use the compute pool.
using boost::capy::run_async;

// Requeue this coroutine on its current executor so ready peers can run. This
// is a scheduling boundary, not a timer or a thread-blocking sleep (#1076).
task<> yield();

// A coroutine-native one-shot event: awaiters suspend on `co_await
// event.wait()` and are all resumed when any thread calls `set()` (each on
// its own executor). The coroutine-first alternative to a condition
// variable — the primitive cross-thread completion signaling goes through
// (e.g. the API surface's MainThreadBridge). The adapter serializes registration,
// signaling and cancellation cleanup because the backend event itself permits
// only one executor (#1076; docs/architecture/runtime.md).
class async_event {
public:
    async_event();
    async_event(const async_event&) = delete;
    async_event& operator=(const async_event&) = delete;

    io_task<> wait();
    void set();
    void clear();
    bool is_set() const;

private:
    struct State;
    std::shared_ptr<State> state_;
};

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
// invoked once, when the coroutine finishes. This is the runtime's off-the-caller-
// thread primitive: it is how any long-running work (a provider CLI subprocess, a
// network round-trip) is run without blocking a latency path such as the GUI
// frame loop, WITHOUT any caller spawning a raw std::thread — the runtime is
// that interface (see CLAUDE.md "async is coroutines, through the runtime").
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

// --- Offload blocking work onto the pool, awaited as a coroutine ---------------
//
// Run a **blocking** callable on a runtime pool worker and resume the awaiting
// coroutine with its result — `co_await runtime::offload([...]{ return blocking(); })`.
// This is the bridge for a backend adapter that can ONLY block (a vendored C
// driver like libpq): the blocking call runs on a pool thread, so the caller's
// coroutine — and every zetta network/main thread — is never pinned. It is the
// runtime's answer to "wrap a blocking library in coroutines without touching a raw
// std::thread". Confine such a callable to the one adapter TU that names the
// backend library (the module-split rule).
//
// The by-value coroutine body offload() schedules — the blocking call runs here,
// on a pool worker. A coroutine FUNCTION (never a capturing-lambda coroutine —
// that dangles): its frame owns the callable + the slot until it completes.
template <class Callable, class Result>
task<> detail_run_blocking(Callable callable,
                           std::shared_ptr<std::optional<Result>> slot) {
    slot->emplace(callable());
    co_return;
}

// The callable is taken by value (it lives in a shared frame with the result +
// completion event, so nothing dangles). `Callable` must be copyable/movable and
// return a value; it runs on a pool worker, so it may only touch thread-safe
// state, exactly like spawn's on_complete.
template <class Callable>
task<std::invoke_result_t<Callable>> offload(Callable callable) {
    using Result = std::invoke_result_t<Callable>;
    auto slot = std::make_shared<std::optional<Result>>();
    auto done = std::make_shared<async_event>();
    spawn(detail_run_blocking<Callable, Result>(std::move(callable), slot),
          [done] { done->set(); });
    [[maybe_unused]] auto ready = co_await done->wait();  // io_result: completion only
    co_return std::move(**slot);
}

// --- Data parallelism: parallel_for over the shared runtime thread pool -------
//
// A blocking data-parallel primitive for CPU-bound work — the runtime's answer to
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

// parallel_chunks with an explicit grain (minimum items per chunk). The default
// grain is tuned for cheap per-item work (a few float ops); callers whose per-item
// work is heavy — a matmul column is a full dot product — pass a smaller grain so
// the loop still parallelizes below the default 8192-item floor. Still capped at
// the worker count, and returns 1 (inline) when count / grain <= 1.
std::size_t parallel_chunks(std::size_t count,
                            std::size_t min_items_per_chunk) noexcept;

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
// never unwind across a public boundary (the same discipline that makes compute operators
// return ComputeStatus rather than throwing), so a body that raises an exception
// is a contract violation and aborts the process rather than unwinding into the
// coroutine backend.
void parallel_for(
    std::size_t count,
    const std::function<void(std::size_t chunk_index, std::size_t begin,
                             std::size_t end)>& body);

// parallel_for with an explicit grain (see the two-arg parallel_chunks). Use for
// heavy per-item work (matmul rows/columns) so it parallelizes below the default
// item floor; the body contract is identical.
void parallel_for(
    std::size_t count, std::size_t min_items_per_chunk,
    const std::function<void(std::size_t chunk_index, std::size_t begin,
                             std::size_t end)>& body);

}  // namespace zetta::runtime
