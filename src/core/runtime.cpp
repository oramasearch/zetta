#include "zetta/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <functional>
#include <latch>
#include <mutex>
#include <thread>
#include <vector>

#include <pthread.h>

#include <boost/capy/ex/async_event.hpp>
#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/thread_pool.hpp>

// The data-parallel slice of the runtime interface (parallel_for / parallel_chunks).
//
// This is the one place — alongside the rest of runtime.hpp — that names the
// Capy backend for fan-out work, so callers (zetta::compute::CpuDevice today)
// stay backend-agnostic. Work is spread across a single process-wide thread pool
// and joined before returning, so parallel_for is a synchronous primitive: the
// calling thread blocks until every chunk has run.
namespace zetta::runtime {

// The backend event's intrusive waiter list is single-executor state. Pub/sub
// publishers, cancellation and HTTP waiters can run on different threads, so
// every access to that list is serialized here. The mutex never spans a wait.
struct async_event::State {
    std::mutex mutex;
    boost::capy::async_event event;

    struct Waiter {
        State& state;
        std::optional<boost::capy::async_event::wait_awaiter> waiter;

        explicit Waiter(State& event_state)
            : state(event_state), waiter(std::in_place, &state.event) {}

        Waiter(Waiter&&) = default;

        ~Waiter() {
            std::lock_guard lock(state.mutex);
            waiter.reset();
        }

        bool await_ready() const noexcept {
            return false;
        }

        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> continuation,
            const boost::capy::io_env* environment) noexcept {
            std::lock_guard lock(state.mutex);
            // Checking the signal and registering are one critical section:
            // a publish between await_ready and await_suspend cannot be lost.
            if (waiter->await_ready()) return continuation;
            return waiter->await_suspend(continuation, environment);
        }

        io_result<> await_resume() noexcept {
            std::lock_guard lock(state.mutex);
            return waiter->await_resume();
        }
    };

    static io_task<> wait(std::shared_ptr<State> state) {
        co_return co_await Waiter(*state);
    }
};

async_event::async_event() : state_(std::make_shared<State>()) {}

io_task<> async_event::wait() {
    return State::wait(state_);
}

void async_event::set() {
    std::lock_guard lock(state_->mutex);
    state_->event.set();
}

void async_event::clear() {
    std::lock_guard lock(state_->mutex);
    state_->event.clear();
}

bool async_event::is_set() const {
    std::lock_guard lock(state_->mutex);
    return state_->event.is_set();
}

task<> yield() {
    struct Yield {
        boost::capy::continuation continuation;

        bool await_ready() const noexcept {
            return false;
        }

        std::coroutine_handle<> await_suspend(
            std::coroutine_handle<> coroutine,
            const boost::capy::io_env* environment) noexcept {
            continuation.h = coroutine;
            environment->executor.post(continuation);
            return std::noop_coroutine();
        }

        void await_resume() const noexcept {
        }
    };
    co_await Yield{};
}

namespace {

std::atomic<std::uint64_t> active_coroutines{0};
std::atomic<std::uint64_t> queued_coroutines{0};
std::atomic<std::uint64_t> blocking_work{0};
std::atomic<bool> pool_started{false};

// One shared worker pool for the whole process. Created on first use with one
// worker per hardware thread; its destructor (at program exit) joins the
// workers. Reusing a single pool across calls avoids paying thread
// creation/teardown on every parallel_for.
boost::capy::thread_pool& shared_pool() {
    static boost::capy::thread_pool pool(0);  // 0 => hardware_concurrency
    pool_started.store(true, std::memory_order_relaxed);
    return pool;
}

// The number of worker threads (>= 1). Mirrors the pool's sizing.
std::size_t worker_count() noexcept {
    const unsigned detected = std::thread::hardware_concurrency();
    return detected == 0 ? 1u : detected;
}

// Below this many items, dispatching to the pool costs more than it saves, so
// parallel_for runs the body inline as a single chunk.
constexpr std::size_t kMinItemsPerChunk = 4096;

// A chunk body run on a pool worker. Parameters are taken by value so they live
// in the coroutine frame (a capturing-lambda coroutine would dangle once the
// temporary lambda is destroyed); `body` outlives the task because parallel_for
// blocks on the latch before returning.
//
// Bodies must not throw: zetta-core compiles with -fexperimental-library and
// "must never unwind across a public boundary" (the same rule that makes compute operators
// return ComputeStatus instead of throwing). A throwing body is a contract
// violation, so it is turned into a loud, immediate abort here rather than left
// to unwind into the coroutine backend with backend-defined consequences.
boost::capy::task<> run_chunk(
    std::size_t chunk_index, std::size_t begin, std::size_t end,
    const std::function<void(std::size_t, std::size_t, std::size_t)>* body) {
    try {
        (*body)(chunk_index, begin, end);
    } catch (...) {
        std::fprintf(stderr,
                     "zetta::runtime::parallel_for: body raised an exception; "
                     "aborting (parallel_for bodies must not throw)\n");
        std::terminate();
    }
    co_return;
}

task<> tracked_coroutine(task<> coroutine) {
    queued_coroutines.fetch_sub(1, std::memory_order_relaxed);
    active_coroutines.fetch_add(1, std::memory_order_relaxed);
    struct Activity {
        ~Activity() {
            active_coroutines.fetch_sub(1, std::memory_order_relaxed);
        }
    } activity;
    co_await std::move(coroutine);
}

std::uint64_t default_thread_stack_bytes() noexcept {
    pthread_attr_t attributes;
    if (pthread_attr_init(&attributes) != 0) {
        return 0;
    }
    std::size_t bytes = 0;
    const int result = pthread_attr_getstacksize(&attributes, &bytes);
    pthread_attr_destroy(&attributes);
    return result == 0 ? bytes : 0;
}

}  // namespace

void spawn(task<> coroutine, std::function<void()> on_complete) {
    // Fire-and-forget the coroutine on the shared pool: run_async schedules it on
    // an executor and invokes the completion when the task finishes, without the
    // caller blocking (unlike parallel_for's latch wait). The completion runs on a
    // pool worker — it just forwards to on_complete, which the caller keeps
    // thread-safe. run_async owns the coroutine frame + the completion until the
    // task completes, so spawn can return immediately.
    queued_coroutines.fetch_add(1, std::memory_order_relaxed);
    auto executor = shared_pool().get_executor();
    boost::capy::run_async(
        executor,
        [callback = std::move(on_complete)](auto&&...) noexcept {
            if (callback) callback();
        })(tracked_coroutine(std::move(coroutine)));
}

MemoryUsage memory_usage() noexcept {
    MemoryUsage usage;
    usage.coroutine_frames = active_coroutines.load(std::memory_order_relaxed) +
                             queued_coroutines.load(std::memory_order_relaxed);
    usage.blocking_queue_depth = blocking_work.load(std::memory_order_relaxed);
    usage.thread_count = pool_started.load(std::memory_order_relaxed)
                             ? worker_count()
                             : 0;
    usage.thread_stack_bytes = usage.thread_count * default_thread_stack_bytes();
    usage.bytes = usage.coroutine_frames * 512 +
                  usage.blocking_queue_depth * 256;
    return usage;
}

namespace {

struct ContextCarriers {
    std::mutex guard;
    std::vector<ContextCarrier> carriers;
};

ContextCarriers& context_carriers() {
    static ContextCarriers state;
    return state;
}

}  // namespace

void register_context_carrier(ContextCarrier carrier) {
    if (!carrier) return;
    ContextCarriers& state = context_carriers();
    const std::lock_guard<std::mutex> lock(state.guard);
    state.carriers.push_back(std::move(carrier));
}

ContextRestorer capture_thread_context() {
    std::vector<ContextCarrier> carriers;
    {
        ContextCarriers& state = context_carriers();
        const std::lock_guard<std::mutex> lock(state.guard);
        carriers = state.carriers;
    }
    std::vector<ContextRestorer> restorers;
    for (const ContextCarrier& carrier : carriers) {
        if (ContextRestorer restorer = carrier()) restorers.push_back(std::move(restorer));
    }
    if (restorers.empty()) return {};
    return [restorers = std::move(restorers)](const std::function<void()>& work) {
        // Nest them: the first carrier's scope is outermost.
        std::function<void()> run = work;
        for (auto it = restorers.rbegin(); it != restorers.rend(); ++it) {
            run = [restorer = *it, inner = std::move(run)] { restorer(inner); };
        }
        run();
    };
}

void blocking_work_started() noexcept {
    blocking_work.fetch_add(1, std::memory_order_relaxed);
}

void blocking_work_finished() noexcept {
    blocking_work.fetch_sub(1, std::memory_order_relaxed);
}

std::size_t parallel_chunks(std::size_t count,
                            std::size_t min_items_per_chunk) noexcept {
    if (count <= 1) return count;  // 0 -> 0, 1 -> 1
    const std::size_t grain = min_items_per_chunk == 0 ? 1 : min_items_per_chunk;
    // Cap at the worker count, and never carve chunks smaller than the grain.
    const std::size_t by_grain = count / grain;
    if (by_grain <= 1) return 1;  // small enough to run inline
    return std::min(worker_count(), by_grain);
}

std::size_t parallel_chunks(std::size_t count) noexcept {
    return parallel_chunks(count, kMinItemsPerChunk);
}

void parallel_for(
    std::size_t count, std::size_t min_items_per_chunk,
    const std::function<void(std::size_t, std::size_t, std::size_t)>& body) {
    if (count == 0) return;

    const std::size_t chunks = parallel_chunks(count, min_items_per_chunk);
    if (chunks <= 1) {
        body(0, 0, count);  // inline fast path — no pool dispatch
        return;
    }

    // Tile [0, count) into `chunks` contiguous ranges, distributing the
    // remainder one extra item at a time to the first ranges. This tiling is a
    // pure function of (count, chunks), so it matches parallel_chunks() and is
    // stable across calls with the same count.
    const std::size_t base = count / chunks;
    const std::size_t remainder = count % chunks;

    auto executor = shared_pool().get_executor();
    std::latch done(static_cast<std::ptrdiff_t>(chunks));

    // Each chunk counts the latch down as it finishes; the calling thread blocks
    // on the latch, so all chunks are done before parallel_for returns.
    const auto on_done = [&done](auto&&...) noexcept { done.count_down(); };

    std::size_t begin = 0;
    for (std::size_t chunk_index = 0; chunk_index < chunks; ++chunk_index) {
        const std::size_t length = base + (chunk_index < remainder ? 1 : 0);
        const std::size_t end = begin + length;
        boost::capy::run_async(executor, on_done)(
            run_chunk(chunk_index, begin, end, &body));
        begin = end;
    }

    done.wait();
}

void parallel_for(
    std::size_t count,
    const std::function<void(std::size_t, std::size_t, std::size_t)>& body) {
    parallel_for(count, kMinItemsPerChunk, body);
}

}  // namespace zetta::runtime
