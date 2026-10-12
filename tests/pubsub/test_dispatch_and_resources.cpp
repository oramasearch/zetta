// Dispatch modes (D24) and resource detection (#1030, D11).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <mutex>
#include <thread>

#include "zetta/pubsub/dispatcher.hpp"
#include "zetta/pubsub/resources.hpp"

namespace zetta::pubsub {
namespace {

struct CapturedLog {
    std::mutex mutex;
    std::vector<std::string> lines;
    LogSink sink() {
        return [this](LogLevel level, std::string_view message) {
            std::lock_guard lock(mutex);
            lines.push_back(std::string(to_string(level)) + " " + std::string(message));
        };
    }
    bool contains(std::string_view text) {
        std::lock_guard lock(mutex);
        for (const auto& line : lines) {
            if (line.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

TEST(PubSubDispatcher, KeepsOrderPerKeyAndSwitchesToThePoolPastTheThreshold) {
    CapturedLog log;
    Dispatcher dispatcher({.mode = DispatchMode::Auto,
                           .topics_per_thread = 4,
                           .minimum_threads = 1,
                           .maximum_threads = 2},
                          log.sink());
    std::mutex mutex;
    std::map<std::string, std::vector<int>> seen;
    for (int key = 0; key < 3; ++key) {
        for (int index = 0; index < 100; ++index) {
            dispatcher.submit("topic:" + std::to_string(key), [&, key, index] {
                std::lock_guard lock(mutex);
                seen["topic:" + std::to_string(key)].push_back(index);
            });
        }
    }
    EXPECT_EQ(dispatcher.effective_mode(), DispatchMode::PerKey);
    for (int key = 3; key < 10; ++key) {  // 10 keys > threshold 4
        for (int index = 0; index < 100; ++index) {
            dispatcher.submit("topic:" + std::to_string(key), [&, key, index] {
                std::lock_guard lock(mutex);
                seen["topic:" + std::to_string(key)].push_back(index);
            });
        }
    }
    dispatcher.drain();
    EXPECT_EQ(dispatcher.effective_mode(), DispatchMode::Pool);
    EXPECT_TRUE(log.contains("dispatch: 5 topics > explicit Auto threshold 4"));
    ASSERT_EQ(seen.size(), 10u);
    for (const auto& [key, order] : seen) {
        ASSERT_EQ(order.size(), 100u) << key;
        for (int index = 0; index < 100; ++index) EXPECT_EQ(order[index], index) << key;
    }
    dispatcher.stop();
}

TEST(PubSubDispatcher, PoolSizesFromActiveKeysAndShrinksWithHysteresis) {
    CapturedLog log;
    Dispatcher dispatcher({.mode = DispatchMode::Pool,
                           .topics_per_thread = 250,
                           .minimum_threads = 1,
                           .maximum_threads = 8,
                           .shrink_hysteresis_keys = 10},
                          log.sink());
    EXPECT_EQ(dispatcher.pool_thread_count(), 1u);
    for (std::size_t key = 0; key < 300; ++key) {
        dispatcher.register_key("topic:" + std::to_string(key));
    }
    EXPECT_EQ(dispatcher.pool_thread_count(), 2u);
    for (std::size_t key = 300; key < 1000; ++key) {
        dispatcher.register_key("topic:" + std::to_string(key));
    }
    EXPECT_EQ(dispatcher.pool_thread_count(), 4u);
    for (std::size_t key = 1000; key-- > 240;) {
        dispatcher.remove_key("topic:" + std::to_string(key));
    }
    EXPECT_EQ(dispatcher.key_count(), 240u);
    EXPECT_EQ(dispatcher.pool_thread_count(), 1u);
    EXPECT_TRUE(log.contains("dispatch: 251 topics; pool 1 \xE2\x86\x92 2 threads"));
    EXPECT_TRUE(log.contains("dispatch: 751 topics; pool 3 \xE2\x86\x92 4 threads"));
    EXPECT_TRUE(log.contains("dispatch: 240 topics; pool 2 \xE2\x86\x92 1 threads"));
}

void expect_order_and_fairness(std::size_t threads) {
    Dispatcher dispatcher({.mode = DispatchMode::Pool,
                           .topics_per_thread = 1,
                           .minimum_threads = threads,
                           .maximum_threads = threads,
                           .per_turn_cap = 4},
                          nullptr);
    std::mutex mutex;
    std::condition_variable gate;
    bool first_started = false;
    bool release_first = false;
    std::vector<int> ordered;
    std::vector<std::string> turns;
    dispatcher.submit("topic:ordered", [&] {
        std::unique_lock lock(mutex);
        first_started = true;
        gate.notify_one();
        gate.wait(lock, [&] { return release_first; });
        ordered.push_back(0);
        turns.push_back("ordered");
    });
    {
        std::unique_lock lock(mutex);
        gate.wait(lock, [&] { return first_started; });
    }
    for (int index = 1; index < 200; ++index) {
        dispatcher.submit("topic:ordered", [&, index] {
            std::lock_guard lock(mutex);
            ordered.push_back(index);
            turns.push_back("ordered");
        });
    }

    std::string peer = "topic:peer";
    while (std::hash<std::string>{}(peer) % threads !=
           std::hash<std::string>{}("topic:ordered") % threads) {
        peer.push_back('x');
    }
    dispatcher.submit(peer, [&] {
        std::lock_guard lock(mutex);
        turns.push_back("peer");
    });
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    gate.notify_one();
    dispatcher.drain();

    ASSERT_EQ(ordered.size(), 200u);
    for (int index = 0; index < 200; ++index) {
        EXPECT_EQ(ordered[index], index);
    }
    const auto peer_turn = std::ranges::find(turns, "peer");
    ASSERT_NE(peer_turn, turns.end());
    EXPECT_LE(std::distance(turns.begin(), peer_turn), 5);
}

TEST(PubSubDispatcher, KeepsOrderAndFairnessWithOnePoolThread) {
    expect_order_and_fairness(1);
}

TEST(PubSubDispatcher, KeepsOrderAndFairnessWithSeveralPoolThreads) {
    expect_order_and_fairness(4);
}

TEST(PubSubDispatcher, KeepsOrderAcrossGrowthAndShrink) {
    Dispatcher dispatcher({.mode = DispatchMode::Pool,
                           .topics_per_thread = 250,
                           .minimum_threads = 1,
                           .maximum_threads = 4,
                           .shrink_hysteresis_keys = 10,
                           .per_turn_cap = 8},
                          nullptr);
    std::mutex mutex;
    std::condition_variable gate;
    bool first_started = false;
    bool release_first = false;
    bool second_started = false;
    bool release_second = false;
    std::thread::id first_worker;
    std::thread::id second_worker;
    std::vector<int> seen;
    std::string target = "topic:resize";
    while (std::hash<std::string>{}(target) % 2 == 0) {
        target.push_back('x');
    }
    dispatcher.register_key(target);
    for (std::size_t key = 0; key < 239; ++key) {
        dispatcher.register_key("topic:" + std::to_string(key));
    }
    dispatcher.submit(target, [&] {
        std::unique_lock lock(mutex);
        first_started = true;
        first_worker = std::this_thread::get_id();
        gate.notify_one();
        gate.wait(lock, [&] { return release_first; });
        seen.push_back(0);
    });
    {
        std::unique_lock lock(mutex);
        gate.wait(lock, [&] { return first_started; });
    }
    dispatcher.submit(target, [&] {
        std::unique_lock lock(mutex);
        second_started = true;
        second_worker = std::this_thread::get_id();
        gate.notify_one();
        gate.wait(lock, [&] { return release_second; });
        seen.push_back(1);
    });
    for (int index = 2; index < 1000; ++index) {
        dispatcher.submit(target, [&, index] {
            std::lock_guard lock(mutex);
            seen.push_back(index);
        });
    }
    for (std::size_t key = 239; key < 299; ++key) {
        dispatcher.register_key("topic:" + std::to_string(key));
    }
    EXPECT_EQ(dispatcher.pool_thread_count(), 2u);
    {
        std::lock_guard lock(mutex);
        release_first = true;
    }
    gate.notify_one();
    {
        std::unique_lock lock(mutex);
        gate.wait(lock, [&] { return second_started; });
    }
    EXPECT_NE(first_worker, second_worker);
    for (std::size_t key = 299; key-- > 239;) {
        dispatcher.remove_key("topic:" + std::to_string(key));
    }
    EXPECT_EQ(dispatcher.pool_thread_count(), 1u);
    {
        std::lock_guard lock(mutex);
        release_second = true;
    }
    gate.notify_one();
    dispatcher.drain();
    ASSERT_EQ(seen.size(), 1000u);
    for (int index = 0; index < 1000; ++index) {
        EXPECT_EQ(seen[index], index);
    }
}

TEST(PubSubDispatcher, MarksWorkerJobsAsNonBlocking) {
    Dispatcher dispatcher({.mode = DispatchMode::Pool,
                           .minimum_threads = 1,
                           .maximum_threads = 1},
                          nullptr);
    std::atomic<bool> blocking_allowed{true};
    dispatcher.submit("topic:non-blocking", [&] {
        blocking_allowed = Dispatcher::blocking_allowed();
    });
    dispatcher.drain();
    EXPECT_FALSE(blocking_allowed.load());
}

#ifndef NDEBUG
TEST(PubSubDispatcher, DebugBuildRejectsBlockingOnAWorker) {
    EXPECT_DEATH(
        {
            Dispatcher dispatcher({.mode = DispatchMode::Pool,
                                   .minimum_threads = 1,
                                   .maximum_threads = 1},
                                  nullptr);
            dispatcher.submit("topic:blocked", [] {
                Dispatcher::assert_blocking_allowed();
            });
            dispatcher.drain();
        },
        "must never block");
}
#endif

TEST(PubSubDispatcherBenchmark, DISABLED_ThroughputOneAndFourThreads) {
    constexpr std::size_t kKeys = 1000;
    constexpr std::size_t kJobsPerKey = 1000;
    for (const std::size_t threads : {std::size_t{1}, std::size_t{4}}) {
        Dispatcher dispatcher({.mode = DispatchMode::Pool,
                               .topics_per_thread = kKeys,
                               .minimum_threads = threads,
                               .maximum_threads = threads},
                              nullptr);
        std::atomic<std::uint64_t> completed{0};
        const auto started = std::chrono::steady_clock::now();
        for (std::size_t key = 0; key < kKeys; ++key) {
            const std::string name = "topic:" + std::to_string(key);
            for (std::size_t job = 0; job < kJobsPerKey; ++job) {
                dispatcher.submit(name, [&] {
                    completed.fetch_add(1, std::memory_order_relaxed);
                });
            }
        }
        dispatcher.drain();
        const auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        const double jobs_per_second =
            static_cast<double>(completed.load()) / elapsed;
        std::cout << "dispatch benchmark: threads=" << threads
                  << " keys=" << kKeys
                  << " jobs=" << completed.load()
                  << " seconds=" << elapsed
                  << " jobs_per_second=" << jobs_per_second << '\n';
        EXPECT_EQ(completed.load(), kKeys * kJobsPerKey);
    }
}

TEST(PubSubDispatcher, InlineRunsOnTheCallerAndPinsOverrideTheMode) {
    Dispatcher dispatcher({.mode = DispatchMode::Inline}, nullptr);
    const auto caller = std::this_thread::get_id();
    std::thread::id ran_on;
    dispatcher.submit("a", [&] { ran_on = std::this_thread::get_id(); });
    EXPECT_EQ(ran_on, caller);

    Dispatcher threaded({.mode = DispatchMode::PerKey}, nullptr);
    threaded.pin("hot", DispatchMode::Inline);
    threaded.submit("hot", [&] { ran_on = std::this_thread::get_id(); });
    EXPECT_EQ(ran_on, caller);
    std::atomic<bool> other_thread{false};
    threaded.submit("cold", [&] { other_thread = std::this_thread::get_id() != caller; });
    threaded.drain();
    EXPECT_TRUE(other_thread.load());
}

TEST(PubSubDispatcher, StopRunsQueuedWorkThenRefuses) {
    Dispatcher dispatcher({.mode = DispatchMode::PerKey}, nullptr);
    std::atomic<int> ran{0};
    for (int index = 0; index < 50; ++index) dispatcher.submit("k", [&] { ++ran; });
    dispatcher.stop();
    EXPECT_EQ(ran.load(), 50);
    EXPECT_FALSE(dispatcher.submit("k", [] {}));
}

// A fake pod: cgroup v2 limits in a nested cgroup, a looser parent, a host.
ResourceProbe fake_probe(std::map<std::string, std::string> files, std::map<std::string, std::string> env,
                         std::uint64_t host_memory = std::uint64_t{256} << 30) {
    ResourceProbe probe;
    probe.proc_root = "/proc";
    probe.read_file = [files](const std::filesystem::path& path) -> std::optional<std::string> {
        auto found = files.find(path.string());
        if (found == files.end()) return std::nullopt;
        return found->second;
    };
    probe.environment = [env](std::string_view name) -> std::optional<std::string> {
        auto found = env.find(std::string(name));
        if (found == env.end()) return std::nullopt;
        return found->second;
    };
    probe.host_memory_total = [host_memory] { return std::optional<std::uint64_t>(host_memory); };
    probe.host_memory_available = [] { return std::optional<std::uint64_t>(std::uint64_t{100} << 30); };
    probe.disk_free = [](const std::filesystem::path&) { return std::optional<std::uint64_t>(std::uint64_t{360} << 30); };
    probe.hardware_threads = [] { return 16U; };
    return probe;
}

TEST(PubSubResources, CgroupV2TightestAncestorWins) {
    const std::map<std::string, std::string> files = {
        {"/proc/self/cgroup", "0::/kubepods/pod1/zetta\n"},
        {"/proc/self/mountinfo",
         "30 25 0:26 / /sys/fs/cgroup rw,nosuid shared:4 - cgroup2 cgroup2 rw\n"},
        {"/sys/fs/cgroup/kubepods/pod1/zetta/memory.max", "max\n"},
        {"/sys/fs/cgroup/kubepods/pod1/memory.max", "4294967296\n"},
        {"/sys/fs/cgroup/kubepods/memory.max", "8589934592\n"},
        {"/sys/fs/cgroup/kubepods/pod1/zetta/cpu.max", "150000 100000\n"},
    };
    const auto budget = detect_resources(fake_probe(files, {}), "/var/lib/zetta");
    EXPECT_EQ(budget.memory.value, std::uint64_t{4} << 30);
    EXPECT_NE(budget.memory.source.find("cgroup v2 memory.max /sys/fs/cgroup/kubepods/pod1"), std::string::npos);
    ASSERT_FALSE(budget.memory_ignored.empty());
    EXPECT_NE(budget.memory_ignored.back().find("host 256.0 GiB"), std::string::npos);
    EXPECT_DOUBLE_EQ(budget.cpus, 1.5);
    EXPECT_EQ(budget.disk_free.value, std::uint64_t{360} << 30);

    CapturedLog log;
    const auto limits = derive_limits(budget, {}, log.sink());
    EXPECT_EQ(limits.memory_bytes, std::uint64_t{1} << 30);        // 25% of 4 GiB
    EXPECT_EQ(limits.spill_disk_bytes, std::uint64_t{180} << 30);  // 50% of 360 GiB
    EXPECT_EQ(limits.topics_per_thread, 250u);
    EXPECT_EQ(limits.dispatch_min_threads, 1u);
    EXPECT_EQ(limits.dispatch_max_threads, 2u);                    // ceil(1.5)
    EXPECT_TRUE(log.contains("memory budget 4.0 GiB (cgroup v2 memory.max"));
    EXPECT_TRUE(log.contains("host 256.0 GiB ignored"));
    EXPECT_TRUE(log.contains("memory for topics 1.0 GiB (25% of budget; override: pubsub.memory)"));
    EXPECT_TRUE(log.contains("spill disk 180.0 GiB (50% of 360.0 GiB free"));
    EXPECT_TRUE(log.contains("cpu 1.5 (cgroup v2 cpu.max 150000 100000)"));
    EXPECT_TRUE(log.contains(
        "dispatch pool one thread per 250 topics, minimum 1, maximum 2 threads"));
}

TEST(PubSubResources, CgroupV1AndDownwardApi) {
    const std::map<std::string, std::string> files = {
        {"/proc/self/cgroup", "5:memory:/docker/abc\n4:cpu,cpuacct:/docker/abc\n"},
        {"/proc/self/mountinfo",
         "40 30 0:35 / /sys/fs/cgroup/memory rw - cgroup cgroup rw,memory\n"
         "41 30 0:36 / /sys/fs/cgroup/cpu,cpuacct rw - cgroup cgroup rw,cpu,cpuacct\n"},
        {"/sys/fs/cgroup/memory/docker/abc/memory.limit_in_bytes", "2147483648\n"},
        {"/sys/fs/cgroup/memory/memory.limit_in_bytes", "9223372036854771712\n"},  // unlimited sentinel
        {"/sys/fs/cgroup/cpu,cpuacct/docker/abc/cpu.cfs_quota_us", "200000\n"},
        {"/sys/fs/cgroup/cpu,cpuacct/docker/abc/cpu.cfs_period_us", "100000\n"},
    };
    const std::map<std::string, std::string> env = {{"ZETTA_POD_MEMORY_LIMIT", "3221225472"},
                                                    {"ZETTA_POD_MEMORY_REQUEST", "1073741824"}};
    const auto budget = detect_resources(fake_probe(files, env), "/data");
    EXPECT_EQ(budget.memory.value, std::uint64_t{2} << 30);  // the cgroup is tighter than the pod limit
    EXPECT_NE(budget.memory.source.find("cgroup v1"), std::string::npos);
    ASSERT_TRUE(budget.memory_request);
    EXPECT_EQ(budget.memory_request->value, std::uint64_t{1} << 30);
    EXPECT_DOUBLE_EQ(budget.cpus, 2.0);
}

TEST(PubSubResources, HostFallbackAndConfigurationOverrides) {
    const auto budget = detect_resources(fake_probe({}, {}, std::uint64_t{64} << 30), "/data",
                                         ResourceOverrides{.cpus = 3});
    EXPECT_EQ(budget.memory.value, std::uint64_t{64} << 30);
    EXPECT_EQ(budget.memory.source, "host");
    EXPECT_DOUBLE_EQ(budget.cpus, 3.0);
    EXPECT_EQ(budget.cpu_source, "configuration");

    const auto configured = detect_resources(fake_probe({}, {}), "/data",
                                             ResourceOverrides{.memory_bytes = std::uint64_t{8} << 30});
    EXPECT_EQ(configured.memory.source, "configuration");
    CapturedLog log;
    const auto limits = derive_limits(
        configured,
        LimitOverrides{.memory_bytes = 12345,
                       .topics_per_thread = 100,
                       .dispatch_min_threads = 3,
                       .dispatch_max_threads = 5},
        log.sink());
    EXPECT_EQ(limits.memory_bytes, 12345u);
    EXPECT_EQ(limits.topics_per_thread, 100u);
    EXPECT_EQ(limits.dispatch_min_threads, 3u);
    EXPECT_EQ(limits.dispatch_max_threads, 5u);
    EXPECT_TRUE(log.contains("memory for topics 12.1 KiB (configuration)"));
}

TEST(PubSubResources, FormatsBytes) {
    EXPECT_EQ(format_bytes(900), "900 B");
    EXPECT_EQ(format_bytes(12 * 1024), "12.0 KiB");
    EXPECT_EQ(format_bytes(std::uint64_t{4} << 30), "4.0 GiB");
}

TEST(PubSubResources, SystemProbeAnswersOnThisMachine) {
    const auto budget = detect_resources(system_probe(), std::filesystem::temp_directory_path());
    EXPECT_GT(budget.memory.value, 0u);
    EXPECT_GT(budget.cpus, 0.0);
    EXPECT_GT(budget.disk_free.value, 0u);
}

}  // namespace
}  // namespace zetta::pubsub
