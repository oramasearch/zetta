#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "zetta/ml/ensemble.hpp"

namespace {

zetta::ml::GbdtParams base_params() {
    zetta::ml::GbdtParams params;
    params.num_trees = 45;
    params.max_depth = 2;
    params.max_bins = 24;
    params.learning_rate = 0.12;
    return params;
}

zetta::ml::EnsembleParams ensemble_params() {
    zetta::ml::EnsembleParams params;
    params.folds = 4;
    params.seed = 1057;
    params.holdout_fraction = 0.2;
    params.meta_iterations = 500;
    params.meta_learning_rate = 0.15;
    params.meta_l2_regularization = 0.001;
    params.bases = {
        zetta::ml::EnsembleBaseSpec{"signal_a", {"signal_a"}, base_params()},
        zetta::ml::EnsembleBaseSpec{"signal_b", {"signal_b"}, base_params()}};
    return params;
}

zetta::ml::EnsembleTrainingData complementary_data(std::size_t rows) {
    zetta::ml::EnsembleTrainingData data;
    data.feature_names = {"signal_a", "signal_b"};
    data.columns.resize(2);
    for (std::size_t row = 0; row < rows; ++row) {
        const double signal_a =
            static_cast<double>((row * 37 + 11) % 101) / 100.0;
        const double signal_b =
            static_cast<double>((row * 61 + 7) % 103) / 102.0;
        const double label = signal_a + signal_b > 1.0 ? 1.0 : 0.0;
        data.columns[0].push_back(signal_a);
        data.columns[1].push_back(signal_b);
        data.labels.push_back(label);
        data.entities.push_back(static_cast<std::int64_t>(row + 1));
    }
    return data;
}

}  // namespace

TEST(EnsembleStacker, RefusesInSamplePredictions) {
    const std::vector<std::vector<double>> predictions = {
        {0.1, 0.9, 0.2, 0.8}, {0.2, 0.8, 0.1, 0.9}};
    const std::vector<double> labels = {0.0, 1.0, 0.0, 1.0};
    zetta::ml::EnsembleParams params;
    params.prediction_source = zetta::ml::StackingPredictionSource::InSample;
    const zetta::ml::AllReduce identity =
        [](const std::string&, const std::vector<double>& local,
           zetta::ml::GbdtReduce) { return local; };

    const zetta::ml::StackerFitResult result =
        zetta::ml::fit_stacker(predictions, labels, params, identity);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.status.message.find("refuses in-sample"),
              std::string::npos);
}

TEST(EnsembleStacker, ComplementaryBasesBeatEachSingleModelOnHoldout) {
    const zetta::ml::EnsembleTrainResult result =
        zetta::ml::train_ensemble(complementary_data(1200), ensemble_params());

    ASSERT_TRUE(result.ok()) << result.status.message;
    ASSERT_EQ(result.base_held_out_metrics.size(), 2u);
    EXPECT_GT(result.held_out_metrics.auc,
              result.best_single_held_out_auc + 0.03);
    EXPECT_GT(result.held_out_auc_lift_over_best_single, 0.03);
    EXPECT_GT(result.oof_rows, 0u);
    EXPECT_GT(result.held_out_rows, 0u);
    EXPECT_EQ(result.model.bases.size(), 2u);
    EXPECT_EQ(result.model.stacker.weights.size(), 2u);
}

TEST(EnsembleStacker, SameSeedProducesIdenticalModelAndScores) {
    const zetta::ml::EnsembleTrainingData data = complementary_data(600);
    const zetta::ml::EnsembleParams params = ensemble_params();

    const zetta::ml::EnsembleTrainResult first =
        zetta::ml::train_ensemble(data, params);
    const zetta::ml::EnsembleTrainResult second =
        zetta::ml::train_ensemble(data, params);

    ASSERT_TRUE(first.ok()) << first.status.message;
    ASSERT_TRUE(second.ok()) << second.status.message;
    EXPECT_EQ(first.model.to_json().dump(), second.model.to_json().dump());
    ASSERT_EQ(first.scores.size(), second.scores.size());
    for (std::size_t row = 0; row < first.scores.size(); ++row) {
        EXPECT_DOUBLE_EQ(first.scores[row].blended_score,
                         second.scores[row].blended_score);
    }
}

TEST(EnsembleStacker, TimeDataReservesWarmupAndLatestRows) {
    zetta::ml::EnsembleTrainingData data = complementary_data(800);
    for (std::size_t row = 0; row < data.row_count(); ++row) {
        data.times.push_back(static_cast<double>(row));
    }
    zetta::ml::EnsembleParams params = ensemble_params();
    params.folds = 3;

    const zetta::ml::EnsembleTrainResult result =
        zetta::ml::train_ensemble(data, params);

    ASSERT_TRUE(result.ok()) << result.status.message;
    EXPECT_GT(result.oof_rows, 0u);
    EXPECT_LT(result.oof_rows, result.train_rows);
    EXPECT_GT(result.held_out_rows, 0u);
    for (const zetta::ml::EnsembleScore& score : result.scores) {
        if (score.held_out) {
            EXPECT_GE(score.entity, 641);
        }
    }
}
