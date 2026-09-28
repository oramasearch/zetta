#include "zetta/ml/next_best_action.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

// CollectiveMessage carries JSON numbers, so transport sentinels must be
// finite. These are far outside any accepted business/time value while
// round-tripping as numbers through the validated collective boundary.
constexpr double kMissingMaximum = -1.0e300;
constexpr double kMissingMinimum = 1.0e300;

struct PolicySums {
  double ips = 0.0;
  double doubly_robust = 0.0;
  double weights = 0.0;
  double squared_weights = 0.0;
  double maximum_weight = 0.0;
  double decisions = 0.0;
  double matches = 0.0;
  double missing_propensities = 0.0;
};

std::vector<double> identity_reduce(const std::string &,
                                    const std::vector<double> &local,
                                    GbdtReduce) {
  return local;
}

std::vector<double> reduce(const NextBestActionAllReduce &allreduce,
                           const std::string &tag,
                           const std::vector<double> &local, GbdtReduce op,
                           bool &failed) {
  const std::vector<double> result = allreduce(tag, local, op);
  if (result.size() != local.size()) {
    failed = true;
    return {};
  }
  return result;
}

double dot(std::span<const double> left, std::span<const double> right) {
  return std::inner_product(left.begin(), left.end(), right.begin(), 0.0);
}

// Small dense positive-definite solve for ALS normal equations. Partial
// pivoting keeps the native reference stable without introducing a matrix
// dependency. The factor count is deliberately small.
bool solve_linear(std::vector<double> matrix, std::vector<double> right,
                  std::vector<double> &solution) {
  const std::size_t size = right.size();
  if (matrix.size() != size * size || size == 0) {
    return false;
  }
  for (std::size_t column = 0; column < size; ++column) {
    std::size_t pivot = column;
    for (std::size_t row = column + 1; row < size; ++row) {
      if (std::abs(matrix[row * size + column]) >
          std::abs(matrix[pivot * size + column])) {
        pivot = row;
      }
    }
    if (std::abs(matrix[pivot * size + column]) < 1e-12) {
      return false;
    }
    if (pivot != column) {
      for (std::size_t entry = column; entry < size; ++entry) {
        std::swap(matrix[column * size + entry], matrix[pivot * size + entry]);
      }
      std::swap(right[column], right[pivot]);
    }
    const double divisor = matrix[column * size + column];
    for (std::size_t entry = column; entry < size; ++entry) {
      matrix[column * size + entry] /= divisor;
    }
    right[column] /= divisor;
    for (std::size_t row = 0; row < size; ++row) {
      if (row == column) {
        continue;
      }
      const double multiplier = matrix[row * size + column];
      for (std::size_t entry = column; entry < size; ++entry) {
        matrix[row * size + entry] -=
            multiplier * matrix[column * size + entry];
      }
      right[row] -= multiplier * right[column];
    }
  }
  solution = std::move(right);
  return true;
}

std::vector<double> gram(const std::vector<double> &factors, std::size_t rows,
                         std::size_t width) {
  std::vector<double> result(width * width, 0.0);
  for (std::size_t row = 0; row < rows; ++row) {
    const std::span<const double> values(factors.data() + row * width, width);
    for (std::size_t left = 0; left < width; ++left) {
      for (std::size_t right = 0; right < width; ++right) {
        result[left * width + right] += values[left] * values[right];
      }
    }
  }
  return result;
}

PolicySums policy_sums(const std::vector<LoggedPolicyDecision> &decisions,
                       double minimum_propensity, double maximum_weight) {
  PolicySums result;
  for (const LoggedPolicyDecision &decision : decisions) {
    result.decisions += 1.0;
    if (!decision.logging_propensity ||
        !std::isfinite(*decision.logging_propensity) ||
        *decision.logging_propensity <= 0.0) {
      result.missing_propensities += 1.0;
      continue;
    }
    const bool matches = decision.logged_action == decision.selected_action;
    const double propensity =
        std::max(*decision.logging_propensity, minimum_propensity);
    const double weight =
        matches ? std::min(1.0 / propensity, maximum_weight) : 0.0;
    result.ips += weight * decision.reward;
    result.doubly_robust +=
        decision.selected_reward_prediction +
        weight * (decision.reward - decision.logged_reward_prediction);
    result.weights += weight;
    result.squared_weights += weight * weight;
    result.maximum_weight = std::max(result.maximum_weight, weight);
    result.matches += matches ? 1.0 : 0.0;
  }
  return result;
}

OfflinePolicyEvaluation finalize_policy_sums(const PolicySums &sums) {
  OfflinePolicyEvaluation result;
  result.decisions = static_cast<std::uint64_t>(sums.decisions);
  result.matched_decisions = static_cast<std::uint64_t>(sums.matches);
  result.maximum_weight = sums.maximum_weight;
  if (sums.decisions == 0.0) {
    result.refusal = "offline policy evaluation refused: no logged decisions";
    return result;
  }
  if (sums.missing_propensities > 0.0) {
    result.refusal =
        "offline policy evaluation refused: logging propensities are "
        "required and are not inferred";
    return result;
  }
  result.available = true;
  result.inverse_propensity_value = sums.ips / sums.decisions;
  result.doubly_robust_value = sums.doubly_robust / sums.decisions;
  if (sums.squared_weights > 0.0) {
    result.effective_sample_size =
        sums.weights * sums.weights / sums.squared_weights;
  }
  return result;
}

} // namespace

OfflinePolicyEvaluation
evaluate_offline_policy(const std::vector<LoggedPolicyDecision> &decisions,
                        double minimum_logging_propensity,
                        double maximum_importance_weight) {
  if (!(minimum_logging_propensity > 0.0) ||
      !(maximum_importance_weight > 0.0)) {
    OfflinePolicyEvaluation result;
    result.refusal =
        "offline policy evaluation refused: clipping bounds must be positive";
    return result;
  }
  return finalize_policy_sums(policy_sums(decisions, minimum_logging_propensity,
                                          maximum_importance_weight));
}

NextBestActionResult train_next_best_action(
    const std::vector<NextBestActionObservation> &local_observations,
    const std::vector<std::string> &feature_names,
    const NextBestActionParams &params,
    const NextBestActionAllReduce &supplied_allreduce) {
  NextBestActionResult output;
  const NextBestActionAllReduce allreduce =
      supplied_allreduce ? supplied_allreduce : identity_reduce;
  if (feature_names.empty()) {
    output.error = "next best action: no feature columns";
    return output;
  }
  if (params.factors == 0 || params.als_iterations == 0 ||
      params.hit_rate_k == 0 || params.implicit_alpha < 0.0 ||
      params.als_regularization <= 0.0 || params.als_weight < 0.0 ||
      params.minimum_logging_propensity <= 0.0 ||
      params.maximum_importance_weight <= 0.0 || params.budget < 0.0) {
    output.error = "next best action: invalid training or policy parameters";
    return output;
  }

  double local_max_entity = -1.0;
  double local_max_action = -1.0;
  for (const NextBestActionObservation &observation : local_observations) {
    if (observation.entity < 0 || observation.action < 0 ||
        observation.features.size() != feature_names.size() ||
        !std::isfinite(observation.response) || observation.response < 0.0 ||
        observation.response > 1.0 || !std::isfinite(observation.value) ||
        !std::isfinite(observation.cost) || observation.cost < 0.0 ||
        !std::isfinite(observation.contacts) || observation.contacts < 0.0 ||
        observation.interaction_count < 0.0 ||
        !std::isfinite(observation.interaction_count) ||
        !std::isfinite(observation.interaction_time) ||
        std::isnan(observation.contact_cap) || observation.contact_cap < 0.0) {
      output.error = "next best action: invalid observation";
      return output;
    }
    if (std::ranges::any_of(observation.features, [](double value) {
          return !std::isfinite(value);
        })) {
      output.error = "next best action: feature values must be finite";
      return output;
    }
    local_max_entity =
        std::max(local_max_entity, static_cast<double>(observation.entity));
    local_max_action =
        std::max(local_max_action, static_cast<double>(observation.action));
  }

  bool collective_failed = false;
  const std::vector<double> global_max =
      reduce(allreduce, "nba.shape", {local_max_entity, local_max_action},
             GbdtReduce::Max, collective_failed);
  if (collective_failed || global_max[0] < 0.0 || global_max[1] < 0.0) {
    output.error =
        "next best action: no observations or shape allreduce failed";
    return output;
  }
  const std::size_t entity_count = static_cast<std::size_t>(global_max[0]) + 1;
  const std::size_t action_count = static_cast<std::size_t>(global_max[1]) + 1;
  if (entity_count > 100'000 || action_count > 10'000) {
    output.error =
        "next best action: dense entity/action ids exceed supported bounds";
    return output;
  }
  const std::vector<double> global_rows = reduce(
      allreduce, "nba.rows", {static_cast<double>(local_observations.size())},
      GbdtReduce::Sum, collective_failed);
  if (collective_failed) {
    output.error = "next best action: row-count allreduce failed";
    return output;
  }
  output.rows = static_cast<std::uint64_t>(global_rows[0]);

  // One binary GBDT per action. All members enter every action's rounds, even
  // when one member owns no rows for that action.
  output.propensity_models.reserve(action_count);
  for (std::size_t action = 0; action < action_count; ++action) {
    std::vector<std::vector<double>> columns(feature_names.size());
    std::vector<double> labels;
    for (const NextBestActionObservation &observation : local_observations) {
      if (observation.action != static_cast<std::int64_t>(action)) {
        continue;
      }
      for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
        columns[feature].push_back(observation.features[feature]);
      }
      labels.push_back(observation.response);
    }
    const AllReduce gbdt_reduce = [&](const std::string &round,
                                      const std::vector<double> &values,
                                      GbdtReduce op) {
      return allreduce("nba.propensity." + std::to_string(action) + "." + round,
                       values, op);
    };
    GbdtTrainResult trained =
        train_gbdt(columns, feature_names, labels, params.propensity,
                   gbdt_reduce, "nba.action." + std::to_string(action));
    if (!trained.ok()) {
      output.error = "next best action: action " + std::to_string(action) +
                     " propensity GBDT failed: " + trained.status.message;
      return output;
    }
    output.propensity_models.push_back(std::move(trained.model));
  }

  // Latest positive interaction per entity is the holdout. The max-time
  // reduction makes the split independent of physical partitioning.
  std::vector<double> local_latest(entity_count, kMissingMaximum);
  double local_positive_count = 0.0;
  for (const NextBestActionObservation &observation : local_observations) {
    if (observation.interaction_count <= 0.0) {
      continue;
    }
    local_positive_count += 1.0;
    const std::size_t entity = static_cast<std::size_t>(observation.entity);
    local_latest[entity] =
        std::max(local_latest[entity], observation.interaction_time);
  }
  const std::vector<double> positive_total =
      reduce(allreduce, "nba.als.positive", {local_positive_count},
             GbdtReduce::Sum, collective_failed);
  std::vector<double> latest = reduce(allreduce, "nba.als.latest", local_latest,
                                      GbdtReduce::Max, collective_failed);
  if (collective_failed) {
    output.error = "next best action: interaction holdout allreduce failed";
    return output;
  }

  const std::size_t grid_size = entity_count * action_count;
  std::vector<double> local_train_counts(grid_size, 0.0);
  std::vector<double> local_holdout_counts(grid_size, 0.0);
  for (const NextBestActionObservation &observation : local_observations) {
    if (observation.interaction_count <= 0.0) {
      continue;
    }
    const std::size_t entity = static_cast<std::size_t>(observation.entity);
    const std::size_t action = static_cast<std::size_t>(observation.action);
    const std::size_t index = entity * action_count + action;
    if (observation.interaction_time >= latest[entity]) {
      local_holdout_counts[index] += observation.interaction_count;
    } else {
      local_train_counts[index] += observation.interaction_count;
    }
  }
  std::vector<double> train_counts =
      reduce(allreduce, "nba.als.train", local_train_counts, GbdtReduce::Sum,
             collective_failed);
  std::vector<double> holdout_counts =
      reduce(allreduce, "nba.als.holdout", local_holdout_counts,
             GbdtReduce::Sum, collective_failed);
  if (collective_failed) {
    output.error = "next best action: interaction-count allreduce failed";
    return output;
  }

  std::vector<double> user_factors(entity_count * params.factors, 0.0);
  std::vector<double> item_factors(action_count * params.factors, 0.0);
  std::mt19937_64 generator(params.seed);
  std::uniform_real_distribution<double> initial(-0.05, 0.05);
  std::ranges::generate(user_factors, [&] { return initial(generator); });
  std::ranges::generate(item_factors, [&] { return initial(generator); });

  if (positive_total[0] > 0.0) {
    const std::size_t width = params.factors;
    const std::size_t equation_width = width * width + width;
    for (std::size_t iteration = 0; iteration < params.als_iterations;
         ++iteration) {
      const std::vector<double> item_gram =
          gram(item_factors, action_count, width);
      std::vector<double> local_user_stats(entity_count * equation_width, 0.0);
      for (const NextBestActionObservation &observation : local_observations) {
        const std::size_t entity = static_cast<std::size_t>(observation.entity);
        if (observation.interaction_count <= 0.0 ||
            observation.interaction_time >= latest[entity]) {
          continue;
        }
        const std::size_t action = static_cast<std::size_t>(observation.action);
        const double confidence =
            1.0 + params.implicit_alpha * observation.interaction_count;
        const std::span<const double> item(item_factors.data() + action * width,
                                           width);
        const std::size_t offset = entity * equation_width;
        for (std::size_t left = 0; left < width; ++left) {
          for (std::size_t right = 0; right < width; ++right) {
            local_user_stats[offset + left * width + right] +=
                (confidence - 1.0) * item[left] * item[right];
          }
          local_user_stats[offset + width * width + left] +=
              confidence * item[left];
        }
      }
      const std::vector<double> user_stats =
          reduce(allreduce, "nba.als.user." + std::to_string(iteration),
                 local_user_stats, GbdtReduce::Sum, collective_failed);
      if (collective_failed) {
        output.error = "next best action: ALS user allreduce failed";
        return output;
      }
      for (std::size_t entity = 0; entity < entity_count; ++entity) {
        std::vector<double> matrix = item_gram;
        std::vector<double> right(width, 0.0);
        const std::size_t offset = entity * equation_width;
        for (std::size_t left = 0; left < width; ++left) {
          for (std::size_t column = 0; column < width; ++column) {
            matrix[left * width + column] +=
                user_stats[offset + left * width + column];
          }
          matrix[left * width + left] += params.als_regularization;
          right[left] = user_stats[offset + width * width + left];
        }
        std::vector<double> solution;
        if (!solve_linear(std::move(matrix), std::move(right), solution)) {
          output.error = "next best action: ALS user solve failed";
          return output;
        }
        std::copy(solution.begin(), solution.end(),
                  user_factors.begin() + entity * width);
      }

      const std::vector<double> user_gram =
          gram(user_factors, entity_count, width);
      std::vector<double> local_item_stats(action_count * equation_width, 0.0);
      for (const NextBestActionObservation &observation : local_observations) {
        const std::size_t entity = static_cast<std::size_t>(observation.entity);
        if (observation.interaction_count <= 0.0 ||
            observation.interaction_time >= latest[entity]) {
          continue;
        }
        const std::size_t action = static_cast<std::size_t>(observation.action);
        const double confidence =
            1.0 + params.implicit_alpha * observation.interaction_count;
        const std::span<const double> user(user_factors.data() + entity * width,
                                           width);
        const std::size_t offset = action * equation_width;
        for (std::size_t left = 0; left < width; ++left) {
          for (std::size_t right = 0; right < width; ++right) {
            local_item_stats[offset + left * width + right] +=
                (confidence - 1.0) * user[left] * user[right];
          }
          local_item_stats[offset + width * width + left] +=
              confidence * user[left];
        }
      }
      const std::vector<double> item_stats =
          reduce(allreduce, "nba.als.item." + std::to_string(iteration),
                 local_item_stats, GbdtReduce::Sum, collective_failed);
      if (collective_failed) {
        output.error = "next best action: ALS item allreduce failed";
        return output;
      }
      for (std::size_t action = 0; action < action_count; ++action) {
        std::vector<double> matrix = user_gram;
        std::vector<double> right(width, 0.0);
        const std::size_t offset = action * equation_width;
        for (std::size_t left = 0; left < width; ++left) {
          for (std::size_t column = 0; column < width; ++column) {
            matrix[left * width + column] +=
                item_stats[offset + left * width + column];
          }
          matrix[left * width + left] += params.als_regularization;
          right[left] = item_stats[offset + width * width + left];
        }
        std::vector<double> solution;
        if (!solve_linear(std::move(matrix), std::move(right), solution)) {
          output.error = "next best action: ALS item solve failed";
          return output;
        }
        std::copy(solution.begin(), solution.end(),
                  item_factors.begin() + action * width);
      }
    }
  }

  std::vector<double> popularity(action_count, 0.0);
  for (std::size_t entity = 0; entity < entity_count; ++entity) {
    for (std::size_t action = 0; action < action_count; ++action) {
      popularity[action] += train_counts[entity * action_count + action];
    }
  }
  std::vector<std::size_t> popular_actions(action_count);
  std::iota(popular_actions.begin(), popular_actions.end(), 0);
  std::stable_sort(popular_actions.begin(), popular_actions.end(),
                   [&](std::size_t left, std::size_t right) {
                     return popularity[left] > popularity[right];
                   });
  double hits = 0.0;
  double popularity_hits = 0.0;
  double held_out = 0.0;
  for (std::size_t entity = 0; entity < entity_count; ++entity) {
    std::vector<std::size_t> actions(action_count);
    std::iota(actions.begin(), actions.end(), 0);
    const std::span<const double> user(
        user_factors.data() + entity * params.factors, params.factors);
    std::stable_sort(
        actions.begin(), actions.end(),
        [&](std::size_t left, std::size_t right) {
          const std::span<const double> left_item(
              item_factors.data() + left * params.factors, params.factors);
          const std::span<const double> right_item(
              item_factors.data() + right * params.factors, params.factors);
          return dot(user, left_item) > dot(user, right_item);
        });
    for (std::size_t action = 0; action < action_count; ++action) {
      if (holdout_counts[entity * action_count + action] <= 0.0) {
        continue;
      }
      held_out += 1.0;
      const std::size_t limit = std::min(params.hit_rate_k, action_count);
      if (std::find(actions.begin(), actions.begin() + limit, action) !=
          actions.begin() + limit) {
        hits += 1.0;
      }
      if (std::find(popular_actions.begin(), popular_actions.begin() + limit,
                    action) != popular_actions.begin() + limit) {
        popularity_hits += 1.0;
      }
    }
  }
  output.holdout_interactions = static_cast<std::uint64_t>(held_out);
  if (held_out > 0.0) {
    output.hit_rate_at_k = hits / held_out;
    output.popularity_hit_rate_at_k = popularity_hits / held_out;
    output.holdout_cutoff = *std::max_element(latest.begin(), latest.end());
  }

  // Aggregate scored candidate rows. False eligibility is sticky, contacts use
  // max, and caps use min, so no partition can relax a hard constraint.
  std::vector<double> local_candidate_sums(grid_size * 5, 0.0);
  std::vector<double> local_ineligible(grid_size, 0.0);
  std::vector<double> local_contacts(grid_size, kMissingMaximum);
  std::vector<double> local_caps(grid_size, kMissingMinimum);
  for (const NextBestActionObservation &observation : local_observations) {
    const std::size_t entity = static_cast<std::size_t>(observation.entity);
    const std::size_t action = static_cast<std::size_t>(observation.action);
    const std::size_t index = entity * action_count + action;
    const double propensity =
        output.propensity_models[action].predict_proba(observation.features);
    local_candidate_sums[index * 5] += 1.0;
    local_candidate_sums[index * 5 + 1] += propensity;
    local_candidate_sums[index * 5 + 2] += observation.value;
    local_candidate_sums[index * 5 + 3] += observation.cost;
    local_candidate_sums[index * 5 + 4] += observation.response;
    local_ineligible[index] += observation.eligible ? 0.0 : 1.0;
    local_contacts[index] =
        std::max(local_contacts[index], observation.contacts);
    const double contact_cap = std::isfinite(observation.contact_cap)
                                   ? observation.contact_cap
                                   : kMissingMinimum;
    local_caps[index] = std::min(local_caps[index], contact_cap);
  }
  const std::vector<double> candidate_sums =
      reduce(allreduce, "nba.candidates.sum", local_candidate_sums,
             GbdtReduce::Sum, collective_failed);
  const std::vector<double> ineligible =
      reduce(allreduce, "nba.candidates.ineligible", local_ineligible,
             GbdtReduce::Sum, collective_failed);
  const std::vector<double> contacts =
      reduce(allreduce, "nba.candidates.contacts", local_contacts,
             GbdtReduce::Max, collective_failed);
  const std::vector<double> caps =
      reduce(allreduce, "nba.candidates.caps", local_caps, GbdtReduce::Min,
             collective_failed);
  if (collective_failed) {
    output.error = "next best action: candidate allreduce failed";
    return output;
  }

  std::vector<std::vector<RankedAction>> by_entity(entity_count);
  for (std::size_t entity = 0; entity < entity_count; ++entity) {
    const std::span<const double> user(
        user_factors.data() + entity * params.factors, params.factors);
    for (std::size_t action = 0; action < action_count; ++action) {
      const std::size_t index = entity * action_count + action;
      const double count = candidate_sums[index * 5];
      if (count <= 0.0) {
        continue;
      }
      const std::span<const double> item(
          item_factors.data() + action * params.factors, params.factors);
      RankedAction ranked;
      ranked.entity = static_cast<std::int64_t>(entity);
      ranked.action = static_cast<std::int64_t>(action);
      ranked.propensity = candidate_sums[index * 5 + 1] / count;
      ranked.affinity = dot(user, item);
      ranked.cost = candidate_sums[index * 5 + 3] / count;
      const double value = candidate_sums[index * 5 + 2] / count;
      ranked.expected_value = ranked.propensity * value - ranked.cost;
      ranked.policy_score =
          ranked.expected_value + params.als_weight * ranked.affinity;
      ranked.eligible = ineligible[index] == 0.0;
      ranked.capped = contacts[index] >= caps[index];
      by_entity[entity].push_back(std::move(ranked));
    }
    std::stable_sort(by_entity[entity].begin(), by_entity[entity].end(),
                     [](const RankedAction &left, const RankedAction &right) {
                       const bool left_allowed = left.eligible && !left.capped;
                       const bool right_allowed =
                           right.eligible && !right.capped;
                       if (left_allowed != right_allowed) {
                         return left_allowed;
                       }
                       if (left.policy_score != right.policy_score) {
                         return left.policy_score > right.policy_score;
                       }
                       return left.action < right.action;
                     });
    for (std::size_t rank = 0; rank < by_entity[entity].size(); ++rank) {
      by_entity[entity][rank].rank = rank + 1;
    }
  }

  struct EntityChoice {
    std::size_t entity = 0;
    std::size_t index = 0;
  };
  std::vector<EntityChoice> choices;
  for (std::size_t entity = 0; entity < entity_count; ++entity) {
    for (std::size_t index = 0; index < by_entity[entity].size(); ++index) {
      const RankedAction &candidate = by_entity[entity][index];
      if (candidate.eligible && !candidate.capped) {
        choices.push_back({entity, index});
        break;
      }
    }
  }
  std::stable_sort(
      choices.begin(), choices.end(),
      [&](const EntityChoice &left, const EntityChoice &right) {
        const RankedAction &left_action = by_entity[left.entity][left.index];
        const RankedAction &right_action = by_entity[right.entity][right.index];
        if (left_action.policy_score != right_action.policy_score) {
          return left_action.policy_score > right_action.policy_score;
        }
        return left.entity < right.entity;
      });
  double spent = 0.0;
  for (const EntityChoice &choice : choices) {
    RankedAction &candidate = by_entity[choice.entity][choice.index];
    if (spent + candidate.cost <= params.budget) {
      candidate.chosen = true;
      candidate.why =
          "highest-ranked eligible action within contact cap and budget";
      spent += candidate.cost;
    } else {
      candidate.why = "not chosen: budget exhausted";
    }
  }
  for (std::vector<RankedAction> &actions : by_entity) {
    for (RankedAction &action : actions) {
      if (action.why.empty()) {
        if (!action.eligible) {
          action.why = "not chosen: ineligible";
        } else if (action.capped) {
          action.why = "not chosen: contact cap reached";
        } else {
          action.why = "not chosen: lower policy score";
        }
      }
      output.recommendations.push_back(std::move(action));
    }
  }

  std::map<std::int64_t, const RankedAction *> chosen;
  std::map<std::pair<std::int64_t, std::int64_t>, const RankedAction *> scored;
  for (const RankedAction &action : output.recommendations) {
    scored[{action.entity, action.action}] = &action;
    if (action.chosen) {
      chosen[action.entity] = &action;
    }
  }
  std::vector<LoggedPolicyDecision> local_decisions;
  local_decisions.reserve(local_observations.size());
  for (const NextBestActionObservation &observation : local_observations) {
    const auto selected = chosen.find(observation.entity);
    const std::int64_t selected_action =
        selected == chosen.end() ? -1 : selected->second->action;
    const auto logged = scored.find({observation.entity, observation.action});
    local_decisions.push_back(
        {.logged_action = observation.action,
         .selected_action = selected_action,
         .reward = observation.response,
         .logging_propensity = observation.logging_propensity,
         .logged_reward_prediction =
             logged == scored.end() ? 0.0 : logged->second->propensity,
         .selected_reward_prediction =
             selected == chosen.end() ? 0.0 : selected->second->propensity});
  }
  const PolicySums local_policy =
      policy_sums(local_decisions, params.minimum_logging_propensity,
                  params.maximum_importance_weight);
  const std::vector<double> summed_policy = reduce(
      allreduce, "nba.offline.sum",
      {local_policy.ips, local_policy.doubly_robust, local_policy.weights,
       local_policy.squared_weights, local_policy.decisions,
       local_policy.matches, local_policy.missing_propensities},
      GbdtReduce::Sum, collective_failed);
  const std::vector<double> maximum_policy =
      reduce(allreduce, "nba.offline.max", {local_policy.maximum_weight},
             GbdtReduce::Max, collective_failed);
  if (collective_failed) {
    output.error = "next best action: offline evaluation allreduce failed";
    return output;
  }
  output.offline =
      finalize_policy_sums({.ips = summed_policy[0],
                            .doubly_robust = summed_policy[1],
                            .weights = summed_policy[2],
                            .squared_weights = summed_policy[3],
                            .maximum_weight = maximum_policy[0],
                            .decisions = summed_policy[4],
                            .matches = summed_policy[5],
                            .missing_propensities = summed_policy[6]});
  return output;
}

} // namespace zetta::ml
