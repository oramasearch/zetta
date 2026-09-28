#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/synthetic_private.hpp"

namespace {

std::vector<nlohmann::json> source_rows() {
    std::vector<nlohmann::json> rows;
    for (std::size_t index = 0; index < 240; ++index) {
        const double first = static_cast<double>(index) / 239.0;
        const double second = std::clamp(
            0.82 * first + 0.12 * std::sin(static_cast<double>(index)), 0.0,
            1.0);
        rows.push_back({{"entity", static_cast<std::int64_t>(index)},
                        {"first", first},
                        {"second", second},
                        {"region", index % 3 == 0 ? "west" : "east"},
                        {"label", first + second > 1.0 ? 1 : 0}});
    }
    return rows;
}

zetta::data::DatasetContract contract() {
    zetta::data::DatasetContract value;
    value.entity_column = "entity";
    value.label_column = "label";
    value.feature_columns = {"first", "second", "region"};
    return value;
}

zetta::ml::SyntheticPrivateParams params(double epsilon = 4.0) {
    zetta::ml::SyntheticPrivateParams value;
    value.epsilon = epsilon;
    value.maximum_epsilon = 8.0;
    value.delta = 1e-6;
    value.maximum_delta = 1e-5;
    value.synthetic_rows = 240;
    value.seed = 1059;
    value.maximum_membership_auc = 0.8;
    value.gbdt.num_trees = 8;
    value.gbdt.max_depth = 2;
    value.gbdt.max_bins = 16;
    value.gbdt.learning_rate = 0.15;
    value.gbdt.min_child_hessian = 0.0;
    return value;
}

double correlation(const std::vector<nlohmann::json>& rows,
                   const std::string& left, const std::string& right) {
    double left_sum = 0.0;
    double right_sum = 0.0;
    for (const nlohmann::json& row : rows) {
        left_sum += row.at(left).get<double>();
        right_sum += row.at(right).get<double>();
    }
    const double left_mean = left_sum / static_cast<double>(rows.size());
    const double right_mean = right_sum / static_cast<double>(rows.size());
    double covariance = 0.0;
    double left_variance = 0.0;
    double right_variance = 0.0;
    for (const nlohmann::json& row : rows) {
        const double left_delta = row.at(left).get<double>() - left_mean;
        const double right_delta = row.at(right).get<double>() - right_mean;
        covariance += left_delta * right_delta;
        left_variance += left_delta * left_delta;
        right_variance += right_delta * right_delta;
    }
    return covariance / std::sqrt(left_variance * right_variance);
}

double private_brier(const zetta::ml::SyntheticPrivateResult& result,
                     const std::vector<nlohmann::json>& rows) {
    double total = 0.0;
    for (const nlohmann::json& row : rows) {
        const std::vector<double> features = {
            row.at("first").get<double>(), row.at("second").get<double>(),
            row.at("region") == "west" ? 1.0 : 0.0};
        const double probability =
            result.private_model.predict_proba(features);
        const double label = row.at("label").get<double>();
        const double error = probability - label;
        total += error * error;
    }
    return total / static_cast<double>(rows.size());
}

}  // namespace

TEST(GaussianCopulaSynthetic, PreservesMarginalsCorrelationsAndCategories) {
    const std::vector<nlohmann::json> source = source_rows();
    const zetta::ml::SyntheticPrivateResult result =
        zetta::ml::run_synthetic_private(source, contract(), params());
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.synthetic_rows.size(), source.size());
    EXPECT_LT(result.metrics.marginal_distance, 0.2);
    EXPECT_LT(result.metrics.correlation_distance, 0.25);
    EXPECT_NEAR(correlation(source, "first", "second"),
                correlation(result.synthetic_rows, "first", "second"), 0.2);
    for (const nlohmann::json& row : result.synthetic_rows) {
        EXPECT_TRUE(row.at("region") == "west" || row.at("region") == "east");
    }
}

TEST(SyntheticPrivacy, CopiesNoSourceRowAndReportsAttackEvaluation) {
    const std::vector<nlohmann::json> source = source_rows();
    const zetta::ml::SyntheticPrivateResult result =
        zetta::ml::run_synthetic_private(source, contract(), params());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_TRUE(result.metrics.privacy_evaluation_passed);
    EXPECT_GT(result.metrics.distance_to_closest_record, 0.0);
    EXPECT_LE(result.metrics.membership_auc, 0.8);
    for (const nlohmann::json& synthetic : result.synthetic_rows) {
        EXPECT_EQ(std::find(source.begin(), source.end(), synthetic), source.end());
    }
}

TEST(SyntheticPrivacy, PublishesNothingWhenEvaluationFails) {
    zetta::ml::SyntheticPrivateParams failing = params();
    failing.maximum_membership_auc = 0.49;
    const zetta::ml::SyntheticPrivateResult result =
        zetta::ml::run_synthetic_private(source_rows(), contract(), failing);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.metrics.privacy_evaluation_passed);
    EXPECT_TRUE(result.synthetic_rows.empty());
    EXPECT_TRUE(result.private_model.trees.empty());
}

TEST(DifferentialPrivacyBudget, RefusesARequestedBudgetAboveTheMaximum) {
    zetta::ml::SyntheticPrivateParams invalid = params();
    invalid.epsilon = 9.0;
    const zetta::ml::SyntheticPrivateResult result =
        zetta::ml::run_synthetic_private(source_rows(), contract(), invalid);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("exceeds"), std::string::npos);
}

TEST(DifferentialPrivateGbdt, ReportsSpendAndDegradesWithSmallerEpsilon) {
    const std::vector<nlohmann::json> source = source_rows();
    const zetta::ml::SyntheticPrivateResult larger =
        zetta::ml::run_synthetic_private(source, contract(), params(6.0));
    const zetta::ml::SyntheticPrivateResult smaller =
        zetta::ml::run_synthetic_private(source, contract(), params(0.35));
    ASSERT_TRUE(larger.ok()) << larger.error;
    ASSERT_TRUE(smaller.ok()) << smaller.error;
    EXPECT_GT(larger.metrics.privacy.epsilon_spent, 0.0);
    EXPECT_LE(larger.metrics.privacy.epsilon_spent, 6.0);
    EXPECT_GT(larger.metrics.privacy.delta_spent, 0.0);
    EXPECT_LE(larger.metrics.privacy.delta_spent, 1e-6);
    EXPECT_LE(smaller.metrics.privacy.epsilon_spent, 0.35);
    EXPECT_TRUE(std::isfinite(larger.private_model.base_score));
    EXPECT_TRUE(std::isfinite(smaller.private_model.base_score));
    EXPECT_LE(private_brier(larger, source), private_brier(smaller, source));
}

TEST(DifferentialPrivateGbdt, MetricHookDoesNotChangeModelOrSpendOrReleaseCurves) {
    const auto source = source_rows();
    for (const auto noise : {zetta::ml::PrivacyNoise::Gaussian,
                             zetta::ml::PrivacyNoise::Laplace}) {
        auto options = params(6.0);
        options.noise = noise;
        const auto baseline =
            zetta::ml::run_synthetic_private(source, contract(), options);
        ASSERT_TRUE(baseline.ok()) << baseline.error;

        std::vector<nlohmann::json> batches;
        zetta::ml::TrainHooks hooks;
        hooks.on_metrics = [&](int step, const auto& values) {
            batches.push_back({{"step", step}, {"values", values}});
            return true;
        };
        const auto tracked =
            zetta::ml::run_synthetic_private(source, contract(), options, hooks);
        ASSERT_TRUE(tracked.ok()) << tracked.error;
        EXPECT_EQ(tracked.private_model.to_json().dump(),
                  baseline.private_model.to_json().dump());
        EXPECT_EQ(tracked.metrics.privacy.epsilon_spent,
                  baseline.metrics.privacy.epsilon_spent);
        EXPECT_EQ(tracked.metrics.privacy.delta_spent,
                  baseline.metrics.privacy.delta_spent);
        EXPECT_EQ(tracked.metrics.privacy.mechanisms,
                  baseline.metrics.privacy.mechanisms);
        EXPECT_EQ(tracked.metrics.to_json().dump(), baseline.metrics.to_json().dump());
        EXPECT_LE(tracked.metrics.privacy.epsilon_spent, options.epsilon);
        EXPECT_LE(tracked.metrics.privacy.delta_spent, options.delta);
        EXPECT_TRUE(batches.empty());
    }
}
