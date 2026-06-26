#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <latch>
#include <numeric>
#include <thread>
#include <vector>

#include "zetta/compute/work_stealing_scheduler.hpp"

using zetta::compute::SchedulerStats;
using zetta::compute::WorkStealingScheduler;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

}  // namespace

// --- Basic execution ---------------------------------------------------------

TEST(WorkStealingScheduler, RunsEverySubmittedTask) {
    WorkStealingScheduler scheduler(4);
    constexpr int kTasks = 500;

    std::atomic<int> ran{0};
    std::latch done(kTasks);
    for (int index = 0; index < kTasks; ++index) {
        scheduler.submit([&ran, &done] {
            ran.fetch_add(1, std::memory_order_relaxed);
            done.count_down();
        });
    }
    done.wait();

    EXPECT_EQ(ran.load(), kTasks);
    EXPECT_EQ(scheduler.stats().executed, static_cast<std::size_t>(kTasks));
    EXPECT_EQ(scheduler.queued(), 0u);
}

// --- parallel_map: correctness + order ---------------------------------------

TEST(WorkStealingScheduler, ParallelMapComputesAllInInputOrder) {
    WorkStealingScheduler scheduler(4);

    std::vector<int> inputs(1000);
    std::iota(inputs.begin(), inputs.end(), 0);

    const std::vector<long> squares =
        scheduler.parallel_map(inputs, [](int value) {
            return static_cast<long>(value) * value;
        });

    ASSERT_EQ(squares.size(), inputs.size());
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        EXPECT_EQ(squares[index], static_cast<long>(index) * index) << index;
    }
    EXPECT_EQ(scheduler.stats().executed, inputs.size());
}

TEST(WorkStealingScheduler, ParallelMapOnEmptyInputIsEmpty) {
    WorkStealingScheduler scheduler(2);
    const std::vector<int> empty;
    EXPECT_TRUE(scheduler.parallel_map(empty, [](int value) {
                            return value;
                        }).empty());
}

// --- The headline acceptance: imbalanced workers, faster ones do more --------

// Worker 0 is made deliberately slow (it sleeps on every task it runs); the
// other three are fast. After a parallel_map of a uniform workload the fast
// workers must have *stolen* the slack and processed far more tasks than the
// slow one — the "fluid, heterogeneity-tolerant" property the scheduler exists
// to provide. The assertions are timing-robust: they only require that stealing
// happened and that the slow worker fell behind, not exact counts.
TEST(WorkStealingScheduler, FasterWorkersStealAndProcessMore) {
    WorkStealingScheduler scheduler(4);
    constexpr int kTasks = 240;

    std::vector<int> inputs(kTasks);
    std::iota(inputs.begin(), inputs.end(), 0);

    const std::vector<long> results =
        scheduler.parallel_map(inputs, [&scheduler](int value) {
            // Only the task *running on* worker 0 is slow, so a task stolen away
            // from worker 0's deque by a fast worker runs at full speed.
            if (scheduler.current_worker_index() == 0) sleep_ms(10);
            return static_cast<long>(value) * 2;
        });

    // Correct + complete regardless of how the work was distributed.
    ASSERT_EQ(results.size(), inputs.size());
    for (int value = 0; value < kTasks; ++value) {
        EXPECT_EQ(results[value], static_cast<long>(value) * 2);
    }

    const SchedulerStats stats = scheduler.stats();
    EXPECT_EQ(stats.executed, static_cast<std::size_t>(kTasks));

    // Stealing happened — the whole point.
    EXPECT_GT(stats.steals, 0u);

    // The fast workers collectively did far more than the slow one.
    const std::size_t fast_total =
        stats.per_worker[1] + stats.per_worker[2] + stats.per_worker[3];
    EXPECT_GT(fast_total, stats.per_worker[0]);
}

// --- current_worker_index seam -----------------------------------------------

TEST(WorkStealingScheduler, CurrentWorkerIndexIsSentinelOutsideThePool) {
    WorkStealingScheduler scheduler(3);
    // The calling (test) thread is not one of the scheduler's workers, so it
    // gets the out-of-range sentinel.
    EXPECT_EQ(scheduler.current_worker_index(), scheduler.worker_count());

    // Inside a task the index is a valid worker id.
    std::latch done(1);
    std::atomic<unsigned> seen{999};
    scheduler.submit([&scheduler, &seen, &done] {
        seen.store(scheduler.current_worker_index());
        done.count_down();
    });
    done.wait();
    EXPECT_LT(seen.load(), scheduler.worker_count());
}

// --- Zero workers: a pure task sink (used by a node that only sources work) ---

TEST(WorkStealingScheduler, ZeroWorkersNeverRunsTasksButIsSafe) {
    WorkStealingScheduler scheduler(0);
    EXPECT_EQ(scheduler.worker_count(), 0u);

    std::atomic<bool> ran{false};
    scheduler.submit([&ran] { ran.store(true); });

    sleep_ms(30);
    EXPECT_FALSE(ran.load());     // no worker exists to run it
    EXPECT_EQ(scheduler.queued(), 1u);
    // Destruction with a task still queued must not hang or crash.
}
