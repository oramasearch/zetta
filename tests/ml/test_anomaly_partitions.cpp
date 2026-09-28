#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/anomaly.hpp"
#include "zetta/ml/anomaly_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

struct Member {
  Member(std::string id, std::uint16_t port, std::vector<std::string> seeds)
      : node(node_config()),
        cluster(std::move(id), cluster_config(port, std::move(seeds))),
        coordinator(cluster, 2) {
    node.start();
  }

  ~Member() {
    coordinator.stop();
    cluster.stop();
    node.stop();
  }

  zetta::NodeConfig node_config() const {
    zetta::NodeConfig config;
    config.data_dir = directory.path().string();
    return config;
  }

  static zetta::cluster::ClusterConfig
  cluster_config(std::uint16_t port, std::vector<std::string> seeds) {
    zetta::cluster::ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    config.seeds = std::move(seeds);
    return config;
  }

  zetta::test::TempDir directory;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
  zetta::compute::JobCoordinator coordinator;
};

std::vector<zetta::ml::AnomalyPartition> partition_fixture() {
  std::vector<zetta::ml::AnomalyPartition> partitions(4);
  for (std::size_t partition_index = 0; partition_index < partitions.size();
       ++partition_index) {
    zetta::ml::AnomalyPartition &partition = partitions[partition_index];
    partition.id = "part-" + std::to_string(partition_index);
    partition.columns.assign(2, {});
    for (std::size_t local_row = 0; local_row < 40; ++local_row) {
      const std::size_t row = partition_index * 40 + local_row;
      partition.ordinals.push_back(row);
      partition.row_keys.push_back(static_cast<std::int64_t>(row));
      partition.columns[0].push_back(static_cast<double>((row * 13) % 17) /
                                     17.0);
      partition.columns[1].push_back(static_cast<double>((row * 7) % 23) /
                                     23.0);
    }
  }
  partitions[0].columns[0][39] = 30.0;
  partitions[3].columns[1][39] = 40.0;
  return partitions;
}

zetta::ml::AnomalyParams params() {
  zetta::ml::AnomalyParams value;
  value.tree_count = 80;
  value.subsample_size = 32;
  value.seed = 20260927;
  value.threshold.contamination.reset();
  value.threshold.alert_budget = 2;
  return value;
}

bool wait_for_cluster(const std::vector<std::unique_ptr<Member>> &members) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    bool ready = true;
    for (const std::unique_ptr<Member> &member : members) {
      if (member->cluster.peer_count() != members.size() - 1) {
        ready = false;
      }
    }
    if (ready) {
      return true;
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

std::map<std::uint64_t, double>
scores_by_ordinal(const std::vector<zetta::ml::AnomalyRecord> &records) {
  std::map<std::uint64_t, double> scores;
  for (const zetta::ml::AnomalyRecord &record : records) {
    scores[record.ordinal] = record.score;
  }
  return scores;
}

} // namespace

TEST(IsolationForestPartitions, DistributedForestMatchesTheSingleMemberOracle) {
  const std::vector<zetta::ml::AnomalyPartition> all = partition_fixture();
  const std::vector<std::string> features = {"latency", "errors"};
  const zetta::ml::AnomalyScoreResult oracle =
      zetta::ml::score_tabular_anomalies(all, features, params());
  ASSERT_TRUE(oracle.ok()) << oracle.status.message;

  constexpr std::uint16_t kBasePort = 47640;
  std::vector<std::unique_ptr<Member>> members;
  members.push_back(std::make_unique<Member>("anomaly-member-a", kBasePort,
                                             std::vector<std::string>{}));
  members.push_back(std::make_unique<Member>(
      "anomaly-member-b", static_cast<std::uint16_t>(kBasePort + 1),
      std::vector<std::string>{"127.0.0.1:" + std::to_string(kBasePort)}));
  for (const std::unique_ptr<Member> &member : members) {
    member->cluster.start();
  }
  ASSERT_TRUE(wait_for_cluster(members));
  for (const std::unique_ptr<Member> &member : members) {
    member->coordinator.start();
  }

  std::vector<std::vector<zetta::ml::AnomalyPartition>> local(2);
  local[0] = {all[0], all[2]};
  local[1] = {all[1], all[3]};
  const std::vector<std::string> global_ids = {"part-0", "part-1", "part-2",
                                               "part-3"};
  std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(2);
  std::vector<zetta::ml::DistributedAnomalyResult> results(2);
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < members.size(); ++index) {
    jobs[index] =
        members[index]->node.create_job(zetta::compute::JobKind::Train);
    ASSERT_TRUE(jobs[index]);
    threads.emplace_back([&, index] {
      results[index] = zetta::ml::run_anomaly_job(
          members[index]->coordinator, *jobs[index], local[index], global_ids,
          features, params(), "anomaly-partition-proof", 8s);
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  const std::map<std::uint64_t, double> oracle_scores =
      scores_by_ordinal(oracle.records);
  for (std::size_t index = 0; index < results.size(); ++index) {
    ASSERT_TRUE(results[index].ok())
        << "member " << index << ": " << jobs[index]->detail();
    EXPECT_EQ(results[index].model.trees.size(), params().tree_count);
    EXPECT_EQ(results[index].records.size(), oracle.records.size());
    EXPECT_EQ(results[index].report.flagged, 2u);
    const std::map<std::uint64_t, double> distributed_scores =
        scores_by_ordinal(results[index].records);
    ASSERT_EQ(distributed_scores.size(), oracle_scores.size());
    for (const auto &[ordinal, score] : oracle_scores) {
      ASSERT_TRUE(distributed_scores.contains(ordinal));
      EXPECT_NEAR(distributed_scores.at(ordinal), score, 1e-12)
          << "member " << index << " row " << ordinal;
    }
  }
  EXPECT_NE(results[0].is_root, results[1].is_root);

  bool saw_first_member_partition = false;
  bool saw_second_member_partition = false;
  for (const zetta::ml::IsolationTree &tree : results[0].model.trees) {
    saw_first_member_partition = saw_first_member_partition ||
                                 tree.partition_id == "part-0" ||
                                 tree.partition_id == "part-2";
    saw_second_member_partition = saw_second_member_partition ||
                                  tree.partition_id == "part-1" ||
                                  tree.partition_id == "part-3";
  }
  EXPECT_TRUE(saw_first_member_partition);
  EXPECT_TRUE(saw_second_member_partition);
}
