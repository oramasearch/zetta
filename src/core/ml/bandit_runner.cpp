#include "zetta/ml/bandit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/storage/lazy_row.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

std::string arm_value(const nlohmann::json &value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_number_integer()) {
    return std::to_string(value.get<std::int64_t>());
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<std::uint64_t>());
  }
  return {};
}

std::string safe_table_prefix(std::string value) {
  for (char &character : value) {
    const bool allowed = (character >= 'a' && character <= 'z') ||
                         (character >= 'A' && character <= 'Z') ||
                         (character >= '0' && character <= '9') ||
                         character == '_';
    if (!allowed) {
      character = '_';
    }
  }
  return value.empty() ? "bandit_policy" : value;
}

std::optional<BanditState>
load_policy_state(const std::shared_ptr<data::Table> &table,
                  std::string &error) {
  if (!table || table->row_count() == 0) {
    return std::nullopt;
  }
  for (std::size_t index = table->row_count(); index > 0; --index) {
    const storage::ParquetLazyPtr row = table->store().at(index - 1);
    if (!row) {
      error = "bandit policy: could not read persisted row: " + row.error();
      return std::nullopt;
    }
    const auto state = row->find("state");
    if (state == row->end()) {
      continue;
    }
    return bandit_state_from_json(*state, &error);
  }
  error = "bandit policy: persisted table has no state column";
  return std::nullopt;
}

bool read_observation(const nlohmann::json &row,
                      const BanditDatasetParams &params,
                      BanditObservation &observation, std::string &error) {
  const auto decision = row.find(params.decision_column);
  const auto arm = row.find(params.arm_column);
  const auto reward = row.find(params.reward_column);
  if (decision == row.end() || arm == row.end() || reward == row.end()) {
    error = "bandit dataset: a row is missing decision, arm, or reward";
    return false;
  }
  observation.decision = arm_value(*decision);
  observation.arm = arm_value(*arm);
  if (observation.decision.empty() || observation.arm.empty() ||
      !reward->is_number()) {
    error = "bandit dataset: decision and arm must be strings or integers, and "
            "reward must be numeric";
    return false;
  }
  observation.reward = reward->get<double>();
  if (!std::isfinite(observation.reward) || observation.reward < 0.0 ||
      observation.reward > 1.0) {
    error = "bandit dataset: reward must be finite and in [0, 1]";
    return false;
  }
  for (const std::string &column : params.context_columns) {
    const auto value = row.find(column);
    if (value == row.end() || !value->is_number()) {
      error = "bandit dataset: context column '" + column +
              "' must be numeric on every row";
      return false;
    }
    observation.context.push_back(value->get<double>());
  }
  return true;
}

double baseline_mean(const BanditState &state, const BanditParams &params) {
  const std::string baseline =
      params.baseline_arm.empty() ? params.arms.front() : params.baseline_arm;
  const auto found = std::find_if(
      state.arms.begin(), state.arms.end(),
      [&](const BanditArmState &arm) { return arm.arm == baseline; });
  if (found == state.arms.end()) {
    return 0.0;
  }
  if (state.algorithm == BanditAlgorithm::Thompson) {
    return found->successes / (found->successes + found->failures);
  }
  return found->observations == 0
             ? 0.0
             : found->cumulative_reward /
                   static_cast<double>(found->observations);
}

std::shared_ptr<data::Database> output_database(Node &node,
                                                const std::string &name) {
  std::shared_ptr<data::Database> database = node.find_database(name);
  if (!database) {
    database = node.create_database(name);
  }
  return database;
}

void fail_run(const std::shared_ptr<pipeline::Run> &run,
              const std::string &reason) {
  if (!run) {
    return;
  }
  run->add_log({util::now_iso8601(), "error", reason});
  run->set_status(pipeline::RunStatus::Failed);
}

} // namespace

BanditDatasetResult run_bandit_dataset(Node &node,
                                       const data::ResolvedDataset &dataset,
                                       const BanditDatasetParams &requested,
                                       std::string project, std::string graph) {
  // Registry dispatch reaches one node, so this adapter reads that resolved
  // table locally. Multi-member callers use run_bandit_job with each member's
  // owned observations; that path allreduces the same sufficient statistics.
  BanditDatasetResult result;
  const std::shared_ptr<pipeline::Run> run =
      node.create_run(std::move(project), std::move(graph));
  if (!run) {
    result.error = "bandit: could not create the run record";
    return result;
  }
  // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
  run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
  result.run_id = run->id();
  run->set_status(pipeline::RunStatus::Running);
  run->add_log({util::now_iso8601(), "info", "bandit training started"});
  for (const std::string &note : dataset.notes) {
    run->add_log({util::now_iso8601(), "info", "dataset: " + note});
  }

  const std::shared_ptr<data::Database> source_database =
      node.find_database(dataset.database);
  const std::shared_ptr<data::Table> source_table =
      source_database ? source_database->find_table(dataset.table) : nullptr;
  if (!source_table) {
    result.error = "bandit dataset: resolved source table is unavailable";
    fail_run(run, result.error);
    return result;
  }

  BanditDatasetParams params = requested;
  if (params.decision_column.empty()) {
    params.decision_column = dataset.contract.entity_column;
  }
  if (params.reward_column.empty()) {
    params.reward_column = dataset.contract.label_column;
  }
  if (params.context_columns.empty() &&
      params.policy.algorithm != BanditAlgorithm::Thompson) {
    params.context_columns = dataset.contract.feature_columns;
    std::erase(params.context_columns, params.arm_column);
  }

  std::vector<BanditObservation> observations;
  observations.reserve(source_table->row_count());
  std::set<std::string> inferred_arms;
  for (std::size_t index = 0; index < source_table->row_count(); ++index) {
    const storage::ParquetLazyPtr row = source_table->store().at(index);
    if (!row) {
      result.error = "bandit dataset: row read failed: " + row.error();
      fail_run(run, result.error);
      return result;
    }
    BanditObservation observation;
    if (!read_observation(*row, params, observation, result.error)) {
      fail_run(run, result.error);
      return result;
    }
    inferred_arms.insert(observation.arm);
    observations.push_back(std::move(observation));
  }
  result.rows_read = observations.size();
  if (params.policy.arms.empty()) {
    params.policy.arms.assign(inferred_arms.begin(), inferred_arms.end());
  }
  if (params.policy.baseline_arm.empty() && !params.policy.arms.empty()) {
    params.policy.baseline_arm = params.policy.arms.front();
  }

  const std::shared_ptr<data::Database> outputs =
      output_database(node, params.output_database);
  if (!outputs) {
    result.error = "bandit: could not create the output database";
    fail_run(run, result.error);
    return result;
  }
  const std::string prefix = safe_table_prefix(params.model_name);
  result.policy_table = prefix + "_policy";
  result.decisions_table = prefix + "_decisions";
  std::optional<BanditState> prior;
  if (const auto existing = outputs->find_table(result.policy_table)) {
    std::string state_error;
    prior = load_policy_state(existing, state_error);
    if (!state_error.empty()) {
      result.error = std::move(state_error);
      fail_run(run, result.error);
      return result;
    }
  }

  const std::size_t dimensions = params.context_columns.size();
  std::string policy_error;
  std::optional<BanditPolicy> policy =
      prior ? BanditPolicy::restore(params.policy, *prior, &policy_error)
            : BanditPolicy::create(params.policy, dimensions, &policy_error);
  if (!policy) {
    result.error = std::move(policy_error);
    fail_run(run, result.error);
    return result;
  }

  std::vector<BanditDecision> decisions;
  decisions.reserve(observations.size());
  std::vector<double> mean_context(dimensions, 0.0);
  double exploration = 0.0;
  for (const BanditObservation &observation : observations) {
    BanditDecision decision =
        policy->choose(observation.decision, observation.context);
    if (decision.chosen_arm.empty() || !(decision.propensity > 0.0)) {
      result.error = "bandit: policy did not produce a logged propensity";
      fail_run(run, result.error);
      return result;
    }
    const double largest = *std::max_element(decision.probabilities.begin(),
                                             decision.probabilities.end());
    exploration += 1.0 - largest;
    decisions.push_back(std::move(decision));
    if (!policy->observe(observation.arm, observation.reward,
                         observation.context, &policy_error)) {
      result.error = std::move(policy_error);
      fail_run(run, result.error);
      return result;
    }
    for (std::size_t index = 0; index < dimensions; ++index) {
      mean_context[index] += observation.context[index];
    }
  }
  if (!observations.empty()) {
    for (double &value : mean_context) {
      value /= static_cast<double>(observations.size());
    }
  }
  const std::vector<BanditAllocation> allocation =
      policy->allocation(mean_context);
  if (allocation.empty()) {
    result.error = "bandit: no allocation was produced";
    fail_run(run, result.error);
    return result;
  }

  outputs->drop_table(result.policy_table);
  outputs->drop_table(result.decisions_table);
  const std::shared_ptr<data::Table> policy_table =
      outputs->create_table(result.policy_table);
  const std::shared_ptr<data::Table> decisions_table =
      outputs->create_table(result.decisions_table);
  if (!policy_table || !decisions_table) {
    result.error = "bandit: could not create output tables";
    fail_run(run, result.error);
    return result;
  }
  const nlohmann::json persisted = bandit_state_to_json(policy->state());
  for (const BanditAllocation &entry : allocation) {
    policy_table->add_row({{"arm", entry.arm},
                           {"context_bucket", entry.context_bucket},
                           {"probability", entry.probability},
                           {"expected_reward", entry.expected_reward},
                           {"state", persisted}});
  }
  for (const BanditDecision &decision : decisions) {
    decisions_table->add_row({{"decision", decision.decision},
                              {"chosen_arm", decision.chosen_arm},
                              {"propensity", decision.propensity},
                              {"expected_reward", decision.expected_reward},
                              {"context_bucket", decision.context_bucket},
                              {"probabilities", decision.probabilities}});
  }

  result.metrics.decisions = policy->state().decisions;
  result.metrics.cumulative_reward = policy->state().cumulative_reward;
  result.metrics.baseline_reward =
      baseline_mean(policy->state(), params.policy) *
      static_cast<double>(policy->state().decisions);
  result.metrics.regret = std::max(0.0, result.metrics.baseline_reward -
                                            result.metrics.cumulative_reward);
  result.metrics.exploration_share =
      observations.empty()
          ? 0.0
          : exploration / static_cast<double>(observations.size());

  ModelValue model_value;
  model_value.name = params.model_name;
  model_value.kind = "bandit";
  model_value.project = run->project();
  model_value.run = run->id();
  model_value.feature_schema = {
      {"decision_column", params.decision_column},
      {"arm_column", params.arm_column},
      {"reward_column", params.reward_column},
      {"context_columns", params.context_columns},
      {"algorithm", to_string(params.policy.algorithm)}};
  model_value.metrics = {
      {"cumulative_reward", result.metrics.cumulative_reward},
      {"baseline_reward", result.metrics.baseline_reward},
      {"regret", result.metrics.regret},
      {"exploration_share", result.metrics.exploration_share},
      {"decisions", static_cast<double>(result.metrics.decisions)},
      {"rows_read", static_cast<double>(result.rows_read)},
      {"rows_excluded", static_cast<double>(result.rows_excluded)}};
  const std::shared_ptr<Model> model =
      node.create_model(std::move(model_value));
  if (!model || !model->set_weights(persisted.dump())) {
    result.error = "bandit: could not persist the policy model";
    fail_run(run, result.error);
    return result;
  }
  result.model_id = model->id();

  run->set_outputs({{"model", params.model_name, result.model_id},
                    {"dataset", "allocation policy",
                     params.output_database + "." + result.policy_table},
                    {"dataset", "decision propensities",
                     params.output_database + "." + result.decisions_table},
                    {"report", "bandit report", run->id()}});
  pipeline::RunLineage lineage;
  lineage.sources = {"dataset:" + dataset.database + "." + dataset.table};
  lineage.steps = {"bandit:observe", "bandit:allocate",
                   "bandit:log_propensity"};
  lineage.freshness = util::now_iso8601();
  lineage.confidence = 1.0;
  run->set_lineage(std::move(lineage));
  run->add_log({util::now_iso8601(), "info",
                "bandit policy and propensity log written"});
  run->set_status(pipeline::RunStatus::Succeeded);
  return result;
}

} // namespace zetta::ml
