// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/synthetic_private_job.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

struct PrivateNode {
    PrivateNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds,
                const std::vector<nlohmann::json>& rows)
        : node(node_config()),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, 2) {
        node.start();
        const std::shared_ptr<zetta::data::Database> database =
            node.create_database("private_data");
        EXPECT_TRUE(database);
        const std::shared_ptr<zetta::data::Table> table =
            database->create_table("training");
        EXPECT_TRUE(table);
        table->set_partitioner(
            std::make_shared<zetta::data::RoundRobinPartitioner>(3));
        for (const nlohmann::json& row : rows) {
            table->add_row(row);
        }
        const zetta::data::PartitionWriteResult written =
            zetta::data::write_partitioned(node, *database, *table);
        EXPECT_TRUE(written.ok()) << written.error;
        table_id = table->id();
    }

    zetta::NodeConfig node_config() const {
        zetta::NodeConfig config;
        config.data_dir = temp_dir.path().string();
        return config;
    }

    static zetta::cluster::ClusterConfig cluster_config(
        std::uint16_t port, const std::vector<std::string>& seeds) {
        zetta::cluster::ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    zetta::test::TempDir temp_dir;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
    std::string table_id;
};

std::vector<nlohmann::json> shard(std::size_t rank) {
    std::vector<nlohmann::json> rows;
    for (std::size_t index = rank; index < 160; index += 2) {
        const double first = static_cast<double>(index) / 159.0;
        const double second =
            std::clamp(0.8 * first + 0.1 * std::sin(index), 0.0, 1.0);
        rows.push_back({{"entity", static_cast<std::int64_t>(index)},
                        {"first", first},
                        {"second", second},
                        {"region", index % 3 == 0 ? "west" : "east"},
                        {"label", first + second > 1.0 ? 1 : 0}});
    }
    return rows;
}

zetta::data::DatasetContract contract() {
    zetta::data::DatasetContract result;
    result.entity_column = "entity";
    result.label_column = "label";
    result.feature_columns = {"first", "second", "region"};
    return result;
}

zetta::ml::SyntheticPrivateParams params() {
    zetta::ml::SyntheticPrivateParams result;
    result.epsilon = 4.0;
    result.delta = 1e-6;
    result.maximum_epsilon = 8.0;
    result.maximum_delta = 1e-5;
    result.synthetic_rows = 160;
    result.seed = 1059;
    result.maximum_membership_auc = 0.9;
    result.gbdt.num_trees = 4;
    result.gbdt.max_depth = 2;
    result.gbdt.max_bins = 16;
    result.gbdt.learning_rate = 0.15;
    result.gbdt.min_child_hessian = 0.0;
    return result;
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

}  // namespace

TEST(SyntheticPrivatePartitions, CombinesOwnedStatisticsAndDpHistograms) {
    constexpr std::uint16_t kBasePort = 47700;
    PrivateNode first("synthetic-private-a", kBasePort, {}, shard(0));
    PrivateNode second("synthetic-private-b", kBasePort + 1,
                       {"127.0.0.1:" + std::to_string(kBasePort)}, shard(1));
    first.cluster.start();
    second.cluster.start();
    ASSERT_TRUE(wait_for([&] {
        return first.cluster.peer_count() == 1 && second.cluster.peer_count() == 1;
    }));
    first.coordinator.start();
    second.coordinator.start();

    std::vector<zetta::ml::DistributedSyntheticPrivateResult> results(2);
    const std::shared_ptr<zetta::compute::ComputeJob> first_job =
        first.node.create_job(zetta::compute::JobKind::Train);
    const std::shared_ptr<zetta::compute::ComputeJob> second_job =
        second.node.create_job(zetta::compute::JobKind::Train);
    std::thread first_thread([&] {
        results[0] = zetta::ml::run_synthetic_private_job(
            first.coordinator, first.node, *first_job, first.table_id,
            contract(), params(), "synthetic-private-partitions", 8s);
    });
    std::thread second_thread([&] {
        results[1] = zetta::ml::run_synthetic_private_job(
            second.coordinator, second.node, *second_job, second.table_id,
            contract(), params(), "synthetic-private-partitions", 8s);
    });
    first_thread.join();
    second_thread.join();

    bool all_ok = true;
    for (std::size_t index = 0; index < results.size(); ++index) {
        all_ok = results[index].ok() && all_ok;
        EXPECT_TRUE(results[index].ok())
            << "node " << index << ": " << results[index].learned.error
            << ", rows=" << results[index].rows_read
            << ", partitions=" << results[index].partitions_read
            << ", job=" << (index == 0 ? first_job->detail()
                                          : second_job->detail());
        EXPECT_EQ(results[index].rows_read, 80u);
        EXPECT_GT(results[index].partitions_read, 0u);
        EXPECT_GT(results[index].local_bytes_read, 0u);
        EXPECT_GT(results[index].wire_bytes_moved, 0u);
        if (results[index].ok()) {
            EXPECT_EQ(results[index].learned.synthetic_rows.size(), 160u);
        }
    }
    if (!all_ok) {
        first.coordinator.stop();
        second.coordinator.stop();
        second.cluster.stop();
        first.cluster.stop();
        return;
    }
    EXPECT_TRUE(results[0].is_root);
    EXPECT_FALSE(results[1].is_root);
    EXPECT_EQ(results[0].learned.private_model.to_json(),
              results[1].learned.private_model.to_json());
    EXPECT_EQ(results[0].learned.synthetic_rows,
              results[1].learned.synthetic_rows);
    EXPECT_DOUBLE_EQ(results[0].learned.metrics.privacy.epsilon_spent,
                     results[1].learned.metrics.privacy.epsilon_spent);

    first.coordinator.stop();
    second.coordinator.stop();
    second.cluster.stop();
    first.cluster.stop();
}
