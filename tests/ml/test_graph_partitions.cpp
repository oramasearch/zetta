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
#include "zetta/ml/graph.hpp"
#include "zetta/ml/graph_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

struct GraphMember {
  GraphMember(std::string id, std::uint16_t port,
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

TEST(GraphPartitions,
     DistributedAllreduceMatchesTheSingleMemberReferenceAcrossThreeMembers) {
  const std::vector<std::vector<zetta::ml::GraphEdge>> partitions = {
      {{.source = 1,
        .destination = 2,
        .weight = 1.0,
        .time = 1.0,
        .source_label = 0},
       {.source = 4,
        .destination = 5,
        .weight = 2.0,
        .time = 2.0,
        .source_label = 1}},
      {{.source = 2, .destination = 3, .weight = 2.0, .time = 2.0},
       {.source = 5, .destination = 6, .weight = 1.0, .time = 3.0}},
      {{.source = 3,
        .destination = 1,
        .weight = 1.0,
        .time = 3.0,
        .source_label = 0},
       {.source = 6,
        .destination = 4,
        .weight = 1.0,
        .time = 4.0,
        .source_label = 1},
       {.source = 1, .destination = 6, .weight = 100.0, .time = 99.0}}};
  zetta::ml::GraphParams params;
  params.prediction_time = 10.0;
  params.embedding_dimensions = 4;
  params.page_rank_iterations = 30;
  params.seed = 17;
  const zetta::ml::GraphResult reference =
      zetta::ml::compute_graph_features(partitions, params);
  ASSERT_TRUE(reference.ok()) << reference.error;

  constexpr std::uint16_t kBasePort = 47580;
  std::vector<std::unique_ptr<GraphMember>> members;
  for (std::size_t index = 0; index < partitions.size(); ++index) {
    std::vector<std::string> seeds;
    for (std::size_t earlier = 0; earlier < index; ++earlier) {
      seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
    }
    members.push_back(std::make_unique<GraphMember>(
        "graph-member-" + std::to_string(index),
        static_cast<std::uint16_t>(kBasePort + index), seeds));
  }
  for (auto &member : members) {
    member->cluster.start();
  }
  ASSERT_TRUE(wait_for([&] {
    for (const auto &member : members) {
      if (member->cluster.peer_count() != members.size() - 1) {
        return false;
      }
    }
    return true;
  }));
  for (auto &member : members) {
    member->coordinator.start();
  }

  std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs(members.size());
  std::vector<zetta::ml::DistributedGraphResult> results(members.size());
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < members.size(); ++index) {
    jobs[index] =
        members[index]->node.create_job(zetta::compute::JobKind::Train);
    threads.emplace_back([&, index] {
      results[index] = zetta::ml::run_graph_job(members[index]->coordinator,
                                                *jobs[index], partitions[index],
                                                params, "graph-partitions", 8s);
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }

  for (std::size_t member = 0; member < members.size(); ++member) {
    ASSERT_TRUE(results[member].ok())
        << "member " << member << ": " << jobs[member]->detail();
    ASSERT_EQ(results[member].graph.features.size(), reference.features.size());
    EXPECT_EQ(results[member].graph.report.edges_read,
              reference.report.edges_read);
    EXPECT_EQ(results[member].graph.report.edges_excluded,
              reference.report.edges_excluded);
    EXPECT_EQ(results[member].graph.report.component_sizes,
              reference.report.component_sizes);
    for (std::size_t index = 0; index < reference.features.size(); ++index) {
      const auto &actual = results[member].graph.features[index];
      const auto &expected = reference.features[index];
      EXPECT_EQ(actual.node, expected.node);
      EXPECT_DOUBLE_EQ(actual.in_degree, expected.in_degree);
      EXPECT_DOUBLE_EQ(actual.out_degree, expected.out_degree);
      EXPECT_DOUBLE_EQ(actual.weighted_in_degree, expected.weighted_in_degree);
      EXPECT_DOUBLE_EQ(actual.weighted_out_degree,
                       expected.weighted_out_degree);
      EXPECT_NEAR(actual.page_rank, expected.page_rank, 1e-12);
      EXPECT_EQ(actual.component, expected.component);
      EXPECT_EQ(actual.propagated_label, expected.propagated_label);
      EXPECT_DOUBLE_EQ(actual.label_score, expected.label_score);
      EXPECT_DOUBLE_EQ(actual.clustering_coefficient,
                       expected.clustering_coefficient);
      EXPECT_EQ(actual.embedding, expected.embedding);
    }
  }

  for (auto &member : members) {
    member->coordinator.stop();
    member->cluster.stop();
  }
}
