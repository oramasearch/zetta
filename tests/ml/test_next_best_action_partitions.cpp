// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/next_best_action_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;
using zetta::ml::DistributedNextBestActionResult;
using zetta::ml::NextBestActionObservation;
using zetta::test::TempDir;

std::vector<NextBestActionObservation> fixture() {
  std::vector<NextBestActionObservation> rows;
  for (std::int64_t entity = 0; entity < 24; ++entity) {
    for (std::int64_t action = 0; action < 6; ++action) {
      const double feature =
          static_cast<double>((entity * 5 + action * 7) % 19) / 18.0;
      const double response = ((entity + action * 2) % 5) < 2 ? 1.0 : 0.0;
      rows.push_back({.entity = entity,
                      .action = action,
                      .features = {feature},
                      .response = response,
                      .value = 5.0 + action,
                      .cost = 0.5 * action,
                      .eligible = !(entity == 3 && action == 2),
                      .contacts = 0.0,
                      .contact_cap = 4.0,
                      .interaction_count = action == entity % 6 ? 3.0 : 0.0,
                      .interaction_time = 1.0,
                      .logging_propensity = 1.0 / 6.0});
      if (action == entity % 6) {
        rows.push_back({.entity = entity,
                        .action = action,
                        .features = {feature},
                        .response = response,
                        .value = 5.0 + action,
                        .cost = 0.5 * action,
                        .eligible = true,
                        .contacts = 0.0,
                        .contact_cap = 4.0,
                        .interaction_count = 3.0,
                        .interaction_time = 2.0,
                        .logging_propensity = 1.0 / 6.0});
      }
    }
  }
  return rows;
}

zetta::ml::NextBestActionParams params() {
  zetta::ml::NextBestActionParams result;
  result.propensity.num_trees = 8;
  result.propensity.max_depth = 2;
  result.propensity.max_bins = 8;
  result.factors = 4;
  result.als_iterations = 6;
  result.hit_rate_k = 5;
  result.seed = 77;
  return result;
}

zetta::NodeConfig node_config(const TempDir &directory) {
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

struct Member {
  Member(std::string id, std::uint16_t port,
         const std::vector<std::string> &seeds)
      : node(node_config(directory)),
        cluster(std::move(id), cluster_config(port, seeds)),
        coordinator(cluster, 2) {
    node.start();
  }

  TempDir directory;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
  zetta::compute::JobCoordinator coordinator;
};

template <class Predicate> bool wait_for(Predicate predicate) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

std::vector<std::unique_ptr<Member>> members(std::size_t count,
                                             std::uint16_t base_port) {
  std::vector<std::unique_ptr<Member>> result;
  for (std::size_t index = 0; index < count; ++index) {
    std::vector<std::string> seeds;
    for (std::size_t prior = 0; prior < index; ++prior) {
      seeds.push_back("127.0.0.1:" + std::to_string(base_port + prior));
    }
    result.push_back(std::make_unique<Member>(
        "nba-" + std::to_string(base_port) + "-" + std::to_string(index),
        static_cast<std::uint16_t>(base_port + index), seeds));
  }
  for (auto &member : result) {
    member->cluster.start();
  }
  EXPECT_TRUE(wait_for([&] {
    for (const auto &member : result) {
      if (member->cluster.peer_count() != count - 1) {
        return false;
      }
    }
    return true;
  }));
  for (auto &member : result) {
    member->coordinator.start();
  }
  return result;
}

std::vector<DistributedNextBestActionResult>
run(std::vector<std::unique_ptr<Member>> &cluster,
    const std::vector<std::vector<NextBestActionObservation>> &partitions) {
  std::vector<DistributedNextBestActionResult> results(cluster.size());
  std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(cluster.size());
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < cluster.size(); ++index) {
    jobs[index] =
        cluster[index]->node.create_job(zetta::compute::JobKind::Train);
    threads.emplace_back([&, index] {
      results[index] = zetta::ml::run_next_best_action_job(
          cluster[index]->coordinator, *jobs[index], partitions[index],
          {"signal"}, params(), "nba-partitions", 8s);
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  for (std::size_t index = 0; index < cluster.size(); ++index) {
    EXPECT_TRUE(results[index].ok())
        << "member " << index << ": " << jobs[index]->detail();
  }
  return results;
}

} // namespace

TEST(NextBestActionPartitions, DistributedMatchesSingleMember) {
  const std::vector<NextBestActionObservation> all = fixture();
  auto oracle_members = members(1, 47650);
  const auto oracle = run(oracle_members, {all});
  ASSERT_EQ(oracle.size(), 1u);
  ASSERT_TRUE(oracle[0].ok()) << oracle[0].error;

  std::vector<std::vector<NextBestActionObservation>> partitions(2);
  for (std::size_t index = 0; index < all.size(); ++index) {
    partitions[index % partitions.size()].push_back(all[index]);
  }
  auto distributed_members = members(2, 47660);
  const auto distributed = run(distributed_members, partitions);
  ASSERT_EQ(distributed.size(), 2u);
  for (std::size_t member = 0; member < distributed.size(); ++member) {
    const DistributedNextBestActionResult &result = distributed[member];
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.propensity_models.size(),
              oracle[0].propensity_models.size());
    for (std::size_t action = 0; action < result.propensity_models.size();
         ++action) {
      EXPECT_EQ(result.propensity_models[action].to_json(),
                distributed[0].propensity_models[action].to_json());
      for (const NextBestActionObservation &observation : all) {
        if (observation.action != static_cast<std::int64_t>(action)) {
          continue;
        }
        EXPECT_NEAR(result.propensity_models[action].predict_proba(
                        observation.features),
                    oracle[0].propensity_models[action].predict_proba(
                        observation.features),
                    1e-3)
            << "member " << member << " action " << action;
      }
    }
    ASSERT_EQ(result.recommendations.size(), oracle[0].recommendations.size());
    for (std::size_t index = 0; index < result.recommendations.size();
         ++index) {
      const auto &actual = result.recommendations[index];
      const auto &expected = oracle[0].recommendations[index];
      EXPECT_EQ(actual.entity, expected.entity);
      EXPECT_EQ(actual.action, expected.action);
      EXPECT_EQ(actual.rank, expected.rank);
      EXPECT_EQ(actual.chosen, expected.chosen);
      EXPECT_NEAR(actual.propensity, expected.propensity, 1e-12);
      EXPECT_NEAR(actual.affinity, expected.affinity, 1e-12);
      EXPECT_NEAR(actual.expected_value, expected.expected_value, 1e-12);
    }
    EXPECT_NEAR(result.hit_rate_at_k, oracle[0].hit_rate_at_k, 1e-12);
    EXPECT_NEAR(result.offline.inverse_propensity_value,
                oracle[0].offline.inverse_propensity_value, 1e-12);
    EXPECT_NEAR(result.offline.doubly_robust_value,
                oracle[0].offline.doubly_robust_value, 1e-12);
  }
}
