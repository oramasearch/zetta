#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ml/uplift_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

constexpr std::size_t kRows = 360;
constexpr std::int64_t kWideEntity = 9'007'199'254'740'993ll;

zetta::ddf::DataFrameSchema schema() {
  using zetta::compute::DataType;
  using zetta::ddf::ColumnSchema;
  zetta::ddf::DataFrameSchema value;
  value.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                   ColumnSchema{.name = "segment", .dtype = DataType::Float64},
                   ColumnSchema{.name = "baseline", .dtype = DataType::Float64},
                   ColumnSchema{.name = "treatment", .dtype = DataType::Int64},
                   ColumnSchema{.name = "outcome", .dtype = DataType::Int64}};
  return value;
}

zetta::ml::UpliftParams params() {
  zetta::ml::UpliftParams value;
  value.outcome_model.num_trees = 16;
  value.outcome_model.max_depth = 3;
  value.outcome_model.max_bins = 16;
  value.holdout_fraction = 0.3;
  value.bootstrap_samples = 30;
  value.seed = 1045;
  return value;
}

double unit(std::size_t index, std::uint64_t salt) {
  std::uint64_t value = static_cast<std::uint64_t>(index) + salt;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  value ^= value >> 31;
  return static_cast<double>(value >> 11) /
         static_cast<double>(std::uint64_t{1} << 53);
}

Json row(std::size_t index) {
  const double segment = unit(index, 0x1045ull);
  const double baseline = unit(index, 0x2045ull);
  const std::int64_t treatment = unit(index, 0x3045ull) < 0.5 ? 1 : 0;
  const double threshold =
      0.12 + baseline * 0.18 + (segment >= 0.5 ? treatment * 0.55 : 0.0);
  const double draw = unit(index, 0x4045ull);
  const std::int64_t entity =
      index + 1 == kRows ? kWideEntity : static_cast<std::int64_t>(index);
  return Json{{"entity", entity},
              {"segment", segment},
              {"baseline", baseline},
              {"treatment", treatment},
              {"outcome", draw < threshold ? 1 : 0}};
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

template <class Predicate> bool wait_for(Predicate predicate) {
  for (int attempt = 0; attempt < 400; ++attempt) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(25ms);
  }
  return false;
}

struct PartitionNode {
  PartitionNode(std::string id, std::uint16_t port,
                std::vector<std::string> seeds, const std::vector<Json> &rows)
      : node(node_config(directory)),
        cluster(std::move(id), cluster_config(port, seeds)),
        coordinator(cluster, 2) {
    node.start();
    const auto database = node.create_database("data");
    const auto table = database->create_table("uplift");
    table->set_partitioner(
        std::make_shared<zetta::data::RoundRobinPartitioner>(4));
    for (const Json &value : rows) {
      table->add_row(value);
    }
    const zetta::data::PartitionWriteResult written =
        zetta::data::write_partitioned(node, *database, *table);
    EXPECT_TRUE(written.ok()) << written.error;
    table_id = table->id();
  }

  zetta::test::TempDir directory;
  zetta::Node node;
  zetta::cluster::Cluster cluster;
  zetta::compute::JobCoordinator coordinator;
  zetta::compute::CpuDevice device;
  std::string table_id;
};

std::vector<std::unique_ptr<PartitionNode>>
make_cluster(const std::vector<std::vector<Json>> &shards,
             std::uint16_t base_port) {
  std::vector<std::unique_ptr<PartitionNode>> nodes;
  for (std::size_t index = 0; index < shards.size(); ++index) {
    std::vector<std::string> seeds;
    for (std::size_t prior = 0; prior < index; ++prior) {
      seeds.push_back("127.0.0.1:" + std::to_string(base_port + prior));
    }
    nodes.push_back(std::make_unique<PartitionNode>(
        "uplift-" + std::to_string(base_port) + "-" + std::to_string(index),
        static_cast<std::uint16_t>(base_port + index), std::move(seeds),
        shards[index]));
  }
  for (auto &node : nodes) {
    node->cluster.start();
  }
  EXPECT_TRUE(wait_for([&] {
    for (const auto &node : nodes) {
      if (node->cluster.peer_count() + 1 != nodes.size()) {
        return false;
      }
    }
    return true;
  }));
  for (auto &node : nodes) {
    node->coordinator.start();
  }
  return nodes;
}

std::vector<zetta::ml::DistributedUpliftResult>
run(std::vector<std::unique_ptr<PartitionNode>> &nodes) {
  std::vector<zetta::ml::DistributedUpliftResult> results(nodes.size());
  std::vector<std::thread> threads;
  for (std::size_t index = 0; index < nodes.size(); ++index) {
    threads.emplace_back([&, index] {
      zetta::ddf::LocalShardResult local = zetta::ddf::load_local_partitions(
          nodes[index]->node, nodes[index]->table_id, schema(),
          nodes[index]->device);
      ASSERT_TRUE(local.ok()) << local.status.message;
      const auto job =
          nodes[index]->node.create_job(zetta::compute::JobKind::Train);
      ASSERT_TRUE(job);
      results[index] = zetta::ml::run_uplift_job(
          nodes[index]->coordinator, *job, local.frame, "entity", "treatment",
          "outcome", {"segment", "baseline"}, params(), "uplift-partitions",
          8s);
    });
  }
  for (std::thread &thread : threads) {
    thread.join();
  }
  return results;
}

std::map<std::int64_t, double> effects(const zetta::ml::UpliftResult &result) {
  std::map<std::int64_t, double> values;
  for (const zetta::ml::TreatmentEffect &effect : result.treatment_effects) {
    values[effect.entity] = effect.estimated_uplift;
  }
  return values;
}

} // namespace

TEST(UpliftPartitions, DistributedOwnedPartitionsMatchSingleNode) {
  std::vector<Json> all;
  for (std::size_t index = 0; index < kRows; ++index) {
    all.push_back(row(index));
  }
  auto oracle_nodes = make_cluster({all}, 47530);
  const auto oracle = run(oracle_nodes);
  ASSERT_EQ(oracle.size(), 1u);
  ASSERT_TRUE(oracle.front().ok()) << oracle.front().uplift.status.message;
  EXPECT_TRUE(effects(oracle.front().uplift).contains(kWideEntity));

  constexpr std::size_t kMembers = 3;
  std::vector<std::vector<Json>> shards(kMembers);
  for (std::size_t index = 0; index < all.size(); ++index) {
    shards[index % kMembers].push_back(all[index]);
  }
  auto distributed_nodes = make_cluster(shards, 47540);
  const auto distributed = run(distributed_nodes);
  ASSERT_EQ(distributed.size(), kMembers);
  for (const auto &result : distributed) {
    ASSERT_TRUE(result.ok()) << result.uplift.status.message;
    EXPECT_EQ(result.uplift.selected_learner,
              distributed.front().uplift.selected_learner);
    EXPECT_EQ(result.uplift.treatment_effects.size(), kRows);
  }

  EXPECT_EQ(distributed.front().uplift.selected_learner,
            oracle.front().uplift.selected_learner);
  EXPECT_NEAR(distributed.front().uplift.t_learner.auuc,
              oracle.front().uplift.t_learner.auuc, 0.02);
  EXPECT_NEAR(distributed.front().uplift.x_learner.auuc,
              oracle.front().uplift.x_learner.auuc, 0.02);
  const auto oracle_effects = effects(oracle.front().uplift);
  const auto distributed_effects = effects(distributed.front().uplift);
  ASSERT_EQ(oracle_effects.size(), distributed_effects.size());
  double absolute_difference = 0.0;
  for (const auto &[entity, effect] : oracle_effects) {
    absolute_difference += std::abs(distributed_effects.at(entity) - effect);
  }
  EXPECT_LT(absolute_difference / static_cast<double>(oracle_effects.size()),
            0.03);
}
