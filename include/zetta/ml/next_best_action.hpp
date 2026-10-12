#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "zetta/ml/gbdt.hpp"

namespace zetta::ml {

// One candidate action observed for one entity. Entity and action identifiers
// are non-negative dense integers so the same allreduce payload has the same
// shape on every member. A row can carry both supervised response data and
// implicit interaction strength; count == 0 is an unobserved weak negative for
// ALS.
struct NextBestActionObservation {
  std::int64_t entity = 0;
  std::int64_t action = 0;
  std::vector<double> features;
  double response = 0.0;
  double value = 1.0;
  double cost = 0.0;
  bool eligible = true;
  double contacts = 0.0;
  double contact_cap = std::numeric_limits<double>::infinity();
  double interaction_count = 0.0;
  double interaction_time = 0.0;
  std::optional<double> logging_propensity;
};

struct NextBestActionParams {
  GbdtParams propensity;
  std::size_t factors = 4;
  std::size_t als_iterations = 8;
  std::size_t hit_rate_k = 5;
  double implicit_alpha = 20.0;
  double als_regularization = 0.1;
  double als_weight = 0.25;
  double budget = std::numeric_limits<double>::infinity();
  double minimum_logging_propensity = 0.01;
  double maximum_importance_weight = 20.0;
  std::uint64_t seed = 1047;
};

struct RankedAction {
  std::int64_t entity = 0;
  std::int64_t action = 0;
  std::size_t rank = 0;
  double propensity = 0.0;
  double affinity = 0.0;
  double expected_value = 0.0;
  double policy_score = 0.0;
  double cost = 0.0;
  bool eligible = false;
  bool capped = false;
  bool chosen = false;
  std::string why;
};

struct OfflinePolicyEvaluation {
  bool available = false;
  double inverse_propensity_value = 0.0;
  double doubly_robust_value = 0.0;
  double effective_sample_size = 0.0;
  double maximum_weight = 0.0;
  std::uint64_t decisions = 0;
  std::uint64_t matched_decisions = 0;
  std::string refusal;
};

struct LoggedPolicyDecision {
  std::int64_t logged_action = 0;
  std::int64_t selected_action = 0;
  double reward = 0.0;
  std::optional<double> logging_propensity;
  double logged_reward_prediction = 0.0;
  double selected_reward_prediction = 0.0;
};

OfflinePolicyEvaluation
evaluate_offline_policy(const std::vector<LoggedPolicyDecision> &decisions,
                        double minimum_logging_propensity = 0.01,
                        double maximum_importance_weight = 20.0);

// The same reduction shape used by GBDT: implementations combine element-wise
// values across every participant. Identity reduction is the single-member
// path.
using NextBestActionAllReduce = std::function<std::vector<double>(
    const std::string &, const std::vector<double> &, GbdtReduce)>;

struct NextBestActionResult {
  std::vector<RankedAction> recommendations;
  std::vector<GbdtModel> propensity_models;
  OfflinePolicyEvaluation offline;
  double hit_rate_at_k = 0.0;
  double popularity_hit_rate_at_k = 0.0;
  double holdout_cutoff = 0.0;
  std::uint64_t holdout_interactions = 0;
  std::uint64_t rows = 0;
  std::string error;

  bool ok() const noexcept { return error.empty(); }
};

// Train and score this member's owned observations. Per-action GBDT histograms,
// ALS normal equations, chronological holdout statistics, candidate scores and
// policy-evaluation totals are combined through allreduce. Every participant
// therefore returns the same models, metrics, ranking and budget decision.
NextBestActionResult train_next_best_action(
    const std::vector<NextBestActionObservation> &local_observations,
    const std::vector<std::string> &feature_names,
    const NextBestActionParams &params = {},
    const NextBestActionAllReduce &allreduce = {});

} // namespace zetta::ml
