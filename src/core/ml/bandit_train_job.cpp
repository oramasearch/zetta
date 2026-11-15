// SPDX-License-Identifier: MIT

#include "zetta/ml/bandit.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

std::size_t state_width(const BanditParams &params, std::size_t dimensions) {
  if (params.algorithm == BanditAlgorithm::Thompson) {
    return 3;
  }
  return 2 + dimensions * dimensions + dimensions;
}

std::size_t arm_index(const BanditParams &params, const std::string &arm) {
  const auto found = std::find(params.arms.begin(), params.arms.end(), arm);
  return found == params.arms.end()
             ? params.arms.size()
             : static_cast<std::size_t>(found - params.arms.begin());
}

bool validate_observations(const std::vector<BanditObservation> &observations,
                           const BanditParams &params, std::size_t dimensions,
                           std::string &error) {
  for (const BanditObservation &observation : observations) {
    if (arm_index(params, observation.arm) == params.arms.size()) {
      error = "bandit: observation names unknown arm '" + observation.arm + "'";
      return false;
    }
    if (!std::isfinite(observation.reward) || observation.reward < 0.0 ||
        observation.reward > 1.0) {
      error = "bandit: rewards must be finite and in [0, 1]";
      return false;
    }
    if (observation.context.size() != dimensions) {
      error = "bandit: observations have inconsistent context dimensions";
      return false;
    }
  }
  return true;
}

std::vector<double>
local_statistics(const std::vector<BanditObservation> &observations,
                 const BanditParams &params, std::size_t dimensions) {
  const std::size_t width = state_width(params, dimensions);
  std::vector<double> statistics(params.arms.size() * width, 0.0);
  for (const BanditObservation &observation : observations) {
    const std::size_t arm = arm_index(params, observation.arm);
    const std::size_t offset = arm * width;
    if (params.algorithm == BanditAlgorithm::Thompson) {
      statistics[offset] += observation.reward;
      statistics[offset + 1] += 1.0 - observation.reward;
      statistics[offset + 2] += 1.0;
      continue;
    }
    statistics[offset] += 1.0;
    statistics[offset + 1] += observation.reward;
    std::size_t cursor = offset + 2;
    for (std::size_t row = 0; row < dimensions; ++row) {
      for (std::size_t column = 0; column < dimensions; ++column) {
        statistics[cursor++] +=
            observation.context[row] * observation.context[column];
      }
    }
    for (std::size_t index = 0; index < dimensions; ++index) {
      statistics[cursor++] += observation.reward * observation.context[index];
    }
  }
  return statistics;
}

BanditState apply_statistics(const BanditParams &params,
                             const std::optional<BanditState> &prior,
                             std::size_t dimensions,
                             const std::vector<double> &statistics,
                             double reward, std::uint64_t decisions) {
  BanditState state;
  if (prior) {
    state = *prior;
  } else {
    BanditParams initial_params = params;
    if (params.algorithm == BanditAlgorithm::Thompson) {
      auto policy = BanditPolicy::create(initial_params, 0);
      state = policy->state();
    } else {
      state.algorithm = params.algorithm;
      state.context_dimensions = dimensions;
      for (const std::string &arm : params.arms) {
        BanditArmState entry;
        entry.arm = arm;
        entry.gram.assign(dimensions * dimensions, 0.0);
        entry.reward_vector.assign(dimensions, 0.0);
        for (std::size_t index = 0; index < dimensions; ++index) {
          entry.gram[index * dimensions + index] = params.ridge;
        }
        state.arms.push_back(std::move(entry));
      }
    }
  }
  const std::size_t width = state_width(params, dimensions);
  for (std::size_t arm = 0; arm < params.arms.size(); ++arm) {
    BanditArmState &target = state.arms[arm];
    const std::size_t offset = arm * width;
    if (params.algorithm == BanditAlgorithm::Thompson) {
      target.successes += statistics[offset];
      target.failures += statistics[offset + 1];
      target.cumulative_reward += statistics[offset];
      target.observations += static_cast<std::uint64_t>(statistics[offset + 2]);
      continue;
    }
    target.observations += static_cast<std::uint64_t>(statistics[offset]);
    target.cumulative_reward += statistics[offset + 1];
    std::size_t cursor = offset + 2;
    for (double &value : target.gram) {
      value += statistics[cursor++];
    }
    for (double &value : target.reward_vector) {
      value += statistics[cursor++];
    }
  }
  state.decisions += decisions;
  state.cumulative_reward += reward;
  return state;
}

double baseline_expected_reward(const BanditState &state,
                                const BanditParams &params) {
  const std::string baseline =
      params.baseline_arm.empty() ? params.arms.front() : params.baseline_arm;
  const auto found = std::find_if(
      state.arms.begin(), state.arms.end(),
      [&](const BanditArmState &arm) { return arm.arm == baseline; });
  if (found == state.arms.end()) {
    return 0.0;
  }
  if (params.algorithm == BanditAlgorithm::Thompson) {
    return found->successes / (found->successes + found->failures);
  }
  if (found->observations == 0) {
    return 0.0;
  }
  return found->cumulative_reward / static_cast<double>(found->observations);
}

} // namespace

BanditTrainResult run_bandit_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<BanditObservation> &local_observations,
    const BanditParams &params, const std::optional<BanditState> &prior_state,
    const std::string &round_tag, std::chrono::milliseconds timeout) {
  BanditTrainResult result;
  const std::string tag = round_tag.empty() ? "ml.bandit" : round_tag;
  result.status = coordinator.run(
      job, [&](compute::JobRound &round) -> compute::JobCoordinator::Outcome {
        result.is_root = round.is_root();
        std::size_t dimensions = 0;
        if (prior_state) {
          dimensions = prior_state->context_dimensions;
        } else if (!local_observations.empty()) {
          dimensions = local_observations.front().context.size();
        }
        const auto dimension_wire = round.allreduce(
            tag + ".dimensions",
            {static_cast<double>(dimensions), static_cast<double>(dimensions)},
            zetta::cluster::CollectiveOp::Maximum, timeout);
        if (!dimension_wire || dimension_wire->size() != 2) {
          result.error = "bandit: context-dimension allreduce failed";
          return {.ok = false, .detail = result.error};
        }
        dimensions = static_cast<std::size_t>((*dimension_wire)[0]);
        std::string error;
        if (!validate_observations(local_observations, params, dimensions,
                                   error)) {
          result.error = std::move(error);
          return {.ok = false, .detail = result.error};
        }
        if (params.algorithm != BanditAlgorithm::Thompson && dimensions == 0) {
          result.error = "bandit: contextual training requires context values";
          return {.ok = false, .detail = result.error};
        }
        std::optional<BanditPolicy> decision_policy =
            prior_state ? BanditPolicy::restore(params, *prior_state, &error)
                        : BanditPolicy::create(params, dimensions, &error);
        if (!decision_policy) {
          result.error = std::move(error);
          return {.ok = false, .detail = result.error};
        }
        std::vector<double> mean_context(dimensions, 0.0);
        for (const BanditObservation &observation : local_observations) {
          for (std::size_t index = 0; index < dimensions; ++index) {
            mean_context[index] += observation.context[index];
          }
          result.decisions.push_back(decision_policy->choose(
              observation.decision, observation.context));
        }

        const std::vector<double> local =
            local_statistics(local_observations, params, dimensions);
        const auto global =
            round.allreduce(tag + ".statistics", local,
                            zetta::cluster::CollectiveOp::Sum, timeout);
        if (!global || global->size() != local.size()) {
          result.error = "bandit: sufficient-statistic allreduce failed";
          return {.ok = false, .detail = result.error};
        }
        const double local_reward = std::accumulate(
            local_observations.begin(), local_observations.end(), 0.0,
            [](double total, const BanditObservation &observation) {
              return total + observation.reward;
            });
        const auto totals = round.allreduce(
            tag + ".totals",
            {local_reward, static_cast<double>(local_observations.size())},
            zetta::cluster::CollectiveOp::Sum, timeout);
        if (!totals || totals->size() != 2) {
          result.error = "bandit: metric allreduce failed";
          return {.ok = false, .detail = result.error};
        }
        result.state = apply_statistics(
            params, prior_state, dimensions, *global, (*totals)[0],
            static_cast<std::uint64_t>((*totals)[1]));
        auto policy = BanditPolicy::restore(params, result.state, &error);
        if (!policy) {
          result.error = std::move(error);
          return {.ok = false, .detail = result.error};
        }
        std::vector<double> context_wire = mean_context;
        context_wire.push_back(static_cast<double>(local_observations.size()));
        const auto global_context =
            round.allreduce(tag + ".context", context_wire,
                            zetta::cluster::CollectiveOp::Sum, timeout);
        if (!global_context || global_context->size() != dimensions + 1) {
          result.error = "bandit: context allreduce failed";
          return {.ok = false, .detail = result.error};
        }
        const double context_count = global_context->back();
        for (std::size_t index = 0; index < dimensions; ++index) {
          mean_context[index] = context_count == 0.0
                                    ? 0.0
                                    : (*global_context)[index] / context_count;
        }
        result.policy = policy->allocation(mean_context);

        double local_exploration = 0.0;
        for (const BanditDecision &decision : result.decisions) {
          const double best =
              decision.probabilities.empty()
                  ? 1.0
                  : *std::max_element(decision.probabilities.begin(),
                                      decision.probabilities.end());
          local_exploration += 1.0 - best;
        }
        const auto exploration =
            round.allreduce(tag + ".exploration", {local_exploration},
                            zetta::cluster::CollectiveOp::Sum, timeout);
        if (!exploration || exploration->size() != 1) {
          result.error = "bandit: exploration-share allreduce failed";
          return {.ok = false, .detail = result.error};
        }
        result.metrics.decisions = result.state.decisions;
        result.metrics.cumulative_reward = result.state.cumulative_reward;
        result.metrics.baseline_reward =
            baseline_expected_reward(result.state, params) *
            static_cast<double>(result.state.decisions);
        result.metrics.regret =
            std::max(0.0, result.metrics.baseline_reward -
                              result.metrics.cumulative_reward);
        result.metrics.exploration_share =
            context_count == 0.0 ? 0.0 : (*exploration)[0] / context_count;
        return {.ok = true,
                .detail = "bandit trained over " +
                          std::to_string(result.state.decisions) +
                          " decisions"};
      });
  return result;
}

} // namespace zetta::ml
