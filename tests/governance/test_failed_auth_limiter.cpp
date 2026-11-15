// The HTTP gate's failed-authentication budget per source (#1417, LSR-244):
// a burst of failures, refilled over time, refused with a retry hint once
// spent, bounded in how many sources it tracks.
#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "zetta/governance/failed_auth_limiter.hpp"

namespace {

using zetta::governance::FailedAuthLimiter;

TEST(FailedAuthLimiter, RefusesASourceThatSpentItsBurstUntilItRefills) {
    FailedAuthLimiter limiter;
    const auto start = FailedAuthLimiter::Clock::now();
    EXPECT_TRUE(limiter.admit("198.51.100.7", start).admitted);
    for (std::uint32_t failure = 0; failure < FailedAuthLimiter::kBurst; ++failure) {
        ASSERT_TRUE(limiter.admit("198.51.100.7", start).admitted) << failure;
        limiter.record_failure("198.51.100.7", start);
    }
    const auto refused = limiter.admit("198.51.100.7", start);
    EXPECT_FALSE(refused.admitted);
    EXPECT_GE(refused.retry_after_seconds, 1u);
    // Another source is unaffected.
    EXPECT_TRUE(limiter.admit("198.51.100.8", start).admitted);
    // One refill period buys one more check.
    EXPECT_TRUE(limiter.admit("198.51.100.7", start + FailedAuthLimiter::kRefill).admitted);
    // A fully refilled source is forgotten.
    EXPECT_TRUE(limiter
                    .admit("198.51.100.7",
                           start + FailedAuthLimiter::kRefill * (FailedAuthLimiter::kBurst + 1))
                    .admitted);
    EXPECT_EQ(limiter.tracked_sources(), 0u);
}

TEST(FailedAuthLimiter, TracksABoundedNumberOfSources) {
    FailedAuthLimiter limiter;
    auto now = FailedAuthLimiter::Clock::now();
    for (std::size_t source = 0; source < FailedAuthLimiter::kMaxSources + 50; ++source) {
        now += std::chrono::microseconds(1);
        limiter.record_failure("source-" + std::to_string(source), now);
    }
    EXPECT_EQ(limiter.tracked_sources(), FailedAuthLimiter::kMaxSources);
}

}  // namespace
