// SPDX-License-Identifier: MIT

#include "zetta/ml/bandit.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <utility>

namespace zetta::ml {
namespace {

constexpr double kTolerance = 1e-10;

std::size_t find_arm(const BanditState &state, std::string_view arm) {
  const auto found = std::find_if(
      state.arms.begin(), state.arms.end(),
      [&](const BanditArmState &candidate) { return candidate.arm == arm; });
  return found == state.arms.end()
             ? state.arms.size()
             : static_cast<std::size_t>(found - state.arms.begin());
}

std::uint64_t hash_text(std::string_view value) {
  std::uint64_t hash = 1469598103934665603ull;
  for (const unsigned char character : value) {
    hash ^= character;
    hash *= 1099511628211ull;
  }
  return hash;
}

std::string context_bucket(const std::vector<double> &context) {
  if (context.empty()) {
    return "global";
  }
  std::string bucket;
  for (const double value : context) {
    if (!bucket.empty()) {
      bucket += ':';
    }
    const long long quantized = std::llround(value * 1000.0);
    bucket += std::to_string(quantized);
  }
  return bucket;
}

double arm_cap(const BanditParams &params, std::string_view arm) {
  const auto found = params.arm_caps.find(std::string(arm));
  return found == params.arm_caps.end() ? params.default_arm_cap
                                        : found->second;
}

bool validate_params(const BanditParams &params, std::size_t dimensions,
                     std::string &error) {
  if (params.arms.size() < 2) {
    error = "bandit: at least two arms are required";
    return false;
  }
  std::vector<std::string> unique = params.arms;
  if (std::ranges::any_of(unique,
                          [](const std::string &arm) { return arm.empty(); })) {
    error = "bandit: arm names must be non-empty";
    return false;
  }
  std::sort(unique.begin(), unique.end());
  if (std::adjacent_find(unique.begin(), unique.end()) != unique.end()) {
    error = "bandit: arm names must be unique";
    return false;
  }
  if (!params.baseline_arm.empty() &&
      std::ranges::find(params.arms, params.baseline_arm) ==
          params.arms.end()) {
    error = "bandit: baseline_arm must name a declared arm";
    return false;
  }
  if (!(params.exploration_floor > 0.0) ||
      params.exploration_floor * static_cast<double>(params.arms.size()) >
          1.0 + kTolerance) {
    error =
        "bandit: exploration_floor must be positive and feasible for every arm";
    return false;
  }
  double cap_sum = 0.0;
  for (const std::string &arm : params.arms) {
    const double cap = arm_cap(params, arm);
    if (cap + kTolerance < params.exploration_floor || cap > 1.0 ||
        !(cap > 0.0)) {
      error = "bandit: arm '" + arm + "' cap must be in [exploration_floor, 1]";
      return false;
    }
    cap_sum += cap;
  }
  if (cap_sum + kTolerance < 1.0) {
    error = "bandit: arm caps cannot carry total probability 1";
    return false;
  }
  if (params.algorithm != BanditAlgorithm::Thompson && dimensions == 0) {
    error = "bandit: contextual algorithms require at least one context value";
    return false;
  }
  if (!(params.ridge > 0.0) || !(params.confidence >= 0.0)) {
    error = "bandit: ridge must be positive and confidence non-negative";
    return false;
  }
  if (params.probability_samples == 0) {
    error = "bandit: probability_samples must be positive";
    return false;
  }
  return true;
}

std::optional<std::vector<double>>
invert_matrix(const std::vector<double> &matrix, std::size_t dimensions) {
  if (matrix.size() != dimensions * dimensions) {
    return std::nullopt;
  }
  std::vector<double> augmented(dimensions * dimensions * 2, 0.0);
  const std::size_t width = dimensions * 2;
  for (std::size_t row = 0; row < dimensions; ++row) {
    for (std::size_t column = 0; column < dimensions; ++column) {
      augmented[row * width + column] = matrix[row * dimensions + column];
    }
    augmented[row * width + dimensions + row] = 1.0;
  }
  for (std::size_t pivot = 0; pivot < dimensions; ++pivot) {
    std::size_t best = pivot;
    for (std::size_t row = pivot + 1; row < dimensions; ++row) {
      if (std::abs(augmented[row * width + pivot]) >
          std::abs(augmented[best * width + pivot])) {
        best = row;
      }
    }
    if (std::abs(augmented[best * width + pivot]) < kTolerance) {
      return std::nullopt;
    }
    for (std::size_t column = 0; column < width; ++column) {
      std::swap(augmented[pivot * width + column],
                augmented[best * width + column]);
    }
    const double scale = augmented[pivot * width + pivot];
    for (std::size_t column = 0; column < width; ++column) {
      augmented[pivot * width + column] /= scale;
    }
    for (std::size_t row = 0; row < dimensions; ++row) {
      if (row == pivot) {
        continue;
      }
      const double factor = augmented[row * width + pivot];
      for (std::size_t column = 0; column < width; ++column) {
        augmented[row * width + column] -=
            factor * augmented[pivot * width + column];
      }
    }
  }
  std::vector<double> inverse(dimensions * dimensions, 0.0);
  for (std::size_t row = 0; row < dimensions; ++row) {
    for (std::size_t column = 0; column < dimensions; ++column) {
      inverse[row * dimensions + column] =
          augmented[row * width + dimensions + column];
    }
  }
  return inverse;
}

std::vector<double> multiply(const std::vector<double> &matrix,
                             const std::vector<double> &vector,
                             std::size_t dimensions) {
  std::vector<double> result(dimensions, 0.0);
  for (std::size_t row = 0; row < dimensions; ++row) {
    for (std::size_t column = 0; column < dimensions; ++column) {
      result[row] += matrix[row * dimensions + column] * vector[column];
    }
  }
  return result;
}

double dot(const std::vector<double> &left, const std::vector<double> &right) {
  return std::inner_product(left.begin(), left.end(), right.begin(), 0.0);
}

std::vector<double> cholesky(const std::vector<double> &covariance,
                             std::size_t dimensions) {
  std::vector<double> lower(dimensions * dimensions, 0.0);
  for (std::size_t row = 0; row < dimensions; ++row) {
    for (std::size_t column = 0; column <= row; ++column) {
      double value = covariance[row * dimensions + column];
      for (std::size_t inner = 0; inner < column; ++inner) {
        value -= lower[row * dimensions + inner] *
                 lower[column * dimensions + inner];
      }
      if (row == column) {
        lower[row * dimensions + column] =
            std::sqrt(std::max(value, kTolerance));
      } else {
        lower[row * dimensions + column] =
            value / lower[column * dimensions + column];
      }
    }
  }
  return lower;
}

double beta_sample(double alpha, double beta, std::mt19937_64 &generator) {
  std::gamma_distribution<double> alpha_distribution(alpha, 1.0);
  std::gamma_distribution<double> beta_distribution(beta, 1.0);
  const double left = alpha_distribution(generator);
  const double right = beta_distribution(generator);
  return left / (left + right);
}

std::vector<double> constrained_probabilities(const BanditParams &params,
                                              const std::vector<double> &raw) {
  const std::size_t count = params.arms.size();
  std::vector<double> probabilities(count, params.exploration_floor);
  std::vector<bool> capped(count, false);
  double remaining =
      1.0 - params.exploration_floor * static_cast<double>(count);
  while (remaining > kTolerance) {
    double weight_sum = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
      if (!capped[index]) {
        weight_sum += std::max(raw[index], kTolerance);
      }
    }
    if (weight_sum <= 0.0) {
      break;
    }
    double allocated = 0.0;
    for (std::size_t index = 0; index < count; ++index) {
      if (capped[index]) {
        continue;
      }
      const double share =
          remaining * std::max(raw[index], kTolerance) / weight_sum;
      const double capacity =
          arm_cap(params, params.arms[index]) - probabilities[index];
      const double addition = std::min(share, capacity);
      probabilities[index] += addition;
      allocated += addition;
      if (capacity - addition <= kTolerance) {
        capped[index] = true;
      }
    }
    if (allocated <= kTolerance) {
      break;
    }
    remaining -= allocated;
  }
  if (std::abs(remaining) > kTolerance) {
    for (std::size_t index = 0; index < count && remaining > kTolerance;
         ++index) {
      const double capacity =
          arm_cap(params, params.arms[index]) - probabilities[index];
      const double addition = std::min(remaining, capacity);
      probabilities[index] += addition;
      remaining -= addition;
    }
  }
  return probabilities;
}

struct PolicyValues {
  std::vector<double> probabilities;
  std::vector<double> expected_rewards;
};

PolicyValues policy_values(const BanditParams &params, const BanditState &state,
                           const std::vector<double> &context,
                           std::mt19937_64 &generator) {
  const std::size_t arm_count = state.arms.size();
  PolicyValues values;
  values.expected_rewards.resize(arm_count, 0.0);
  std::vector<double> raw(arm_count, 0.0);
  if (params.algorithm == BanditAlgorithm::Thompson) {
    std::vector<double> wins(arm_count, 0.0);
    for (std::size_t arm = 0; arm < arm_count; ++arm) {
      const BanditArmState &current = state.arms[arm];
      values.expected_rewards[arm] =
          current.successes / (current.successes + current.failures);
    }
    for (std::size_t sample = 0; sample < params.probability_samples;
         ++sample) {
      std::size_t best = 0;
      double best_value = -std::numeric_limits<double>::infinity();
      for (std::size_t arm = 0; arm < arm_count; ++arm) {
        const BanditArmState &current = state.arms[arm];
        const double value =
            beta_sample(current.successes, current.failures, generator);
        if (value > best_value) {
          best = arm;
          best_value = value;
        }
      }
      wins[best] += 1.0;
    }
    for (std::size_t arm = 0; arm < arm_count; ++arm) {
      raw[arm] = wins[arm] / static_cast<double>(params.probability_samples);
    }
    values.probabilities = constrained_probabilities(params, raw);
    return values;
  }

  std::vector<std::vector<double>> means(arm_count);
  std::vector<std::vector<double>> inverses(arm_count);
  for (std::size_t arm = 0; arm < arm_count; ++arm) {
    inverses[arm] =
        *invert_matrix(state.arms[arm].gram, state.context_dimensions);
    means[arm] = multiply(inverses[arm], state.arms[arm].reward_vector,
                          state.context_dimensions);
    values.expected_rewards[arm] =
        std::clamp(dot(means[arm], context), 0.0, 1.0);
  }
  if (params.algorithm == BanditAlgorithm::LinUcb) {
    std::size_t best = 0;
    double best_score = -std::numeric_limits<double>::infinity();
    for (std::size_t arm = 0; arm < arm_count; ++arm) {
      const std::vector<double> projected =
          multiply(inverses[arm], context, state.context_dimensions);
      const double uncertainty =
          std::sqrt(std::max(0.0, dot(context, projected)));
      const double score =
          values.expected_rewards[arm] + params.confidence * uncertainty;
      if (score > best_score) {
        best = arm;
        best_score = score;
      }
    }
    raw[best] = 1.0;
    values.probabilities = constrained_probabilities(params, raw);
    return values;
  }

  std::normal_distribution<double> normal(0.0, 1.0);
  std::vector<std::vector<double>> factors;
  factors.reserve(arm_count);
  for (const std::vector<double> &inverse : inverses) {
    factors.push_back(cholesky(inverse, state.context_dimensions));
  }
  for (std::size_t sample = 0; sample < params.probability_samples; ++sample) {
    std::size_t best = 0;
    double best_score = -std::numeric_limits<double>::infinity();
    for (std::size_t arm = 0; arm < arm_count; ++arm) {
      std::vector<double> noise(state.context_dimensions, 0.0);
      for (double &value : noise) {
        value = normal(generator);
      }
      std::vector<double> sampled = means[arm];
      for (std::size_t row = 0; row < state.context_dimensions; ++row) {
        for (std::size_t column = 0; column <= row; ++column) {
          sampled[row] +=
              params.confidence *
              factors[arm][row * state.context_dimensions + column] *
              noise[column];
        }
      }
      const double score = dot(sampled, context);
      if (score > best_score) {
        best = arm;
        best_score = score;
      }
    }
    raw[best] += 1.0;
  }
  for (double &value : raw) {
    value /= static_cast<double>(params.probability_samples);
  }
  values.probabilities = constrained_probabilities(params, raw);
  return values;
}

BanditState initial_state(const BanditParams &params, std::size_t dimensions) {
  BanditState state;
  state.algorithm = params.algorithm;
  state.context_dimensions = dimensions;
  for (const std::string &arm : params.arms) {
    BanditArmState entry;
    entry.arm = arm;
    if (dimensions > 0) {
      entry.gram.assign(dimensions * dimensions, 0.0);
      entry.reward_vector.assign(dimensions, 0.0);
      for (std::size_t index = 0; index < dimensions; ++index) {
        entry.gram[index * dimensions + index] = params.ridge;
      }
    }
    state.arms.push_back(std::move(entry));
  }
  return state;
}

bool validate_state(const BanditParams &params, const BanditState &state,
                    std::string &error) {
  if (state.algorithm != params.algorithm) {
    error = "bandit state: algorithm does not match the requested algorithm";
    return false;
  }
  if (state.arms.size() != params.arms.size()) {
    error = "bandit state: arm count does not match the requested arms";
    return false;
  }
  for (std::size_t index = 0; index < state.arms.size(); ++index) {
    const BanditArmState &arm = state.arms[index];
    if (arm.arm != params.arms[index]) {
      error = "bandit state: arm ordering does not match the requested arms";
      return false;
    }
    if (!(arm.successes > 0.0) || !(arm.failures > 0.0)) {
      error = "bandit state: Beta parameters must be positive";
      return false;
    }
    if (!std::isfinite(arm.cumulative_reward) || arm.cumulative_reward < 0.0 ||
        arm.cumulative_reward > static_cast<double>(arm.observations)) {
      error = "bandit state: arm reward totals are invalid";
      return false;
    }
    if (params.algorithm != BanditAlgorithm::Thompson &&
        (arm.gram.size() !=
             state.context_dimensions * state.context_dimensions ||
         arm.reward_vector.size() != state.context_dimensions)) {
      error =
          "bandit state: contextual sufficient statistics have the wrong shape";
      return false;
    }
  }
  return validate_params(params, state.context_dimensions, error);
}

} // namespace

std::string_view to_string(BanditAlgorithm algorithm) {
  switch (algorithm) {
  case BanditAlgorithm::Thompson:
    return "thompson";
  case BanditAlgorithm::LinUcb:
    return "linucb";
  case BanditAlgorithm::LinearThompson:
    return "linear_thompson";
  }
  return "thompson";
}

std::optional<BanditAlgorithm>
bandit_algorithm_from_string(std::string_view name) {
  if (name == "thompson") {
    return BanditAlgorithm::Thompson;
  }
  if (name == "linucb") {
    return BanditAlgorithm::LinUcb;
  }
  if (name == "linear_thompson") {
    return BanditAlgorithm::LinearThompson;
  }
  return std::nullopt;
}

BanditPolicy::BanditPolicy(BanditParams params, BanditState state)
    : params_(std::move(params)), state_(std::move(state)) {}

std::optional<BanditPolicy> BanditPolicy::create(const BanditParams &params,
                                                 std::size_t context_dimensions,
                                                 std::string *error) {
  const std::size_t dimensions = context_dimensions;
  std::string reason;
  if (!validate_params(params, dimensions, reason)) {
    if (error != nullptr) {
      *error = std::move(reason);
    }
    return std::nullopt;
  }
  return BanditPolicy(params, initial_state(params, dimensions));
}

std::optional<BanditPolicy> BanditPolicy::restore(const BanditParams &params,
                                                  const BanditState &state,
                                                  std::string *error) {
  std::string reason;
  if (!validate_state(params, state, reason)) {
    if (error != nullptr) {
      *error = std::move(reason);
    }
    return std::nullopt;
  }
  return BanditPolicy(params, state);
}

BanditDecision BanditPolicy::choose(std::string decision,
                                    const std::vector<double> &context) const {
  BanditDecision result;
  result.decision = std::move(decision);
  result.context_bucket = context_bucket(context);
  if ((params_.algorithm == BanditAlgorithm::Thompson && !context.empty()) ||
      (params_.algorithm != BanditAlgorithm::Thompson &&
       context.size() != state_.context_dimensions)) {
    return result;
  }
  const std::uint64_t local_seed = params_.seed ^ hash_text(result.decision) ^
                                   hash_text(result.context_bucket) ^
                                   (state_.decisions * 0x9e3779b97f4a7c15ull);
  std::mt19937_64 generator(local_seed);
  PolicyValues values = policy_values(params_, state_, context, generator);
  std::discrete_distribution<std::size_t> choose_arm(
      values.probabilities.begin(), values.probabilities.end());
  const std::size_t selected = choose_arm(generator);
  result.chosen_arm = state_.arms[selected].arm;
  result.propensity = values.probabilities[selected];
  result.expected_reward = values.expected_rewards[selected];
  result.probabilities = std::move(values.probabilities);
  return result;
}

bool BanditPolicy::observe(std::string_view arm, double reward,
                           const std::vector<double> &context,
                           std::string *error) {
  const std::size_t index = find_arm(state_, arm);
  if (index == state_.arms.size()) {
    if (error != nullptr) {
      *error = "bandit: unknown arm '" + std::string(arm) + "'";
    }
    return false;
  }
  if (!(reward >= 0.0 && reward <= 1.0) || !std::isfinite(reward)) {
    if (error != nullptr) {
      *error = "bandit: reward must be finite and in [0, 1]";
    }
    return false;
  }
  BanditArmState &current = state_.arms[index];
  if (params_.algorithm == BanditAlgorithm::Thompson) {
    if (!context.empty()) {
      if (error != nullptr) {
        *error =
            "bandit: context-free Thompson sampling does not accept context";
      }
      return false;
    }
    current.successes += reward;
    current.failures += 1.0 - reward;
  } else {
    if (context.size() != state_.context_dimensions) {
      if (error != nullptr) {
        *error = "bandit: context has the wrong number of values";
      }
      return false;
    }
    for (std::size_t row = 0; row < state_.context_dimensions; ++row) {
      current.reward_vector[row] += reward * context[row];
      for (std::size_t column = 0; column < state_.context_dimensions;
           ++column) {
        current.gram[row * state_.context_dimensions + column] +=
            context[row] * context[column];
      }
    }
  }
  ++current.observations;
  current.cumulative_reward += reward;
  ++state_.decisions;
  state_.cumulative_reward += reward;
  return true;
}

std::vector<BanditAllocation>
BanditPolicy::allocation(const std::vector<double> &context) const {
  const BanditDecision decision = choose("allocation", context);
  std::vector<BanditAllocation> result;
  if (decision.probabilities.size() != state_.arms.size()) {
    return result;
  }
  std::mt19937_64 generator(params_.seed ^ hash_text("allocation-values") ^
                            hash_text(context_bucket(context)));
  const PolicyValues values =
      policy_values(params_, state_, context, generator);
  for (std::size_t index = 0; index < state_.arms.size(); ++index) {
    result.push_back({state_.arms[index].arm, context_bucket(context),
                      decision.probabilities[index],
                      values.expected_rewards[index]});
  }
  return result;
}

nlohmann::json bandit_state_to_json(const BanditState &state) {
  nlohmann::json arms = nlohmann::json::array();
  for (const BanditArmState &arm : state.arms) {
    arms.push_back({{"arm", arm.arm},
                    {"successes", arm.successes},
                    {"failures", arm.failures},
                    {"gram", arm.gram},
                    {"reward_vector", arm.reward_vector},
                    {"cumulative_reward", arm.cumulative_reward},
                    {"observations", arm.observations}});
  }
  return {{"algorithm", to_string(state.algorithm)},
          {"context_dimensions", state.context_dimensions},
          {"decisions", state.decisions},
          {"cumulative_reward", state.cumulative_reward},
          {"arms", std::move(arms)}};
}

std::optional<BanditState> bandit_state_from_json(const nlohmann::json &value,
                                                  std::string *error) {
  const auto fail = [error](std::string reason) -> std::optional<BanditState> {
    if (error != nullptr) {
      *error = std::move(reason);
    }
    return std::nullopt;
  };
  if (!value.is_object() || !value.contains("algorithm") ||
      !value.at("algorithm").is_string() || !value.contains("arms") ||
      !value.at("arms").is_array()) {
    return fail("bandit state: expected an object with algorithm and arms");
  }
  const auto algorithm =
      bandit_algorithm_from_string(value.at("algorithm").get<std::string>());
  if (!algorithm) {
    return fail("bandit state: unknown algorithm");
  }
  BanditState state;
  state.algorithm = *algorithm;
  state.context_dimensions = value.value("context_dimensions", std::size_t{0});
  state.decisions = value.value("decisions", std::uint64_t{0});
  state.cumulative_reward = value.value("cumulative_reward", 0.0);
  for (const nlohmann::json &item : value.at("arms")) {
    if (!item.is_object() || !item.contains("arm") ||
        !item.at("arm").is_string()) {
      return fail("bandit state: every arm must have a string name");
    }
    BanditArmState arm;
    arm.arm = item.at("arm").get<std::string>();
    arm.successes = item.value("successes", 1.0);
    arm.failures = item.value("failures", 1.0);
    arm.gram = item.value("gram", std::vector<double>{});
    arm.reward_vector = item.value("reward_vector", std::vector<double>{});
    arm.cumulative_reward = item.value("cumulative_reward", 0.0);
    arm.observations = item.value("observations", std::uint64_t{0});
    state.arms.push_back(std::move(arm));
  }
  return state;
}

} // namespace zetta::ml
