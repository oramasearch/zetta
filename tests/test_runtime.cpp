#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "zetta/runtime.hpp"

namespace {

zetta::runtime::task<int> inner() { co_return 20; }

// co_await composes through the seam; result flows back out of block_on.
zetta::runtime::task<int> outer() {
    const int partial = co_await inner();
    co_return partial + 22;
}

// Take state by value (shared_ptr) into the frame — never by reference (the
// coroutine dangling-parameter footgun).
zetta::runtime::task<> set_flag(std::shared_ptr<bool> flag) {
    *flag = true;
    co_return;
}

// Records `value` into shared, thread-safe state — the shape a spawned task uses
// to hand its result back to a polling caller. Inputs by value into the frame.
zetta::runtime::task<> record(std::shared_ptr<std::atomic<int>> sink, int value) {
    sink->store(value, std::memory_order_relaxed);
    co_return;
}

}  // namespace

TEST(Runtime, BlockOnReturnsValueAndAwaitComposes) {
    EXPECT_EQ(zetta::runtime::block_on(outer()), 42);
}

TEST(Runtime, BlockOnRunsVoidTask) {
    auto flag = std::make_shared<bool>(false);
    zetta::runtime::block_on(set_flag(flag));
    EXPECT_TRUE(*flag);
}

// spawn runs the coroutine off the calling thread and signals completion — the
// caller polls a flag (as the GUI frame loop does) instead of blocking.
TEST(Runtime, SpawnRunsOffThreadAndSignalsCompletion) {
    auto sink = std::make_shared<std::atomic<int>>(0);
    auto done = std::make_shared<std::atomic<bool>>(false);

    zetta::runtime::spawn(record(sink, 7),
                          [done] { done->store(true, std::memory_order_release); });

    // Poll for up to ~10s, the way a frame loop would (never blocking on the work).
    for (int i = 0; i < 10000 && !done->load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(done->load(std::memory_order_acquire));
    EXPECT_EQ(sink->load(std::memory_order_relaxed), 7);
}

// --- parallel_for ------------------------------------------------------------

TEST(RuntimeParallelFor, EmptyRangeRunsNothing) {
    bool called = false;
    zetta::runtime::parallel_for(
        0, [&](std::size_t, std::size_t, std::size_t) { called = true; });
    EXPECT_FALSE(called);
}

TEST(RuntimeParallelFor, ChunksTileTheRangeExactlyOnce) {
    // A large range so parallel_chunks() returns more than one chunk; every
    // index must be visited exactly once across disjoint, contiguous chunks.
    constexpr std::size_t count = 500'000;
    std::vector<int> visits(count, 0);

    zetta::runtime::parallel_for(
        count, [&](std::size_t, std::size_t begin, std::size_t end) {
            for (std::size_t i = begin; i < end; ++i) ++visits[i];
        });

    for (std::size_t i = 0; i < count; ++i) ASSERT_EQ(visits[i], 1) << "at " << i;
}

TEST(RuntimeParallelFor, ChunkIndicesAreDenseAndRangesAreContiguous) {
    constexpr std::size_t count = 500'000;
    const std::size_t chunks = zetta::runtime::parallel_chunks(count);
    EXPECT_GE(chunks, 1u);

    std::vector<std::pair<std::size_t, std::size_t>> ranges(chunks, {0, 0});
    std::atomic<std::size_t> seen{0};
    zetta::runtime::parallel_for(
        count, [&](std::size_t chunk, std::size_t begin, std::size_t end) {
            ASSERT_LT(chunk, chunks);
            ranges[chunk] = {begin, end};
            seen.fetch_add(1);
        });

    EXPECT_EQ(seen.load(), chunks);
    // Chunks tile [0, count) head to tail with no gaps or overlap.
    EXPECT_EQ(ranges.front().first, 0u);
    EXPECT_EQ(ranges.back().second, count);
    for (std::size_t c = 1; c < chunks; ++c)
        EXPECT_EQ(ranges[c].first, ranges[c - 1].second);
}

TEST(RuntimeParallelFor, ParallelSumMatchesSequential) {
    constexpr std::size_t count = 1'000'000;
    std::vector<std::int64_t> data(count);
    std::iota(data.begin(), data.end(), std::int64_t{1});

    std::atomic<std::int64_t> total{0};
    zetta::runtime::parallel_for(
        count, [&](std::size_t, std::size_t begin, std::size_t end) {
            std::int64_t local = 0;
            for (std::size_t i = begin; i < end; ++i) local += data[i];
            total.fetch_add(local);
        });

    const std::int64_t expected =
        static_cast<std::int64_t>(count) * (count + 1) / 2;
    EXPECT_EQ(total.load(), expected);
}

TEST(RuntimeParallelFor, SmallRangeRunsInlineAsOneChunk) {
    EXPECT_EQ(zetta::runtime::parallel_chunks(0), 0u);
    EXPECT_EQ(zetta::runtime::parallel_chunks(1), 1u);
    EXPECT_EQ(zetta::runtime::parallel_chunks(16), 1u);  // below the grain

    std::size_t chunk_count = 0;
    zetta::runtime::parallel_for(
        16, [&](std::size_t chunk, std::size_t, std::size_t) {
            EXPECT_EQ(chunk, 0u);
            ++chunk_count;
        });
    EXPECT_EQ(chunk_count, 1u);
}

// offload runs a BLOCKING callable on a pool worker and resumes the awaiting
// coroutine with its result — the bridge for a blocking backend (libpq). The
// result must flow back, and the blocking must happen off the calling thread.
namespace {
zetta::runtime::task<int> awaits_offload(std::shared_ptr<std::thread::id> ran_on) {
    const int value = co_await zetta::runtime::offload([ran_on]() -> int {
        *ran_on = std::this_thread::get_id();
        // A real blocking call (a DB round-trip) would sit here; a short sleep
        // stands in — on a pool worker, so the caller's thread is never pinned.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        return 99;
    });
    co_return value;
}
}  // namespace

TEST(Runtime, OffloadReturnsResultFromAPoolWorker) {
    auto ran_on = std::make_shared<std::thread::id>();
    const std::thread::id caller = std::this_thread::get_id();

    const int result = zetta::runtime::block_on(awaits_offload(ran_on));

    EXPECT_EQ(result, 99);                 // the blocking result flowed back
    EXPECT_NE(*ran_on, std::thread::id{});  // the callable actually ran
    EXPECT_NE(*ran_on, caller);             // …on a pool worker, not the caller
}

TEST(RuntimeParallelForDeathTest, ThrowingBodyAborts) {
    // parallel_for bodies must not throw — zetta-core never unwinds across a
    // seam. A throwing body is a contract violation and aborts the process.
    EXPECT_DEATH(
        zetta::runtime::parallel_for(
            500'000,
            [](std::size_t, std::size_t, std::size_t) {
                throw std::runtime_error("boom");
            }),
        "");
}
