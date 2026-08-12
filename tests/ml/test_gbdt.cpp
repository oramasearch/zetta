#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
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
#include "zetta/compute/checkpoint.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_train_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// Distributed histogram GBDT training (binary log-loss) -> Model (#390): the unit
// surface (the model + params JSON boundaries, AUC/log-loss metrics, the
// single-node trainer learning a separable signal) plus the distributed `train`
// ComputeJob (single-node == multi-node equivalent trees, and the churn fixture
// trained to a target AUC, producing a Model).
namespace {

namespace fs = std::filesystem;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::ml::Model;
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
using zetta::ml::evaluate_binary;
using zetta::ml::GbdtMetrics;
using zetta::ml::GbdtModel;
using zetta::ml::GbdtParams;
using zetta::ml::GbdtTrainResult;
using zetta::ml::RegressionTree;
using zetta::ml::run_train_job;
using zetta::ml::train_gbdt;
using zetta::ml::TreeNode;
using zetta::test::TempDir;
using Json = nlohmann::json;

// =====================================================================
// JSON boundaries
// =====================================================================

TEST(GbdtParamsJson, RoundTrips) {
    GbdtParams params;
    params.num_trees = 40;
    params.max_depth = 3;
    params.max_bins = 16;
    params.learning_rate = 0.25;
    params.l2_regularization = 2.0;
    params.min_split_gain = 0.1;
    params.min_child_hessian = 0.5;
    const auto parsed = GbdtParams::from_json(params.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->num_trees, 40);
    EXPECT_EQ(parsed->max_depth, 3);
    EXPECT_EQ(parsed->max_bins, 16);
    EXPECT_DOUBLE_EQ(parsed->learning_rate, 0.25);
    EXPECT_DOUBLE_EQ(parsed->l2_regularization, 2.0);
    EXPECT_DOUBLE_EQ(parsed->min_split_gain, 0.1);
    EXPECT_DOUBLE_EQ(parsed->min_child_hessian, 0.5);
}

TEST(GbdtParamsJson, OmittedFieldsKeepDefaults) {
    const auto parsed = GbdtParams::from_json(Json::object());
    ASSERT_TRUE(parsed.has_value());
    const GbdtParams defaults;
    EXPECT_EQ(parsed->num_trees, defaults.num_trees);
    EXPECT_EQ(parsed->max_bins, defaults.max_bins);
}

TEST(GbdtParamsJson, RejectsBadInput) {
    EXPECT_FALSE(GbdtParams::from_json(Json::array()).has_value());     // not an object
    EXPECT_FALSE(GbdtParams::from_json({{"num_trees", 0}}).has_value());   // < 1
    EXPECT_FALSE(GbdtParams::from_json({{"max_bins", 1}}).has_value());    // < 2
    EXPECT_FALSE(GbdtParams::from_json({{"learning_rate", 0.0}}).has_value());  // not > 0
    EXPECT_FALSE(GbdtParams::from_json({{"l2_regularization", -1.0}}).has_value());
    EXPECT_FALSE(GbdtParams::from_json({{"num_trees", 2.5}}).has_value());  // not integer
    EXPECT_FALSE(GbdtParams::from_json({{"surprise", 1}}).has_value());     // unknown key
}

GbdtModel sample_model() {
    GbdtModel model;
    model.base_score = -0.4;
    model.learning_rate = 0.3;
    model.feature_names = {"a", "b"};
    RegressionTree tree;
    tree.nodes = {TreeNode{.feature = 0, .threshold = 1.5, .left = 1, .right = 2},
                  TreeNode{.feature = -1, .value = -0.2},
                  TreeNode{.feature = -1, .value = 0.7}};
    model.trees = {std::move(tree)};
    return model;
}

TEST(GbdtModelJson, RoundTrips) {
    const GbdtModel model = sample_model();
    std::string error;
    const auto parsed = GbdtModel::from_json(model.to_json(), &error);
    ASSERT_TRUE(parsed.has_value()) << error;
    EXPECT_EQ(parsed->objective, "binary_logistic");
    EXPECT_DOUBLE_EQ(parsed->base_score, -0.4);
    EXPECT_EQ(parsed->feature_names, (std::vector<std::string>{"a", "b"}));
    ASSERT_EQ(parsed->trees.size(), 1u);
    ASSERT_EQ(parsed->trees[0].nodes.size(), 3u);
    EXPECT_EQ(parsed->trees[0].nodes[0].feature, 0);
    EXPECT_DOUBLE_EQ(parsed->trees[0].nodes[2].value, 0.7);
    // Prediction survives the round-trip.
    const std::vector<double> x{2.0, 0.0};
    EXPECT_DOUBLE_EQ(parsed->predict_raw(x), model.predict_raw(x));
}

TEST(GbdtModelJson, RejectsMalformed) {
    const Json good = sample_model().to_json();
    ASSERT_TRUE(GbdtModel::from_json(good).has_value());

    {  // unknown objective
        Json bad = good;
        bad["objective"] = "multiclass";
        EXPECT_FALSE(GbdtModel::from_json(bad).has_value());
    }
    {  // unknown key
        Json bad = good;
        bad["surprise"] = 1;
        EXPECT_FALSE(GbdtModel::from_json(bad).has_value());
    }
    {  // child index out of range (structural rule the schema can't express)
        Json bad = good;
        bad["trees"][0]["nodes"][0]["left"] = 99;
        EXPECT_FALSE(GbdtModel::from_json(bad).has_value());
    }
    {  // missing required node field
        Json bad = good;
        bad["trees"][0]["nodes"][0].erase("threshold");
        EXPECT_FALSE(GbdtModel::from_json(bad).has_value());
    }
}

// =====================================================================
// Metrics
// =====================================================================

TEST(GbdtMetrics, AucSeparation) {
    // Perfectly separated probabilities -> AUC 1.0.
    GbdtMetrics perfect = evaluate_binary({0.1, 0.2, 0.8, 0.9}, {0, 0, 1, 1});
    EXPECT_DOUBLE_EQ(perfect.auc, 1.0);
    EXPECT_DOUBLE_EQ(perfect.accuracy, 1.0);
    // Reversed -> AUC 0.0.
    GbdtMetrics reversed = evaluate_binary({0.9, 0.8, 0.2, 0.1}, {0, 0, 1, 1});
    EXPECT_DOUBLE_EQ(reversed.auc, 0.0);
    // A single class present -> AUC defined as 0.5.
    GbdtMetrics one_class = evaluate_binary({0.3, 0.6}, {0, 0});
    EXPECT_DOUBLE_EQ(one_class.auc, 0.5);
    // Ties at the boundary average to 0.5.
    GbdtMetrics tied = evaluate_binary({0.5, 0.5}, {0, 1});
    EXPECT_DOUBLE_EQ(tied.auc, 0.5);
}

// =====================================================================
// Single-node training learns a separable signal
// =====================================================================

// A clean dataset: feature 0 separates the classes (a gap between the two
// clusters), features 1/2 are deterministic noise.
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

TEST(GbdtTrain, LearnsSeparablePattern) {
    const Synthetic data = make_synthetic(60);
    GbdtParams params;
    params.num_trees = 20;
    params.max_depth = 3;
    const GbdtTrainResult trained =
        train_gbdt(data.columns, data.names, data.labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    EXPECT_EQ(trained.model.tree_count(), 20u);

    // The model orders the classes correctly -> training AUC 1.0.
    std::vector<double> probabilities(data.labels.size());
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        std::vector<double> x{data.columns[0][r], data.columns[1][r],
                              data.columns[2][r]};
        probabilities[r] = trained.model.predict_proba(x);
    }
    const GbdtMetrics metrics = evaluate_binary(probabilities, data.labels);
    EXPECT_DOUBLE_EQ(metrics.auc, 1.0);
    EXPECT_GT(metrics.accuracy, 0.99);

    // Feature 0 carries the signal -> it is the dominant driver.
    ASSERT_EQ(trained.feature_importances.size(), 3u);
    EXPECT_GT(trained.feature_importances[0], trained.feature_importances[1]);
    EXPECT_GT(trained.feature_importances[0], trained.feature_importances[2]);
}

TEST(GbdtTrain, RejectsSingleClass) {
    const Synthetic data = make_synthetic(20);
    std::vector<double> all_zero(data.labels.size(), 0.0);
    GbdtParams params;
    const GbdtTrainResult trained =
        train_gbdt(data.columns, data.names, all_zero, params);
    EXPECT_FALSE(trained.ok());
}

// =====================================================================
// Distributed `train` ComputeJob — the loopback cluster harness
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

// A training schema: the feature columns as Float64, the label as Float64.
DataFrameSchema training_schema(const std::vector<std::string>& features,
                                const std::string& label) {
    DataFrameSchema schema;
    for (const std::string& name : features) {
        schema.columns.push_back(
            ColumnSchema{.name = name, .dtype = DataType::Float64});
    }
    schema.columns.push_back(
        ColumnSchema{.name = label, .dtype = DataType::Float64});
    return schema;
}

// Rows of a synthetic dataset as JSON docs (feature columns + label).
std::vector<Json> synthetic_rows(const Synthetic& data, const std::string& label) {
    std::vector<Json> rows;
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        Json doc;
        for (std::size_t f = 0; f < data.names.size(); ++f) {
            doc[data.names[f]] = data.columns[f][r];
        }
        doc[label] = data.labels[r];
        rows.push_back(std::move(doc));
    }
    return rows;
}

// Run a `train` ComputeJob across `node_count` loopback nodes, each owning a
// contiguous shard of `rows`. Returns node 0's (root's) result.
zetta::ml::DistributedTrainResult run_distributed_train(
    const std::vector<Json>& rows, const std::vector<std::string>& features,
    const std::string& label, const GbdtParams& params, std::size_t node_count,
    std::uint16_t base_port, bool register_model,
    zetta::ml::CalibrationConfig calibration = {}) {
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
        auto frame = load_data_frame(
            make_store(slice(rows, index * per, (index + 1) * per)),
            training_schema(features, label), *devices[index]);
        EXPECT_TRUE(frame.ok()) << frame.status.message;
        frames.push_back(std::move(frame.frame));
    }

    std::vector<zetta::ml::DistributedTrainResult> results(node_count);
    std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < node_count; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Train);
        threads.emplace_back([&, index] {
            results[index] = run_train_job(
                nodes[index]->coordinator, *jobs[index], frames[index], features,
                label, params, register_model ? &nodes[index]->node : nullptr, {},
                {}, calibration);
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t index = 0; index < node_count; ++index) {
        EXPECT_EQ(results[index].status, JobStatus::Succeeded)
            << "node " << index << ": " << jobs[index]->detail();
        EXPECT_EQ(jobs[index]->kind(), JobKind::Train) << "node " << index;
        // The model is identical on every node (trees grown off global histograms).
        EXPECT_EQ(results[index].model.to_json(), results[0].model.to_json())
            << "node " << index;
        // And the global metrics agree on every node (broadcast).
        EXPECT_NEAR(results[index].metrics.auc, results[0].metrics.auc, 1e-12)
            << "node " << index;
    }
    EXPECT_TRUE(results[0].is_root);

    auto out = std::move(results[0]);
    // Keep the root node alive long enough to read its registered model.
    if (register_model) {
        EXPECT_FALSE(out.model_id.empty());
        std::shared_ptr<Model> model = nodes[0]->node.find_model(out.model_id);
        EXPECT_TRUE(model) << "root should have registered the model";
    }
    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
    return out;
}

TEST(GbdtDistributed, SingleNodeMatchesMultiNode) {
    const Synthetic data = make_synthetic(60);
    const std::vector<Json> rows = synthetic_rows(data, "y");
    GbdtParams params;
    params.num_trees = 8;
    params.max_depth = 3;

    const auto single =
        run_distributed_train(rows, data.names, "y", params, 1, 22100, false);
    const auto multi =
        run_distributed_train(rows, data.names, "y", params, 3, 22110, false);

    ASSERT_TRUE(single.ok());
    ASSERT_TRUE(multi.ok());
    // Equivalent trees: the same ensemble shape and the same dominant split.
    // (Across a different sharding the per-bin histogram sums differ only in the
    // last FP bits — the standard GBDT "equivalent up to floating-point" guarantee
    // — so structure is compared by shape + predictions, not bit-for-bit.)
    ASSERT_EQ(single.model.tree_count(), multi.model.tree_count());
    EXPECT_NEAR(single.model.base_score, multi.model.base_score, 1e-9);
    for (std::size_t t = 0; t < single.model.tree_count(); ++t) {
        EXPECT_EQ(single.model.trees[t].nodes.size(),
                  multi.model.trees[t].nodes.size())
            << "tree " << t;
    }
    // Feature 0 is the unambiguous root split in every tree, on either sharding.
    EXPECT_EQ(single.model.trees[0].nodes[0].feature,
              multi.model.trees[0].nodes[0].feature);
    // And the predictions agree on every row (to a tight tolerance).
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        std::vector<double> x{data.columns[0][r], data.columns[1][r],
                              data.columns[2][r]};
        EXPECT_NEAR(single.model.predict_proba(x), multi.model.predict_proba(x),
                    1e-6)
            << "row " << r;
    }
    EXPECT_NEAR(single.metrics.auc, multi.metrics.auc, 1e-9);
}

TEST(GbdtDistributed, CalibratesProducedModel) {
    // A larger synthetic so the content-hash held-out split reliably carries both
    // classes for the calibration fit.
    const Synthetic data = make_synthetic(400);
    const std::vector<Json> rows = synthetic_rows(data, "y");
    GbdtParams params;
    params.num_trees = 8;
    params.max_depth = 3;
    zetta::ml::CalibrationConfig calibration;
    calibration.enabled = true;
    calibration.method = zetta::ml::CalibrationMethod::Platt;
    calibration.holdout_fraction = 0.25;

    const auto single = run_distributed_train(rows, data.names, "y", params, 1,
                                              22300, false, calibration);
    const auto multi = run_distributed_train(rows, data.names, "y", params, 2,
                                             22310, false, calibration);
    ASSERT_TRUE(single.ok());
    ASSERT_TRUE(multi.ok());

    // The produced model is calibrated end-to-end and its weights body re-validates
    // against the GBDT model schema (carrying the calibration).
    EXPECT_TRUE(single.calibrated);
    ASSERT_TRUE(single.model.calibration.has_value());
    EXPECT_EQ(single.model.calibration->method,
              zetta::ml::CalibrationMethod::Platt);
    std::string error;
    EXPECT_TRUE(GbdtModel::from_json(single.model.to_json(), &error).has_value())
        << error;

    // Single-node and multi-node fit an equivalent calibrated model (the held-out
    // split is sharding-invariant): same trees, agreeing calibrated probabilities.
    ASSERT_TRUE(multi.model.calibration.has_value());
    ASSERT_EQ(single.model.tree_count(), multi.model.tree_count());
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        std::vector<double> x{data.columns[0][r], data.columns[1][r],
                              data.columns[2][r]};
        EXPECT_NEAR(single.model.predict_proba(x), multi.model.predict_proba(x),
                    1e-4)
            << "row " << r;
    }
}

// =====================================================================
// The real churn fixture — featurize then train to a target AUC -> Model
// =====================================================================
// The numeric encoding + the standard featurize spec / GBDT params are the
// canonical zetta::churn_solution.hpp surface (encode_churn_dataset,
// churn_events_schema / churn_accounts_schema, churn_featurize_spec,
// default_churn_gbdt_params).

// The churn feature table's rows as training docs (entity dropped; features + label).
std::vector<Json> feature_table_rows(const FeatureTable& table) {
    std::vector<Json> rows;
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        Json doc;
        for (std::size_t c = 0; c < table.feature_count(); ++c) {
            doc[table.feature_names[c]] = table.features[c][r];
        }
        doc[table.label_name] = table.labels[r];
        rows.push_back(std::move(doc));
    }
    return rows;
}

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

TEST(GbdtChurn, TrainsToTargetAucAndProducesModel) {
    const FeatureTable table = churn_feature_table();
    ASSERT_GT(table.row_count(), 0u);
    ASSERT_TRUE(table.has_label());
    const std::vector<Json> rows = feature_table_rows(table);

    const auto result = run_distributed_train(
        rows, table.feature_names, table.label_name,
        zetta::usecases::churn::default_churn_gbdt_params(),
        /*node_count=*/1, 22200, /*register_model=*/true);
    ASSERT_TRUE(result.ok());
    // The churn signal is learnable: a strong AUC on the (small) fixture.
    EXPECT_GE(result.metrics.auc, 0.9) << "auc " << result.metrics.auc;

    // The weights body round-trips through the model JSON Schema.
    std::string error;
    const auto reparsed = GbdtModel::from_json(result.model.to_json(), &error);
    EXPECT_TRUE(reparsed.has_value()) << error;

    // failed_payments is a real churn driver (positive importance).
    const auto failed = std::find(table.feature_names.begin(),
                                  table.feature_names.end(), "failed_payments");
    ASSERT_NE(failed, table.feature_names.end());
    const std::size_t failed_index =
        static_cast<std::size_t>(failed - table.feature_names.begin());
    ASSERT_LT(failed_index, result.feature_importances.size());
    EXPECT_GT(result.feature_importances[failed_index], 0.0);
}

TEST(GbdtChurn, SingleNodeEqualsMultiNode) {
    const FeatureTable table = churn_feature_table();
    const std::vector<Json> rows = feature_table_rows(table);
    const GbdtParams params = zetta::usecases::churn::default_churn_gbdt_params();

    const auto single = run_distributed_train(
        rows, table.feature_names, table.label_name, params, 1, 22210, false);
    const auto multi = run_distributed_train(
        rows, table.feature_names, table.label_name, params, 2, 22220, false);
    ASSERT_TRUE(single.ok());
    ASSERT_TRUE(multi.ok());
    // Same ensemble, equivalent up to floating-point reduction order across the
    // two shardings -> the same AUC over the (gathered) global rows.
    ASSERT_EQ(single.model.tree_count(), multi.model.tree_count());
    EXPECT_NEAR(single.model.base_score, multi.model.base_score, 1e-9);
    EXPECT_NEAR(single.metrics.auc, multi.metrics.auc, 1e-9);
}

// =====================================================================
// Checkpoint / restart (RULE-5 / #468)
// =====================================================================

// Run a single-node `train` job over `rows`, optionally checkpointing through
// `store`. Returns the result (the produced model lives on it).
zetta::ml::DistributedTrainResult run_single_node_train(
    ComputeNode& cn, const std::vector<Json>& rows,
    const std::vector<std::string>& features, const std::string& label,
    const GbdtParams& params, const std::string& round_tag,
    zetta::ml::TrainCheckpointConfig checkpoint) {
    CpuDevice device;
    auto frame =
        load_data_frame(make_store(rows), training_schema(features, label), device);
    EXPECT_TRUE(frame.ok()) << frame.status.message;
    std::shared_ptr<ComputeJob> job = cn.node.create_job(JobKind::Train);
    return run_train_job(cn.coordinator, *job, frame.frame, features, label, params,
                         nullptr, {}, round_tag, {}, zetta::ml::kDefaultGbdtTimeout,
                         checkpoint);
}

// The headline acceptance: a `train` job that is checkpointed and then "evicted"
// (its process restarts) resumes from the last checkpoint, growing only the
// remaining trees, and produces a model **identical** to an uninterrupted run.
TEST(GbdtCheckpoint, ResumesTrainingFromCheckpointWithoutRecompute) {
    const Synthetic data = make_synthetic(80);
    const std::vector<Json> rows = synthetic_rows(data, "y");
    const std::string tag = "ckpt-test";

    GbdtParams params;
    params.max_depth = 3;

    ComputeNode cn("node-ckpt", 22300, {}, 2);
    cn.cluster.start();
    cn.coordinator.start();

    TempDir store_dir;
    zetta::compute::FileCheckpointStore store(store_dir.path() / "checkpoints");

    // Attempt 1 ("evicted" after 4 of 10 trees): a short run that keeps its
    // checkpoint, standing in for a node reclaimed partway through.
    GbdtParams partial = params;
    partial.num_trees = 4;
    zetta::ml::TrainCheckpointConfig keep{
        .store = &store, .checkpoint_every = 1, .clear_on_success = false};
    const auto first =
        run_single_node_train(cn, rows, data.names, "y", partial, tag, keep);
    ASSERT_TRUE(first.ok()) << "first attempt should succeed";
    EXPECT_EQ(first.resumed_from_round, 0u);  // started fresh
    // A checkpoint at round 4 survived.
    const std::optional<zetta::compute::JobCheckpoint> mid = store.latest(tag);
    ASSERT_TRUE(mid);
    EXPECT_EQ(mid->sequence, 4u);

    // Attempt 2 (the "restart"): the full 10-tree job over the SAME store resumes
    // from the round-4 checkpoint.
    params.num_trees = 10;
    zetta::ml::TrainCheckpointConfig resume{
        .store = &store, .checkpoint_every = 1, .clear_on_success = false};
    const auto resumed =
        run_single_node_train(cn, rows, data.names, "y", params, tag, resume);
    ASSERT_TRUE(resumed.ok());
    EXPECT_EQ(resumed.resumed_from_round, 4u);  // picked up where it left off
    EXPECT_EQ(resumed.model.tree_count(), 10u);

    // The result is identical to an uninterrupted from-scratch 10-tree run.
    ComputeNode control_node("node-ctrl", 22310, {}, 2);
    control_node.cluster.start();
    control_node.coordinator.start();
    TempDir control_dir;
    zetta::compute::FileCheckpointStore control_store(control_dir.path() / "cp");
    const auto control = run_single_node_train(control_node, rows, data.names, "y",
                                               params, "control", {});
    ASSERT_TRUE(control.ok());
    EXPECT_EQ(control.resumed_from_round, 0u);
    EXPECT_EQ(resumed.model.to_json(), control.model.to_json());

    cn.coordinator.stop();
    cn.cluster.stop();
    control_node.coordinator.stop();
    control_node.cluster.stop();
}

// clear_on_success (the default) discards a job's checkpoints once it completes, so
// a restart of a finished job does not resume (it has its durable Model instead).
TEST(GbdtCheckpoint, ClearsCheckpointsOnSuccess) {
    const Synthetic data = make_synthetic(40);
    const std::vector<Json> rows = synthetic_rows(data, "y");
    GbdtParams params;
    params.num_trees = 5;
    params.max_depth = 2;

    ComputeNode cn("node-clear", 22320, {}, 2);
    cn.cluster.start();
    cn.coordinator.start();

    TempDir store_dir;
    zetta::compute::FileCheckpointStore store(store_dir.path() / "cp");
    zetta::ml::TrainCheckpointConfig config{.store = &store};  // clear_on_success=true
    const auto result =
        run_single_node_train(cn, rows, data.names, "y", params, "clear-tag", config);
    ASSERT_TRUE(result.ok());
    EXPECT_FALSE(store.latest("clear-tag"));  // cleared once training finished

    cn.coordinator.stop();
    cn.cluster.stop();
}

// =====================================================================
// Held-out metrics (#953)
// =====================================================================

// Features that say nothing about the label. A model can only score above chance
// on this by memorizing individual rows — which is exactly what an in-sample
// metric rewards and a held-out metric exposes.
Synthetic make_noise(std::size_t rows) {
    Synthetic data;
    data.columns.assign(3, {});
    for (std::size_t i = 0; i < rows; ++i) {
        // Deterministic, but with no relationship to the label below.
        data.columns[0].push_back(static_cast<double>((i * 37) % 101) / 101.0);
        data.columns[1].push_back(static_cast<double>((i * 53) % 97) / 97.0);
        data.columns[2].push_back(static_cast<double>((i * 71) % 89) / 89.0);
        data.labels.push_back(i % 2 == 0 ? 1.0 : 0.0);
    }
    return data;
}

// Asking for no holdout must leave the result saying so, not carrying zeros. A
// zeroed AUC reads as a model that ranks backwards, which is a worse lie than
// the in-sample number this exists to qualify.
TEST(GbdtHoldout, AbsentHoldoutIsMarkedAbsentRatherThanZero) {
    const Synthetic data = make_synthetic(80);
    GbdtParams params;
    params.num_trees = 8;

    ComputeNode cn("node-hold-off", 22400, {}, 2);
    cn.cluster.start();
    cn.coordinator.start();
    const auto result = run_single_node_train(
        cn, synthetic_rows(data, "y"), data.names, "y", params, "hold-off", {});
    ASSERT_TRUE(result.ok());

    EXPECT_FALSE(result.has_held_out);
    EXPECT_EQ(result.held_out_rows, 0u);
    EXPECT_EQ(result.train_rows, 80u);  // every row trained AND was scored
    EXPECT_GT(result.metrics.auc, 0.9);

    cn.coordinator.stop();
    cn.cluster.stop();
}

// The load-bearing test. On data with no signal, a model that genuinely never saw
// the held-out rows cannot rank them: in-sample climbs by memorization, held-out
// sits at chance. If the split leaked — if the "held-out" rows were trained on
// after all — both figures would rise together and this fails.
TEST(GbdtHoldout, HeldOutRowsWereNotTrainedOn) {
    const Synthetic data = make_noise(400);
    GbdtParams params;
    params.num_trees = 40;   // deep enough, and enough of them, to memorize
    params.max_depth = 6;
    params.holdout_fraction = 0.3;

    ComputeNode cn("node-hold-on", 22410, {}, 2);
    cn.cluster.start();
    cn.coordinator.start();
    const auto result = run_single_node_train(
        cn, synthetic_rows(data, "y"), data.names, "y", params, "hold-on", {});
    ASSERT_TRUE(result.ok());

    ASSERT_TRUE(result.has_held_out);
    // Every row is accounted for, and the split is near the fraction asked for
    // (the row hash is content-based, so it lands close, not exactly).
    EXPECT_EQ(result.train_rows + result.held_out_rows, 400u);
    EXPECT_GT(result.held_out_rows, 80u);
    EXPECT_LT(result.held_out_rows, 160u);

    EXPECT_GT(result.metrics.auc, 0.75) << "in-sample should show memorization";
    EXPECT_LT(result.held_out.auc, 0.65) << "held-out must sit near chance; a "
                                            "high value here means the split "
                                            "leaked training rows";

    cn.coordinator.stop();
    cn.cluster.stop();
}

// Whatever a run writes into the Model's `metrics` must survive the entity
// boundary, because it is validated at the RECEIVING peer. A value the schema
// rejects still registers on the root and still replicates — it simply fails to
// materialize anywhere else, surfacing as "the model does not resolve on every
// member" a long way from the code that wrote it. `metrics` is declared as
// `additionalProperties: {type: number}`, so every value has to be a number: this
// caught a nested `in_sample` object and a `headline_metric` string that had
// passed every single-node test.
TEST(GbdtHoldout, RegisteredModelMetricsSurviveTheEntityBoundary) {
    const Synthetic data = make_synthetic(120);
    GbdtParams params;
    params.num_trees = 8;
    params.holdout_fraction = 0.3;

    ComputeNode cn("node-hold-entity", 22440, {}, 2);
    cn.cluster.start();
    cn.coordinator.start();
    CpuDevice device;
    auto frame = load_data_frame(make_store(synthetic_rows(data, "y")),
                                 training_schema(data.names, "y"), device);
    ASSERT_TRUE(frame.ok()) << frame.status.message;
    std::shared_ptr<ComputeJob> job = cn.node.create_job(JobKind::Train);
    const auto result =
        run_train_job(cn.coordinator, *job, frame.frame, data.names, "y", params,
                      &cn.node, "holdout-entity-model");
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.has_held_out);

    std::shared_ptr<zetta::ml::Model> registered;
    for (const auto& model : cn.node.models()) {
        if (model->name() == "holdout-entity-model") registered = model;
    }
    ASSERT_TRUE(registered) << "the run registered no model";

    std::string error;
    EXPECT_TRUE(zetta::ml::parse_model_value(zetta::ml::to_json(registered->value()),
                                            &error))
        << "the registered model does not pass its own boundary schema, so it "
           "would fail to materialize on every peer: " << error;

    // And the held-out figure is actually carried, by presence.
    const nlohmann::json& metrics = registered->metrics();
    ASSERT_TRUE(metrics.contains("held_out_auc"));
    EXPECT_DOUBLE_EQ(metrics["held_out_auc"].get<double>(), result.held_out.auc);
    EXPECT_EQ(metrics["held_out_rows"].get<std::uint64_t>(), result.held_out_rows);
    for (const auto& [key, entry] : metrics.items()) {
        EXPECT_TRUE(entry.is_number()) << "metrics['" << key << "'] is not a number";
    }

    cn.coordinator.stop();
    cn.cluster.stop();
}

// The holdout is decided by a content hash of the row, so WHICH rows are held out
// does not depend on how the rows were spread over nodes. Two shardings of the
// same data therefore report the same held-out figure — without this, a run's
// headline metric would change when the cluster resized.
TEST(GbdtHoldout, HoldoutIsShardingInvariant) {
    const Synthetic data = make_noise(300);
    const std::vector<Json> rows = synthetic_rows(data, "y");
    GbdtParams params;
    params.num_trees = 12;
    params.max_depth = 4;
    params.holdout_fraction = 0.25;

    const auto single =
        run_distributed_train(rows, data.names, "y", params, 1, 22420, false);
    const auto multi =
        run_distributed_train(rows, data.names, "y", params, 3, 22430, false);

    ASSERT_TRUE(single.has_held_out);
    ASSERT_TRUE(multi.has_held_out);
    EXPECT_EQ(single.held_out_rows, multi.held_out_rows);
    EXPECT_EQ(single.train_rows, multi.train_rows);
    EXPECT_NEAR(single.held_out.auc, multi.held_out.auc, 1e-9);
    EXPECT_NEAR(single.held_out.log_loss, multi.held_out.log_loss, 1e-9);
}

}  // namespace

// --- squared-error objective (#984, M-4) ---------------------------------------------------
//
// Forecasting predicts a continuous value, and until this landed the learner had exactly one
// objective: binary logistic. That is why M-4 needed learner work after all — not a second
// learner, but a second LOSS. Everything else (histogram building, split finding, the allreduce)
// is objective-agnostic, which is what made it a contained change.

TEST(GbdtRegression, SquaredErrorFitsAContinuousTarget) {
    // y = 3*x0 + 10, well outside [0,1] — a sigmoid-squashed model cannot express it.
    std::vector<double> feature;
    std::vector<double> labels;
    for (int value = 0; value < 200; ++value) {
        feature.push_back(static_cast<double>(value % 20));
        labels.push_back(3.0 * static_cast<double>(value % 20) + 10.0);
    }
    const std::vector<std::vector<double>> columns = {feature};

    zetta::ml::GbdtParams params;
    params.objective = "squared_error";
    params.num_trees = 40;
    params.learning_rate = 0.3;

    const zetta::ml::GbdtTrainResult result =
        zetta::ml::train_gbdt(columns, {"x0"}, labels, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.model.objective, "squared_error")
        << "the model must record which loss grew it, or prediction cannot know how to read it";

    double error = 0.0;
    for (std::size_t row = 0; row < labels.size(); ++row) {
        error += std::abs(result.model.predict_proba({feature[row]}) - labels[row]);
    }
    error /= static_cast<double>(labels.size());
    EXPECT_LT(error, 3.0) << "mean absolute error " << error
                          << " — a regression that cannot get within 3 of a linear target has "
                             "not fit anything";
}

// The failure this would otherwise cause is silent and total: a forecast of 143.7 units comes
// back as 1.0, because predict_proba squashed it.
TEST(GbdtRegression, APredictionIsNotSquashedThroughASigmoid) {
    std::vector<double> feature;
    std::vector<double> labels;
    for (int value = 0; value < 100; ++value) {
        feature.push_back(static_cast<double>(value));
        labels.push_back(100.0 + static_cast<double>(value));
    }

    zetta::ml::GbdtParams params;
    params.objective = "squared_error";
    params.num_trees = 30;

    const zetta::ml::GbdtTrainResult result =
        zetta::ml::train_gbdt({feature}, {"x0"}, labels, params);
    ASSERT_TRUE(result.ok()) << result.status.message;

    const double prediction = result.model.predict_proba({50.0});
    EXPECT_GT(prediction, 10.0)
        << "a squashed prediction lands in [0,1]; this must be on the scale of the target";
}

// Regression labels are continuous, so the classification guard would reject perfectly good
// data — every daily-revenue series has all-positive labels.
TEST(GbdtRegression, AllPositiveLabelsAreNotASingleClassError) {
    std::vector<double> feature;
    std::vector<double> labels;
    for (int value = 0; value < 80; ++value) {
        feature.push_back(static_cast<double>(value % 8));
        labels.push_back(500.0 + static_cast<double>(value % 8) * 7.0);
    }

    zetta::ml::GbdtParams params;
    params.objective = "squared_error";
    params.num_trees = 10;

    EXPECT_TRUE(zetta::ml::train_gbdt({feature}, {"x0"}, labels, params).ok())
        << "all-positive continuous labels are ordinary regression data, not a single class";
}

// And the objective that ships must be untouched.
TEST(GbdtRegression, BinaryLogisticIsUnchanged) {
    std::vector<double> feature;
    std::vector<double> labels;
    for (int value = 0; value < 200; ++value) {
        feature.push_back(static_cast<double>(value % 10));
        labels.push_back(value % 10 >= 5 ? 1.0 : 0.0);
    }

    zetta::ml::GbdtParams params;  // default objective
    params.num_trees = 20;

    const zetta::ml::GbdtTrainResult result =
        zetta::ml::train_gbdt({feature}, {"x0"}, labels, params);
    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_EQ(result.model.objective, "binary_logistic");
    EXPECT_GT(result.model.predict_proba({9.0}), 0.5);
    EXPECT_LT(result.model.predict_proba({0.0}), 0.5);
}

// A parameter that is not on the schema is one no caller can set. `partition_only` lived on
// AddDatasetOptions for a whole release and was never on `add_dataset`'s schema, so nothing
// could reach it (#952). `objective` would have repeated that exactly: the learner supports
// squared error and, without this, only C++ callers could ask for it.
TEST(GbdtRegression, TheObjectiveIsReachableOverJson) {
    zetta::ml::GbdtParams params;
    params.objective = "squared_error";
    const nlohmann::json encoded = params.to_json();
    ASSERT_TRUE(encoded.contains("objective")) << "an unencoded field cannot round-trip";

    const std::optional<zetta::ml::GbdtParams> decoded =
        zetta::ml::GbdtParams::from_json(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->objective, "squared_error");
}

// An unknown objective must be REFUSED, not silently defaulted. Falling back is how a caller
// asks for a forecast and receives a classifier: the run succeeds, the metrics look reasonable,
// and nothing anywhere says the loss was not the one requested.
TEST(GbdtRegression, AnUnknownObjectiveIsRefusedRatherThanDefaulted) {
    nlohmann::json value = zetta::ml::GbdtParams{}.to_json();
    value["objective"] = "poisson";
    EXPECT_FALSE(zetta::ml::GbdtParams::from_json(value).has_value())
        << "an unimplemented objective must fail loudly at the boundary";

    value["objective"] = 7;
    EXPECT_FALSE(zetta::ml::GbdtParams::from_json(value).has_value());
}

TEST(GbdtRegression, TheDefaultObjectiveIsUnchangedWhenAbsent) {
    nlohmann::json value = zetta::ml::GbdtParams{}.to_json();
    value.erase("objective");
    const auto decoded = zetta::ml::GbdtParams::from_json(value);
    ASSERT_TRUE(decoded.has_value()) << "objective is optional; omitting it must still parse";
    EXPECT_EQ(decoded->objective, "binary_logistic");
}
