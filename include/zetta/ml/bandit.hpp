// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"

namespace zetta {
class Node;
}
namespace zetta::data {
struct ResolvedDataset;
}

// Native multi-armed and contextual bandits (#1054, M-15).
//
// Context-free policies use Beta-Bernoulli Thompson sampling. Contextual
// policies keep one ridge-regression sufficient-statistic pair (A, b) per arm
// and choose with LinUCB or full-covariance linear Thompson sampling. The
// coordinated runner combines only those per-arm statistics with allreduce, so
// rows stay on their owning members and single-member and multi-member runs
// produce the same policy.
//
// Every returned decision carries the probability with which its arm was
// chosen. Probability projection enforces a positive floor and an effective cap
// for every arm. See docs/ml/bandit.md.
namespace zetta::ml {

enum class BanditAlgorithm {
  Thompson,
  LinUcb,
  LinearThompson,
};

std::string_view to_string(BanditAlgorithm algorithm);
std::optional<BanditAlgorithm>
bandit_algorithm_from_string(std::string_view name);

struct BanditParams {
  BanditAlgorithm algorithm = BanditAlgorithm::Thompson;
  std::vector<std::string> arms;
  double exploration_floor = 0.05;
  double default_arm_cap = 1.0;
  std::map<std::string, double> arm_caps;
  double ridge = 1.0;
  double confidence = 1.0;
  std::uint64_t seed = 1;
  std::size_t probability_samples = 256;
  std::string baseline_arm;
};

struct BanditObservation {
  std::string decision;
  std::string arm;
  double reward = 0.0;
  std::vector<double> context;
};

struct BanditArmState {
  std::string arm;
  double successes = 1.0;
  double failures = 1.0;
  std::vector<double> gram;
  std::vector<double> reward_vector;
  double cumulative_reward = 0.0;
  std::uint64_t observations = 0;
};

struct BanditState {
  BanditAlgorithm algorithm = BanditAlgorithm::Thompson;
  std::size_t context_dimensions = 0;
  std::uint64_t decisions = 0;
  double cumulative_reward = 0.0;
  std::vector<BanditArmState> arms;
};

struct BanditAllocation {
  std::string arm;
  std::string context_bucket;
  double probability = 0.0;
  double expected_reward = 0.0;
};

struct BanditDecision {
  std::string decision;
  std::string chosen_arm;
  double propensity = 0.0;
  double expected_reward = 0.0;
  std::string context_bucket;
  std::vector<double> probabilities;
};

struct BanditMetrics {
  double cumulative_reward = 0.0;
  double baseline_reward = 0.0;
  double regret = 0.0;
  double exploration_share = 0.0;
  std::uint64_t decisions = 0;
};

struct BanditTrainResult {
  BanditState state;
  std::vector<BanditAllocation> policy;
  std::vector<BanditDecision> decisions;
  BanditMetrics metrics;
  compute::JobStatus status = compute::JobStatus::Failed;
  bool is_root = false;
  std::string error;

  bool ok() const noexcept { return status == compute::JobStatus::Succeeded; }
};

// A deterministic online policy. Restore with `from_json`, call `choose`, then
// `observe` when the reward arrives. `choose` never returns without a
// propensity.
class BanditPolicy {
public:
  static std::optional<BanditPolicy> create(const BanditParams &params,
                                            std::size_t context_dimensions = 0,
                                            std::string *error = nullptr);
  static std::optional<BanditPolicy> restore(const BanditParams &params,
                                             const BanditState &state,
                                             std::string *error = nullptr);

  BanditDecision choose(std::string decision,
                        const std::vector<double> &context = {}) const;
  bool observe(std::string_view arm, double reward,
               const std::vector<double> &context = {},
               std::string *error = nullptr);
  std::vector<BanditAllocation>
  allocation(const std::vector<double> &context = {}) const;

  const BanditState &state() const noexcept { return state_; }

private:
  BanditPolicy(BanditParams params, BanditState state);

  BanditParams params_;
  BanditState state_;
};

nlohmann::json bandit_state_to_json(const BanditState &state);
std::optional<BanditState> bandit_state_from_json(const nlohmann::json &value,
                                                  std::string *error = nullptr);

// Distributed training over this member's owned observations. Call
// concurrently on every participant with the same params and prior state.
// Per-arm reward/count or ridge statistics are combined with
// JobRound::allreduce.
BanditTrainResult run_bandit_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const std::vector<BanditObservation> &local_observations,
    const BanditParams &params,
    const std::optional<BanditState> &prior_state = std::nullopt,
    const std::string &round_tag = {},
    std::chrono::milliseconds timeout = std::chrono::milliseconds{10000});

struct BanditDatasetParams {
  BanditParams policy;
  std::string decision_column;
  std::string arm_column = "arm";
  std::string reward_column;
  std::vector<std::string> context_columns;
  std::string model_name = "bandit_policy";
  std::string output_database = "bandit_results";
};

struct BanditDatasetResult {
  std::string run_id;
  std::string model_id;
  std::string policy_table;
  std::string decisions_table;
  BanditMetrics metrics;
  std::uint64_t rows_read = 0;
  std::uint64_t rows_excluded = 0;
  std::string error;

  bool ok() const noexcept { return error.empty() && !run_id.empty(); }
};

// Run M-15 over a resolved table and materialize its model, policy table,
// decision log, run outputs, and lineage. An existing policy table with the
// same model name is restored before the new observations are applied.
BanditDatasetResult run_bandit_dataset(Node &node,
                                       const data::ResolvedDataset &dataset,
                                       const BanditDatasetParams &params,
                                       std::string project = {},
                                       std::string graph = {});

} // namespace zetta::ml
