// SPDX-License-Identifier: MIT

// zetta::util::fuzzy_score — the command palette's ranking primitive (#625).
// Pure function, so the palette's matching behaviour is pinned here headless.

#include <gtest/gtest.h>

#include "zetta/util/fuzzy.hpp"

namespace {

using zetta::util::fuzzy_score;

TEST(FuzzyScore, EmptyQueryMatchesEverythingAtZero) {
    EXPECT_EQ(fuzzy_score("", "Cluster"), 0);
    EXPECT_EQ(fuzzy_score("", ""), 0);
}

TEST(FuzzyScore, RequiresEveryQueryCharacterInOrder) {
    EXPECT_TRUE(fuzzy_score("clu", "Cluster").has_value());
    EXPECT_FALSE(fuzzy_score("clux", "Cluster").has_value());
    // In order: "ts" matches "Tests"; "st" also matches ("teSTs") but a
    // reversed fragment that never appears in order does not.
    EXPECT_FALSE(fuzzy_score("ba", "abc").has_value());
}

TEST(FuzzyScore, CaseInsensitive) {
    const auto lower = fuzzy_score("cluster", "Cluster");
    const auto upper = fuzzy_score("CLUSTER", "cluster");
    ASSERT_TRUE(lower.has_value());
    ASSERT_TRUE(upper.has_value());
    EXPECT_EQ(*lower, *upper);
}

TEST(FuzzyScore, QueryLongerThanCandidateNeverMatches) {
    EXPECT_FALSE(fuzzy_score("clusters", "Cluster").has_value());
}

TEST(FuzzyScore, ConsecutiveFragmentBeatsScatteredMatch) {
    // "clu" is a prefix fragment of "Cluster" but scattered in "Cloud
    // inspector" (c-l ... u), so the palette ranks Cluster first.
    const auto fragment = fuzzy_score("clu", "Cluster");
    const auto scattered = fuzzy_score("clu", "Cloud inspector");
    ASSERT_TRUE(fragment.has_value());
    ASSERT_TRUE(scattered.has_value());
    EXPECT_GT(*fragment, *scattered);
}

TEST(FuzzyScore, WordInitialsRankHighly) {
    // "gw" hits the two word starts of "Graph workspace".
    const auto initials = fuzzy_score("gw", "Graph workspace");
    const auto buried = fuzzy_score("gw", "wiring warehouse glow");
    ASSERT_TRUE(initials.has_value());
    EXPECT_TRUE(!buried.has_value() || *initials > *buried);
}

TEST(FuzzyScore, ShorterCandidateWinsOnTies) {
    const auto tight = fuzzy_score("run", "Runs");
    const auto sprawling = fuzzy_score("run", "Runs and their long lineage");
    ASSERT_TRUE(tight.has_value());
    ASSERT_TRUE(sprawling.has_value());
    EXPECT_GT(*tight, *sprawling);
}

TEST(FuzzyScore, MatchesSnakeCaseCapabilityNames) {
    // The palette matches registry names too ("create_project").
    EXPECT_TRUE(fuzzy_score("crepro", "create_project").has_value());
    EXPECT_TRUE(fuzzy_score("cp", "create_project").has_value());
}

}  // namespace
