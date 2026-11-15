// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
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
#include "zetta/ml/label_efficiency.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobCoordinator;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::ml::LabelEfficiencyParams;
using zetta::ml::LabelEfficiencyResult;
using zetta::ml::LabelEfficiencyRow;
using zetta::ml::LabellingFunction;
using zetta::ml::RuleOperator;
using zetta::ml::UncertaintyMeasure;
using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) {
            return true;
        }
        sleep_ms(25);
    }
    return false;
}

struct ComputeNode {
    ComputeNode(std::string id, std::uint16_t port,
                const std::vector<std::string>& seeds)
        : node(NodeConfig{temp_dir.path().string(), false}),
          cluster(std::move(id), [&] {
              ClusterConfig config;
              config.listen_host = "127.0.0.1";
              config.listen_port = port;
              config.port_range = 1;
              config.seeds = seeds;
              return config;
          }()),
          coordinator(cluster, 2) {
        node.start();
    }

    TempDir temp_dir;
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

std::vector<std::unique_ptr<ComputeNode>> build_cluster(std::size_t count,
                                                        std::uint16_t port) {
    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" +
                            std::to_string(port + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            "label-node-" + std::to_string(port) + "-" +
                std::to_string(index),
            static_cast<std::uint16_t>(port + index), seeds));
    }
    for (const auto& node : nodes) {
        node->cluster.start();
    }
    EXPECT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != count - 1) {
                return false;
            }
        }
        return true;
    }));
    for (const auto& node : nodes) {
        node->coordinator.start();
    }
    return nodes;
}

std::vector<LabelEfficiencyResult> run_distributed(
    std::vector<std::unique_ptr<ComputeNode>>& nodes,
    const std::vector<std::vector<LabelEfficiencyRow>>& shards,
    const std::vector<std::string>& feature_columns,
    const LabelEfficiencyParams& params, const std::string& tag) {
    std::vector<LabelEfficiencyResult> results(nodes.size());
    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes.size());
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Train);
        threads.emplace_back([&, index] {
            results[index] = zetta::ml::run_label_efficiency_job(
                nodes[index]->coordinator, *jobs[index], shards[index],
                feature_columns, params, tag, 8s);
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        EXPECT_EQ(jobs[index]->status(), JobStatus::Succeeded)
            << jobs[index]->detail();
    }
    return results;
}

std::uint64_t test_hash(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

} // namespace

TEST(ActiveLearning, UncertaintyWithDiversityBeatsRandomAtTheSameBudget) {
    constexpr std::size_t kRows = 360;
    std::vector<std::vector<LabelEfficiencyRow>> shards(2);
    for (std::size_t index = 0; index < kRows; ++index) {
        const double feature =
            -1.0 + 2.0 * static_cast<double>(index) /
                       static_cast<double>(kRows - 1);
        const int label = feature >= 0.23 ? 1 : 0;
        shards[index % shards.size()].push_back(
            {static_cast<std::int64_t>(index), {feature}, label});
    }

    LabelEfficiencyParams params;
    params.queue_size = 10;
    params.initial_labels = 2;
    params.gold_fraction = 0.25;
    params.seed = 37;
    params.uncertainty = UncertaintyMeasure::Entropy;
    params.diversity_weight = 0.1;
    params.logistic_iterations = 12;
    params.logistic_learning_rate = 0.3;
    params.label_model_iterations = 12;
    params.labelling_functions = {
        LabellingFunction{"rule-near-boundary", "signal",
                           RuleOperator::GreaterEqual, 0.15, 1, 0},
    };

    std::vector<std::unique_ptr<ComputeNode>> nodes =
        build_cluster(2, 47580);
    const std::vector<LabelEfficiencyResult> results = run_distributed(
        nodes, shards, {"signal"}, params, "active-planted");
    ASSERT_EQ(results.size(), 2u);
    for (const LabelEfficiencyResult& result : results) {
        ASSERT_TRUE(result.ok()) << result.error;
        EXPECT_EQ(result.label_queue.size(), params.queue_size);
        EXPECT_GE(result.metrics.active_accuracy_at_budget, 0.9);
        EXPECT_GT(result.metrics.active_accuracy_at_budget,
                  result.metrics.random_accuracy_at_budget);
        EXPECT_GT(result.metrics.labels_saved, 0u);
        EXPECT_EQ(result.metrics.labelling_budget,
                  params.initial_labels + params.queue_size);
    }
    EXPECT_EQ(results[0].label_queue, results[1].label_queue);
}

TEST(LabelModel, RecoversLabellingFunctionAccuraciesFromSyntheticVotes) {
    constexpr std::size_t kRows = 800;
    constexpr std::int64_t kEntityBase = std::int64_t{1} << 60;
    const std::vector<double> planted = {0.9, 0.8, 0.7};
    std::vector<LabelEfficiencyRow> rows;
    rows.reserve(kRows);
    for (std::size_t index = 0; index < kRows; ++index) {
        const int label = static_cast<int>(test_hash(index) & 1ull);
        std::vector<double> features;
        for (std::size_t function = 0; function < planted.size(); ++function) {
            const std::uint64_t sample =
                test_hash(index ^ test_hash(100 + function));
            const double unit = static_cast<double>(sample >> 11) /
                                static_cast<double>(1ull << 53);
            const bool correct = unit < planted[function];
            features.push_back(correct ? label : 1 - label);
        }
        rows.push_back(
            {kEntityBase + static_cast<std::int64_t>(index),
             std::move(features), label});
    }

    LabelEfficiencyParams params;
    params.queue_size = 3;
    params.initial_labels = 2;
    params.gold_fraction = 0.25;
    params.seed = 91;
    params.logistic_iterations = 4;
    params.label_model_iterations = 60;
    params.labelling_functions = {
        LabellingFunction{"strong", "strong_vote",
                           RuleOperator::GreaterEqual, 0.5, 1, 0},
        LabellingFunction{"medium", "medium_vote",
                           RuleOperator::GreaterEqual, 0.5, 1, 0},
        LabellingFunction{"weak", "weak_vote",
                           RuleOperator::GreaterEqual, 0.5, 1, 0},
    };

    std::vector<std::unique_ptr<ComputeNode>> nodes =
        build_cluster(1, 47590);
    const std::vector<LabelEfficiencyResult> results = run_distributed(
        nodes, {rows}, {"strong_vote", "medium_vote", "weak_vote"},
        params, "label-model-planted");
    ASSERT_EQ(results.size(), 1u);
    const LabelEfficiencyResult& result = results.front();
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.labelling_function_accuracies.size(), planted.size());
    for (std::size_t index = 0; index < planted.size(); ++index) {
        EXPECT_NEAR(result.labelling_function_accuracies[index].estimated,
                    planted[index], 0.12)
            << result.labelling_function_accuracies[index].name;
    }
    EXPECT_GE(result.metrics.label_model_gold_accuracy, 0.85);
    EXPECT_EQ(result.local_weak_labels.size(),
              result.metrics.unlabelled_rows);
    for (const zetta::ml::WeakLabel& weak : result.local_weak_labels) {
        EXPECT_GE(weak.entity, kEntityBase);
        EXPECT_LT(weak.entity,
                  kEntityBase + static_cast<std::int64_t>(kRows));
        EXPECT_FALSE(zetta::ml::is_label_efficiency_gold(weak.entity, params));
        EXPECT_GE(weak.probability, 0.0);
        EXPECT_LE(weak.probability, 1.0);
        EXPECT_EQ(weak.contributing_functions.size(), planted.size());
    }
    for (const zetta::ml::LabelQueueEntry& queued : result.label_queue) {
        EXPECT_FALSE(
            zetta::ml::is_label_efficiency_gold(queued.entity, params));
    }
}
