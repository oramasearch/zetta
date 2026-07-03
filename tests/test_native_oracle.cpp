#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/churn_dataset.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/data_type.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/ddf/data_frame.hpp"
#include "zetta/ddf/featurize.hpp"
#include "zetta/ddf/featurize_job.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_explain_job.hpp"
#include "zetta/ml/gbdt_train_job.hpp"
#include "zetta/ml/golden_oracle.hpp"
#include "zetta/ml/tree_shap.hpp"
#include "zetta/node.hpp"
#include "zetta/storage/json_store.hpp"

// The **native correctness oracle** (RSK-8, #395): prove the from-scratch native
// featurize / GBDT-train / TreeSHAP reimplementations are numerically correct WITHOUT
// running any reference (no Python, no XGBoost, no foreign runtime — ADR-17). Three
// self-contained C++ layers, all over the committed synthetic churn fixture:
//
//   1. Golden numeric fixtures — expected featurize / train / SHAP outputs committed
//      as static data (tests/fixtures/golden/churn_oracle.json, generated once offline
//      by zetta's own engine via the env-gated write mode below) and asserted within
//      tolerance. The committed values are also cross-checked here against the
//      leakage-free ground truth (featurize) and the SHAP efficiency identity, so the
//      golden is itself proven correct, not merely pinned.
//   2. Analytic micro-tests — tiny hand-built datasets whose GBDT split, leaf Newton
//      step, and SHAP attributions are computable by hand; assert the closed-form.
//   3. Cross-cluster equivalence — single-node == 2-node == 3-node results are
//      identical within tolerance (the distributed implementation is deterministic
//      regardless of node/shard count), and equal the single-node golden.
//
// Regenerate the golden after an intended numeric change with:
//   ZETTA_WRITE_GOLDEN=1 ctest --test-dir build -R NativeOracle --output-on-failure
// then commit the updated fixture. CI never sets the env var (read-only assert).
namespace {

namespace fs = std::filesystem;

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::ComputeJob;
using zetta::JobKind;
using zetta::JobStatus;
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
using zetta::ddf::run_featurize_job;
using zetta::ddf::WindowedFeature;
using zetta::ml::dominant_driver;
using zetta::ml::evaluate_binary;
using zetta::ml::GbdtMetrics;
using zetta::ml::GbdtModel;
using zetta::ml::GbdtParams;
using zetta::ml::GbdtTrainResult;
using zetta::ml::GoldenOracle;
using zetta::ml::load_golden_oracle;
using zetta::ml::model_base_value;
using zetta::ml::RegressionTree;
using zetta::ml::run_explain_job;
using zetta::ml::run_train_job;
using zetta::ml::ShapExplanation;
using zetta::ml::train_gbdt;
using zetta::ml::tree_shap;
using zetta::ml::TreeNode;
using zetta::test::TempDir;
using Json = nlohmann::json;

// =====================================================================
// Small shared helpers (used across all three layers)
// =====================================================================

// The index of `name` in `names` (asserts it is present — the caller has already
// established the column/feature exists).
std::size_t index_of(const std::vector<std::string>& names, const std::string& name) {
    const auto found = std::find(names.begin(), names.end(), name);
    EXPECT_NE(found, names.end()) << "missing column " << name;
    return static_cast<std::size_t>(found - names.begin());
}

zetta::JsonStore make_store(const std::vector<Json>& rows) {
    zetta::JsonStore store;
    for (std::size_t index = 0; index < rows.size(); ++index)
        store.add("row-" + std::to_string(index), rows[index]);
    return store;
}

// Load a frame straight from a row vector (the single-node convenience).
DataFrame load_data_frame_store(const std::vector<Json>& rows,
                                const DataFrameSchema& schema, CpuDevice& device) {
    auto loaded = load_data_frame(make_store(rows), schema, device);
    EXPECT_TRUE(loaded.ok()) << loaded.status.message;
    return std::move(loaded.frame);
}

// =====================================================================
// The canonical churn pipeline: one spec + one set of params, shared by every
// layer (golden generation, the golden asserts, and the equivalence runs).
// =====================================================================

// A linear day index within 2026 (the only year the fixture uses).
std::int64_t day_of_year_2026(const std::string& iso) {
    const int month = std::stoi(iso.substr(5, 2));
    const int day = std::stoi(iso.substr(8, 2));
    static const int before[] = {0,   0,   31,  59,  90,  120, 151,
                                 181, 212, 243, 273, 304, 334};
    return before[month] + day;
}
std::int64_t event_type_code(const std::string& type) {
    if (type == "login") return 1;
    if (type == "feature_use") return 2;
    if (type == "payment_failed") return 3;
    if (type == "support_ticket") return 4;
    if (type == "csm_contact") return 5;
    if (type == "comms") return 6;
    return 0;
}

// The numeric (events, accounts) stores the DDF pipeline operates over, plus the stable
// account_id -> entity-key index.
struct ChurnNumeric {
    std::vector<Json> events;
    std::vector<Json> accounts;
    std::map<std::string, std::int64_t> entity_index;
    std::int64_t as_of = 0;
};

ChurnNumeric encode_churn(const zetta::ChurnDataset& dataset) {
    ChurnNumeric out;
    for (const zetta::ChurnAccount& account : dataset.accounts) {
        const std::int64_t key =
            static_cast<std::int64_t>(out.entity_index.size());
        out.entity_index.emplace(account.account_id, key);
        out.accounts.push_back({{"entity", key},
                                {"mrr", account.mrr},
                                {"seats", account.seats},
                                {"tenure_days", account.tenure_days},
                                {"churned", account.churned ? 1 : 0}});
    }
    for (const zetta::ChurnEvent& event : dataset.events) {
        out.events.push_back({{"entity", out.entity_index.at(event.account_id)},
                              {"ts", day_of_year_2026(event.event_ts)},
                              {"value", event.value},
                              {"type", event_type_code(event.event_type)}});
    }
    out.as_of = day_of_year_2026(dataset.as_of_date);
    return out;
}

DataFrameSchema events_schema() {
    DataFrameSchema schema;
    schema.columns = {ColumnSchema{.name = "entity", .dtype = DataType::Int64},
                      ColumnSchema{.name = "ts", .dtype = DataType::Int64},
                      ColumnSchema{.name = "value", .dtype = DataType::Float64},
                      ColumnSchema{.name = "type", .dtype = DataType::Int64}};
    return schema;
}
DataFrameSchema accounts_schema() {
    DataFrameSchema schema;
    schema.columns = {
        ColumnSchema{.name = "entity", .dtype = DataType::Int64},
        ColumnSchema{.name = "mrr", .dtype = DataType::Float64},
        ColumnSchema{.name = "seats", .dtype = DataType::Int64},
        ColumnSchema{.name = "tenure_days", .dtype = DataType::Int64},
        ColumnSchema{.name = "churned", .dtype = DataType::Int64}};
    return schema;
}

// The churn point-in-time feature spec (the shape of churn_features.sql): windowed
// event features + carried account features + the label. The single canonical spec the
// whole oracle uses.
FeaturizeSpec canonical_spec(std::int64_t as_of) {
    FeaturizeSpec spec;
    spec.entity_column = "entity";
    spec.time_column = "ts";
    spec.as_of = static_cast<double>(as_of);
    spec.event_features = {
        WindowedFeature{.name = "failed_payments",
                        .agg = zetta::ddf::AggregateOp::Count,
                        .type_column = "type",
                        .type_value = 3.0},
        WindowedFeature{.name = "support_severity",
                        .agg = zetta::ddf::AggregateOp::Sum,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 4.0},
        WindowedFeature{.name = "total_events",
                        .agg = zetta::ddf::AggregateOp::Count},
        WindowedFeature{.name = "mean_usage",
                        .agg = zetta::ddf::AggregateOp::Mean,
                        .value_column = "value",
                        .type_column = "type",
                        .type_value = 2.0},
    };
    spec.account_features = {"mrr", "seats", "tenure_days"};
    spec.label_column = "churned";
    return spec;
}

GbdtParams canonical_params() {
    GbdtParams params;
    params.num_trees = 30;
    params.max_depth = 3;
    params.max_bins = 16;
    params.learning_rate = 0.3;
    params.l2_regularization = 1.0;
    params.min_child_hessian = 1e-3;
    return params;
}

ChurnNumeric load_churn_numeric() {
    const fs::path churn_dir = fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn";
    const auto loaded = zetta::load_churn_dataset(churn_dir);
    EXPECT_TRUE(loaded.ok()) << loaded.error;
    return encode_churn(loaded.dataset);
}

// The single-node featurize reference (left-join, dense over the account universe).
FeatureTable reference_feature_table(const ChurnNumeric& numeric) {
    CpuDevice device;
    auto events =
        load_data_frame_store(numeric.events, events_schema(), device);
    auto accounts =
        load_data_frame_store(numeric.accounts, accounts_schema(), device);
    auto result = point_in_time_featurize(events, accounts, canonical_spec(numeric.as_of));
    EXPECT_TRUE(result.ok()) << result.status.message;
    return std::move(result.table);
}

// =====================================================================
// More shared test helpers (slicing, frames, row encodings)
// =====================================================================

std::vector<Json> slice(const std::vector<Json>& rows, std::size_t begin,
                        std::size_t end) {
    return std::vector<Json>(rows.begin() + std::min(begin, rows.size()),
                             rows.begin() + std::min(end, rows.size()));
}

// A training schema: the feature columns as Float64, the label as Float64.
DataFrameSchema training_schema(const std::vector<std::string>& features,
                                const std::string& label) {
    DataFrameSchema schema;
    for (const std::string& name : features)
        schema.columns.push_back(ColumnSchema{.name = name, .dtype = DataType::Float64});
    schema.columns.push_back(ColumnSchema{.name = label, .dtype = DataType::Float64});
    return schema;
}
// An explain schema: the entity key (Int64) + each feature column (Float64).
DataFrameSchema explain_schema(const std::vector<std::string>& features) {
    DataFrameSchema schema;
    schema.columns.push_back(ColumnSchema{.name = "entity", .dtype = DataType::Int64});
    for (const std::string& name : features)
        schema.columns.push_back(ColumnSchema{.name = name, .dtype = DataType::Float64});
    return schema;
}

// The feature table's rows as training docs (entity dropped; features + label).
std::vector<Json> training_rows(const FeatureTable& table) {
    std::vector<Json> rows;
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        Json doc;
        for (std::size_t c = 0; c < table.feature_count(); ++c)
            doc[table.feature_names[c]] = table.features[c][r];
        doc[table.label_name] = table.labels[r];
        rows.push_back(std::move(doc));
    }
    return rows;
}
// The feature table's rows as {entity, feature columns…} docs for the explain frame.
std::vector<Json> explain_rows(const FeatureTable& table) {
    std::vector<Json> rows;
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        Json doc;
        doc["entity"] = table.entities[r];
        for (std::size_t c = 0; c < table.feature_count(); ++c)
            doc[table.feature_names[c]] = table.features[c][r];
        rows.push_back(std::move(doc));
    }
    return rows;
}

// One feature row (the design-matrix row) for entity index `r`.
std::vector<double> feature_row(const FeatureTable& table, std::size_t r) {
    std::vector<double> row(table.feature_count());
    for (std::size_t c = 0; c < table.feature_count(); ++c) row[c] = table.features[c][r];
    return row;
}

// =====================================================================
// Golden generation: the single-node native references, serialized.
// =====================================================================

GoldenOracle generate_oracle() {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeatureTable table = reference_feature_table(numeric);

    GoldenOracle oracle;
    oracle.dataset = "synthetic-churn-uc1";
    oracle.description =
        "Golden featurize/train/TreeSHAP outputs over tests/fixtures/churn, generated "
        "by zetta's native engine (no foreign runtime). See docs/ml/native-oracle.md.";

    // Featurize.
    oracle.featurize.entities = table.entities;
    oracle.featurize.feature_names = table.feature_names;
    oracle.featurize.features = table.features;
    oracle.featurize.label_name = table.label_name;
    oracle.featurize.labels = table.labels;

    // Train (single-node reference; the distributed path matches it).
    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, canonical_params());
    EXPECT_TRUE(trained.ok()) << trained.status.message;
    oracle.train.feature_names = table.feature_names;
    oracle.train.base_score = trained.model.base_score;
    oracle.train.tree_count = static_cast<std::int64_t>(trained.model.tree_count());
    oracle.train.feature_importances = trained.feature_importances;
    oracle.train.probabilities.resize(table.row_count());
    for (std::size_t r = 0; r < table.row_count(); ++r)
        oracle.train.probabilities[r] = trained.model.predict_proba(feature_row(table, r));
    const GbdtMetrics metrics =
        evaluate_binary(oracle.train.probabilities, table.labels);
    oracle.train.auc = metrics.auc;
    oracle.train.log_loss = metrics.log_loss;
    oracle.train.accuracy = metrics.accuracy;

    // SHAP (exact TreeSHAP over the trained model).
    oracle.shap.feature_names = table.feature_names;
    oracle.shap.base_value = model_base_value(trained.model);
    oracle.shap.mean_abs_shap.assign(table.feature_count(), 0.0);
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        const ShapExplanation shap = tree_shap(trained.model, feature_row(table, r));
        zetta::ml::GoldenEntityShap row;
        row.entity = table.entities[r];
        row.predicted = shap.predicted;
        row.dominant = dominant_driver(trained.model, shap).feature;
        row.values = shap.values;
        for (std::size_t f = 0; f < table.feature_count(); ++f)
            oracle.shap.mean_abs_shap[f] += std::abs(shap.values[f]);
        oracle.shap.per_entity.push_back(std::move(row));
    }
    if (table.row_count() > 0) {
        for (double& value : oracle.shap.mean_abs_shap)
            value /= static_cast<double>(table.row_count());
    }
    std::size_t top = 0;
    for (std::size_t f = 1; f < oracle.shap.mean_abs_shap.size(); ++f)
        if (oracle.shap.mean_abs_shap[f] > oracle.shap.mean_abs_shap[top]) top = f;
    if (!oracle.shap.mean_abs_shap.empty())
        oracle.shap.top_driver = table.feature_names[top];
    return oracle;
}

fs::path golden_path() {
    return fs::path(ZETTA_TEST_FIXTURES_DIR) / "golden" / "churn_oracle.json";
}

GoldenOracle load_golden() {
    const auto loaded = load_golden_oracle(golden_path());
    EXPECT_TRUE(loaded.ok()) << loaded.error
                             << " (regenerate with ZETTA_WRITE_GOLDEN=1)";
    return loaded.oracle;
}

// Write the golden fixture when ZETTA_WRITE_GOLDEN is set — the offline generator,
// run before any test so the read tests see fresh data. A no-op otherwise (CI reads
// the committed file). Registered as a global environment so it runs first.
class GoldenWriteEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        if (std::getenv("ZETTA_WRITE_GOLDEN") == nullptr) return;
        const GoldenOracle oracle = generate_oracle();
        const fs::path path = golden_path();
        std::error_code error_code;
        fs::create_directories(path.parent_path(), error_code);
        std::ofstream out(path);
        out << oracle.to_json().dump(2) << "\n";
    }
};
[[maybe_unused]] const ::testing::Environment* kGoldenEnv =
    ::testing::AddGlobalTestEnvironment(new GoldenWriteEnvironment);

// =====================================================================
// Layer 1 — golden numeric fixtures
// =====================================================================

// Independent leakage-free ground truth for the windowed features, computed straight
// from the raw events (no engine) — proves the featurize golden is *correct*, not just
// pinned to whatever the engine produced.
struct GroundTruth {
    std::map<std::int64_t, double> failed_payments, support_severity, total_events;
};
GroundTruth ground_truth(const ChurnNumeric& numeric) {
    GroundTruth truth;
    for (const Json& event : numeric.events) {
        const auto key = event.at("entity").get<std::int64_t>();
        const auto day = event.at("ts").get<std::int64_t>();
        const auto type = event.at("type").get<std::int64_t>();
        const auto value = event.at("value").get<double>();
        if (day > numeric.as_of) continue;  // leakage cutoff: time <= as_of
        truth.total_events[key] += 1.0;
        if (type == 3) truth.failed_payments[key] += 1.0;
        if (type == 4) truth.support_severity[key] += value;
    }
    return truth;
}

TEST(NativeOracleGolden, FeaturizeMatchesGoldenAndGroundTruth) {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeatureTable table = reference_feature_table(numeric);
    const GoldenOracle golden = load_golden();

    // (a) The native featurize matches the committed golden, exactly (integer/sum
    // aggregates are exact across platforms).
    ASSERT_EQ(table.feature_names, golden.featurize.feature_names);
    ASSERT_EQ(table.entities, golden.featurize.entities);
    ASSERT_EQ(table.feature_count(), golden.featurize.features.size());
    for (std::size_t c = 0; c < table.feature_count(); ++c) {
        ASSERT_EQ(table.features[c].size(), golden.featurize.features[c].size());
        for (std::size_t r = 0; r < table.features[c].size(); ++r)
            EXPECT_DOUBLE_EQ(table.features[c][r], golden.featurize.features[c][r])
                << "feature " << table.feature_names[c] << " row " << r;
    }
    ASSERT_EQ(table.labels.size(), golden.featurize.labels.size());
    for (std::size_t r = 0; r < table.labels.size(); ++r)
        EXPECT_DOUBLE_EQ(table.labels[r], golden.featurize.labels[r]) << "label " << r;

    // (b) The golden's windowed features equal the independent leakage-free ground
    // truth — the golden is verified, not merely a snapshot.
    GroundTruth truth = ground_truth(numeric);
    const std::vector<double>* failed = table.feature("failed_payments");
    const std::vector<double>* support = table.feature("support_severity");
    const std::vector<double>* total = table.feature("total_events");
    ASSERT_NE(failed, nullptr);
    ASSERT_NE(support, nullptr);
    ASSERT_NE(total, nullptr);
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        const std::int64_t key = golden.featurize.entities[r];
        EXPECT_DOUBLE_EQ(golden.featurize.features[index_of(golden.featurize.feature_names,
                                                            "failed_payments")][r],
                         truth.failed_payments.count(key) ? truth.failed_payments[key] : 0.0)
            << "entity " << key;
        EXPECT_DOUBLE_EQ((*support)[r], truth.support_severity.count(key)
                                            ? truth.support_severity[key]
                                            : 0.0)
            << "entity " << key;
        EXPECT_DOUBLE_EQ((*total)[r],
                         truth.total_events.count(key) ? truth.total_events[key] : 0.0)
            << "entity " << key;
    }
}

TEST(NativeOracleGolden, TrainMatchesGolden) {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeatureTable table = reference_feature_table(numeric);
    const GoldenOracle golden = load_golden();

    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, canonical_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;

    EXPECT_EQ(static_cast<std::int64_t>(trained.model.tree_count()),
              golden.train.tree_count);
    EXPECT_NEAR(trained.model.base_score, golden.train.base_score, 1e-9);

    // Per-entity probabilities match the committed golden within tolerance (1e-5
    // absorbs cross-platform transcendental rounding on the committed fixture while
    // still catching any real regression, which moves predictions by far more).
    ASSERT_EQ(table.row_count(), golden.train.probabilities.size());
    for (std::size_t r = 0; r < table.row_count(); ++r)
        EXPECT_NEAR(trained.model.predict_proba(feature_row(table, r)),
                    golden.train.probabilities[r], 1e-5)
            << "entity " << table.entities[r];

    // Metrics + per-feature importances match.
    const GbdtMetrics metrics = evaluate_binary(
        [&] {
            std::vector<double> probabilities(table.row_count());
            for (std::size_t r = 0; r < table.row_count(); ++r)
                probabilities[r] = trained.model.predict_proba(feature_row(table, r));
            return probabilities;
        }(),
        table.labels);
    EXPECT_NEAR(metrics.auc, golden.train.auc, 1e-6);
    EXPECT_NEAR(metrics.log_loss, golden.train.log_loss, 1e-6);
    EXPECT_NEAR(metrics.accuracy, golden.train.accuracy, 1e-9);
    ASSERT_EQ(trained.feature_importances.size(),
              golden.train.feature_importances.size());
    for (std::size_t f = 0; f < trained.feature_importances.size(); ++f)
        EXPECT_NEAR(trained.feature_importances[f], golden.train.feature_importances[f],
                    1e-4 + 1e-4 * std::abs(golden.train.feature_importances[f]))
            << "feature " << table.feature_names[f];

    // Sanity: the committed golden is a meaningful model (the churn signal is learnable
    // on this fixture), and failed_payments is a positive driver.
    EXPECT_GE(golden.train.auc, 0.8);
    const std::size_t failed =
        index_of(golden.train.feature_names, "failed_payments");
    EXPECT_GT(golden.train.feature_importances[failed], 0.0);
}

TEST(NativeOracleGolden, ShapMatchesGoldenAndIsEfficient) {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeatureTable table = reference_feature_table(numeric);
    const GoldenOracle golden = load_golden();

    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, canonical_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    const double base = model_base_value(trained.model);
    EXPECT_NEAR(base, golden.shap.base_value, 1e-7);
    ASSERT_EQ(golden.shap.per_entity.size(), table.row_count());

    for (std::size_t r = 0; r < table.row_count(); ++r) {
        const ShapExplanation shap = tree_shap(trained.model, feature_row(table, r));
        const auto& expected = golden.shap.per_entity[r];
        EXPECT_EQ(table.entities[r], expected.entity);
        EXPECT_NEAR(shap.predicted, expected.predicted, 1e-5) << "entity " << expected.entity;
        ASSERT_EQ(shap.values.size(), expected.values.size());
        for (std::size_t f = 0; f < shap.values.size(); ++f)
            EXPECT_NEAR(shap.values[f], expected.values[f], 1e-5)
                << "entity " << expected.entity << " feature " << table.feature_names[f];
        EXPECT_EQ(dominant_driver(trained.model, shap).feature, expected.dominant)
            << "entity " << expected.entity;

        // The committed golden values themselves satisfy efficiency (a mathematical
        // identity, independent of the engine): base + Σφ == predicted.
        double sum = golden.shap.base_value;
        for (const double value : expected.values) sum += value;
        EXPECT_NEAR(sum, expected.predicted, 1e-6) << "golden entity " << expected.entity;
    }

    // The global driver ranking matches, and failed_payments is the top churn driver.
    ASSERT_EQ(golden.shap.mean_abs_shap.size(), table.feature_count());
    EXPECT_EQ(golden.shap.top_driver, "failed_payments");
}

// The loader is a real validated boundary: malformed golden fails the load with a
// reason instead of silently passing a vacuous test.
TEST(NativeOracleGolden, LoaderRejectsMalformed) {
    TempDir dir;
    auto write = [&](const std::string& name, const Json& doc) {
        const fs::path path = dir.path() / name;
        std::ofstream(path) << doc.dump();
        return path;
    };
    Json good;
    {
        std::ifstream in(golden_path());
        ASSERT_TRUE(in.good()) << "committed golden missing";
        in >> good;
    }
    EXPECT_TRUE(load_golden_oracle(write("good.json", good)).ok());

    {  // missing a required section
        Json bad = good;
        bad.erase("shap");
        EXPECT_FALSE(load_golden_oracle(write("no_shap.json", bad)).ok());
    }
    {  // unknown top-level key
        Json bad = good;
        bad["surprise"] = 1;
        EXPECT_FALSE(load_golden_oracle(write("extra.json", bad)).ok());
    }
    {  // structural rule the schema can't express: a feature column dropped
        Json bad = good;
        bad["featurize"]["features"].erase(0);
        EXPECT_FALSE(load_golden_oracle(write("ragged.json", bad)).ok());
    }
    {  // not JSON at all
        const fs::path path = dir.path() / "garbage.json";
        std::ofstream(path) << "{not json";
        EXPECT_FALSE(load_golden_oracle(path).ok());
    }
    EXPECT_FALSE(load_golden_oracle(dir.path() / "nope.json").ok());  // missing file
}

// =====================================================================
// Layer 2 — analytic micro-tests (closed-form, hand-computable)
// =====================================================================

// One balanced, perfectly-separable feature. With λ=0, learning_rate=1 a single
// depth-1 tree puts the closed-form Newton step in each leaf:
//   p0 = base rate = 0.5  ->  base_score = logit(0.5) = 0,  every Hessian h = 0.25
//   left  (y=0): G = n·(0.5-0) = 0.5n, H = 0.25n  ->  w = -G/H = -2
//   right (y=1): G = n·(0.5-1) = -0.5n            ->  w = +2
// so predict_raw is exactly ∓2, base_value (cover-weighted, symmetric covers) is 0,
// and with one feature SHAP gives it the whole gap: φ0(x) = predict_raw(x) − 0.
TEST(NativeOracleAnalytic, GbdtSplitAndShapClosedForm) {
    constexpr int n = 8;  // per class -> balanced -> base rate exactly 0.5
    std::vector<std::vector<double>> columns(1);
    std::vector<double> labels;
    for (int i = 0; i < n; ++i) { columns[0].push_back(0.0); labels.push_back(0.0); }
    for (int i = 0; i < n; ++i) { columns[0].push_back(1.0); labels.push_back(1.0); }

    GbdtParams params;
    params.num_trees = 1;
    params.max_depth = 1;
    params.max_bins = 4;
    params.learning_rate = 1.0;
    params.l2_regularization = 0.0;
    params.min_split_gain = 0.0;
    params.min_child_hessian = 1e-6;

    const GbdtTrainResult trained = train_gbdt(columns, {"f0"}, labels, params);
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    ASSERT_EQ(trained.model.tree_count(), 1u);
    ASSERT_EQ(trained.model.trees[0].nodes.size(), 3u);  // root + 2 leaves
    EXPECT_EQ(trained.model.trees[0].nodes[0].feature, 0);

    EXPECT_NEAR(trained.model.base_score, 0.0, 1e-12);
    EXPECT_NEAR(trained.model.predict_raw({0.0}), -2.0, 1e-9);
    EXPECT_NEAR(trained.model.predict_raw({1.0}), 2.0, 1e-9);
    EXPECT_NEAR(model_base_value(trained.model), 0.0, 1e-12);

    // SHAP ties to the split: the single feature gets the whole gap.
    const ShapExplanation high = tree_shap(trained.model, {1.0});
    EXPECT_NEAR(high.values[0], 2.0, 1e-9);
    EXPECT_NEAR(high.efficiency_residual(), 0.0, 1e-12);
    const ShapExplanation low = tree_shap(trained.model, {0.0});
    EXPECT_NEAR(low.values[0], -2.0, 1e-9);
    EXPECT_NEAR(low.efficiency_residual(), 0.0, 1e-12);
}

// A hand-built depth-2 tree with asymmetric training cover, where the path-dependent
// TreeSHAP attributions are computable by hand (exercises Algorithm 2's extend/unwind
// beyond a single split):
//   root f0@0.5 (cover 10) -> left leaf a=-1 (cover 6), right node f1@0.5 (cover 4)
//                          -> right.left leaf b=0 (cover 1), right.right leaf c=2 (cover 3)
//   base_value = 0.6·a + 0.1·b + 0.3·c = 0
//   x=(1,1) reaches c=2:  φ0 = -0.6a + 0.075b + 0.525c = 1.65,  φ1 = 0.175(c-b) = 0.35
//   x=(0,0) reaches a=-1: φ0 = -0.7,  φ1 = -0.3
GbdtModel asymmetric_cover_model() {
    GbdtModel model;
    model.base_score = 0.0;
    model.learning_rate = 1.0;
    model.feature_names = {"f0", "f1"};
    RegressionTree tree;
    tree.nodes = {
        TreeNode{.feature = 0, .threshold = 0.5, .left = 1, .right = 2, .cover = 10.0},
        TreeNode{.feature = -1, .value = -1.0, .cover = 6.0},
        TreeNode{.feature = 1, .threshold = 0.5, .left = 3, .right = 4, .cover = 4.0},
        TreeNode{.feature = -1, .value = 0.0, .cover = 1.0},
        TreeNode{.feature = -1, .value = 2.0, .cover = 3.0},
    };
    model.trees = {std::move(tree)};
    return model;
}

TEST(NativeOracleAnalytic, TreeShapAsymmetricCoverClosedForm) {
    const GbdtModel model = asymmetric_cover_model();
    EXPECT_NEAR(model_base_value(model), 0.0, 1e-12);

    const ShapExplanation high = tree_shap(model, {1.0, 1.0});
    EXPECT_NEAR(high.predicted, 2.0, 1e-12);
    EXPECT_NEAR(high.values[0], 1.65, 1e-12);
    EXPECT_NEAR(high.values[1], 0.35, 1e-12);
    EXPECT_NEAR(high.efficiency_residual(), 0.0, 1e-12);
    EXPECT_EQ(dominant_driver(model, high).feature, "f0");

    const ShapExplanation low = tree_shap(model, {0.0, 0.0});
    EXPECT_NEAR(low.predicted, -1.0, 1e-12);
    EXPECT_NEAR(low.values[0], -0.7, 1e-12);
    EXPECT_NEAR(low.values[1], -0.3, 1e-12);
    EXPECT_NEAR(low.efficiency_residual(), 0.0, 1e-12);
}

// =====================================================================
// Layer 3 — cross-cluster equivalence (single == 2 == 3 shards)
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

// Bring up `node_count` loopback nodes (a fully-connected cluster + started
// coordinators), run `body(nodes)`, then tear everything down. Centralizes the harness
// so each stage's runner stays tiny.
template <class Body>
void with_cluster(std::size_t node_count, std::uint16_t base_port, Body body) {
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < node_count; ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier)
            seeds.push_back("127.0.0.1:" + std::to_string(base_port + earlier));
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(base_port + index), seeds, 2));
    }
    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes)
            if (node->cluster.peer_count() != node_count - 1) return false;
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    body(nodes);

    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
}

// Run the featurize ComputeJob across `node_count` nodes; return the global table.
FeatureTable run_featurize(const ChurnNumeric& numeric, const FeaturizeSpec& spec,
                           std::size_t node_count, std::uint16_t base_port) {
    FeatureTable table;
    with_cluster(node_count, base_port, [&](auto& nodes) {
        std::vector<std::unique_ptr<CpuDevice>> devices;
        std::vector<DataFrame> events_frames, accounts_frames;
        const std::size_t evt_per = (numeric.events.size() + node_count - 1) / node_count;
        const std::size_t acc_per = (numeric.accounts.size() + node_count - 1) / node_count;
        for (std::size_t index = 0; index < node_count; ++index) {
            devices.push_back(std::make_unique<CpuDevice>());
            events_frames.push_back(load_data_frame_store(
                slice(numeric.events, index * evt_per, (index + 1) * evt_per),
                events_schema(), *devices[index]));
            accounts_frames.push_back(load_data_frame_store(
                slice(numeric.accounts, index * acc_per, (index + 1) * acc_per),
                accounts_schema(), *devices[index]));
        }
        std::vector<zetta::ddf::DistributedFeaturizeResult> results(node_count);
        std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
        std::vector<std::thread> threads;
        for (std::size_t index = 0; index < node_count; ++index) {
            jobs[index] = nodes[index]->node.create_job(JobKind::Featurize);
            threads.emplace_back([&, index] {
                results[index] = run_featurize_job(nodes[index]->coordinator, *jobs[index],
                                                   events_frames[index],
                                                   accounts_frames[index], spec);
            });
        }
        for (auto& thread : threads) thread.join();
        for (std::size_t index = 0; index < node_count; ++index)
            EXPECT_EQ(results[index].status, JobStatus::Succeeded)
                << "node " << index << ": " << jobs[index]->detail();
        table = std::move(results[0].table);
    });
    return table;
}

// Run the train ComputeJob across `node_count` nodes; return the produced model.
GbdtModel run_train(const std::vector<Json>& rows,
                    const std::vector<std::string>& features, const std::string& label,
                    const GbdtParams& params, std::size_t node_count,
                    std::uint16_t base_port, GbdtMetrics* metrics_out = nullptr) {
    GbdtModel model;
    with_cluster(node_count, base_port, [&](auto& nodes) {
        std::vector<std::unique_ptr<CpuDevice>> devices;
        std::vector<DataFrame> frames;
        const std::size_t per = (rows.size() + node_count - 1) / node_count;
        for (std::size_t index = 0; index < node_count; ++index) {
            devices.push_back(std::make_unique<CpuDevice>());
            frames.push_back(load_data_frame_store(
                slice(rows, index * per, (index + 1) * per),
                training_schema(features, label), *devices[index]));
        }
        std::vector<zetta::ml::DistributedTrainResult> results(node_count);
        std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
        std::vector<std::thread> threads;
        for (std::size_t index = 0; index < node_count; ++index) {
            jobs[index] = nodes[index]->node.create_job(JobKind::Train);
            threads.emplace_back([&, index] {
                results[index] = run_train_job(nodes[index]->coordinator, *jobs[index],
                                               frames[index], features, label, params);
            });
        }
        for (auto& thread : threads) thread.join();
        for (std::size_t index = 0; index < node_count; ++index)
            EXPECT_EQ(results[index].status, JobStatus::Succeeded)
                << "node " << index << ": " << jobs[index]->detail();
        model = results[0].model;
        if (metrics_out != nullptr) *metrics_out = results[0].metrics;
    });
    return model;
}

// Run the explain ComputeJob across `node_count` nodes; return entity -> φ.
std::map<std::int64_t, std::vector<double>> run_explain(
    const GbdtModel& model, const std::vector<Json>& rows,
    const std::vector<std::string>& features, std::size_t node_count,
    std::uint16_t base_port, std::vector<double>* mean_abs_out = nullptr) {
    std::map<std::int64_t, std::vector<double>> by_entity;
    with_cluster(node_count, base_port, [&](auto& nodes) {
        std::vector<std::unique_ptr<CpuDevice>> devices;
        std::vector<DataFrame> frames;
        const std::size_t per = (rows.size() + node_count - 1) / node_count;
        for (std::size_t index = 0; index < node_count; ++index) {
            devices.push_back(std::make_unique<CpuDevice>());
            frames.push_back(load_data_frame_store(
                slice(rows, index * per, (index + 1) * per),
                explain_schema(features), *devices[index]));
        }
        std::vector<zetta::ml::DistributedExplainResult> results(node_count);
        std::vector<std::shared_ptr<ComputeJob>> jobs(node_count);
        std::vector<std::thread> threads;
        for (std::size_t index = 0; index < node_count; ++index) {
            jobs[index] = nodes[index]->node.create_job(JobKind::Explain);
            threads.emplace_back([&, index] {
                results[index] = run_explain_job(nodes[index]->coordinator, *jobs[index],
                                                 model, frames[index], "entity");
            });
        }
        for (auto& thread : threads) thread.join();
        for (std::size_t index = 0; index < node_count; ++index) {
            EXPECT_EQ(results[index].status, JobStatus::Succeeded)
                << "node " << index << ": " << jobs[index]->detail();
            for (const auto& entry : results[index].explanations)
                by_entity[entry.entity] = entry.values;
        }
        if (mean_abs_out != nullptr) *mean_abs_out = results[0].mean_abs_shap;
    });
    return by_entity;
}

TEST(NativeOracleEquivalence, FeaturizeIdenticalAcrossShardCounts) {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeaturizeSpec spec = canonical_spec(numeric.as_of);
    const GoldenOracle golden = load_golden();

    const FeatureTable one = run_featurize(numeric, spec, 1, 24000);
    const FeatureTable two = run_featurize(numeric, spec, 2, 24010);
    const FeatureTable three = run_featurize(numeric, spec, 3, 24020);

    // All shard counts agree, and equal the committed single-node golden.
    ASSERT_EQ(one.entities, golden.featurize.entities);
    ASSERT_EQ(two.entities, golden.featurize.entities);
    ASSERT_EQ(three.entities, golden.featurize.entities);
    for (const std::string& name : golden.featurize.feature_names) {
        const std::vector<double>* a = one.feature(name);
        const std::vector<double>* b = two.feature(name);
        const std::vector<double>* c = three.feature(name);
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        ASSERT_NE(c, nullptr);
        const std::vector<double>& g =
            golden.featurize.features[index_of(golden.featurize.feature_names, name)];
        for (std::size_t r = 0; r < g.size(); ++r) {
            EXPECT_DOUBLE_EQ((*a)[r], g[r]) << name << " row " << r;
            EXPECT_DOUBLE_EQ((*b)[r], g[r]) << name << " row " << r;
            EXPECT_DOUBLE_EQ((*c)[r], g[r]) << name << " row " << r;
        }
    }
}

TEST(NativeOracleEquivalence, TrainIdenticalAcrossShardCounts) {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeatureTable table = reference_feature_table(numeric);
    const std::vector<Json> rows = training_rows(table);
    const GbdtParams params = canonical_params();
    const GoldenOracle golden = load_golden();

    GbdtMetrics m1, m2, m3;
    const GbdtModel one = run_train(rows, table.feature_names, table.label_name, params,
                                    1, 24100, &m1);
    const GbdtModel two = run_train(rows, table.feature_names, table.label_name, params,
                                    2, 24110, &m2);
    const GbdtModel three = run_train(rows, table.feature_names, table.label_name, params,
                                      3, 24120, &m3);

    // AUC is rank-based, so it is stable across shardings; the single-node job
    // reproduces the committed golden.
    EXPECT_NEAR(m1.auc, m2.auc, 1e-9);
    EXPECT_NEAR(m1.auc, m3.auc, 1e-9);
    EXPECT_NEAR(m1.auc, golden.train.auc, 1e-6);
    // Per-row probabilities: the single-node path (identity reduction) matches the
    // committed golden; the resharded models are equivalent only **up to floating-point
    // reduction order** — the standard GBDT guarantee, since the per-bin histogram sums
    // are accumulated in a different order across shardings — so they are compared at
    // the repo's documented cross-shard tolerance (GbdtDistributed uses the same 1e-4).
    for (std::size_t r = 0; r < table.row_count(); ++r) {
        const std::vector<double> x = feature_row(table, r);
        const double reference = one.predict_proba(x);
        EXPECT_NEAR(reference, golden.train.probabilities[r], 1e-5)
            << "entity " << table.entities[r];
        EXPECT_NEAR(two.predict_proba(x), reference, 1e-4)
            << "entity " << table.entities[r];
        EXPECT_NEAR(three.predict_proba(x), reference, 1e-4)
            << "entity " << table.entities[r];
    }
}

TEST(NativeOracleEquivalence, ExplainIdenticalAcrossShardCounts) {
    const ChurnNumeric numeric = load_churn_numeric();
    const FeatureTable table = reference_feature_table(numeric);
    const GbdtTrainResult trained =
        train_gbdt(table.features, table.feature_names, table.labels, canonical_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    const std::vector<Json> rows = explain_rows(table);
    const GoldenOracle golden = load_golden();

    std::vector<double> mean1, mean2, mean3;
    const auto one = run_explain(trained.model, rows, table.feature_names, 1, 24200, &mean1);
    const auto two = run_explain(trained.model, rows, table.feature_names, 2, 24210, &mean2);
    const auto three = run_explain(trained.model, rows, table.feature_names, 3, 24220, &mean3);

    ASSERT_EQ(one.size(), table.row_count());
    ASSERT_EQ(two.size(), table.row_count());
    ASSERT_EQ(three.size(), table.row_count());

    // Per-entity φ are identical regardless of sharding, and match the committed golden.
    for (const auto& expected : golden.shap.per_entity) {
        const auto found = one.find(expected.entity);
        ASSERT_NE(found, one.end()) << "entity " << expected.entity;
        ASSERT_EQ(found->second.size(), expected.values.size());
        for (std::size_t f = 0; f < expected.values.size(); ++f) {
            EXPECT_NEAR(one.at(expected.entity)[f], expected.values[f], 1e-5)
                << "entity " << expected.entity << " feature " << f;
            // SHAP runs over the identical (passed-in) model on every node, so the
            // per-entity attributions are bit-stable across shardings, not just close.
            EXPECT_NEAR(two.at(expected.entity)[f], one.at(expected.entity)[f], 1e-9)
                << "entity " << expected.entity << " feature " << f;
            EXPECT_NEAR(three.at(expected.entity)[f], one.at(expected.entity)[f], 1e-9)
                << "entity " << expected.entity << " feature " << f;
        }
    }
    // The population driver ranking agrees across shardings and with the golden.
    ASSERT_EQ(mean1.size(), golden.shap.mean_abs_shap.size());
    for (std::size_t f = 0; f < mean1.size(); ++f) {
        EXPECT_NEAR(mean1[f], mean2[f], 1e-9);
        EXPECT_NEAR(mean1[f], mean3[f], 1e-9);
        EXPECT_NEAR(mean1[f], golden.shap.mean_abs_shap[f], 1e-5) << "feature " << f;
    }
}

}  // namespace
