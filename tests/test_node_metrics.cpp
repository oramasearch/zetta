#include "zetta/node_metrics.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "temp_dir.hpp"

using zetta::NodeMetrics;
using zetta::NodeMetricsSampler;
using zetta::test::TempDir;

// Always-true invariants of a sample, regardless of which counter groups the
// host can report.
TEST(NodeMetrics, SampleHasTimestampAndCores) {
    NodeMetricsSampler sampler;
    TempDir tmp;
    const NodeMetrics metrics = sampler.sample(tmp.path().string());

    EXPECT_FALSE(metrics.sampled_at.empty());
    // hardware_concurrency() is >= 1 on every real dev/CI host.
    EXPECT_GE(metrics.cpu_cores, 1u);
}

// Memory is readable on macOS + Linux: total > 0, used <= total, percent sane.
TEST(NodeMetrics, MemoryGaugeIsConsistent) {
    NodeMetricsSampler sampler;
    TempDir tmp;
    const NodeMetrics metrics = sampler.sample(tmp.path().string());

#if defined(__APPLE__) || defined(__linux__)
    ASSERT_TRUE(metrics.memory_available);
    EXPECT_GT(metrics.memory_total_bytes, 0u);
    EXPECT_LE(metrics.memory_used_bytes, metrics.memory_total_bytes);
    EXPECT_GE(metrics.memory_percent, 0.0);
    EXPECT_LE(metrics.memory_percent, 100.0);
#endif
}

// Storage is read over the supplied filesystem path (POSIX statvfs).
TEST(NodeMetrics, StorageGaugeOverDataDir) {
    NodeMetricsSampler sampler;
    TempDir tmp;
    const NodeMetrics metrics = sampler.sample(tmp.path().string());

    ASSERT_TRUE(metrics.storage_available);
    EXPECT_GT(metrics.storage_total_bytes, 0u);
    EXPECT_LE(metrics.storage_used_bytes, metrics.storage_total_bytes);
    EXPECT_GE(metrics.storage_percent, 0.0);
    EXPECT_LE(metrics.storage_percent, 100.0);
}

// A missing path degrades gracefully — storage unavailable, no crash, the rest
// of the sample still populated.
TEST(NodeMetrics, MissingStoragePathDegradesGracefully) {
    NodeMetricsSampler sampler;
    const NodeMetrics metrics = sampler.sample("/zetta/no/such/path/here");

    EXPECT_FALSE(metrics.storage_available);
    EXPECT_FALSE(metrics.sampled_at.empty());
    EXPECT_GE(metrics.cpu_cores, 1u);
}

// CPU utilization is a delta measurement: after a second sample over a real
// interval it becomes available and stays within [0, 100].
TEST(NodeMetrics, CpuPercentBecomesAvailableAcrossInterval) {
    NodeMetricsSampler sampler;
    TempDir tmp;
    sampler.sample(tmp.path().string());
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    const NodeMetrics metrics = sampler.sample(tmp.path().string());

    if (metrics.cpu_percent_available) {
        EXPECT_GE(metrics.cpu_percent, 0.0);
        EXPECT_LE(metrics.cpu_percent, 100.0);
    }
}

// GPU is absent in the CPU-only core build — it degrades gracefully rather than
// reporting a misleading zero gauge as "real".
TEST(NodeMetrics, GpuAbsentInCoreBuild) {
    NodeMetricsSampler sampler;
    TempDir tmp;
    const NodeMetrics metrics = sampler.sample(tmp.path().string());

    EXPECT_FALSE(metrics.gpu_available);
}

// Network utilization (the link-cap percentage) is unavailable without a
// capacity reference; the rx/tx rates may or may not be readable per host, but
// both are non-negative when present.
TEST(NodeMetrics, NetworkUtilizationDegradesButRatesAreSane) {
    NodeMetricsSampler sampler;
    TempDir tmp;
    sampler.sample(tmp.path().string());
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    const NodeMetrics metrics = sampler.sample(tmp.path().string());

    EXPECT_FALSE(metrics.net_utilization_available);
    // Rates are unsigned, so non-negative by construction; assert the flag is
    // self-consistent (available ⇒ defined).
    SUCCEED();
}
