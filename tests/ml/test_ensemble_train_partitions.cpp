// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ml/ensemble.hpp"
#include "zetta/ml/ensemble_train_job.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

namespace {

using namespace std::chrono_literals;
using Json = nlohmann::json;

struct EnsembleNode {
    EnsembleNode(std::string id, std::uint16_t port,
                 const std::vector<std::string>& seeds)
        : node(node_config()),
          cluster(std::move(id), cluster_config(port, seeds)),
          coordinator(cluster, 2) {
        node.start();
    }

    zetta::NodeConfig node_config() {
        zetta::NodeConfig config;
        config.data_dir = directory.path().string();
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

    zetta::test::TempDir directory;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
    zetta::compute::CpuDevice device;
};

std::vector<Json> planted_rows(std::size_t count) {
    std::vector<Json> rows;
    for (std::size_t index = 0; index < count; ++index) {
        const double first = static_cast<double>((index * 17) % 101) / 100.0;
        const double second = static_cast<double>((index * 43) % 103) / 102.0;
        const bool first_region = index % 2 == 0;
        const bool positive = first_region ? first > 0.48 : second > 0.52;
        rows.push_back(Json{{"entity", static_cast<std::int64_t>(index)},
                            {"first", first},
                            {"second", second},
                            {"label", positive ? 1.0 : 0.0}});
    }
    return rows;
}

zetta::ddf::DataFrameSchema schema() {
    using zetta::compute::DataType;
    using zetta::ddf::ColumnSchema;
    zetta::ddf::DataFrameSchema value;
    value.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ColumnSchema{.name = "first", .dtype = DataType::Float64},
        ColumnSchema{.name = "second", .dtype = DataType::Float64},
        ColumnSchema{.name = "label", .dtype = DataType::Float64}};
    return value;
}

zetta::storage::JsonStore store_for(const std::vector<Json>& rows,
                                    std::size_t parity) {
    zetta::storage::JsonStore store;
    for (std::size_t index = parity; index < rows.size(); index += 2) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

zetta::ml::EnsembleParams parameters() {
    zetta::ml::EnsembleParams params;
    params.folds = 3;
    params.holdout_fraction = 0.2;
    params.meta_iterations = 80;
    params.meta_learning_rate = 0.2;
    zetta::ml::EnsembleBaseSpec first;
    first.name = "first-only";
    first.feature_names = {"first"};
    first.params.num_trees = 5;
    first.params.max_depth = 2;
    zetta::ml::EnsembleBaseSpec second;
    second.name = "second-only";
    second.feature_names = {"second"};
    second.params.num_trees = 5;
    second.params.max_depth = 2;
    params.bases = {first, second};
    return params;
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

TEST(EnsembleTrainPartitions, OwnedRowsProduceOneDistributedStack) {
    constexpr std::uint16_t first_port = 47950;
    EnsembleNode first("ensemble-a", first_port, {});
    EnsembleNode second("ensemble-b", first_port + 1,
                        {"127.0.0.1:" + std::to_string(first_port)});
    first.cluster.start();
    second.cluster.start();
    ASSERT_TRUE(wait_for([&] {
        return first.cluster.peer_count() == 1 &&
               second.cluster.peer_count() == 1;
    }));
    first.coordinator.start();
    second.coordinator.start();

    const std::vector<Json> rows = planted_rows(160);
    zetta::ddf::DataFrameResult first_frame = zetta::ddf::load_data_frame(
        store_for(rows, 0), schema(), first.device);
    zetta::ddf::DataFrameResult second_frame = zetta::ddf::load_data_frame(
        store_for(rows, 1), schema(), second.device);
    ASSERT_TRUE(first_frame.ok()) << first_frame.status.message;
    ASSERT_TRUE(second_frame.ok()) << second_frame.status.message;

    std::shared_ptr<zetta::compute::ComputeJob> first_job =
        first.node.create_job(zetta::compute::JobKind::Train);
    std::shared_ptr<zetta::compute::ComputeJob> second_job =
        second.node.create_job(zetta::compute::JobKind::Train);
    std::vector<zetta::ml::DistributedEnsembleResult> results(2);
    std::thread first_thread([&] {
        results[0] = zetta::ml::run_ensemble_train_job(
            first.coordinator, *first_job, first_frame.frame,
            {"first", "second"}, "label", "entity", "", parameters(),
            std::nullopt, "ensemble-partitions", 8s);
    });
    std::thread second_thread([&] {
        results[1] = zetta::ml::run_ensemble_train_job(
            second.coordinator, *second_job, second_frame.frame,
            {"first", "second"}, "label", "entity", "", parameters(),
            std::nullopt, "ensemble-partitions", 8s);
    });
    first_thread.join();
    second_thread.join();

    ASSERT_TRUE(results[0].ok()) << first_job->detail();
    ASSERT_TRUE(results[1].ok()) << second_job->detail();
    EXPECT_TRUE(results[0].is_root);
    EXPECT_FALSE(results[1].is_root);
    ASSERT_EQ(results[0].training.model.stacker.weights.size(), 2u);
    ASSERT_EQ(results[1].training.model.stacker.weights.size(), 2u);
    for (std::size_t index = 0; index < 2; ++index) {
        EXPECT_NEAR(results[0].training.model.stacker.weights[index],
                    results[1].training.model.stacker.weights[index], 1e-12);
    }
    EXPECT_NEAR(results[0].training.held_out_metrics.auc,
                results[1].training.held_out_metrics.auc, 1e-12);
    EXPECT_EQ(results[0].training.oof_rows +
                  results[0].training.held_out_rows,
              160u);

    first.coordinator.stop();
    second.coordinator.stop();
    first.cluster.stop();
    second.cluster.stop();
}
