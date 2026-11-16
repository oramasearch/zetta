// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/entity_resolution.hpp"
#include "zetta/ml/entity_resolution_job.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using namespace std::chrono_literals;
using zetta::ml::EntityRecord;
using zetta::ml::EntityResolutionResult;

std::vector<EntityRecord> duplicate_fixture() {
  return {
      {"crm", "1", "Alice Johnson", "10 Pine Street", "alice@example.com",
       "604-555-0101", 0.0, false, true, true, "alice"},
      {"billing", "101", "Alyce Johnson", "10 Pine St",
       "ALICE@example.com ", "+1 604 555 0101", 0.0, false, true, true,
       "alice"},
      {"crm", "2", "Bob Martinez", "22 Oak Avenue", "bob@example.com",
       "604-555-0102", 0.0, false, true, true, "bob"},
      {"billing", "102", "Bbo Martinez", "22 Oak Ave", "bob@example.com",
       "6045550102", 0.0, false, true, true, "bob"},
      {"crm", "3", "Carol Singh", "9 Cedar Road", "carol@example.com",
       "604-555-0103", 0.0, false, true, true, "carol"},
      {"billing", "103", "Carol Sing", "9 Cedar Rd", "carol@example.com",
       "6045550103", 0.0, false, true, true, "carol"},
      {"crm", "4", "David Chen", "18 Birch Lane", "david@example.com",
       "604-555-0104", 0.0, false, true, true, "david"},
      {"billing", "104", "Dvaid Chen", "18 Birch Ln", "david@example.com",
       "6045550104", 0.0, false, true, true, "david"},
      {"crm", "5", "Eva Brown", "77 Lake Drive", "eva@example.com",
       "604-555-0105", 0.0, false, true, true, "eva"},
      {"billing", "105", "Eve Brown", "77 Lake Dr", "eva@example.com",
       "6045550105", 0.0, false, true, true, "eva"},
      {"crm", "bridge-a", "Jordan Lee", "1 Main", "first@example.com",
       "1111", 0.0, false, true, false, "jordan-first"},
      {"support", "bridge", "Jordan Lee", "1 Main", "", "", 0.0, false,
       false, false, "jordan-first"},
      {"billing", "bridge-b", "Jordan Lee", "1 Main",
       "second@example.com", "2222", 0.0, false, true, false,
       "jordan-second"},
  };
}

std::vector<std::string> assignments(const EntityResolutionResult &result) {
  std::vector<std::string> values;
  for (const auto &entity : result.entities) {
    values.push_back(entity.source + ":" + entity.record_id + "=" +
                     entity.entity_id);
  }
  std::sort(values.begin(), values.end());
  return values;
}

struct EntityResolutionMember {
  EntityResolutionMember(std::string id, std::uint16_t port,
                         const std::vector<std::string> &seeds)
      : node(node_config()),
        cluster(std::move(id), cluster_config(port, seeds)),
        coordinator(cluster, 2) {
    node.start();
  }

  zetta::NodeConfig node_config() const {
    zetta::NodeConfig config;
    config.data_dir = directory.path().string();
    return config;
  }

  static zetta::cluster::ClusterConfig
  cluster_config(std::uint16_t port, const std::vector<std::string> &seeds) {
    zetta::cluster::ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = port;
    config.port_range = 1;
    config.seeds = seeds;
    return config;
  }

  zetta::test::TempDir directory;
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

} // namespace

TEST(EntityResolutionPartitions,
     OwnedCandidatesAndMergedComponentsMatchTheSingleMemberOracle) {
  const std::vector<EntityRecord> records = duplicate_fixture();
  const EntityResolutionResult reference =
      zetta::ml::resolve_entities({records});
  ASSERT_TRUE(reference.ok()) << reference.error;

  constexpr std::uint16_t kBasePort = 47620;
  constexpr std::size_t kMemberCount = 3;
  std::vector<std::unique_ptr<EntityResolutionMember>> members;
  for (std::size_t index = 0; index < kMemberCount; ++index) {
    std::vector<std::string> seeds;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      seeds.push_back("127.0.0.1:" +
                      std::to_string(kBasePort + earlier));
    }
    members.push_back(std::make_unique<EntityResolutionMember>(
        "entity-resolution-member-" + std::to_string(index),
        static_cast<std::uint16_t>(kBasePort + index), seeds));
  }
  for (auto &member : members) {
    member->cluster.start();
  }
  ASSERT_TRUE(wait_for([&] {
    return std::all_of(members.begin(), members.end(), [&](const auto &member) {
      return member->cluster.peer_count() == kMemberCount - 1;
    });
  }));
  for (auto &member : members) {
    member->coordinator.start();
  }

  std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(kMemberCount);
  std::vector<zetta::ml::DistributedEntityResolutionResult> results(
      kMemberCount);
  std::vector<zetta::ml::EntityMatchPair> streamed_pairs;
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < kMemberCount; ++index) {
    jobs[index] =
        members[index]->node.create_job(zetta::compute::JobKind::Train);
    threads.emplace_back([&, index] {
      zetta::ml::EntityResolutionJobOptions options;
      options.stream_pairs = true;
      options.is_output_member = index == 1;
      if (options.is_output_member) {
        options.pair_sink = [&](const auto &page) {
          streamed_pairs.insert(streamed_pairs.end(), page.begin(), page.end());
          return true;
        };
      }
      results[index] = zetta::ml::run_entity_resolution_job(
          members[index]->coordinator, *jobs[index], records, {},
          "entity-resolution-partitions", 8s, options);
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  std::size_t owned_pair_count = 0;
  for (std::size_t member = 0; member < kMemberCount; ++member) {
    ASSERT_TRUE(results[member].ok())
        << "member " << member << ": " << jobs[member]->detail();
    EXPECT_EQ(assignments(results[member].resolution), assignments(reference));
    EXPECT_EQ(results[member].resolution.scoring_method,
              reference.scoring_method);
    EXPECT_DOUBLE_EQ(results[member].resolution.metrics.pairwise_f1,
                     reference.metrics.pairwise_f1);
    EXPECT_DOUBLE_EQ(results[member].resolution.metrics.bcubed_precision,
                     reference.metrics.bcubed_precision);
    EXPECT_DOUBLE_EQ(results[member].resolution.metrics.bcubed_recall,
                     reference.metrics.bcubed_recall);
    EXPECT_EQ(results[member].resolution.metrics.candidate_pairs,
              reference.metrics.candidate_pairs);
    owned_pair_count += results[member].owned_pairs.size();
    for (const auto &pair : results[member].owned_pairs) {
      EXPECT_EQ(pair.owner_partition, results[member].participant_index);
    }
  }
  EXPECT_EQ(owned_pair_count, reference.pairs.size());
  EXPECT_EQ(streamed_pairs.size(), reference.pairs.size());
  EXPECT_TRUE(std::all_of(results.begin(), results.end(), [](const auto &result) {
    return !result.owned_pairs.empty();
  }));

  for (auto &member : members) {
    member->coordinator.stop();
    member->cluster.stop();
  }
}

TEST(EntityResolutionLaunch, MalformedPayloadIsRefusedBeforeReadingData) {
  zetta::test::TempDir directory;
  zetta::NodeConfig node_config;
  node_config.data_dir = directory.path().string();
  zetta::Node node(node_config);
  zetta::cluster::ClusterConfig cluster_config;
  cluster_config.listen_host = "127.0.0.1";
  cluster_config.listen_port = 47660;
  cluster_config.port_range = 1;
  const std::string member_id = "entity-resolution-launch-member";
  zetta::cluster::Cluster cluster(member_id, cluster_config);
  node.start();
  cluster.start();
  node.attach_cluster(cluster, 2);

  zetta::compute::CoordinatedLaunchRequest request;
  request.id = zetta::util::new_uuid();
  request.kind = "entity_resolution";
  request.initiator = member_id;
  request.participants = {member_id};
  request.payload = {{"params", {{"seed", "not an integer"}}}};
  zetta::cluster::Signal signal;
  signal.kind = zetta::cluster::SignalKind::Task;
  signal.method = "run.launch";
  signal.from = member_id;
  signal.payload = zetta::compute::encode_coordinated_launch(request);

  node.coordinated_launch()->on_task(signal);
  EXPECT_EQ(node.coordinated_launch()->drain(), 1u);
  EXPECT_TRUE(node.jobs().empty());
  EXPECT_TRUE(node.runs().empty());
  bool refused = false;
  for (const auto &span : node.span_recorder().snapshot()) {
    if (span.name == "run.launch:entity_resolution") {
      refused = !span.ok &&
                span.error.find("entity resolution launch:") !=
                    std::string::npos;
    }
  }
  EXPECT_TRUE(refused);

  cluster.stop();
  node.stop();
}
