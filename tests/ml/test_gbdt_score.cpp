// SPDX-License-Identifier: MIT

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
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/gbdt_score_job.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// Batch scoring / rank-at-risk over a trained GBDT Model (#392): the unit surface
// (the pure scorer ranking entities by calibrated risk, the model-load JSON
// boundary) plus the distributed `score` ComputeJob (single-node == multi-node
// ranking, and the churn fixture scored to a ranked at-risk list matching the
// trained model).
namespace {

namespace fs = std::filesystem;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::ml::Model;
using zetta::ml::ModelValue;
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
using zetta::ml::EntityScore;
using zetta::ml::GbdtModel;
using zetta::ml::GbdtParams;
using zetta::ml::GbdtTrainResult;
using zetta::ml::load_gbdt_model;
using zetta::ml::predict_risk;
using zetta::ml::run_score_job;
using zetta::ml::score_gbdt;
using zetta::ml::train_gbdt;
using zetta::test::TempDir;
using Json = nlohmann::json;

// =====================================================================
// A separable synthetic dataset (feature 0 carries the churn signal).
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

GbdtModel trained_synthetic_model(const Synthetic& data) {
    GbdtParams params;
    params.num_trees = 20;
    params.max_depth = 3;
    const GbdtTrainResult trained =
        train_gbdt(data.columns, data.names, data.labels, params);
    EXPECT_TRUE(trained.ok()) << trained.status.message;
    return trained.model;
}

// Entity keys 0..rows-1, in dataset order.
std::vector<std::int64_t> identity_entities(std::size_t rows) {
    std::vector<std::int64_t> entities(rows);
    for (std::size_t i = 0; i < rows; ++i) entities[i] = static_cast<std::int64_t>(i);
    return entities;
}

// =====================================================================
// Pure scoring + ranking
// =====================================================================

TEST(ScoreGbdt, RanksByRiskDescendingAndMatchesProba) {
    const Synthetic data = make_synthetic(60);
    const GbdtModel model = trained_synthetic_model(data);
    const std::vector<std::int64_t> entities = identity_entities(data.labels.size());

    const auto scored =
        score_gbdt(model, data.columns, data.names, entities);
    ASSERT_TRUE(scored.ok()) << scored.status.message;
    ASSERT_EQ(scored.ranked.size(), data.labels.size());

    // Strictly non-increasing risk down the list, ties broken by entity ascending.
    for (std::size_t i = 1; i < scored.ranked.size(); ++i) {
        const EntityScore& prev = scored.ranked[i - 1];
        const EntityScore& cur = scored.ranked[i];
        EXPECT_TRUE(prev.risk > cur.risk ||
                    (prev.risk == cur.risk && prev.entity < cur.entity))
            << "at rank " << i;
    }

    // Every listed risk equals the model's predict_proba for that entity's row.
    for (const EntityScore& score : scored.ranked) {
        const std::size_t row = static_cast<std::size_t>(score.entity);
        const std::vector<double> x{data.columns[0][row], data.columns[1][row],
                                    data.columns[2][row]};
        EXPECT_DOUBLE_EQ(score.risk, model.predict_proba(x));
        EXPECT_GE(score.risk, 0.0);
        EXPECT_LE(score.risk, 1.0);
    }

    // The model separates the classes -> every positive outranks every negative.
    std::size_t boundary = 0;
    while (boundary < scored.ranked.size() &&
           data.labels[static_cast<std::size_t>(scored.ranked[boundary].entity)] ==
               1.0) {
        ++boundary;
    }
    for (std::size_t i = boundary; i < scored.ranked.size(); ++i) {
        EXPECT_DOUBLE_EQ(
            data.labels[static_cast<std::size_t>(scored.ranked[i].entity)], 0.0)
            << "positive ranked below a negative at " << i;
    }
}

TEST(ScoreGbdt, TopKTruncatesToHighestRisk) {
    const Synthetic data = make_synthetic(40);
    const GbdtModel model = trained_synthetic_model(data);
    const std::vector<std::int64_t> entities = identity_entities(data.labels.size());

    const auto all = score_gbdt(model, data.columns, data.names, entities);
    const auto top = score_gbdt(model, data.columns, data.names, entities, 5);
    ASSERT_TRUE(top.ok());
    ASSERT_EQ(top.ranked.size(), 5u);
    // top_k is exactly the prefix of the full ranking.
    for (std::size_t i = 0; i < top.ranked.size(); ++i) {
        EXPECT_EQ(top.ranked[i].entity, all.ranked[i].entity);
        EXPECT_DOUBLE_EQ(top.ranked[i].risk, all.ranked[i].risk);
    }
}

TEST(ScoreGbdt, ColumnOrderIndependent) {
    const Synthetic data = make_synthetic(30);
    const GbdtModel model = trained_synthetic_model(data);
    const std::vector<std::int64_t> entities = identity_entities(data.labels.size());

    // Present the same columns in a permuted order with a different naming order.
    std::vector<std::vector<double>> shuffled{data.columns[2], data.columns[0],
                                              data.columns[1]};
    std::vector<std::string> shuffled_names{"f2", "f0", "f1"};

    const auto direct = score_gbdt(model, data.columns, data.names, entities);
    const auto permuted = score_gbdt(model, shuffled, shuffled_names, entities);
    ASSERT_TRUE(permuted.ok()) << permuted.status.message;
    ASSERT_EQ(direct.ranked.size(), permuted.ranked.size());
    for (std::size_t i = 0; i < direct.ranked.size(); ++i) {
        EXPECT_EQ(direct.ranked[i].entity, permuted.ranked[i].entity);
        EXPECT_DOUBLE_EQ(direct.ranked[i].risk, permuted.ranked[i].risk);
    }
}

TEST(ScoreGbdt, RejectsMissingFeatureColumn) {
    const Synthetic data = make_synthetic(20);
    const GbdtModel model = trained_synthetic_model(data);
    const std::vector<std::int64_t> entities = identity_entities(data.labels.size());

    // Drop the column the model needs for feature f0.
    std::vector<std::vector<double>> partial{data.columns[1], data.columns[2]};
    std::vector<std::string> partial_names{"f1", "f2"};
    const auto scored = score_gbdt(model, partial, partial_names, entities);
    EXPECT_FALSE(scored.ok());

    std::vector<double> risks;
    const auto status = predict_risk(model, partial, partial_names, risks);
    EXPECT_FALSE(status.ok());
}

// =====================================================================
// load_gbdt_model — the weights-body JSON boundary
// =====================================================================

TEST(LoadGbdtModel, RoundTripsThroughModelEntity) {
    const Synthetic data = make_synthetic(20);
    const GbdtModel model = trained_synthetic_model(data);

    TempDir dir;
    NodeConfig config;
    config.data_dir = dir.path().string();
    Node node(config);
    node.start();

    ModelValue value;
    value.kind = "classifier";
    std::shared_ptr<Model> registered = node.create_model(value);
    ASSERT_TRUE(registered);
    ASSERT_TRUE(registered->set_weights(model.to_json().dump()));

    std::string error;
    const auto loaded = load_gbdt_model(*registered, &error);
    ASSERT_TRUE(loaded.has_value()) << error;
    // Predictions survive the disk round-trip.
    const std::vector<double> x{0.8, 0.3, 0.2};
    EXPECT_DOUBLE_EQ(loaded->predict_proba(x), model.predict_proba(x));
}

TEST(LoadGbdtModel, RejectsMalformedBody) {
    TempDir dir;
    NodeConfig config;
    config.data_dir = dir.path().string();
    Node node(config);
    node.start();

    ModelValue value;
    value.kind = "classifier";
    std::shared_ptr<Model> registered = node.create_model(value);
    ASSERT_TRUE(registered);
    // A body that is valid JSON but not a valid GBDT model.
    ASSERT_TRUE(registered->set_weights(Json{{"objective", "multiclass"}}.dump()));
    std::string error;
    EXPECT_FALSE(load_gbdt_model(*registered, &error).has_value());
    EXPECT_FALSE(error.empty());
}

// =====================================================================
// Distributed `score` ComputeJob — the loopback cluster harness
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

// A scoring schema: the entity key as Int64, each feature as Float64.
DataFrameSchema scoring_schema(const std::vector<std::string>& features) {
    DataFrameSchema schema;
    schema.columns.push_back(
        ColumnSchema{.name = "entity", .dtype = DataType::Int64});
    for (const std::string& name : features) {
        schema.columns.push_back(
            ColumnSchema{.name = name, .dtype = DataType::Float64});
    }
    return schema;
}

// Run a `score` ComputeJob across `node_count` loopback nodes, each owning a
// contiguous shard of `rows`. Returns node 0's (root's) result.
zetta::ml::DistributedScoreResult run_distributed_score(
    const std::vector<Json>& rows, const std::vector<std::string>& features,
    const GbdtModel& model, std::size_t node_count, std::uint16_t base_port,
    std::size_t top_k = 0) {
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
            scoring_schema(features), *devices[index]);
        EXPECT_TRUE(frame.ok()) << frame.status.message;
        frames.push_back(std::move(frame.frame));
    }

    std::vector<zetta::ml::DistributedScoreResult> results(node_count);
    std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < node_count; ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Score);
        threads.emplace_back([&, index] {
            results[index] = run_score_job(nodes[index]->coordinator, *jobs[index],
                                           frames[index], model, "entity", top_k);
        });
    }
    for (auto& thread : threads) thread.join();

    for (std::size_t index = 0; index < node_count; ++index) {
        EXPECT_EQ(results[index].status, JobStatus::Succeeded)
            << "node " << index << ": " << jobs[index]->detail();
        EXPECT_EQ(jobs[index]->kind(), JobKind::Score) << "node " << index;
        // The global ranked list is identical on every node (gather/rank/broadcast).
        EXPECT_EQ(results[index].ranked.size(), results[0].ranked.size())
            << "node " << index;
        if (results[index].ranked.size() != results[0].ranked.size()) continue;
        for (std::size_t r = 0; r < results[0].ranked.size(); ++r) {
            EXPECT_EQ(results[index].ranked[r].entity, results[0].ranked[r].entity)
                << "node " << index << " rank " << r;
            EXPECT_NEAR(results[index].ranked[r].risk, results[0].ranked[r].risk,
                        1e-12)
                << "node " << index << " rank " << r;
        }
    }
    EXPECT_TRUE(results[0].is_root);

    auto out = std::move(results[0]);
    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
    return out;
}

// Synthetic rows as scoring docs: entity key + feature columns (no label).
std::vector<Json> scoring_rows(const Synthetic& data) {
    std::vector<Json> rows;
    for (std::size_t r = 0; r < data.labels.size(); ++r) {
        Json doc;
        doc["entity"] = static_cast<std::int64_t>(r);
        for (std::size_t f = 0; f < data.names.size(); ++f) {
            doc[data.names[f]] = data.columns[f][r];
        }
        rows.push_back(std::move(doc));
    }
    return rows;
}

TEST(ScoreDistributed, SingleNodeMatchesMultiNodeAndPureScorer) {
    const Synthetic data = make_synthetic(60);
    const GbdtModel model = trained_synthetic_model(data);
    const std::vector<Json> rows = scoring_rows(data);

    const auto single =
        run_distributed_score(rows, data.names, model, 1, 22300);
    const auto multi = run_distributed_score(rows, data.names, model, 3, 22310);
    ASSERT_TRUE(single.ok());
    ASSERT_TRUE(multi.ok());

    // The pure host-side scorer is the reference; the distributed path must match.
    const auto reference =
        score_gbdt(model, data.columns, data.names, identity_entities(rows.size()));
    ASSERT_TRUE(reference.ok());

    ASSERT_EQ(single.ranked.size(), reference.ranked.size());
    ASSERT_EQ(multi.ranked.size(), reference.ranked.size());
    for (std::size_t r = 0; r < reference.ranked.size(); ++r) {
        EXPECT_EQ(single.ranked[r].entity, reference.ranked[r].entity) << "rank " << r;
        EXPECT_EQ(multi.ranked[r].entity, reference.ranked[r].entity) << "rank " << r;
        EXPECT_NEAR(single.ranked[r].risk, reference.ranked[r].risk, 1e-12);
        EXPECT_NEAR(multi.ranked[r].risk, reference.ranked[r].risk, 1e-12);
    }
}

TEST(ScoreDistributed, TopKReturnsHighestRiskAcrossShards) {
    const Synthetic data = make_synthetic(40);
    const GbdtModel model = trained_synthetic_model(data);
    const std::vector<Json> rows = scoring_rows(data);

    const auto top = run_distributed_score(rows, data.names, model, 3, 22320, 6);
    ASSERT_TRUE(top.ok());
    ASSERT_EQ(top.ranked.size(), 6u);
    const auto reference =
        score_gbdt(model, data.columns, data.names, identity_entities(rows.size()), 6);
    ASSERT_EQ(reference.ranked.size(), 6u);
    for (std::size_t r = 0; r < 6; ++r) {
        EXPECT_EQ(top.ranked[r].entity, reference.ranked[r].entity) << "rank " << r;
    }
}

// =====================================================================
// The real churn fixture — featurize, train, score -> ranked at-risk list
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

// The churn feature table's rows as scoring docs (entity + features, no label).
std::vector<Json> churn_scoring_rows(const FeatureTable& table) {
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

TEST(ScoreChurn, RanksAtRiskMatchingTheTrainedModel) {
    const FeatureTable table = churn_feature_table();
    ASSERT_GT(table.row_count(), 0u);
    ASSERT_TRUE(table.has_label());

    // Train on the table (host-side), then score every entity through the model.
    GbdtParams params;
    params.num_trees = 30;
    params.max_depth = 3;
    params.max_bins = 16;
    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    const GbdtModel& model = trained.model;

    // The distributed `score` job over the table reproduces the pure scorer.
    const std::vector<Json> rows = churn_scoring_rows(table);
    const auto distributed =
        run_distributed_score(rows, table.feature_names, model, 2, 22330);
    ASSERT_TRUE(distributed.ok());

    std::vector<std::int64_t> entities(table.entities.begin(), table.entities.end());
    const auto reference =
        score_gbdt(model, table.features, table.feature_names, entities);
    ASSERT_TRUE(reference.ok());
    ASSERT_EQ(distributed.ranked.size(), reference.ranked.size());
    for (std::size_t r = 0; r < reference.ranked.size(); ++r) {
        EXPECT_EQ(distributed.ranked[r].entity, reference.ranked[r].entity)
            << "rank " << r;
        EXPECT_NEAR(distributed.ranked[r].risk, reference.ranked[r].risk, 1e-9);
    }

    // The at-risk ranking is meaningful: churned accounts carry a higher mean risk
    // than retained ones (the model learned the churn signal, surfaced as risk).
    std::map<std::int64_t, double> label_of;
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        label_of[table.entities[r]] = table.labels[r];
    }
    double churned_sum = 0.0, retained_sum = 0.0;
    std::size_t churned_count = 0, retained_count = 0;
    for (const EntityScore& score : reference.ranked) {
        if (label_of[score.entity] == 1.0) {
            churned_sum += score.risk;
            ++churned_count;
        } else {
            retained_sum += score.risk;
            ++retained_count;
        }
    }
    ASSERT_GT(churned_count, 0u);
    ASSERT_GT(retained_count, 0u);
    EXPECT_GT(churned_sum / churned_count, retained_sum / retained_count)
        << "churned accounts should rank as higher risk on average";
}

}  // namespace
