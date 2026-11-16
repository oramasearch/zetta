// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdint>

#include <nlohmann/json.hpp>

#include "zetta/compute/scaling_policy.hpp"

using zetta::compute::ScalingAction;
using zetta::compute::ScalingBounds;
using zetta::compute::ScalingDecision;
using zetta::compute::ScalingObjective;
using zetta::compute::ScalingPolicy;
using zetta::compute::ScalingSample;

namespace {

ScalingPolicy minimize_cost(std::uint32_t min_nodes, std::uint32_t max_nodes,
                            double cost_cap = 0.0) {
    return ScalingPolicy{ScalingObjective::MinimizeCost,
                         ScalingBounds{min_nodes, max_nodes, cost_cap}, 0.0};
}

ScalingPolicy runtime_target(std::uint32_t min_nodes, std::uint32_t max_nodes,
                             double target_seconds, double cost_cap = 0.0) {
    return ScalingPolicy{ScalingObjective::RuntimeTarget,
                         ScalingBounds{min_nodes, max_nodes, cost_cap},
                         target_seconds};
}

ScalingPolicy data_shape(std::uint32_t min_nodes, std::uint32_t max_nodes) {
    return ScalingPolicy{ScalingObjective::DataShape,
                         ScalingBounds{min_nodes, max_nodes, 0.0}, 0.0};
}

// A live sample: progress in [0,1], elapsed seconds, accumulated cost, current
// node allocation — the (progress, elapsed, cost) signals the acceptance names.
ScalingSample sample(double progress, double elapsed, double cost,
                     std::uint32_t nodes) {
    return ScalingSample{progress, elapsed, cost, nodes};
}

}  // namespace

// ---- objective / action enum names -----------------------------------------

TEST(ScalingPolicy, ObjectiveNamesRoundTrip) {
    EXPECT_EQ(zetta::compute::to_string(ScalingObjective::MinimizeCost), "minimize_cost");
    EXPECT_EQ(zetta::compute::to_string(ScalingObjective::RuntimeTarget), "runtime_target");
    EXPECT_EQ(zetta::compute::to_string(ScalingObjective::DataShape), "data_shape");

    ScalingObjective objective{};
    EXPECT_TRUE(zetta::compute::scaling_objective_from_string("runtime_target", objective));
    EXPECT_EQ(objective, ScalingObjective::RuntimeTarget);
    EXPECT_FALSE(zetta::compute::scaling_objective_from_string("maximize_joy", objective));
}

TEST(ScalingPolicy, ActionNames) {
    EXPECT_EQ(zetta::compute::to_string(ScalingAction::ScaleDown), "scale_down");
    EXPECT_EQ(zetta::compute::to_string(ScalingAction::Hold), "hold");
    EXPECT_EQ(zetta::compute::to_string(ScalingAction::ScaleUp), "scale_up");
}

// ---- clamp_nodes -----------------------------------------------------------

TEST(ScalingPolicy, ClampNodesHoldsTheEnvelope) {
    const ScalingPolicy policy = minimize_cost(2, 8);
    EXPECT_EQ(policy.clamp_nodes(1), 2u);   // below floor
    EXPECT_EQ(policy.clamp_nodes(5), 5u);   // inside
    EXPECT_EQ(policy.clamp_nodes(99), 8u);  // above ceiling
}

// ---- objective evaluation: MinimizeCost ------------------------------------

TEST(ScalingPolicy, MinimizeCostHoldsAtFloorAndRewardsLowSpend) {
    const ScalingPolicy policy = minimize_cost(1, 16);

    // Running above the floor on a cheap job → recommend shrinking to min_nodes.
    const ScalingDecision cheap = policy.evaluate(sample(0.5, 10.0, 5.0, 4));
    EXPECT_EQ(cheap.recommended_nodes, 1u);
    EXPECT_EQ(cheap.action, ScalingAction::ScaleDown);
    EXPECT_TRUE(cheap.within_cost_cap);
    EXPECT_DOUBLE_EQ(cheap.reward, -5.0);  // reward = -cost

    // A pricier sample scores strictly worse (the objective is to spend less).
    const ScalingDecision pricey = policy.evaluate(sample(0.5, 10.0, 20.0, 4));
    EXPECT_LT(pricey.reward, cheap.reward);

    // Already at the floor → hold, not scale-down.
    const ScalingDecision floored = policy.evaluate(sample(0.5, 10.0, 5.0, 1));
    EXPECT_EQ(floored.action, ScalingAction::Hold);
}

// ---- objective evaluation: RuntimeTarget -----------------------------------

TEST(ScalingPolicy, RuntimeTargetScalesUpWhenBehind) {
    const ScalingPolicy policy = runtime_target(1, 16, /*target=*/100.0);

    // 25% done after 50s → projected 200s, double the 100s target → behind.
    // Linear-speedup heuristic doubles the fleet: 4 → ceil(4 * 200/100) = 8.
    const ScalingDecision behind = policy.evaluate(sample(0.25, 50.0, 0.0, 4));
    EXPECT_EQ(behind.action, ScalingAction::ScaleUp);
    EXPECT_EQ(behind.recommended_nodes, 8u);
    EXPECT_DOUBLE_EQ(behind.reward, -100.0);  // -|200 - 100|
}

TEST(ScalingPolicy, RuntimeTargetScalesDownWhenAhead) {
    const ScalingPolicy policy = runtime_target(1, 16, /*target=*/100.0);

    // 80% done after 40s → projected 50s, well under the 100s target → ahead.
    // 8 → ceil(8 * 50/100) = 4, so release nodes.
    const ScalingDecision ahead = policy.evaluate(sample(0.8, 40.0, 0.0, 8));
    EXPECT_EQ(ahead.action, ScalingAction::ScaleDown);
    EXPECT_EQ(ahead.recommended_nodes, 4u);
    EXPECT_DOUBLE_EQ(ahead.reward, -50.0);  // -|50 - 100|
}

TEST(ScalingPolicy, RuntimeTargetRewardPeaksOnTarget) {
    const ScalingPolicy policy = runtime_target(1, 16, /*target=*/100.0);

    // Exactly on pace: 50% done after 50s → projected 100s == target → reward 0.
    const ScalingDecision on_target = policy.evaluate(sample(0.5, 50.0, 0.0, 4));
    EXPECT_DOUBLE_EQ(on_target.reward, 0.0);

    // Off-target in either direction scores worse than on-target.
    const ScalingDecision late = policy.evaluate(sample(0.25, 50.0, 0.0, 4));
    const ScalingDecision early = policy.evaluate(sample(0.8, 40.0, 0.0, 4));
    EXPECT_LT(late.reward, on_target.reward);
    EXPECT_LT(early.reward, on_target.reward);
}

TEST(ScalingPolicy, RuntimeTargetHoldsBeforeFirstProgress) {
    const ScalingPolicy policy = runtime_target(2, 16, /*target=*/100.0);

    // No progress yet → no throughput signal → hold at current, neutral reward.
    const ScalingDecision unknown = policy.evaluate(sample(0.0, 5.0, 0.0, 6));
    EXPECT_EQ(unknown.action, ScalingAction::Hold);
    EXPECT_EQ(unknown.recommended_nodes, 6u);
    EXPECT_DOUBLE_EQ(unknown.reward, 0.0);
}

// ---- objective evaluation: DataShape ---------------------------------------

TEST(ScalingPolicy, DataShapeSizesToRemainingWork) {
    const ScalingPolicy policy = data_shape(2, 10);  // span of 8

    // Early (10% done) → lots of work left → toward the ceiling.
    // desired = 2 + ceil(8 * 0.9) = 2 + 8 = 10.
    const ScalingDecision early = policy.evaluate(sample(0.1, 5.0, 0.0, 2));
    EXPECT_EQ(early.recommended_nodes, 10u);
    EXPECT_EQ(early.action, ScalingAction::ScaleUp);

    // Nearly done (95% done) → almost no work left → toward the floor.
    // desired = 2 + ceil(8 * 0.05) = 2 + 1 = 3.
    const ScalingDecision late = policy.evaluate(sample(0.95, 95.0, 0.0, 10));
    EXPECT_EQ(late.recommended_nodes, 3u);
    EXPECT_EQ(late.action, ScalingAction::ScaleDown);

    // Reward peaks (0) when the current allocation already matches the desired.
    const ScalingDecision matched = policy.evaluate(sample(0.1, 5.0, 0.0, 10));
    EXPECT_DOUBLE_EQ(matched.reward, 0.0);
    EXPECT_EQ(matched.action, ScalingAction::Hold);
}

// ---- the cost cap is a hard constraint across every objective --------------

TEST(ScalingPolicy, CostCapShedsToFloorRegardlessOfObjective) {
    // A runtime-target policy that would otherwise scale UP (it is behind)...
    const ScalingPolicy policy = runtime_target(1, 16, /*target=*/100.0,
                                                /*cost_cap=*/50.0);

    // ...but cost has blown past the cap → shed to min_nodes instead.
    const ScalingDecision capped = policy.evaluate(sample(0.25, 50.0, 60.0, 8));
    EXPECT_FALSE(capped.within_cost_cap);
    EXPECT_EQ(capped.recommended_nodes, 1u);
    EXPECT_EQ(capped.action, ScalingAction::ScaleDown);

    // Right at the cap is still within budget, so the objective applies normally:
    // this sample is behind (projected 200s > 100s target) → the runtime-target
    // logic scales UP, not the shed-to-floor path.
    const ScalingDecision at_cap = policy.evaluate(sample(0.25, 50.0, 50.0, 8));
    EXPECT_TRUE(at_cap.within_cost_cap);
    EXPECT_EQ(at_cap.action, ScalingAction::ScaleUp);
    EXPECT_EQ(at_cap.recommended_nodes, 16u);  // ceil(8 * 200/100), clamped to max
}

// ---- JSON boundary: round-trip ---------------------------------------------

TEST(ScalingPolicy, JsonRoundTrip) {
    const ScalingPolicy policy = runtime_target(2, 12, /*target=*/300.0,
                                                /*cost_cap=*/250.0);
    const nlohmann::json json = policy.to_json();
    EXPECT_EQ(json["objective"], "runtime_target");
    EXPECT_EQ(json["bounds"]["min_nodes"], 2);
    EXPECT_EQ(json["bounds"]["max_nodes"], 12);
    EXPECT_EQ(json["bounds"]["cost_cap"], 250.0);
    EXPECT_EQ(json["target_seconds"], 300.0);

    const auto parsed = ScalingPolicy::from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, policy);

    // MinimizeCost with no cap / no target omits the optional fields.
    const ScalingPolicy cheap = minimize_cost(1, 4);
    const nlohmann::json cheap_json = cheap.to_json();
    EXPECT_FALSE(cheap_json["bounds"].contains("cost_cap"));
    EXPECT_FALSE(cheap_json.contains("target_seconds"));
    const auto cheap_parsed = ScalingPolicy::from_json(cheap_json);
    ASSERT_TRUE(cheap_parsed);
    EXPECT_EQ(*cheap_parsed, cheap);
}

// ---- JSON boundary: rejects malformed input --------------------------------

TEST(ScalingPolicy, FromJsonRejectsMalformedAtTheBoundary) {
    using nlohmann::json;

    EXPECT_FALSE(ScalingPolicy::from_json(json::array()));   // not an object
    EXPECT_FALSE(ScalingPolicy::from_json(json::object()));  // no objective/bounds
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "maximize_joy"},
             {"bounds", {{"min_nodes", 1}, {"max_nodes", 2}}}}));  // unknown objective
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "minimize_cost"}}));  // no bounds

    // min_nodes must be >= 1.
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "minimize_cost"},
             {"bounds", {{"min_nodes", 0}, {"max_nodes", 4}}}}));

    // max_nodes must be >= min_nodes.
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "minimize_cost"},
             {"bounds", {{"min_nodes", 8}, {"max_nodes", 4}}}}));

    // negative cost_cap / target_seconds.
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "minimize_cost"},
             {"bounds", {{"min_nodes", 1}, {"max_nodes", 4}, {"cost_cap", -1.0}}}}));
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "runtime_target"},
             {"bounds", {{"min_nodes", 1}, {"max_nodes", 4}}},
             {"target_seconds", -5.0}}));

    // A runtime-target objective with no positive target is meaningless.
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "runtime_target"},
             {"bounds", {{"min_nodes", 1}, {"max_nodes", 4}}}}));
    EXPECT_FALSE(ScalingPolicy::from_json(
        json{{"objective", "runtime_target"},
             {"bounds", {{"min_nodes", 1}, {"max_nodes", 4}}},
             {"target_seconds", 0.0}}));

    // ...but a positive target makes it valid.
    EXPECT_TRUE(ScalingPolicy::from_json(
        json{{"objective", "runtime_target"},
             {"bounds", {{"min_nodes", 1}, {"max_nodes", 4}}},
             {"target_seconds", 120.0}}));
}
