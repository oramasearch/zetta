#include "zetta/ml/ensemble.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

constexpr double kProbabilityEpsilon = 1e-12;

double sigmoid(double value) {
    if (value >= 0.0) {
        const double exponent = std::exp(-value);
        return 1.0 / (1.0 + exponent);
    }
    const double exponent = std::exp(value);
    return exponent / (1.0 + exponent);
}

double logit(double probability) {
    const double clamped =
        std::clamp(probability, kProbabilityEpsilon,
                   1.0 - kProbabilityEpsilon);
    return std::log(clamped / (1.0 - clamped));
}

std::uint64_t mix(std::uint64_t value) {
    value ^= value >> 30;
    value *= 0xbf58476d1ce4e5b9ULL;
    value ^= value >> 27;
    value *= 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

double hash_fraction(std::int64_t entity, std::uint64_t seed) {
    const std::uint64_t hashed =
        mix(static_cast<std::uint64_t>(entity) ^ seed);
    return static_cast<double>(hashed >> 11) /
           static_cast<double>(std::uint64_t{1} << 53);
}

std::size_t hash_fold(std::int64_t entity, std::uint64_t seed,
                      std::size_t folds) {
    return static_cast<std::size_t>(
        mix(static_cast<std::uint64_t>(entity) ^ seed) % folds);
}

compute::ComputeStatus validate_data(const EnsembleTrainingData& data,
                                     const EnsembleParams& params) {
    if (data.columns.empty() || data.feature_names.empty() ||
        data.columns.size() != data.feature_names.size()) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble: feature columns and names must be non-empty and aligned");
    }
    for (const std::vector<double>& column : data.columns) {
        if (column.size() != data.row_count()) {
            return compute::ComputeStatus::invalid_argument(
                "ensemble: feature columns are ragged");
        }
    }
    if (data.entities.size() != data.row_count()) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble: entity and label row counts differ");
    }
    if (data.time_dependent() && data.times.size() != data.row_count()) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble: time and label row counts differ");
    }
    if (params.folds < 2) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble: folds must be at least 2");
    }
    if (!(params.holdout_fraction > 0.0 &&
          params.holdout_fraction < 0.5)) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble: holdout_fraction must be greater than 0 and less than 0.5");
    }
    if (params.meta_iterations == 0 || params.meta_learning_rate <= 0.0 ||
        params.meta_l2_regularization < 0.0) {
        return compute::ComputeStatus::invalid_argument(
            "ensemble: invalid meta-model optimization parameters");
    }
    return compute::ComputeStatus::success();
}

std::vector<EnsembleBaseSpec>
base_specs(const EnsembleTrainingData& data, const EnsembleParams& params) {
    if (!params.bases.empty()) {
        return params.bases;
    }
    EnsembleBaseSpec shallow;
    shallow.name = "shallow_gbdt";
    shallow.feature_names = data.feature_names;
    shallow.params.num_trees = 40;
    shallow.params.max_depth = 2;
    shallow.params.learning_rate = 0.15;

    EnsembleBaseSpec deep;
    deep.name = "deep_gbdt";
    deep.feature_names = data.feature_names;
    deep.params.num_trees = 70;
    deep.params.max_depth = 4;
    deep.params.learning_rate = 0.08;
    return {std::move(shallow), std::move(deep)};
}

struct ResolvedBaseSpec {
    EnsembleBaseSpec spec;
    std::vector<std::size_t> indices;
};

std::vector<ResolvedBaseSpec>
resolve_bases(const EnsembleTrainingData& data, const EnsembleParams& params,
              std::string& error) {
    std::vector<ResolvedBaseSpec> resolved;
    for (EnsembleBaseSpec spec : base_specs(data, params)) {
        if (spec.name.empty()) {
            error = "ensemble: every base model needs a name";
            return {};
        }
        if (spec.feature_names.empty()) {
            spec.feature_names = data.feature_names;
        }
        ResolvedBaseSpec base;
        base.spec = std::move(spec);
        for (const std::string& feature : base.spec.feature_names) {
            const auto found =
                std::find(data.feature_names.begin(), data.feature_names.end(),
                          feature);
            if (found == data.feature_names.end()) {
                error = "ensemble: base model '" + base.spec.name +
                        "' names unknown feature '" + feature + "'";
                return {};
            }
            base.indices.push_back(static_cast<std::size_t>(
                found - data.feature_names.begin()));
        }
        resolved.push_back(std::move(base));
    }
    if (resolved.size() < 2) {
        error = "ensemble: stacking needs at least two base models";
        return {};
    }
    return resolved;
}

std::vector<std::vector<double>> subset_columns(
    const EnsembleTrainingData& data, const std::vector<std::size_t>& features,
    const std::vector<std::size_t>& rows) {
    std::vector<std::vector<double>> result(features.size());
    for (std::size_t feature = 0; feature < features.size(); ++feature) {
        result[feature].reserve(rows.size());
        for (const std::size_t row : rows) {
            result[feature].push_back(data.columns[features[feature]][row]);
        }
    }
    return result;
}

std::vector<double> subset_labels(const EnsembleTrainingData& data,
                                  const std::vector<std::size_t>& rows) {
    std::vector<double> result;
    result.reserve(rows.size());
    for (const std::size_t row : rows) {
        result.push_back(data.labels[row]);
    }
    return result;
}

std::vector<double> feature_row(const EnsembleTrainingData& data,
                                const std::vector<std::size_t>& features,
                                std::size_t row) {
    std::vector<double> result;
    result.reserve(features.size());
    for (const std::size_t feature : features) {
        result.push_back(data.columns[feature][row]);
    }
    return result;
}

struct SplitPlan {
    std::vector<bool> held_out;
    std::vector<int> fold;
    double training_time_minimum = 0.0;
    double training_time_maximum = 0.0;
};

std::vector<double> reduce_or_empty(const AllReduce& allreduce,
                                    const std::string& tag,
                                    const std::vector<double>& local,
                                    GbdtReduce operation) {
    if (!allreduce) {
        return local;
    }
    return allreduce(tag, local, operation);
}

SplitPlan build_split_plan(const EnsembleTrainingData& data,
                           const EnsembleParams& params,
                           const AllReduce& allreduce,
                           const std::string& tag, std::string& error) {
    SplitPlan plan;
    plan.held_out.assign(data.row_count(), false);
    plan.fold.assign(data.row_count(), -1);
    if (!data.time_dependent()) {
        for (std::size_t row = 0; row < data.row_count(); ++row) {
            plan.held_out[row] =
                hash_fraction(data.entities[row], params.seed) <
                params.holdout_fraction;
            if (!plan.held_out[row]) {
                plan.fold[row] = static_cast<int>(hash_fold(
                    data.entities[row], params.seed ^ 0x9e3779b97f4a7c15ULL,
                    params.folds));
            }
        }
        return plan;
    }

    double local_minimum = std::numeric_limits<double>::infinity();
    double local_maximum = -std::numeric_limits<double>::infinity();
    for (const double time : data.times) {
        local_minimum = std::min(local_minimum, time);
        local_maximum = std::max(local_maximum, time);
    }
    const std::vector<double> global_minimum = reduce_or_empty(
        allreduce, tag + ".time.minimum", {local_minimum}, GbdtReduce::Min);
    const std::vector<double> global_maximum = reduce_or_empty(
        allreduce, tag + ".time.maximum", {local_maximum}, GbdtReduce::Max);
    if (global_minimum.size() != 1 || global_maximum.size() != 1 ||
        !std::isfinite(global_minimum[0]) ||
        !std::isfinite(global_maximum[0]) ||
        global_maximum[0] <= global_minimum[0]) {
        error = "ensemble: time-dependent data needs at least two distinct times";
        return {};
    }
    const double holdout_start =
        global_minimum[0] +
        (global_maximum[0] - global_minimum[0]) *
            (1.0 - params.holdout_fraction);
    plan.training_time_minimum = global_minimum[0];
    plan.training_time_maximum = holdout_start;
    const double block_width =
        (holdout_start - global_minimum[0]) /
        static_cast<double>(params.folds + 1);
    if (block_width <= 0.0) {
        error = "ensemble: time-dependent fold width is empty";
        return {};
    }
    for (std::size_t row = 0; row < data.row_count(); ++row) {
        if (data.times[row] >= holdout_start) {
            plan.held_out[row] = true;
            continue;
        }
        const double offset = data.times[row] - global_minimum[0];
        const std::size_t block = std::min<std::size_t>(
            params.folds,
            static_cast<std::size_t>(std::max(0.0, offset) / block_width));
        if (block > 0) {
            plan.fold[row] = static_cast<int>(block - 1);
        }
    }
    return plan;
}

std::vector<double> gather_values(const EnsembleAllGather& allgather,
                                  const std::string& tag,
                                  const std::vector<double>& local) {
    if (!allgather) {
        return local;
    }
    return allgather(tag, local);
}

GbdtMetrics gathered_metrics(const EnsembleAllGather& allgather,
                             const std::string& tag,
                             const std::vector<double>& probabilities,
                             const std::vector<double>& labels) {
    std::vector<double> local;
    local.reserve(probabilities.size() * 2);
    for (std::size_t row = 0; row < probabilities.size(); ++row) {
        local.push_back(probabilities[row]);
        local.push_back(labels[row]);
    }
    const std::vector<double> gathered = gather_values(allgather, tag, local);
    std::vector<double> global_probabilities;
    std::vector<double> global_labels;
    for (std::size_t index = 0; index + 1 < gathered.size(); index += 2) {
        global_probabilities.push_back(gathered[index]);
        global_labels.push_back(gathered[index + 1]);
    }
    return evaluate_binary(global_probabilities, global_labels);
}

std::uint64_t global_count(const AllReduce& allreduce, const std::string& tag,
                           std::size_t local) {
    const std::vector<double> reduced = reduce_or_empty(
        allreduce, tag, {static_cast<double>(local)}, GbdtReduce::Sum);
    if (reduced.size() != 1 || reduced[0] < 0.0) {
        return 0;
    }
    return static_cast<std::uint64_t>(reduced[0]);
}

}  // namespace

double StackerModel::predict_proba(
    const std::vector<double>& base_predictions) const {
    if (base_predictions.size() != weights.size()) {
        return 0.5;
    }
    double raw = intercept;
    for (std::size_t index = 0; index < weights.size(); ++index) {
        raw += weights[index] * base_predictions[index];
    }
    return sigmoid(raw);
}

std::vector<double>
EnsembleModel::base_predictions(const std::vector<double>& features) const {
    std::vector<double> predictions;
    predictions.reserve(bases.size());
    for (const EnsembleBaseModel& base : bases) {
        std::vector<double> selected;
        selected.reserve(base.feature_indices.size());
        for (const std::size_t feature : base.feature_indices) {
            if (feature >= features.size()) {
                return {};
            }
            selected.push_back(features[feature]);
        }
        predictions.push_back(base.model.predict_proba(selected));
    }
    return predictions;
}

double EnsembleModel::predict_proba(const std::vector<double>& features) const {
    return stacker.predict_proba(base_predictions(features));
}

nlohmann::json EnsembleModel::to_json() const {
    nlohmann::json bases_json = nlohmann::json::array();
    for (const EnsembleBaseModel& base : bases) {
        bases_json.push_back({{"name", base.name},
                              {"feature_indices", base.feature_indices},
                              {"model", base.model.to_json()}});
    }
    return nlohmann::json{
        {"kind", "ensemble"},
        {"feature_names", feature_names},
        {"bases", std::move(bases_json)},
        {"meta_model",
         {{"kind", "regularized_logistic_regression"},
          {"intercept", stacker.intercept},
          {"weights", stacker.weights}}}};
}

StackerFitResult fit_stacker(
    const std::vector<std::vector<double>>& predictions,
    const std::vector<double>& labels, const EnsembleParams& params,
    const AllReduce& allreduce, const std::string& tag) {
    StackerFitResult result;
    if (params.prediction_source != StackingPredictionSource::OutOfFold) {
        result.status = compute::ComputeStatus::invalid_argument(
            "ensemble: the stacker refuses in-sample predictions; only "
            "out-of-fold predictions may fit the meta-model");
        return result;
    }
    if (predictions.size() < 2) {
        result.status = compute::ComputeStatus::invalid_argument(
            "ensemble: the stacker needs at least two base prediction columns");
        return result;
    }
    for (const std::vector<double>& column : predictions) {
        if (column.size() != labels.size()) {
            result.status = compute::ComputeStatus::invalid_argument(
                "ensemble: meta-model prediction columns are ragged");
            return result;
        }
    }

    double local_positive = 0.0;
    for (const double label : labels) {
        if (label != 0.0 && label != 1.0) {
            result.status = compute::ComputeStatus::invalid_argument(
                "ensemble: labels must be binary");
            return result;
        }
        local_positive += label;
    }
    const std::vector<double> totals = reduce_or_empty(
        allreduce, tag + ".label_totals",
        {static_cast<double>(labels.size()), local_positive}, GbdtReduce::Sum);
    if (totals.size() != 2 || totals[0] <= 1.0 || totals[1] <= 0.0 ||
        totals[1] >= totals[0]) {
        result.status = compute::ComputeStatus::invalid_argument(
            "ensemble: out-of-fold rows need both label classes");
        return result;
    }

    result.model.intercept = logit(totals[1] / totals[0]);
    result.model.weights.assign(predictions.size(), 0.0);
    result.rows = static_cast<std::uint64_t>(totals[0]);
    for (std::size_t iteration = 0; iteration < params.meta_iterations;
         ++iteration) {
        std::vector<double> local_gradient(predictions.size() + 1, 0.0);
        for (std::size_t row = 0; row < labels.size(); ++row) {
            std::vector<double> values(predictions.size());
            for (std::size_t base = 0; base < predictions.size(); ++base) {
                values[base] = predictions[base][row];
            }
            const double residual =
                result.model.predict_proba(values) - labels[row];
            local_gradient[0] += residual;
            for (std::size_t base = 0; base < predictions.size(); ++base) {
                local_gradient[base + 1] += residual * values[base];
            }
        }
        const std::vector<double> gradient = reduce_or_empty(
            allreduce, tag + ".gradient." + std::to_string(iteration),
            local_gradient, GbdtReduce::Sum);
        if (gradient.size() != local_gradient.size()) {
            result.status = compute::ComputeStatus::backend_error(
                "ensemble: meta-model gradient allreduce failed");
            return result;
        }
        result.model.intercept -=
            params.meta_learning_rate * gradient[0] / totals[0];
        for (std::size_t base = 0; base < predictions.size(); ++base) {
            const double regularized =
                gradient[base + 1] / totals[0] +
                params.meta_l2_regularization * result.model.weights[base];
            result.model.weights[base] -=
                params.meta_learning_rate * regularized;
        }
    }
    result.status = compute::ComputeStatus::success();
    return result;
}

compute::ComputeStatus generate_oof_predictions(
    const EnsembleTrainingData& data, const EnsembleParams& params,
    const std::vector<ResolvedBaseSpec>& bases, const SplitPlan& split,
    const AllReduce& allreduce, const std::string& tag,
    std::vector<std::vector<double>>& predictions) {
    predictions.assign(
        bases.size(),
        std::vector<double>(data.row_count(),
                            std::numeric_limits<double>::quiet_NaN()));
    for (std::size_t base_index = 0; base_index < bases.size(); ++base_index) {
        const ResolvedBaseSpec& base = bases[base_index];
        for (std::size_t fold = 0; fold < params.folds; ++fold) {
            std::vector<std::size_t> training_rows;
            std::vector<std::size_t> validation_rows;
            for (std::size_t row = 0; row < data.row_count(); ++row) {
                if (split.held_out[row]) {
                    continue;
                }
                if (data.time_dependent()) {
                    if (split.fold[row] == static_cast<int>(fold)) {
                        validation_rows.push_back(row);
                    } else if (split.fold[row] < static_cast<int>(fold)) {
                        training_rows.push_back(row);
                    }
                } else if (split.fold[row] == static_cast<int>(fold)) {
                    validation_rows.push_back(row);
                } else {
                    training_rows.push_back(row);
                }
            }
            GbdtTrainResult trained = train_gbdt(
                subset_columns(data, base.indices, training_rows),
                base.spec.feature_names, subset_labels(data, training_rows),
                base.spec.params, allreduce,
                tag + ".base." + std::to_string(base_index) + ".fold." +
                    std::to_string(fold));
            if (!trained.ok()) {
                return compute::ComputeStatus::backend_error(
                    "ensemble: OOF base '" + base.spec.name + "' fold " +
                    std::to_string(fold) + " failed: " + trained.status.message);
            }
            for (const std::size_t row : validation_rows) {
                predictions[base_index][row] = trained.model.predict_proba(
                    feature_row(data, base.indices, row));
            }
        }
    }
    return compute::ComputeStatus::success();
}

std::vector<std::size_t> complete_oof_rows(
    const SplitPlan& split,
    const std::vector<std::vector<double>>& predictions) {
    std::vector<std::size_t> rows;
    for (std::size_t row = 0; row < split.fold.size(); ++row) {
        if (split.held_out[row] || split.fold[row] < 0) {
            continue;
        }
        const bool complete = std::all_of(
            predictions.begin(), predictions.end(),
            [row](const std::vector<double>& values) {
                return std::isfinite(values[row]);
            });
        if (complete) {
            rows.push_back(row);
        }
    }
    return rows;
}

std::vector<std::vector<double>> select_oof_predictions(
    const std::vector<std::vector<double>>& predictions,
    const std::vector<std::size_t>& rows) {
    std::vector<std::vector<double>> selected(predictions.size());
    for (std::size_t base = 0; base < predictions.size(); ++base) {
        selected[base].reserve(rows.size());
        for (const std::size_t row : rows) {
            selected[base].push_back(predictions[base][row]);
        }
    }
    return selected;
}

GbdtMetrics evaluate_oof(
    const StackerModel& stacker,
    const std::vector<std::vector<double>>& predictions,
    const std::vector<double>& labels, const EnsembleAllGather& allgather,
    const std::string& tag) {
    std::vector<double> scores;
    scores.reserve(labels.size());
    for (std::size_t row = 0; row < labels.size(); ++row) {
        std::vector<double> values(predictions.size());
        for (std::size_t base = 0; base < predictions.size(); ++base) {
            values[base] = predictions[base][row];
        }
        scores.push_back(stacker.predict_proba(values));
    }
    return gathered_metrics(allgather, tag + ".oof.metrics", scores, labels);
}

compute::ComputeStatus refit_base_models(
    const EnsembleTrainingData& data,
    const std::vector<ResolvedBaseSpec>& bases,
    const std::vector<std::size_t>& training_rows, const AllReduce& allreduce,
    const std::string& tag, EnsembleModel& model) {
    model.feature_names = data.feature_names;
    for (std::size_t base_index = 0; base_index < bases.size(); ++base_index) {
        const ResolvedBaseSpec& base = bases[base_index];
        GbdtTrainResult trained = train_gbdt(
            subset_columns(data, base.indices, training_rows),
            base.spec.feature_names, subset_labels(data, training_rows),
            base.spec.params, allreduce,
            tag + ".base." + std::to_string(base_index) + ".refit");
        if (!trained.ok()) {
            return compute::ComputeStatus::backend_error(
                "ensemble: full refit for base '" + base.spec.name +
                "' failed: " + trained.status.message);
        }
        model.bases.push_back(EnsembleBaseModel{
            base.spec.name, base.indices, std::move(trained.model)});
    }
    return compute::ComputeStatus::success();
}

void evaluate_holdout(
    const EnsembleTrainingData& data,
    const std::vector<std::size_t>& holdout_rows,
    const EnsembleAllGather& allgather, const std::string& tag,
    EnsembleTrainResult& result) {
    std::vector<std::vector<double>> base_scores(result.model.bases.size());
    std::vector<double> labels;
    std::vector<double> stack_scores;
    for (const std::size_t row : holdout_rows) {
        std::vector<double> features(data.columns.size());
        for (std::size_t feature = 0; feature < data.columns.size(); ++feature) {
            features[feature] = data.columns[feature][row];
        }
        const std::vector<double> predictions =
            result.model.base_predictions(features);
        for (std::size_t base = 0; base < predictions.size(); ++base) {
            base_scores[base].push_back(predictions[base]);
        }
        stack_scores.push_back(
            result.model.stacker.predict_proba(predictions));
        labels.push_back(data.labels[row]);
    }
    for (std::size_t base = 0; base < base_scores.size(); ++base) {
        const GbdtMetrics metrics = gathered_metrics(
            allgather, tag + ".base." + std::to_string(base) +
                           ".holdout.metrics",
            base_scores[base], labels);
        result.base_held_out_metrics.push_back(metrics);
        result.best_single_held_out_auc =
            std::max(result.best_single_held_out_auc, metrics.auc);
    }
    result.held_out_metrics = gathered_metrics(
        allgather, tag + ".stack.holdout.metrics", stack_scores, labels);
    result.held_out_auc_lift_over_best_single =
        result.held_out_metrics.auc - result.best_single_held_out_auc;
}

void build_scores(const EnsembleTrainingData& data, const SplitPlan& split,
                  EnsembleTrainResult& result) {
    result.scores.reserve(data.row_count());
    for (std::size_t row = 0; row < data.row_count(); ++row) {
        std::vector<double> features(data.columns.size());
        for (std::size_t feature = 0; feature < data.columns.size(); ++feature) {
            features[feature] = data.columns[feature][row];
        }
        const std::vector<double> predictions =
            result.model.base_predictions(features);
        result.scores.push_back(
            EnsembleScore{data.entities[row], data.labels[row],
                          result.model.stacker.predict_proba(predictions),
                          split.held_out[row], predictions});
    }
}

EnsembleTrainResult train_ensemble(
    const EnsembleTrainingData& data, const EnsembleParams& params,
    const AllReduce& allreduce, const EnsembleAllGather& allgather,
    const std::string& tag) {
    EnsembleTrainResult result;
    result.status = validate_data(data, params);
    if (!result.ok()) {
        return result;
    }

    std::string error;
    const std::vector<ResolvedBaseSpec> bases =
        resolve_bases(data, params, error);
    if (!error.empty()) {
        result.status = compute::ComputeStatus::invalid_argument(error);
        return result;
    }
    const SplitPlan split =
        build_split_plan(data, params, allreduce, tag, error);
    if (!error.empty()) {
        result.status = compute::ComputeStatus::invalid_argument(error);
        return result;
    }

    std::vector<std::vector<double>> oof_predictions;
    result.status = generate_oof_predictions(
        data, params, bases, split, allreduce, tag, oof_predictions);
    if (!result.ok()) {
        return result;
    }
    const std::vector<std::size_t> oof_rows =
        complete_oof_rows(split, oof_predictions);
    const std::vector<std::vector<double>> meta_predictions =
        select_oof_predictions(oof_predictions, oof_rows);
    const std::vector<double> meta_labels = subset_labels(data, oof_rows);
    StackerFitResult stacker = fit_stacker(
        meta_predictions, meta_labels, params, allreduce, tag + ".meta");
    if (!stacker.ok()) {
        result.status = stacker.status;
        return result;
    }
    result.oof_rows = stacker.rows;

    result.oof_metrics = evaluate_oof(stacker.model, meta_predictions,
                                      meta_labels, allgather, tag);

    std::vector<std::size_t> training_rows;
    std::vector<std::size_t> holdout_rows;
    for (std::size_t row = 0; row < data.row_count(); ++row) {
        (split.held_out[row] ? holdout_rows : training_rows).push_back(row);
    }
    result.train_rows = global_count(allreduce, tag + ".train.rows",
                                     training_rows.size());
    result.held_out_rows = global_count(allreduce, tag + ".holdout.rows",
                                        holdout_rows.size());
    if (result.held_out_rows == 0) {
        result.status = compute::ComputeStatus::invalid_argument(
            "ensemble: the external holdout is empty");
        return result;
    }

    result.model.stacker = std::move(stacker.model);
    result.status = refit_base_models(data, bases, training_rows, allreduce, tag,
                                      result.model);
    if (!result.ok()) {
        return result;
    }
    evaluate_holdout(data, holdout_rows, allgather, tag, result);
    build_scores(data, split, result);
    result.status = compute::ComputeStatus::success();
    return result;
}

EnsembleTrainResult train_ensemble(const EnsembleTrainingData& data,
                                   const EnsembleParams& params) {
    const AllReduce identity =
        [](const std::string&, const std::vector<double>& local, GbdtReduce) {
            return local;
        };
    const EnsembleAllGather gather =
        [](const std::string&, const std::vector<double>& local) {
            return local;
        };
    return train_ensemble(data, params, identity, gather);
}

}  // namespace zetta::ml
