#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_explain_job.hpp"
#include "zetta/ml/tree_shap.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// Exact TreeSHAP explanation -> per-entity drivers (#393): the unit surface (the
// efficiency invariant base + Σφ = predict_raw, a hand-checked known value, the
// per-entity dominant driver on a separable signal) plus the distributed `explain`
// ComputeJob (per-entity SHAP over the churn fixture, a global driver ranking, and
// single-node == multi-node equivalence).
namespace {

namespace fs = std::filesystem;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::compute::CpuDevice;
using zetta::compute::DataType;
using zetta::compute::JobCoordinator;
using zetta::ddf::ColumnSchema;
using zetta::ddf::DataFrame;
using zetta::ddf::DataFrameSchema;
using zetta::ddf::FeatureTable;
using zetta::ddf::FeaturizeSpec;
using zetta::ddf::load_data_frame;
using zetta::ddf::point_in_time_featurize;
using zetta::ddf::WindowedFeature;
using zetta::ml::dominant_driver;
using zetta::ml::GbdtModel;
using zetta::ml::GbdtParams;
using zetta::ml::GbdtTrainResult;
using zetta::ml::model_base_value;
using zetta::ml::RegressionTree;
using zetta::ml::run_explain_job;
using zetta::ml::ShapDriver;
using zetta::ml::ShapExplanation;
using zetta::ml::train_gbdt;
using zetta::ml::tree_shap;
using zetta::ml::TreeNode;
using zetta::test::TempDir;
using Json = nlohmann::json;

// =====================================================================
// A hand-built single tree with known cover -> a closed-form SHAP value
// =====================================================================

// root (cover 4) splits feature 0 at 0.5: left leaf -1 (cover 3), right leaf +2
// (cover 1). E[f] = 3/4*(-1) + 1/4*(+2) = -0.25. With one feature, that feature gets
// the whole gap, so φ_0(x) = f(x) - E[f] exactly.
GbdtModel one_split_model() {
    GbdtModel model;
    model.base_score = 0.0;
    model.learning_rate = 1.0;
    model.feature_names = {"f0", "f1"};
    RegressionTree tree;
    tree.nodes = {
        TreeNode{.feature = 0, .threshold = 0.5, .left = 1, .right = 2,
                 .value = 0.0, .cover = 4.0},
        TreeNode{.feature = -1, .value = -1.0, .cover = 3.0},
        TreeNode{.feature = -1, .value = 2.0, .cover = 1.0},
    };
    model.trees = {std::move(tree)};
    return model;
}

TEST(TreeShapKnownValue, OneSplitClosedForm) {
    const GbdtModel model = one_split_model();
    EXPECT_DOUBLE_EQ(model_base_value(model), -0.25);

    // x takes the right leaf (value +2): φ_0 = 2 - (-0.25) = 2.25, φ_1 = 0.
    const ShapExplanation high = tree_shap(model, {0.8, 0.0});
    ASSERT_EQ(high.values.size(), 2u);
    EXPECT_NEAR(high.values[0], 2.25, 1e-12);
    EXPECT_NEAR(high.values[1], 0.0, 1e-12);
    EXPECT_NEAR(high.predicted, 2.0, 1e-12);
    EXPECT_NEAR(high.efficiency_residual(), 0.0, 1e-12);

    // x takes the left leaf (value -1): φ_0 = -1 - (-0.25) = -0.75.
    const ShapExplanation low = tree_shap(model, {0.2, 9.0});
    EXPECT_NEAR(low.values[0], -0.75, 1e-12);
    EXPECT_NEAR(low.values[1], 0.0, 1e-12);
    EXPECT_NEAR(low.efficiency_residual(), 0.0, 1e-12);

    // The dominant driver is f0, pushing up for the high case, down for the low.
    const ShapDriver up = dominant_driver(model, high);
    EXPECT_EQ(up.feature, "f0");
    EXPECT_GT(up.value, 0.0);
    const ShapDriver down = dominant_driver(model, low);
    EXPECT_EQ(down.feature, "f0");
    EXPECT_LT(down.value, 0.0);
}

// A degenerate constant model (no features): SHAP is empty, base == prediction.
TEST(TreeShapKnownValue, NoFeatures) {
    GbdtModel model;
    model.base_score = -0.7;
    const ShapExplanation shap = tree_shap(model, {});
    EXPECT_TRUE(shap.values.empty());
    EXPECT_DOUBLE_EQ(shap.base_value, -0.7);
    EXPECT_DOUBLE_EQ(shap.predicted, -0.7);
    EXPECT_NEAR(shap.efficiency_residual(), 0.0, 1e-12);
    EXPECT_EQ(dominant_driver(model, shap).feature_index, -1);
}

// =====================================================================
// A trained model on a separable signal: efficiency + dominant driver
// =====================================================================

struct Synthetic {
    std::vector<std::vector<double>> columns;  // [feature][row]
    std::vector<std::string> names{"f0", "f1", "f2"};
    std::vector<double> labels;
};

Synthetic make_synthetic(std::size_t rows) {
    Synthetic data;
    data.columns.assign(3, {});
    for (std::size_t i = 0; i < rows; ++i) {
        const bool positive = i >= rows / 2;
        const double f0 = positive ? 0.70 + static_cast<double>(i % 5) * 0.02
                                   : 0.10 + static_cast<double>(i % 5) * 0.02;
        data.columns[0].push_back(f0);
        data.columns[1].push_back(static_cast<double>(i % 7) / 7.0);
        data.columns[2].push_back(static_cast<double>((i * 3) % 5) / 5.0);
        data.labels.push_back(positive ? 1.0 : 0.0);
    }
    return data;
}

TEST(TreeShapTrained, EfficiencyAndDominantDriver) {
    const Synthetic data = make_synthetic(60);
    GbdtParams params;
    params.num_trees = 20;
    params.max_depth = 3;
    const GbdtTrainResult trained =
        train_gbdt(data.columns, data.names, data.labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;

    const double base = model_base_value(trained.model);
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        const std::vector<double> x{data.columns[0][r], data.columns[1][r],
                                    data.columns[2][r]};
        const ShapExplanation shap = tree_shap(trained.model, x);
        // Efficiency: the attributions reconstruct the margin exactly.
        EXPECT_NEAR(shap.base_value, base, 1e-12);
        EXPECT_NEAR(shap.efficiency_residual(), 0.0, 1e-7) << "row " << r;
        EXPECT_NEAR(shap.predicted, trained.model.predict_raw(x), 1e-12);
        // Feature 0 carries the signal -> it is the dominant driver for every row,
        // pushing up for positives and down for negatives.
        const ShapDriver driver = dominant_driver(trained.model, shap);
        EXPECT_EQ(driver.feature, "f0") << "row " << r;
        if (data.labels[r] >= 0.5) {
            EXPECT_GT(driver.value, 0.0) << "row " << r;
        } else {
            EXPECT_LT(driver.value, 0.0) << "row " << r;
        }
    }
}

// =====================================================================
// The distributed `explain` ComputeJob — the loopback cluster harness
// =====================================================================

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}
template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

struct ComputeNode {
    ComputeNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds, unsigned workers)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
    }
    static NodeConfig node_config(const TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }
    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }
    TempDir temp_dir;  // declared first so it outlives `node`
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

zetta::storage::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::storage::JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index) {
        store.add("row-" + std::to_string(index), rows[index]);
    }
    return store;
}

std::vector<Json> slice(const std::vector<Json>& rows, std::size_t begin,
                        std::size_t end) {
    return std::vector<Json>(rows.begin() + std::min(begin, rows.size()),
                             rows.begin() + std::min(end, rows.size()));
}

// The feature frame schema for explanation: the entity key (Int64) + each feature
// column (Float64). No label is needed — explanation is over X only.
DataFrameSchema explain_schema(const std::vector<std::string>& features) {
    DataFrameSchema schema;
    schema.columns.push_back(
        ColumnSchema{.name = "entity", .dtype = DataType::Int64});
    for (const std::string& name : features) {
        schema.columns.push_back(
            ColumnSchema{.name = name, .dtype = DataType::Float64});
    }
    return schema;
}

// A FeatureTable's rows as {entity, feature columns…} docs for the explain frame.
std::vector<Json> explain_rows(const FeatureTable& table) {
    std::vector<Json> rows;
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        Json doc;
        doc["entity"] = table.entities[r];
        for (std::size_t c = 0; c < table.feature_count(); ++c) {
            doc[table.feature_names[c]] = table.features[c][r];
        }
        rows.push_back(std::move(doc));
    }
    return rows;
}

// Run an `explain` ComputeJob across `node_count` loopback nodes, each owning a
// contiguous shard of `rows`. Collects every node's per-entity explanations into one
// map (the global batch) and returns it alongside node 0's (root's) result.
struct ExplainRun {
    zetta::ml::DistributedExplainResult root;
    std::map<std::int64_t, std::vector<double>> by_entity;  // entity -> φ
    std::map<std::int64_t, std::string> dominant;           // entity -> driver name
};

ExplainRun run_distributed_explain(const GbdtModel& model,
                                   const std::vector<Json>& rows,
                                   const std::vector<std::string>& features,
                                   std::size_t node_count,
                                   std::uint16_t base_port) {
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < node_count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(base_port + index), seeds, 2));
    }
    for (auto& node : nodes) node->cluster.start();
    EXPECT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != node_count - 1) return false;
        }
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    std::vector<std::unique_ptr<CpuDevice>> devices;
    std::vector<DataFrame> frames;
    const std::size_t per = (rows.size() + node_count - 1) / node_count;
    for (std::size_t index = 0; index < node_count; ++index) {
        devices.push_back(std::make_unique<CpuDevice>());
        auto frame =
            load_data_frame(make_store(slice(rows, index * per, (index + 1) * per)),
                            explain_schema(features), *devices[index]);
        EXPECT_TRUE(frame.ok()) << frame.status.message;
        frames.push_back(std::move(frame.frame));
    }

    std::vector<zetta::ml::DistributedExplainResult> results(node_count);
    std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < node_count; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Explain);
        threads.emplace_back([&, index] {
            results[index] = run_explain_job(nodes[index]->coordinator,
                                             *jobs[index], model, frames[index],
                                             "entity");
        });
    }
    for (auto& thread : threads) thread.join();

    ExplainRun out;
    for (std::size_t index = 0; index < node_count; ++index) {
        EXPECT_EQ(results[index].status, JobStatus::Succeeded)
            << "node " << index << ": " << jobs[index]->detail();
        EXPECT_EQ(jobs[index]->kind(), JobKind::Explain) << "node " << index;
        // The global mean-|φ| ranking is identical on every node (allreduced).
        EXPECT_EQ(results[index].mean_abs_shap.size(),
                  results[0].mean_abs_shap.size());
        for (std::size_t f = 0; f < results[0].mean_abs_shap.size(); ++f) {
            EXPECT_NEAR(results[index].mean_abs_shap[f],
                        results[0].mean_abs_shap[f], 1e-9)
                << "node " << index << " feature " << f;
        }
        EXPECT_NEAR(results[index].base_value, results[0].base_value, 1e-12);
        for (const auto& entry : results[index].explanations) {
            out.by_entity[entry.entity] = entry.values;
            out.dominant[entry.entity] = entry.dominant_feature;
        }
    }
    EXPECT_TRUE(results[0].is_root);
    out.root = std::move(results[0]);

    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
    return out;
}

// =====================================================================
// The real churn fixture: featurize -> train -> explain -> drivers
// =====================================================================
// The numeric encoding + the standard featurize spec are the canonical
// zetta::churn_solution.hpp surface (encode_churn_dataset, churn_events_schema /
// churn_accounts_schema, churn_featurize_spec).

FeatureTable churn_feature_table() {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::usecases::churn::load_churn_dataset(churn_dir);
    EXPECT_TRUE(loaded.ok()) << loaded.error;
    const zetta::usecases::churn::ChurnNumericShards numeric =
        zetta::usecases::churn::encode_churn_dataset(loaded.dataset);
    CpuDevice device;
    auto events = load_data_frame(make_store(numeric.events),
                                  zetta::usecases::churn::churn_events_schema(), device);
    auto accounts = load_data_frame(make_store(numeric.accounts),
                                    zetta::usecases::churn::churn_accounts_schema(), device);
    EXPECT_TRUE(events.ok());
    EXPECT_TRUE(accounts.ok());
    auto result = point_in_time_featurize(events.frame, accounts.frame,
                                          zetta::usecases::churn::churn_featurize_spec(numeric.as_of));
    EXPECT_TRUE(result.ok()) << result.status.message;
    return std::move(result.table);
}

GbdtModel train_churn_model(const FeatureTable& table) {
    GbdtParams params;
    params.num_trees = 30;
    params.max_depth = 3;
    params.max_bins = 16;
    params.learning_rate = 0.3;
    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, params);
    EXPECT_TRUE(trained.ok()) << trained.status.message;
    return trained.model;
}

TEST(TreeShapChurn, PerEntityDriversAndEfficiency) {
    const FeatureTable table = churn_feature_table();
    ASSERT_GT(table.row_count(), 0u);
    const GbdtModel model = train_churn_model(table);
    const std::vector<Json> rows = explain_rows(table);

    const ExplainRun run =
        run_distributed_explain(model, rows, table.feature_names, 1, 23100);
    ASSERT_TRUE(run.root.ok());
    EXPECT_EQ(run.root.global_entity_count, table.row_count());

    // Every entity got an explanation that reconstructs its margin exactly (the
    // headline SHAP property) and a non-empty dominant driver.
    const double base = model_base_value(model);
    for (const auto& entry : run.root.explanations) {
        double sum = base;
        for (const double value : entry.values) sum += value;
        EXPECT_NEAR(sum, entry.predicted, 1e-7) << "entity " << entry.entity;
        EXPECT_FALSE(entry.dominant_feature.empty()) << "entity " << entry.entity;
    }

    // failed_payments is a real churn driver at the population level: a strictly
    // positive mean-|φ|, and (being the strongest signal in the fixture) the top
    // global driver.
    const auto failed = std::find(table.feature_names.begin(),
                                  table.feature_names.end(), "failed_payments");
    ASSERT_NE(failed, table.feature_names.end());
    const std::size_t failed_index =
        static_cast<std::size_t>(failed - table.feature_names.begin());
    EXPECT_GT(run.root.mean_abs_shap[failed_index], 0.0);
    EXPECT_EQ(run.root.top_driver, "failed_payments");

    // The account with the most failed payments is pushed toward churn by them:
    // failed_payments is its dominant driver, with a positive attribution.
    const std::vector<double>* failures = table.feature("failed_payments");
    ASSERT_NE(failures, nullptr);
    const std::size_t worst = static_cast<std::size_t>(
        std::max_element(failures->begin(), failures->end()) - failures->begin());
    ASSERT_GT((*failures)[worst], 0.0);
    const std::int64_t worst_entity = table.entities[worst];
    const ShapExplanation worst_shap = tree_shap(model, [&] {
        std::vector<double> row(table.feature_count());
        for (std::size_t f = 0; f < table.feature_count(); ++f) {
            row[f] = table.features[f][worst];
        }
        return row;
    }());
    EXPECT_GT(worst_shap.values[failed_index], 0.0);
    EXPECT_EQ(run.dominant.at(worst_entity), "failed_payments");
}

TEST(TreeShapChurn, SingleNodeEqualsMultiNode) {
    const FeatureTable table = churn_feature_table();
    const GbdtModel model = train_churn_model(table);
    const std::vector<Json> rows = explain_rows(table);

    const ExplainRun single =
        run_distributed_explain(model, rows, table.feature_names, 1, 23200);
    const ExplainRun multi =
        run_distributed_explain(model, rows, table.feature_names, 2, 23210);
    ASSERT_TRUE(single.root.ok());
    ASSERT_TRUE(multi.root.ok());

    // The global batch is identical regardless of how entities were sharded: same
    // entity universe, same per-entity SHAP values, same dominant drivers.
    EXPECT_EQ(single.by_entity.size(), table.row_count());
    EXPECT_EQ(multi.by_entity.size(), table.row_count());
    for (const auto& [entity, values] : single.by_entity) {
        const auto found = multi.by_entity.find(entity);
        ASSERT_NE(found, multi.by_entity.end()) << "entity " << entity;
        ASSERT_EQ(values.size(), found->second.size());
        for (std::size_t f = 0; f < values.size(); ++f) {
            EXPECT_NEAR(values[f], found->second[f], 1e-9)
                << "entity " << entity << " feature " << f;
        }
        EXPECT_EQ(single.dominant.at(entity), multi.dominant.at(entity))
            << "entity " << entity;
    }
    // And the population driver ranking agrees.
    ASSERT_EQ(single.root.mean_abs_shap.size(), multi.root.mean_abs_shap.size());
    for (std::size_t f = 0; f < single.root.mean_abs_shap.size(); ++f) {
        EXPECT_NEAR(single.root.mean_abs_shap[f], multi.root.mean_abs_shap[f],
                    1e-9);
    }
    EXPECT_EQ(single.root.top_driver, multi.root.top_driver);
}

}  // namespace
