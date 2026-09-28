#include "zetta/runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>
#include <latch>
#include <thread>

#include <boost/capy/ex/run_async.hpp>
#include <boost/capy/ex/thread_pool.hpp>

// The data-parallel slice of the runtime seam (parallel_for / parallel_chunks).
//
// This is the one place — alongside the rest of runtime.hpp — that names the
// Capy backend for fan-out work, so callers (zetta::compute::CpuDevice today)
// stay backend-agnostic. Work is spread across a single process-wide thread pool
// and joined before returning, so parallel_for is a synchronous primitive: the
// calling thread blocks until every chunk has run.
namespace zetta::runtime {

namespace {

// One shared worker pool for the whole process. Created on first use with one
// worker per hardware thread; its destructor (at program exit) joins the
// workers. Reusing a single pool across calls avoids paying thread
// creation/teardown on every parallel_for.
boost::capy::thread_pool& shared_pool() {
    static boost::capy::thread_pool pool(0);  // 0 => hardware_concurrency
    return pool;
}

// The pool for work that blocks waiting (spawn_blocking, #1063): long polls park
// here, never on the compute pool above.
boost::capy::thread_pool& blocking_pool() {
    static boost::capy::thread_pool pool(kBlockingThreads);
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
// "must never unwind across a seam" (the same rule that makes compute operators
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

}  // namespace

void spawn(task<> coroutine, std::function<void()> on_complete) {
    // Fire-and-forget the coroutine on the shared pool: run_async schedules it on
    // an executor and invokes the completion when the task finishes, without the
    // caller blocking (unlike parallel_for's latch wait). The completion runs on a
    // pool worker — it just forwards to on_complete, which the caller keeps
    // thread-safe. run_async owns the coroutine frame + the completion until the
    // task completes, so spawn can return immediately.
    auto executor = shared_pool().get_executor();
    boost::capy::run_async(
        executor,
        [callback = std::move(on_complete)](auto&&...) noexcept {
            if (callback) callback();
        })(std::move(coroutine));
}

void spawn_blocking(task<> coroutine, std::function<void()> on_complete) {
    auto executor = blocking_pool().get_executor();
    boost::capy::run_async(
        executor,
        [callback = std::move(on_complete)](auto&&...) noexcept {
            if (callback) callback();
        })(std::move(coroutine));
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
