#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/bandit.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

zetta::ml::BanditParams thompson_params() {
  zetta::ml::BanditParams params;
  params.arms = {"control", "best"};
  params.exploration_floor = 0.01;
  params.default_arm_cap = 0.99;
  params.seed = 1054;
  params.probability_samples = 512;
  params.baseline_arm = "best";
  return params;
}

double probability_sum(const zetta::ml::BanditDecision &decision) {
  return std::accumulate(decision.probabilities.begin(),
                         decision.probabilities.end(), 0.0);
}

template <class Predicate> bool wait_for(Predicate predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

zetta::NodeConfig node_config(const zetta::test::TempDir &directory) {
  zetta::NodeConfig config;
  config.data_dir = directory.path().string();
  return config;
}

zetta::cluster::ClusterConfig
cluster_config(std::uint16_t port, const std::vector<std::string> &seeds) {
  zetta::cluster::ClusterConfig config;
  config.listen_host = "127.0.0.1";
  config.listen_port = port;
  config.port_range = 1;
  config.seeds = seeds;
  return config;
}

struct BanditMember {
  BanditMember(std::string id, std::uint16_t port,
               const std::vector<std::string> &seeds)
      : node(node_config(directory)),
        cluster(std::move(id), cluster_config(port, seeds)),
        coordinator(cluster, 2) {
    node.start();
  }

  zetta::test::TempDir directory;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
  zetta::compute::JobCoordinator coordinator;
};

} // namespace

TEST(BanditThompson, ConvergesWithLoggedPropensitiesAndDecliningAverageRegret) {
  const zetta::ml::BanditParams params = thompson_params();
  auto policy = zetta::ml::BanditPolicy::create(params);
  ASSERT_TRUE(policy.has_value());

  std::mt19937_64 environment(7);
  std::uniform_real_distribution<double> unit(0.0, 1.0);
  double expected_regret = 0.0;
  double midpoint_regret = 0.0;
  constexpr std::size_t decisions = 3000;
  for (std::size_t index = 0; index < decisions; ++index) {
    const zetta::ml::BanditDecision decision =
        policy->choose("decision-" + std::to_string(index));
    ASSERT_FALSE(decision.chosen_arm.empty());
    EXPECT_NEAR(probability_sum(decision), 1.0, 1e-12);
    EXPECT_GE(decision.propensity, params.exploration_floor);
    EXPECT_LE(decision.propensity, params.default_arm_cap);

    const double rate = decision.chosen_arm == "best" ? 0.8 : 0.2;
    const double reward = unit(environment) < rate ? 1.0 : 0.0;
    ASSERT_TRUE(policy->observe(decision.chosen_arm, reward));
    expected_regret += 0.8 - rate;
    if (index + 1 == decisions / 2) {
      midpoint_regret = expected_regret;
    }
  }

  const auto allocation = policy->allocation();
  ASSERT_EQ(allocation.size(), 2u);
  const auto best = std::find_if(allocation.begin(), allocation.end(),
                                 [](const zetta::ml::BanditAllocation &value) {
                                   return value.arm == "best";
                                 });
  ASSERT_NE(best, allocation.end());
  EXPECT_GT(best->probability, 0.9);
  EXPECT_LT((expected_regret - midpoint_regret) /
                static_cast<double>(decisions / 2),
            midpoint_regret / static_cast<double>(decisions / 2));
}

TEST(BanditThompson, StateRoundTripsAcrossPolicyRestart) {
  const zetta::ml::BanditParams params = thompson_params();
  auto first = zetta::ml::BanditPolicy::create(params);
  ASSERT_TRUE(first.has_value());
  for (std::size_t index = 0; index < 80; ++index) {
    ASSERT_TRUE(first->observe("best", 1.0));
    ASSERT_TRUE(first->observe("control", 0.0));
  }

  const nlohmann::json saved = zetta::ml::bandit_state_to_json(first->state());
  std::string error;
  const auto state = zetta::ml::bandit_state_from_json(saved, &error);
  ASSERT_TRUE(state.has_value()) << error;
  auto restarted = zetta::ml::BanditPolicy::restore(params, *state, &error);
  ASSERT_TRUE(restarted.has_value()) << error;

  EXPECT_EQ(restarted->state().decisions, first->state().decisions);
  EXPECT_EQ(zetta::ml::bandit_state_to_json(restarted->state()), saved);
  EXPECT_EQ(restarted->choose("after-restart").chosen_arm,
            first->choose("after-restart").chosen_arm);
}

TEST(BanditThompson, RefusesAnUnknownFixedBaseline) {
  zetta::ml::BanditParams params = thompson_params();
  params.baseline_arm = "missing";
  std::string error;
  EXPECT_FALSE(zetta::ml::BanditPolicy::create(params, 0, &error));
  EXPECT_NE(error.find("baseline_arm"), std::string::npos);
}

TEST(BanditState, PolicyTableSurvivesNodeRestartAndFeedsTheNextRun) {
  zetta::test::TempDir directory;
  zetta::ml::BanditDatasetParams run_params;
  run_params.policy = thompson_params();
  run_params.model_name = "restart_policy";
  zetta::data::DatasetContract contract;
  contract.entity_column = "decision_id";
  contract.label_column = "reward";
  contract.feature_columns = {"context"};

  {
    zetta::Node first(node_config(directory));
    ASSERT_TRUE(first.start());
    const auto database = first.create_database("experiments");
    ASSERT_TRUE(database);
    const auto table = database->create_table("first_batch");
    ASSERT_TRUE(table);
    for (int index = 0; index < 20; ++index) {
      table->add_row({{"decision_id", index},
                      {"arm", index % 2 == 0 ? "best" : "control"},
                      {"reward", index % 2 == 0 ? 1 : 0},
                      {"context", index}});
    }
    const auto resolved = zetta::data::resolve_dataset(
        first, zetta::data::parse_dataset_path("experiments.first_batch"),
        contract);
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    const auto result = zetta::ml::run_bandit_dataset(
        first, resolved, run_params, "project", "graph");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.metrics.decisions, 20u);
    first.stop();
  }

  {
    zetta::Node restarted(node_config(directory));
    ASSERT_TRUE(restarted.start());
    const auto database = restarted.find_database("experiments");
    ASSERT_TRUE(database);
    const auto table = database->create_table("second_batch");
    ASSERT_TRUE(table);
    for (int index = 20; index < 40; ++index) {
      table->add_row({{"decision_id", index},
                      {"arm", index % 2 == 0 ? "best" : "control"},
                      {"reward", index % 2 == 0 ? 1 : 0},
                      {"context", index}});
    }
    const auto resolved = zetta::data::resolve_dataset(
        restarted, zetta::data::parse_dataset_path("experiments.second_batch"),
        contract);
    ASSERT_TRUE(resolved.ok()) << resolved.error;
    const auto result = zetta::ml::run_bandit_dataset(
        restarted, resolved, run_params, "project", "graph");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.metrics.decisions, 40u);
    const auto outputs = restarted.find_database("bandit_results");
    ASSERT_TRUE(outputs);
    const auto policy = outputs->find_table("restart_policy_policy");
    ASSERT_TRUE(policy);
    ASSERT_GT(policy->row_count(), 0u);
    const auto row = policy->store().at(0);
    ASSERT_TRUE(row);
    EXPECT_EQ(row->at("state").at("decisions"), 40u);
    restarted.stop();
  }
}

TEST(LinUCB, LearnsPerArmContextAndEnforcesEveryCap) {
  zetta::ml::BanditParams params;
  params.algorithm = zetta::ml::BanditAlgorithm::LinUcb;
  params.arms = {"left", "right"};
  params.exploration_floor = 0.1;
  params.default_arm_cap = 0.8;
  params.arm_caps["right"] = 0.7;
  params.seed = 42;
  auto policy = zetta::ml::BanditPolicy::create(params, 2);
  ASSERT_TRUE(policy.has_value());

  for (std::size_t index = 0; index < 200; ++index) {
    ASSERT_TRUE(policy->observe("left", 1.0, {1.0, 0.0}));
    ASSERT_TRUE(policy->observe("left", 0.0, {0.0, 1.0}));
    ASSERT_TRUE(policy->observe("right", 0.0, {1.0, 0.0}));
    ASSERT_TRUE(policy->observe("right", 1.0, {0.0, 1.0}));
  }

  const auto left_context = policy->choose("left-context", {1.0, 0.0});
  const auto right_context = policy->choose("right-context", {0.0, 1.0});
  EXPECT_GT(left_context.probabilities[0], left_context.probabilities[1]);
  EXPECT_GT(right_context.probabilities[1], right_context.probabilities[0]);
  EXPECT_NEAR(probability_sum(left_context), 1.0, 1e-12);
  EXPECT_NEAR(probability_sum(right_context), 1.0, 1e-12);
  EXPECT_LE(left_context.probabilities[0], 0.8 + 1e-12);
  EXPECT_LE(left_context.probabilities[1], 0.7 + 1e-12);
  EXPECT_LE(right_context.probabilities[0], 0.8 + 1e-12);
  EXPECT_LE(right_context.probabilities[1], 0.7 + 1e-12);
}

TEST(LinearThompson, LogsAValidPropensityForContextualDecision) {
  zetta::ml::BanditParams params;
  params.algorithm = zetta::ml::BanditAlgorithm::LinearThompson;
  params.arms = {"a", "b", "guardrail"};
  params.exploration_floor = 0.05;
  params.default_arm_cap = 0.9;
  params.arm_caps["guardrail"] = 0.2;
  params.seed = 9;
  auto policy = zetta::ml::BanditPolicy::create(params, 2);
  ASSERT_TRUE(policy.has_value());
  for (std::size_t index = 0; index < 40; ++index) {
    ASSERT_TRUE(policy->observe("a", 1.0, {1.0, 0.0}));
    ASSERT_TRUE(policy->observe("b", 1.0, {0.0, 1.0}));
    ASSERT_TRUE(policy->observe("guardrail", 0.0, {1.0, 1.0}));
  }
  const auto decision = policy->choose("contextual", {0.8, 0.2});
  EXPECT_FALSE(decision.chosen_arm.empty());
  EXPECT_GE(decision.propensity, params.exploration_floor);
  EXPECT_NEAR(probability_sum(decision), 1.0, 1e-12);
  EXPECT_LE(decision.probabilities[2], 0.2 + 1e-12);
}

TEST(BanditDistributed, AllreduceProducesTheSingleMemberPolicy) {
  BanditMember first("bandit-a", 47550, {});
  BanditMember second("bandit-b", 47551, {"127.0.0.1:47550"});
  first.cluster.start();
  second.cluster.start();
  ASSERT_TRUE(wait_for([&] {
    return first.cluster.peer_count() == 1 && second.cluster.peer_count() == 1;
  }));
  first.coordinator.start();
  second.coordinator.start();

  const zetta::ml::BanditParams params = thompson_params();
  std::vector<zetta::ml::BanditObservation> first_rows;
  std::vector<zetta::ml::BanditObservation> second_rows;
  for (std::size_t index = 0; index < 100; ++index) {
    first_rows.push_back({"a-" + std::to_string(index), "best", 1.0, {}});
    second_rows.push_back({"b-" + std::to_string(index), "control", 0.0, {}});
  }
  const auto first_job = first.node.create_job(zetta::compute::JobKind::Train);
  const auto second_job =
      second.node.create_job(zetta::compute::JobKind::Train);
  ASSERT_TRUE(first_job);
  ASSERT_TRUE(second_job);

  zetta::ml::BanditTrainResult first_result;
  zetta::ml::BanditTrainResult second_result;
  std::thread first_thread([&] {
    first_result = zetta::ml::run_bandit_job(first.coordinator, *first_job,
                                             first_rows, params, std::nullopt,
                                             "bandit-distributed", 8s);
  });
  std::thread second_thread([&] {
    second_result = zetta::ml::run_bandit_job(second.coordinator, *second_job,
                                              second_rows, params, std::nullopt,
                                              "bandit-distributed", 8s);
  });
  first_thread.join();
  second_thread.join();

  ASSERT_TRUE(first_result.ok()) << first_result.error;
  ASSERT_TRUE(second_result.ok()) << second_result.error;
  EXPECT_EQ(zetta::ml::bandit_state_to_json(first_result.state),
            zetta::ml::bandit_state_to_json(second_result.state));
  EXPECT_EQ(first_result.metrics.decisions, 200u);
  EXPECT_DOUBLE_EQ(first_result.metrics.cumulative_reward, 100.0);
  ASSERT_EQ(first_result.decisions.size(), first_rows.size());
  for (const zetta::ml::BanditDecision &decision : first_result.decisions) {
    EXPECT_FALSE(decision.chosen_arm.empty());
    EXPECT_GT(decision.propensity, 0.0);
    EXPECT_NEAR(probability_sum(decision), 1.0, 1e-12);
  }
  ASSERT_EQ(first_result.policy.size(), 2u);
  EXPECT_NEAR(first_result.policy[0].probability +
                  first_result.policy[1].probability,
              1.0, 1e-12);
}
