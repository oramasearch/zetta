// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "zetta/ml/next_best_action.hpp"

namespace {

using zetta::ml::LoggedPolicyDecision;
using zetta::ml::NextBestActionObservation;
using zetta::ml::NextBestActionParams;
using zetta::ml::RankedAction;

NextBestActionParams small_params() {
  NextBestActionParams params;
  params.propensity.num_trees = 8;
  params.propensity.max_depth = 2;
  params.propensity.max_bins = 8;
  params.factors = 4;
  params.als_iterations = 8;
  params.hit_rate_k = 5;
  return params;
}

} // namespace

TEST(NextBestActionOfflinePolicy, IpsAndDoublyRobustRecoverKnownValue) {
  std::vector<LoggedPolicyDecision> decisions;
  for (int index = 0; index < 100; ++index) {
    const bool logged_target = index % 2 == 0;
    const bool reward = logged_target ? index % 10 != 0 : index % 10 == 1;
    decisions.push_back({.logged_action = logged_target ? 1 : 0,
                         .selected_action = 1,
                         .reward = reward ? 1.0 : 0.0,
                         .logging_propensity = 0.5,
                         .logged_reward_prediction = logged_target ? 0.8 : 0.2,
                         .selected_reward_prediction = 0.8});
  }
  const auto evaluated = zetta::ml::evaluate_offline_policy(decisions);
  ASSERT_TRUE(evaluated.available) << evaluated.refusal;
  EXPECT_NEAR(evaluated.inverse_propensity_value, 0.8, 1e-12);
  EXPECT_NEAR(evaluated.doubly_robust_value, 0.8, 1e-12);
  EXPECT_NEAR(evaluated.effective_sample_size, 50.0, 1e-12);
  EXPECT_DOUBLE_EQ(evaluated.maximum_weight, 2.0);
}

TEST(NextBestActionOfflinePolicy, RefusesMissingLoggingPropensities) {
  const std::vector<LoggedPolicyDecision> decisions = {
      {.logged_action = 0,
       .selected_action = 0,
       .reward = 1.0,
       .logging_propensity = std::nullopt,
       .logged_reward_prediction = 0.5,
       .selected_reward_prediction = 0.5}};
  const auto evaluated = zetta::ml::evaluate_offline_policy(decisions);
  EXPECT_FALSE(evaluated.available);
  EXPECT_NE(evaluated.refusal.find("required"), std::string::npos);
  EXPECT_NE(evaluated.refusal.find("not inferred"), std::string::npos);
}

TEST(NextBestActionPolicy, IneligibleActionsAndContactCapsAreHardConstraints) {
  std::vector<NextBestActionObservation> observations;
  for (std::int64_t entity = 0; entity < 12; ++entity) {
    for (std::int64_t action = 0; action < 2; ++action) {
      const double feature = static_cast<double>(entity) / 11.0;
      NextBestActionObservation observation;
      observation.entity = entity;
      observation.action = action;
      observation.features = {feature};
      observation.response = action == 0 ? (feature > 0.45 ? 1.0 : 0.0)
                                         : (feature < 0.55 ? 1.0 : 0.0);
      observation.value = 10.0;
      observation.cost = 1.0;
      observation.logging_propensity = 0.5;
      if (entity == 0 && action == 1) {
        observation.eligible = false;
      }
      if (entity == 1 && action == 0) {
        observation.contacts = 2.0;
        observation.contact_cap = 2.0;
      }
      observations.push_back(std::move(observation));
    }
  }
  NextBestActionParams params = small_params();
  params.als_weight = 0.0;
  const auto result =
      zetta::ml::train_next_best_action(observations, {"engagement"}, params);
  ASSERT_TRUE(result.ok()) << result.error;

  std::map<std::int64_t, int> chosen_per_entity;
  for (const RankedAction &action : result.recommendations) {
    if (!action.chosen) {
      continue;
    }
    EXPECT_TRUE(action.eligible);
    EXPECT_FALSE(action.capped);
    ++chosen_per_entity[action.entity];
  }
  for (const auto &[entity, count] : chosen_per_entity) {
    EXPECT_EQ(count, 1) << entity;
  }
  const auto ineligible = std::ranges::find_if(
      result.recommendations, [](const RankedAction &action) {
        return action.entity == 0 && action.action == 1;
      });
  ASSERT_NE(ineligible, result.recommendations.end());
  EXPECT_FALSE(ineligible->chosen);
  const auto capped = std::ranges::find_if(
      result.recommendations, [](const RankedAction &action) {
        return action.entity == 1 && action.action == 0;
      });
  ASSERT_NE(capped, result.recommendations.end());
  EXPECT_FALSE(capped->chosen);
}

TEST(NextBestActionALS, PlantedAffinityBeatsPopularityOnTimeHoldout) {
  constexpr std::int64_t kUsers = 36;
  constexpr std::int64_t kActions = 12;
  std::vector<NextBestActionObservation> observations;
  for (std::int64_t entity = 0; entity < kUsers; ++entity) {
    const std::int64_t preferred = entity % kActions;
    for (std::int64_t action = 0; action < kActions; ++action) {
      const double feature =
          static_cast<double>((entity * 7 + action * 3) % 17) / 16.0;
      observations.push_back({.entity = entity,
                              .action = action,
                              .features = {feature},
                              .response = feature > 0.45 ? 1.0 : 0.0,
                              .value = 1.0,
                              .cost = 0.0,
                              .eligible = true,
                              .contacts = 0.0,
                              .contact_cap = 10.0,
                              .interaction_count = 0.0,
                              .interaction_time = 0.0,
                              .logging_propensity = 1.0 / kActions});
    }
    for (double time : {1.0, 2.0}) {
      observations.push_back({.entity = entity,
                              .action = preferred,
                              .features = {static_cast<double>(entity % 2)},
                              .response = static_cast<double>(entity % 2),
                              .value = 1.0,
                              .cost = 0.0,
                              .eligible = true,
                              .contacts = 0.0,
                              .contact_cap = 10.0,
                              .interaction_count = 4.0,
                              .interaction_time = time,
                              .logging_propensity = 1.0 / kActions});
    }
  }
  NextBestActionParams params = small_params();
  params.factors = 8;
  params.als_iterations = 12;
  params.implicit_alpha = 30.0;
  const auto result =
      zetta::ml::train_next_best_action(observations, {"signal"}, params);
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.holdout_interactions, kUsers);
  EXPECT_DOUBLE_EQ(result.holdout_cutoff, 2.0);
  EXPECT_GT(result.hit_rate_at_k, 0.8);
  EXPECT_GT(result.hit_rate_at_k, result.popularity_hit_rate_at_k + 0.25);
}
