// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <format>
#include <ranges>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/featurize_plan.hpp"
#include "zetta/node.hpp"

namespace {

using Json = nlohmann::json;
using zetta::data::DatasetGrain;
using zetta::data::FeaturePlanOptions;
using zetta::data::SemanticType;

struct FeaturizationFixture : ::testing::Test {
    zetta::test::TempDir directory;
    zetta::Node node{zetta::NodeConfig{directory.path().string(), false}};

    void SetUp() override { ASSERT_TRUE(node.start()); }
    void TearDown() override { node.stop(); }

    void land(const std::string& table, std::vector<Json> rows) {
        zetta::data::AddDatasetOptions options;
        options.database = "features";
        options.table_name = table;
        options.partitioned = false;
        const auto added = zetta::data::add_dataset_rows(node, std::move(rows), options);
        ASSERT_TRUE(added.ok()) << added.error;
    }

    zetta::data::DatasetAnalysis analyze(const std::string& table,
                                         const zetta::data::AnalysisOptions& options = {}) {
        return zetta::data::analyze_dataset(
            node, zetta::data::parse_dataset_path("features." + table), options);
    }

    zetta::data::FeaturePlan plan(const std::string& table, FeaturePlanOptions options) {
        return zetta::data::plan_features(
            node, zetta::data::parse_dataset_path("features." + table), options);
    }
};

std::vector<Json> entity_rows() {
    std::vector<Json> rows;
    for (int index = 0; index < 200; ++index) {
        rows.push_back({
            {"customer_id", index + 1},
            {"active", index % 2 == 0},
            {"plan", std::format("plan_{}", index % 4)},
            {"campaign", std::format("campaign_{}", index % 30)},
            {"amount", index == 17 ? -999 : index % 100},
            {"signup_at", std::format("2025-{:02}-{:02}", index % 6 + 1, index % 28 + 1)},
            {"renewal_at", index == 42 ? "2026-01-02" : "2025-08-01"},
            {"notes", "This is a long customer note with several distinct words "
                      "for analysis " +
                          std::to_string(index)},
            {"churned", index % 3 == 0 ? 1 : 0},
            {"churn_copy", index % 3 == 0 ? 1 : 0},
        });
    }
    return rows;
}

std::vector<Json> event_rows() {
    std::vector<Json> rows;
    for (int index = 0; index < 120; ++index) {
        rows.push_back({
            {"event_id", index + 1},
            {"customer_id", index % 12 + 1},
            {"event_time",
             std::format("2025-{:02}-{:02}T12:00:00Z", index % 6 + 1, index % 28 + 1)},
            {"event_type", std::format("kind_{}", index % 3)},
            {"amount", index % 25},
            {"churned", index % 12 == 0 ? 1 : 0},
        });
    }
    return rows;
}

std::vector<Json> time_series_rows() {
    std::vector<Json> rows;
    for (int index = 0; index < 120; ++index) {
        rows.push_back({
            {"entity_id", index % 6 + 1},
            {"timestamp", std::format("2025-{:02}-{:02}T{:02}:00:00Z", index / 28 + 1,
                                      index % 28 + 1, index % 24)},
            {"value", index % 40},
        });
    }
    return rows;
}

const zetta::data::ColumnAnalysis& column(const zetta::data::DatasetAnalysis& analysis,
                                          const std::string& name) {
    const auto found = std::ranges::find_if(
        analysis.columns,
        [&name](const zetta::data::ColumnAnalysis& candidate) { return candidate.name == name; });
    EXPECT_NE(found, analysis.columns.end()) << name;
    return *found;
}

TEST_F(FeaturizationFixture, AnalyzeDatasetInfersSemanticTypesEvidenceAndLeakage) {
    land("accounts", entity_rows());
    zetta::data::AnalysisOptions options;
    options.label_column = "churned";
    options.as_of = "2025-12-31";
    const auto analysis = analyze("accounts", options);

    ASSERT_TRUE(analysis.ok()) << analysis.error;
    EXPECT_EQ(analysis.grain, DatasetGrain::Entity);
    EXPECT_EQ(column(analysis, "customer_id").semantic_type, SemanticType::Identifier);
    EXPECT_EQ(column(analysis, "active").semantic_type, SemanticType::Boolean);
    EXPECT_EQ(column(analysis, "plan").semantic_type, SemanticType::Categorical);
    EXPECT_EQ(column(analysis, "campaign").semantic_type, SemanticType::Categorical);
    EXPECT_GT(column(analysis, "campaign").distinct_count, 20u);
    EXPECT_EQ(column(analysis, "amount").semantic_type, SemanticType::Numeric);
    EXPECT_FALSE(column(analysis, "amount").sentinel_values.empty());
    EXPECT_EQ(column(analysis, "signup_at").semantic_type, SemanticType::Datetime);
    EXPECT_EQ(column(analysis, "notes").semantic_type, SemanticType::FreeText);
    EXPECT_TRUE(std::ranges::any_of(
        analysis.leakage_candidates, [](const zetta::data::LeakageCandidate& candidate) {
            return candidate.column == "churn_copy" && candidate.score >= 0.98;
        }));
    EXPECT_TRUE(std::ranges::any_of(
        analysis.leakage_candidates, [](const zetta::data::LeakageCandidate& candidate) {
            return candidate.column == "renewal_at" &&
                   candidate.reason.find("later than as_of") != std::string::npos;
        }));
    EXPECT_TRUE(
        std::ranges::any_of(analysis.candidate_keys, [](const zetta::data::CandidateKey& key) {
            return key.columns == std::vector<std::string>{"customer_id"} && key.uniqueness > 0.98;
        }));
}

// #1216: the Getting started churn columns. On a small table every measure is
// unique or contiguous in the sample; none of that makes a numeric column a key.
std::vector<Json> getting_started_rows(int count) {
    const std::vector<std::string> plans = {"basic", "pro", "enterprise"};
    std::vector<Json> rows;
    for (int index = 0; index < count; ++index) {
        rows.push_back({{"account_id", 1001 + index},
                        {"support_tickets", index % 11},
                        {"tenure_months", 1 + index},
                        {"monthly_spend", 19.5 + index * 3.25},
                        {"failed_payments", index % 4},
                        {"plan", plans[(index / 2) % 3]},
                        {"churned", index % 3 == 0 ? 1 : 0}});
    }
    return rows;
}

TEST_F(FeaturizationFixture, NumericMeasuresAreNotIdentifiersWithoutAKeyName) {
    land("small_accounts", getting_started_rows(12));
    const auto analysis = analyze("small_accounts");
    ASSERT_TRUE(analysis.ok()) << analysis.error;
    EXPECT_EQ(column(analysis, "account_id").semantic_type, SemanticType::Identifier);
    for (const auto* measure : {"support_tickets", "tenure_months", "monthly_spend",
                                "failed_payments"}) {
        EXPECT_EQ(column(analysis, measure).semantic_type, SemanticType::Numeric) << measure;
    }
    // Only the named key is a candidate key, though tenure_months and
    // monthly_spend are unique on 12 rows.
    ASSERT_EQ(analysis.candidate_keys.size(), 1u);
    EXPECT_EQ(analysis.candidate_keys.front().columns,
              std::vector<std::string>{"account_id"});
    EXPECT_EQ(analysis.grain, DatasetGrain::Entity);

    FeaturePlanOptions options;
    options.methodology = "M-1";
    options.label_column = "churned";
    options.entity_column = "account_id";
    const auto planned = plan("small_accounts", options);
    ASSERT_TRUE(planned.ok()) << planned.error;
    ASSERT_EQ(planned.status, "ready");
    for (const auto* measure : {"support_tickets", "tenure_months", "monthly_spend",
                                "failed_payments"}) {
        EXPECT_TRUE(std::ranges::any_of(planned.entries, [measure](const auto& entry) {
            return entry.source_columns == std::vector<std::string>{measure} &&
                   std::ranges::contains(entry.features, std::string(measure));
        })) << measure;
    }
    EXPECT_FALSE(std::ranges::any_of(planned.excluded, [](const auto& exclusion) {
        return exclusion.reason.find("identifiers are excluded") != std::string::npos &&
               exclusion.source_column != "account_id";
    }));

    // Integer-coded counts on a larger table meet the categorical bounds; a
    // numeric-only trainer still reads them as numbers, never as one-hot.
    land("accounts_400", getting_started_rows(400));
    const auto larger = plan("accounts_400", options);
    ASSERT_TRUE(larger.ok()) << larger.error;
    ASSERT_EQ(larger.status, "ready");
    EXPECT_EQ(column(larger.analysis, "support_tickets").semantic_type,
              SemanticType::Categorical);
    for (const auto* count : {"support_tickets", "failed_payments"}) {
        EXPECT_TRUE(std::ranges::any_of(larger.entries, [count](const auto& entry) {
            return entry.features == std::vector<std::string>{count, std::string(count) +
                                                                         "__missing"};
        })) << count;
    }
}

TEST_F(FeaturizationFixture, AnalyzeDatasetDetectsEventTimeSeriesAndSnapshotGrains) {
    land("events", event_rows());
    land("series", time_series_rows());

    std::vector<Json> snapshots;
    for (int index = 0; index < 60; ++index) {
        snapshots.push_back({{"customer_id", index % 6 + 1},
                             {"snapshot_date", std::format("2025-{:02}-01", index % 10 + 1)},
                             {"balance", index % 20}});
    }
    land("snapshots", std::move(snapshots));

    EXPECT_EQ(analyze("events").grain, DatasetGrain::Event);
    EXPECT_EQ(analyze("series").grain, DatasetGrain::TimeSeries);
    EXPECT_EQ(analyze("snapshots").grain, DatasetGrain::Snapshot);
}

TEST_F(FeaturizationFixture, AnalyzeDatasetFindsJoinableTablesByKeyOverlap) {
    std::vector<Json> accounts;
    std::vector<Json> labels;
    for (int index = 0; index < 40; ++index) {
        accounts.push_back({{"customer_id", index + 1}, {"amount", index % 10}});
        labels.push_back({{"account_key", index + 1}, {"churned", index % 2}});
    }
    land("accounts", std::move(accounts));
    land("labels", std::move(labels));

    const auto analysis = analyze("accounts");
    ASSERT_TRUE(analysis.ok()) << analysis.error;
    EXPECT_TRUE(
        std::ranges::any_of(analysis.joinable_tables, [](const zetta::data::JoinCandidate& join) {
            return join.table == "labels" && join.source_column == "customer_id" &&
                   join.target_column == "account_key" && join.key_overlap == 1.0 &&
                   std::ranges::contains(join.target_roles, "label");
        }));
}

TEST_F(FeaturizationFixture, FeaturePlansDifferForTreesSegmentationAndForecasts) {
    land("accounts", entity_rows());
    land("series", time_series_rows());

    FeaturePlanOptions tree_options;
    tree_options.methodology = "M-1";
    tree_options.label_column = "churned";
    tree_options.time_column = "signup_at";
    tree_options.as_of = "2025-12-31";
    const auto tree = plan("accounts", tree_options);
    ASSERT_TRUE(tree.ok()) << tree.error;
    ASSERT_EQ(tree.status, "ready");
    // M-1's trainer reads numeric columns (#1216) and one-hot encodes a text
    // category itself (#1203): plan (4 levels) and campaign (30 levels) are within
    // the 32-level cap, so they are planned as <column>=<level> indicators fitted on
    // the training rows, never as target-encoded features.
    for (const auto* text : {"plan", "campaign"}) {
        EXPECT_TRUE(std::ranges::any_of(tree.entries, [text](const auto& entry) {
            return entry.source_columns == std::vector<std::string>{text} &&
                   entry.features == std::vector<std::string>{std::string(text) + "=<level>"} &&
                   entry.transform.find("fitted on the training rows only") !=
                       std::string::npos &&
                   entry.transform.find("all zeros") != std::string::npos;
        })) << text;
    }
    // A numeric-only trainer that does not encode categories (M-5 survival) still
    // excludes them, saying how to include them.
    FeaturePlanOptions survival_options = tree_options;
    survival_options.methodology = "M-5";
    const auto survival = plan("accounts", survival_options);
    ASSERT_TRUE(survival.ok()) << survival.error;
    for (const auto* text : {"plan", "campaign"}) {
        EXPECT_TRUE(std::ranges::any_of(survival.excluded, [text](const auto& exclusion) {
            return exclusion.source_column == text &&
                   exclusion.reason.find("reads numeric columns only") != std::string::npos &&
                   exclusion.reason.find("0/1 column per level") != std::string::npos;
        })) << text;
    }
    EXPECT_FALSE(std::ranges::any_of(tree.entries, [](const auto& entry) {
        return std::ranges::any_of(entry.features, [](const std::string& feature) {
            return feature.ends_with("__one_hot") || feature.ends_with("__target_mean");
        });
    }));

    FeaturePlanOptions encoding_options = tree_options;
    encoding_options.methodology = "label_efficiency";
    const auto encoding = plan("accounts", encoding_options);
    ASSERT_TRUE(encoding.ok()) << encoding.error;
    ASSERT_EQ(encoding.status, "ready");
    EXPECT_TRUE(std::ranges::any_of(encoding.entries, [](const auto& entry) {
        return std::ranges::contains(entry.source_columns, "campaign") &&
               entry.transform.find("out-of-fold target encode") != std::string::npos &&
               entry.transform.find("time-aware folds") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(encoding.entries, [](const auto& entry) {
        return std::ranges::contains(entry.features, "plan__one_hot");
    }));
    EXPECT_TRUE(std::ranges::any_of(tree.entries, [](const auto& entry) {
        return std::ranges::contains(entry.source_columns, "amount") &&
               entry.transform.find("no scaling") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(tree.excluded, [](const auto& exclusion) {
        return exclusion.source_column == "notes" && exclusion.reason == "needs M-10 embeddings";
    }));

    FeaturePlanOptions segmentation_options;
    segmentation_options.methodology = "segmentation";
    segmentation_options.as_of = "2025-12-31";
    const auto segmentation = plan("accounts", segmentation_options);
    ASSERT_TRUE(segmentation.ok()) << segmentation.error;
    ASSERT_EQ(segmentation.status, "ready");
    EXPECT_TRUE(std::ranges::any_of(segmentation.entries, [](const auto& entry) {
        return std::ranges::contains(entry.source_columns, "amount") &&
               entry.transform.find("robust-scale") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(segmentation.excluded, [](const auto& exclusion) {
        return exclusion.source_column == "customer_id";
    }));

    FeaturePlanOptions forecast_options;
    forecast_options.methodology = "forecast";
    forecast_options.label_column = "value";
    forecast_options.time_column = "timestamp";
    const auto forecast = plan("series", forecast_options);
    ASSERT_TRUE(forecast.ok()) << forecast.error;
    ASSERT_EQ(forecast.status, "ready");
    EXPECT_TRUE(std::ranges::any_of(forecast.entries, [](const auto& entry) {
        return std::ranges::contains(entry.features, "value__lag_7") &&
               entry.transform.find("temporal split") != std::string::npos;
    }));
}

TEST(FeaturePlanRules, SpeechBanditAndOptimizationAliasesUseNativeRules) {
    for (const auto *alias : {"M-14", "speech", "audio", "M-15", "bandit", "bandits",
                              "sequential_decisioning", "M-16", "optimization",
                              "operations_research"}) {
        SCOPED_TRACE(alias);
        const auto rule = zetta::data::methodology_rule(alias);
        ASSERT_TRUE(rule);
        EXPECT_EQ(rule->family, zetta::data::MethodologyFamily::Native);
        EXPECT_FALSE(rule->prediction_per_entity);
        EXPECT_EQ(rule->supervised, rule->canonical_name == "M-15");
    }
}

TEST(FeaturePlanRules, RoundFourAliasesResolveToTheirExecutionFamilies) {
    const std::vector<std::pair<std::string, std::vector<std::string>>> aliases = {
        {"M-17", {"M-17", "bayesian", "probabilistic", "uncertainty"}},
        {"M-18", {"M-18", "ensemble", "ensembling", "stacking"}},
        {"M-19", {"M-19", "label_efficiency", "active_learning", "weak_supervision"}},
        {"M-20", {"M-20", "synthetic_private", "privacy_preserving", "synthetic_data"}},
        {"M-21", {"M-21", "online_learning", "streaming"}},
    };
    for (const auto& [id, names] : aliases) {
        for (const auto& name : names) {
            SCOPED_TRACE(name);
            const auto rule = zetta::data::methodology_rule(name);
            ASSERT_TRUE(rule);
            EXPECT_EQ(rule->canonical_name, id);
            EXPECT_EQ(rule->family, id == "M-18" ? zetta::data::MethodologyFamily::Tree
                                                : zetta::data::MethodologyFamily::Native);
            EXPECT_EQ(rule->supervised, id != "M-21");
            EXPECT_EQ(rule->prediction_per_entity, id == "M-18" || id == "M-19" || id == "M-20");
        }
    }
}

TEST_F(FeaturizationFixture, FeaturePlanRequiresAnApprovedEventAggregation) {
    land("events", event_rows());
    FeaturePlanOptions options;
    options.methodology = "M-1";
    options.label_column = "churned";
    options.time_column = "event_time";
    options.as_of = "2025-12-31T00:00:00Z";

    const auto plan_result = plan("events", options);
    ASSERT_TRUE(plan_result.ok()) << plan_result.error;
    ASSERT_EQ(plan_result.status, "transform_required");
    ASSERT_TRUE(plan_result.transform_required);
    EXPECT_EQ(plan_result.transform_required->name, "aggregate_events_to_entity");
    EXPECT_NE(plan_result.transform_required->proposed_sql.find("COUNT(*) AS event_count"),
              std::string::npos);
    EXPECT_NE(plan_result.transform_required->proposed_sql.find("GROUP BY customer_id"),
              std::string::npos);
}

TEST_F(FeaturizationFixture, FeaturePlanRefusesMissingSnapshotAndLongData) {
    land("accounts", entity_rows());
    FeaturePlanOptions entity_options;
    entity_options.methodology = "M-1";
    entity_options.label_column = "churned";
    const auto missing_snapshot = plan("accounts", entity_options);
    ASSERT_EQ(missing_snapshot.status, "transform_required");
    ASSERT_TRUE(missing_snapshot.transform_required);
    EXPECT_EQ(missing_snapshot.transform_required->name, "add_snapshot_date");

    std::vector<Json> long_rows;
    for (int index = 0; index < 40; ++index) {
        long_rows.push_back({{"customer_id", index % 10 + 1},
                             {"metric", index % 2 == 0 ? "balance" : "usage"},
                             {"value", index % 12},
                             {"churned", index % 3 == 0 ? 1 : 0}});
    }
    land("long_measurements", std::move(long_rows));
    FeaturePlanOptions long_options;
    long_options.methodology = "M-1";
    long_options.label_column = "churned";
    const auto pivot = plan("long_measurements", long_options);
    ASSERT_EQ(pivot.status, "transform_required");
    ASSERT_TRUE(pivot.transform_required);
    EXPECT_EQ(pivot.transform_required->name, "pivot_long_to_wide");
}

} // namespace
