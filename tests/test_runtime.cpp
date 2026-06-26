#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
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

}  // namespace

TEST(Runtime, BlockOnReturnsValueAndAwaitComposes) {
    EXPECT_EQ(zetta::runtime::block_on(outer()), 42);
}

TEST(Runtime, BlockOnRunsVoidTask) {
    auto flag = std::make_shared<bool>(false);
    zetta::runtime::block_on(set_flag(flag));
    EXPECT_TRUE(*flag);
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
