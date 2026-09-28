#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/vision_train_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

struct VisionMember {
  VisionMember(std::string id, std::uint16_t port,
               std::vector<std::string> seeds)
      : node(node_config()),
        cluster(std::move(id), cluster_config(port, std::move(seeds))),
        coordinator(cluster, 2) {
    node.start();
  }

  zetta::NodeConfig node_config() const {
    zetta::NodeConfig config;
    config.data_dir = temporary.path().string();
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

  zetta::test::TempDir temporary;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
  zetta::compute::JobCoordinator coordinator;
};

bool wait_for_peers(const std::vector<std::unique_ptr<VisionMember>> &members) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    bool ready = true;
    for (const auto &member : members) {
      ready = ready && member->cluster.peer_count() == members.size() - 1;
    }
    if (ready) {
      return true;
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

std::vector<zetta::ml::VisionExample> member_examples(std::size_t member) {
  std::vector<zetta::ml::VisionExample> examples;
  for (std::size_t index = 0; index < 40; ++index) {
    const std::size_t label = (member + index) % 2;
    const float signal = label == 0 ? -1.5F : 1.5F;
    examples.push_back(
        {"image-" + std::to_string(member) + "-" + std::to_string(index),
         "source-" + std::to_string(member) + "-" + std::to_string(index / 4),
         label,
         {signal, signal * 0.25F, 0.2F},
         index % 5 == 0});
  }
  return examples;
}

} // namespace

TEST(VisionTrainPartitions, AllreducesOwnedPartitionStatisticsAcrossMembers) {
  constexpr std::uint16_t base_port = 47920;
  std::vector<std::unique_ptr<VisionMember>> members;
  members.push_back(std::make_unique<VisionMember>("vision-a", base_port,
                                                   std::vector<std::string>{}));
  members.push_back(std::make_unique<VisionMember>(
      "vision-b", base_port + 1,
      std::vector<std::string>{"127.0.0.1:" + std::to_string(base_port)}));
  for (auto &member : members) {
    member->cluster.start();
  }
  ASSERT_TRUE(wait_for_peers(members));
  for (auto &member : members) {
    member->coordinator.start();
  }

  std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs;
  std::vector<zetta::ml::VisionTrainResult> results(members.size());
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < members.size(); ++index) {
    jobs.push_back(
        members[index]->node.create_job(zetta::compute::JobKind::Train));
    threads.emplace_back([&, index] {
      results[index] = zetta::ml::run_vision_train_job(
          members[index]->coordinator, *jobs[index], member_examples(index),
          {.classes = 2, .epochs = 35, .learning_rate = 0.2, .seed = 17},
          "vision-partition-test", 8s);
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  for (std::size_t index = 0; index < results.size(); ++index) {
    ASSERT_TRUE(results[index].ok()) << results[index].error;
    EXPECT_EQ(jobs[index]->status(), zetta::compute::JobStatus::Succeeded);
    EXPECT_GT(results[index].metrics.accuracy, 0.95);
    EXPECT_EQ(results[index].metrics.train_rows, 64u);
    EXPECT_EQ(results[index].metrics.held_out_rows, 16u);
    EXPECT_EQ(results[index].head.weights, results.front().head.weights);
    EXPECT_EQ(results[index].metrics.confusion_matrix,
              results.front().metrics.confusion_matrix);
  }
}
