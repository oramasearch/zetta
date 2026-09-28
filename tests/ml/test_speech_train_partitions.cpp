#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/speech_train_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

struct SpeechMember {
  SpeechMember(std::string id, std::uint16_t port,
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

bool wait_for_peers(const std::vector<std::unique_ptr<SpeechMember>> &members) {
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

} // namespace

TEST(SpeechTrainPartitions, AllreducesOwnedPartitionStatisticsAcrossMembers) {
  constexpr std::uint16_t base_port = 47940;
  std::vector<std::unique_ptr<SpeechMember>> members;
  members.push_back(std::make_unique<SpeechMember>("speech-a", base_port,
                                                   std::vector<std::string>{}));
  members.push_back(std::make_unique<SpeechMember>(
      "speech-b", base_port + 1,
      std::vector<std::string>{"127.0.0.1:" + std::to_string(base_port)}));
  for (auto &member : members) {
    member->cluster.start();
  }
  ASSERT_TRUE(wait_for_peers(members));
  for (auto &member : members) {
    member->coordinator.start();
  }

  std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs;
  std::vector<zetta::ml::SpeechStatistics> results(members.size());
  std::vector<std::string> errors(members.size());
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < members.size(); ++index) {
    jobs.push_back(
        members[index]->node.create_job(zetta::compute::JobKind::Train));
    threads.emplace_back([&, index] {
      zetta::ml::SpeechStatistics local;
      local.recordings_read = index + 1;
      local.recordings_excluded = index;
      local.segments = (index + 1) * 2;
      local.word_error.edits = index + 2;
      local.word_error.reference_words = (index + 1) * 5;
      results[index] = zetta::ml::run_speech_statistics_job(
          members[index]->coordinator, *jobs[index], local,
          "speech-partition-test", 8s, &errors[index]);
    });
  }
  for (auto &thread : threads) {
    thread.join();
  }
  for (std::size_t index = 0; index < members.size(); ++index) {
    EXPECT_TRUE(errors[index].empty()) << errors[index];
    EXPECT_EQ(jobs[index]->status(), zetta::compute::JobStatus::Succeeded);
    EXPECT_EQ(results[index].recordings_read, 3u);
    EXPECT_EQ(results[index].recordings_excluded, 1u);
    EXPECT_EQ(results[index].segments, 6u);
    EXPECT_EQ(results[index].word_error.edits, 5u);
    EXPECT_EQ(results[index].word_error.reference_words, 15u);
  }
}
