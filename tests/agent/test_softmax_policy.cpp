// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

#include "zetta/agent/softmax_policy.hpp"

namespace {

using zetta::agent::SoftmaxPolicy;

TEST(SoftmaxPolicy, UniformWhenLogitsAreEqual) {
    const SoftmaxPolicy policy(3);
    const std::vector<double> probs = policy.probabilities();
    ASSERT_EQ(probs.size(), 3u);
    for (double p : probs) {
        EXPECT_NEAR(p, 1.0 / 3.0, 1e-12);
    }
}

TEST(SoftmaxPolicy, ProbabilitiesSumToOne) {
    const SoftmaxPolicy policy({2.0, -1.0, 0.5, 3.0});
    double sum = 0.0;
    for (double p : policy.probabilities()) {
        sum += p;
    }
    EXPECT_NEAR(sum, 1.0, 1e-12);
}

TEST(SoftmaxPolicy, HigherLogitMeansHigherProbability) {
    const SoftmaxPolicy policy({2.0, 0.0, 0.0});
    const std::vector<double> probs = policy.probabilities();
    EXPECT_GT(probs[0], probs[1]);
    EXPECT_NEAR(probs[1], probs[2], 1e-12);  // the two equal logits tie
}

TEST(SoftmaxPolicy, SampleIsDeterministicAndSpansActions) {
    const SoftmaxPolicy policy({0.0, 0.0, 0.0, 0.0});  // uniform over 4
    EXPECT_EQ(policy.sample(0.0), 0u);    // first quarter
    EXPECT_EQ(policy.sample(0.3), 1u);    // second quarter
    EXPECT_EQ(policy.sample(0.6), 2u);    // third quarter
    EXPECT_EQ(policy.sample(0.99), 3u);   // last quarter
    EXPECT_EQ(policy.sample(1.0), 3u);    // rounding guard -> last action
}

TEST(SoftmaxPolicy, LogProbMatchesProbabilities) {
    const SoftmaxPolicy policy({1.0, -0.5, 0.25});
    const std::vector<double> probs = policy.probabilities();
    for (std::size_t action = 0; action < policy.action_count(); ++action) {
        EXPECT_NEAR(policy.log_prob(action), std::log(probs[action]), 1e-12);
    }
    EXPECT_EQ(policy.log_prob(99), -std::numeric_limits<double>::infinity());
}

}  // namespace
