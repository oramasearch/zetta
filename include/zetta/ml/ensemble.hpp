#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_status.hpp"
#include "zetta/ml/gbdt.hpp"

// Native out-of-fold stacking (#1057, methodology M-18).
//
// Each base learner is a histogram GBDT trained through the same injected
// allreduce used by the distributed GBDT job. The meta-model is regularized
// logistic regression. Its gradients and row counts are allreduced, so every
// participant fits identical weights from its owned rows. After fitting from
// out-of-fold predictions, every base is refit over all non-holdout training
// rows. Time-dependent data uses expanding-window folds and reserves the latest
// observations for the external holdout.
//
// No pretrained weights or third-party ML runtime are used. See
// docs/ml/ensemble.md.
namespace zetta::ml {

enum class StackingPredictionSource {
    OutOfFold,
    InSample,
};

struct EnsembleBaseSpec {
    std::string name;
    std::vector<std::string> feature_names;
    GbdtParams params;
};

struct EnsembleParams {
    std::size_t folds = 5;
    std::uint64_t seed = 42;
    double holdout_fraction = 0.2;
    std::size_t meta_iterations = 300;
    double meta_learning_rate = 0.1;
    double meta_l2_regularization = 0.01;
    StackingPredictionSource prediction_source =
        StackingPredictionSource::OutOfFold;
    std::vector<EnsembleBaseSpec> bases;
};

struct EnsembleTrainingData {
    std::vector<std::vector<double>> columns;
    std::vector<std::string> feature_names;
    std::vector<double> labels;
    std::vector<std::int64_t> entities;
    std::vector<double> times;

    std::size_t row_count() const noexcept {
        return labels.size();
    }
    bool time_dependent() const noexcept {
        return !times.empty();
    }
};

struct StackerModel {
    double intercept = 0.0;
    std::vector<double> weights;

    double predict_proba(const std::vector<double>& base_predictions) const;
};

struct StackerFitResult {
    StackerModel model;
    compute::ComputeStatus status;
    std::uint64_t rows = 0;

    bool ok() const noexcept {
        return status.ok();
    }
};

struct EnsembleBaseModel {
    std::string name;
    std::vector<std::size_t> feature_indices;
    GbdtModel model;
};

struct EnsembleModel {
    std::vector<std::string> feature_names;
    std::vector<EnsembleBaseModel> bases;
    StackerModel stacker;

    std::vector<double>
    base_predictions(const std::vector<double>& features) const;
    double predict_proba(const std::vector<double>& features) const;
    nlohmann::json to_json() const;
};

struct EnsembleScore {
    std::int64_t entity = 0;
    double label = 0.0;
    double blended_score = 0.0;
    bool held_out = false;
    std::vector<double> base_scores;
};

struct EnsembleTrainResult {
    EnsembleModel model;
    GbdtMetrics oof_metrics;
    std::vector<GbdtMetrics> base_held_out_metrics;
    GbdtMetrics held_out_metrics;
    double best_single_held_out_auc = 0.0;
    double held_out_auc_lift_over_best_single = 0.0;
    std::uint64_t oof_rows = 0;
    std::uint64_t train_rows = 0;
    std::uint64_t held_out_rows = 0;
    std::vector<EnsembleScore> scores;
    compute::ComputeStatus status;

    bool ok() const noexcept {
        return status.ok();
    }
};

using EnsembleAllGather =
    std::function<std::vector<double>(const std::string& tag,
                                      const std::vector<double>& local)>;

StackerFitResult fit_stacker(
    const std::vector<std::vector<double>>& predictions,
    const std::vector<double>& labels, const EnsembleParams& params,
    const AllReduce& allreduce, const std::string& tag = "ensemble.meta");

EnsembleTrainResult train_ensemble(
    const EnsembleTrainingData& data, const EnsembleParams& params,
    const AllReduce& allreduce, const EnsembleAllGather& allgather,
    const std::string& tag = "ensemble");

EnsembleTrainResult train_ensemble(const EnsembleTrainingData& data,
                                   const EnsembleParams& params);

}  // namespace zetta::ml
