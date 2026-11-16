// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/rank.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

zetta::ml::RankDataset synthetic_ranking_data() {
    zetta::ml::RankDataset dataset;
    dataset.feature_names = {"ranking_signal", "noise"};
    dataset.features.assign(2, {});
    for (std::int64_t group = 0; group < 40; ++group) {
        for (std::int64_t entity = 0; entity < 10; ++entity) {
            const double relevance = static_cast<double>((group % 2) * 10 + entity / 2);
            dataset.features[0].push_back(static_cast<double>(entity));
            dataset.features[1].push_back(static_cast<double>(group % 2));
            dataset.labels.push_back(relevance);
            dataset.groups.push_back(group);
            dataset.entities.push_back(group * 100 + entity);
        }
    }
    return dataset;
}

zetta::ml::RankDataset groups_for_shard(const zetta::ml::RankDataset& source, std::size_t shard,
                                        std::size_t shard_count) {
    zetta::ml::RankDataset selected;
    selected.feature_names = source.feature_names;
    selected.features.assign(source.features.size(), {});
    for (std::size_t row = 0; row < source.row_count(); ++row) {
        if (static_cast<std::size_t>(source.groups[row]) % shard_count != shard) {
            continue;
        }
        for (std::size_t feature = 0; feature < source.features.size(); ++feature) {
            selected.features[feature].push_back(source.features[feature][row]);
        }
        selected.labels.push_back(source.labels[row]);
        selected.groups.push_back(source.groups[row]);
        selected.entities.push_back(source.entities[row]);
    }
    return selected;
}

struct RankNode {
    RankNode(std::string id, std::uint16_t port, const std::vector<std::string>& seeds)
        : node(node_config()), cluster(std::move(id), cluster_config(port, seeds)),
          coordinator(cluster, 2) {
        node.start();
    }

    zetta::NodeConfig node_config() const {
        zetta::NodeConfig config;
        config.data_dir = directory.path().string();
        return config;
    }

    static zetta::cluster::ClusterConfig cluster_config(std::uint16_t port,
                                                        const std::vector<std::string>& seeds) {
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

bool wait_for_cluster(const std::vector<std::unique_ptr<RankNode>>& nodes) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        const bool ready = std::all_of(nodes.begin(), nodes.end(), [&](const auto& node) {
            return node->cluster.peer_count() == nodes.size() - 1;
        });
        if (ready) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

std::vector<zetta::ml::DistributedRankResult>
run_rank_cluster(std::vector<std::unique_ptr<RankNode>>& nodes,
                 const zetta::ml::RankDataset& dataset, const zetta::ml::RankParams& params) {
    std::vector<zetta::ml::DistributedRankResult> results(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        std::shared_ptr<zetta::compute::ComputeJob> job =
            nodes[index]->node.create_job(zetta::compute::JobKind::Train);
        threads.emplace_back([&, index, job] {
            const zetta::ml::RankDataset shard = groups_for_shard(dataset, index, nodes.size());
            results[index] =
                zetta::ml::run_lambda_mart_job(nodes[index]->coordinator, *job, shard, params,
                                               nullptr, {}, "rank-partition-test", 8s);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    return results;
}

} // namespace

TEST(RankGroupSplit, KeepsEveryGroupWhollyInOneSide) {
    const zetta::ml::RankDataset dataset = synthetic_ranking_data();
    const zetta::ml::RankGroupSplit split =
        zetta::ml::split_rank_groups(dataset.groups, 0.25, 1048);

    std::set<std::int64_t> training(split.training_groups.begin(), split.training_groups.end());
    std::set<std::int64_t> validation(split.validation_groups.begin(),
                                      split.validation_groups.end());
    EXPECT_FALSE(training.empty());
    EXPECT_FALSE(validation.empty());
    for (const std::int64_t group : training) {
        EXPECT_EQ(validation.count(group), 0u);
    }
    for (const std::size_t row : split.training_rows) {
        EXPECT_EQ(training.count(dataset.groups[row]), 1u);
    }
    for (const std::size_t row : split.validation_rows) {
        EXPECT_EQ(validation.count(dataset.groups[row]), 1u);
    }
}

TEST(LambdaMart, LearnsAUsefulDeterministicRanking) {
    const zetta::ml::RankDataset dataset = synthetic_ranking_data();
    const zetta::ml::RankGroupSplit split =
        zetta::ml::split_rank_groups(dataset.groups, 0.25, 1048);

    zetta::ml::RankDataset training;
    zetta::ml::RankDataset validation;
    training.feature_names = dataset.feature_names;
    validation.feature_names = dataset.feature_names;
    training.features.assign(dataset.features.size(), {});
    validation.features.assign(dataset.features.size(), {});
    const auto append = [&](zetta::ml::RankDataset& target, std::size_t row) {
        for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
            target.features[feature].push_back(dataset.features[feature][row]);
        }
        target.labels.push_back(dataset.labels[row]);
        target.groups.push_back(dataset.groups[row]);
        target.entities.push_back(dataset.entities[row]);
    };
    for (const std::size_t row : split.training_rows) {
        append(training, row);
    }
    for (const std::size_t row : split.validation_rows) {
        append(validation, row);
    }

    zetta::ml::RankParams params;
    params.gbdt.num_trees = 3;
    params.gbdt.max_depth = 1;
    params.gbdt.max_bins = 16;
    params.gbdt.learning_rate = 0.2;
    params.cutoff = 5;
    const zetta::ml::LambdaMartTrainResult first = zetta::ml::train_lambda_mart(training, params);
    const zetta::ml::LambdaMartTrainResult second = zetta::ml::train_lambda_mart(training, params);
    ASSERT_TRUE(first.ok()) << first.status.message;
    ASSERT_TRUE(second.ok()) << second.status.message;
    EXPECT_EQ(first.model.to_json(), second.model.to_json());

    std::vector<double> scores;
    std::vector<double> row(validation.feature_names.size(), 0.0);
    for (std::size_t index = 0; index < validation.row_count(); ++index) {
        for (std::size_t feature = 0; feature < validation.features.size(); ++feature) {
            row[feature] = validation.features[feature][index];
        }
        scores.push_back(first.model.predict_raw(row));
    }
    const zetta::ml::RankMetrics metrics =
        zetta::ml::evaluate_ranking(scores, validation.labels, validation.groups, params.cutoff);
    EXPECT_GT(metrics.ndcg_at_k, 0.8);
    EXPECT_GT(metrics.yield_at_k, 15.0);

    zetta::ml::GbdtParams pointwise_params = params.gbdt;
    pointwise_params.objective = "squared_error";
    const zetta::ml::GbdtTrainResult pointwise = zetta::ml::train_gbdt(
        training.features, training.feature_names, training.labels, pointwise_params);
    ASSERT_TRUE(pointwise.ok()) << pointwise.status.message;
    std::vector<double> pointwise_scores;
    for (std::size_t index = 0; index < validation.row_count(); ++index) {
        for (std::size_t feature = 0; feature < validation.features.size(); ++feature) {
            row[feature] = validation.features[feature][index];
        }
        pointwise_scores.push_back(pointwise.model.predict_raw(row));
    }
    const zetta::ml::RankMetrics pointwise_metrics = zetta::ml::evaluate_ranking(
        pointwise_scores, validation.labels, validation.groups, params.cutoff);
    EXPECT_GT(metrics.ndcg_at_k, pointwise_metrics.ndcg_at_k);
}

TEST(RankPartitions, DistributedModelMatchesSingleNode) {
    const zetta::ml::RankDataset dataset = synthetic_ranking_data();
    zetta::ml::RankParams params;
    params.gbdt.num_trees = 5;
    params.gbdt.max_depth = 2;
    params.gbdt.max_bins = 16;
    params.gbdt.learning_rate = 0.2;

    std::vector<std::unique_ptr<RankNode>> oracle;
    oracle.push_back(std::make_unique<RankNode>("rank-oracle", 48300, std::vector<std::string>{}));
    oracle[0]->cluster.start();
    oracle[0]->coordinator.start();
    const std::vector<zetta::ml::DistributedRankResult> expected =
        run_rank_cluster(oracle, dataset, params);
    ASSERT_TRUE(expected[0].ok());

    std::vector<std::unique_ptr<RankNode>> nodes;
    for (std::size_t index = 0; index < 3; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(48310 + earlier));
        }
        nodes.push_back(std::make_unique<RankNode>("rank-node-" + std::to_string(index),
                                                   static_cast<std::uint16_t>(48310 + index),
                                                   seeds));
    }
    for (auto& node : nodes) {
        node->cluster.start();
    }
    ASSERT_TRUE(wait_for_cluster(nodes));
    for (auto& node : nodes) {
        node->coordinator.start();
    }
    const std::vector<zetta::ml::DistributedRankResult> actual =
        run_rank_cluster(nodes, dataset, params);

    for (const zetta::ml::DistributedRankResult& result : actual) {
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result.model.tree_count(), expected[0].model.tree_count());
        std::vector<double> row(dataset.feature_names.size(), 0.0);
        for (std::size_t index = 0; index < dataset.row_count(); ++index) {
            for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
                row[feature] = dataset.features[feature][index];
            }
            EXPECT_NEAR(result.model.predict_raw(row), expected[0].model.predict_raw(row), 1e-12);
        }
        EXPECT_DOUBLE_EQ(result.validation.ndcg_at_k, expected[0].validation.ndcg_at_k);
        EXPECT_DOUBLE_EQ(result.validation.yield_at_k, expected[0].validation.yield_at_k);
        EXPECT_EQ(result.ranked.size(), dataset.row_count());
    }
}

TEST(NDCG, RewardsCorrectOrderWithinEachGroup) {
    const std::vector<double> labels = {0.0, 1.0, 2.0, 0.0, 2.0, 1.0};
    const std::vector<std::int64_t> groups = {7, 7, 7, 8, 8, 8};
    const zetta::ml::RankMetrics wrong =
        zetta::ml::evaluate_ranking({3.0, 2.0, 1.0, 3.0, 1.0, 2.0}, labels, groups, 2);
    const zetta::ml::RankMetrics right =
        zetta::ml::evaluate_ranking({1.0, 2.0, 3.0, 1.0, 3.0, 2.0}, labels, groups, 2);
    EXPECT_GT(right.ndcg_at_k, wrong.ndcg_at_k);
    EXPECT_DOUBLE_EQ(right.ndcg_at_k, 1.0);
    EXPECT_DOUBLE_EQ(right.yield_at_k, 3.0);
}
