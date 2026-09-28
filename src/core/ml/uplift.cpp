#include "zetta/ml/uplift.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <string>
#include <utility>

namespace zetta::ml {
namespace {

constexpr double kTieTolerance = 1e-12;

bool is_binary(double value) { return value == 0.0 || value == 1.0; }

compute::ComputeStatus validate_dataset(const UpliftDataset &dataset,
                                        bool allow_empty = false) {
  const std::size_t rows = dataset.row_count();
  if (rows == 0 && !allow_empty) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: the dataset has no rows");
  }
  if (dataset.feature_names.empty() ||
      dataset.features.size() != dataset.feature_names.size()) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: feature names and columns do not match");
  }
  if (dataset.treatment.size() != rows || dataset.outcome.size() != rows) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: entity, treatment, and outcome columns have different "
        "lengths");
  }
  for (const std::vector<double> &column : dataset.features) {
    if (column.size() != rows) {
      return compute::ComputeStatus::invalid_argument(
          "uplift: feature columns are ragged");
    }
  }
  if (!std::ranges::all_of(dataset.treatment, is_binary)) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: treatment column must be binary {0,1}");
  }
  if (!std::ranges::all_of(dataset.outcome, is_binary)) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: outcome column must be binary {0,1}");
  }
  return compute::ComputeStatus::success();
}

compute::ComputeStatus validate_params(const UpliftParams &params) {
  if (params.holdout_fraction <= 0.0 || params.holdout_fraction >= 1.0) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: holdout_fraction must be greater than 0 and less than 1");
  }
  if (params.targeting_fraction <= 0.0 || params.targeting_fraction > 1.0) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: targeting_fraction must be in (0,1]");
  }
  if (params.overlap_lower < 0.05 || params.overlap_upper > 0.95 ||
      params.overlap_lower >= params.overlap_upper) {
    return compute::ComputeStatus::invalid_argument(
        "uplift: overlap bounds must stay within [0.05,0.95] and lower must be "
        "less than upper");
  }
  return compute::ComputeStatus::success();
}

std::uint64_t mix_hash(std::uint64_t hash, std::uint64_t value) {
  for (int byte = 0; byte < 8; ++byte) {
    hash ^= (value >> (byte * 8)) & 0xffull;
    hash *= 1099511628211ull;
  }
  return hash;
}

bool is_holdout(std::int64_t entity, double treatment,
                const UpliftParams &params) {
  std::uint64_t hash = 1469598103934665603ull;
  hash = mix_hash(hash, static_cast<std::uint64_t>(entity));
  hash = mix_hash(hash, std::bit_cast<std::uint64_t>(treatment));
  hash = mix_hash(hash, params.seed);
  const double unit =
      static_cast<double>(hash >> 11) / static_cast<double>(1ull << 53);
  return unit < params.holdout_fraction;
}

std::vector<std::vector<double>>
select_columns(const std::vector<std::vector<double>> &columns,
               const std::vector<std::size_t> &indices) {
  std::vector<std::vector<double>> selected(columns.size());
  for (std::size_t feature = 0; feature < columns.size(); ++feature) {
    selected[feature].reserve(indices.size());
    for (const std::size_t row : indices) {
      selected[feature].push_back(columns[feature][row]);
    }
  }
  return selected;
}

std::vector<double> select_values(const std::vector<double> &values,
                                  const std::vector<std::size_t> &indices) {
  std::vector<double> selected;
  selected.reserve(indices.size());
  for (const std::size_t row : indices) {
    selected.push_back(values[row]);
  }
  return selected;
}

std::vector<double> feature_row(const UpliftDataset &dataset, std::size_t row) {
  std::vector<double> values(dataset.features.size());
  for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
    values[feature] = dataset.features[feature][row];
  }
  return values;
}

double t_effect(const UpliftModelBundle &models,
                const std::vector<double> &features) {
  return models.treated_outcome.predict_proba(features) -
         models.control_outcome.predict_proba(features);
}

double x_effect(const UpliftModelBundle &models,
                const std::vector<double> &features, double propensity) {
  const double treated = models.treated_effect.predict_raw(features);
  const double control = models.control_effect.predict_raw(features);
  return propensity * control + (1.0 - propensity) * treated;
}

bool eligible(const UpliftScoredRow &row, const UpliftParams &params) {
  return row.propensity >= params.overlap_lower &&
         row.propensity <= params.overlap_upper;
}

double transformed_outcome(const UpliftScoredRow &row) {
  return row.outcome * (row.treatment / row.propensity -
                        (1.0 - row.treatment) / (1.0 - row.propensity));
}

double learner_score(const UpliftScoredRow &row, UpliftLearner learner) {
  return learner == UpliftLearner::T ? row.t_uplift : row.x_uplift;
}

struct CurveResult {
  UpliftLearnerMetrics metrics;
  std::vector<QiniPoint> points;
};

CurveResult make_curve(const std::vector<UpliftScoredRow> &source,
                       UpliftLearner learner, double targeting_fraction) {
  std::vector<UpliftScoredRow> rows = source;
  std::stable_sort(
      rows.begin(), rows.end(),
      [learner](const UpliftScoredRow &left, const UpliftScoredRow &right) {
        const double left_score = learner_score(left, learner);
        const double right_score = learner_score(right, learner);
        if (left_score != right_score) {
          return left_score > right_score;
        }
        return left.entity < right.entity;
      });

  CurveResult result;
  result.points.push_back(QiniPoint{});
  if (rows.empty()) {
    return result;
  }

  std::vector<double> cumulative(rows.size());
  double running = 0.0;
  for (std::size_t index = 0; index < rows.size(); ++index) {
    running += transformed_outcome(rows[index]);
    cumulative[index] = running;
  }

  constexpr std::size_t kCurveIntervals = 20;
  for (std::size_t interval = 1; interval <= kCurveIntervals; ++interval) {
    const std::size_t count = std::max<std::size_t>(
        1, (rows.size() * interval + kCurveIntervals - 1) / kCurveIntervals);
    const std::size_t bounded = std::min(count, rows.size());
    const double fraction =
        static_cast<double>(bounded) / static_cast<double>(rows.size());
    const double total = cumulative[bounded - 1];
    result.points.push_back(QiniPoint{fraction,
                                      total / static_cast<double>(rows.size()),
                                      total / static_cast<double>(bounded)});
  }

  double auuc = 0.0;
  double qini_area = 0.0;
  const double final_gain = result.points.back().incremental_gain;
  for (std::size_t index = 1; index < result.points.size(); ++index) {
    const QiniPoint &left = result.points[index - 1];
    const QiniPoint &right = result.points[index];
    const double width = right.fraction - left.fraction;
    auuc += width * (left.uplift + right.uplift) / 2.0;
    const double left_above_random =
        left.incremental_gain - final_gain * left.fraction;
    const double right_above_random =
        right.incremental_gain - final_gain * right.fraction;
    qini_area += width * (left_above_random + right_above_random) / 2.0;
  }
  const std::size_t target_count = std::max<std::size_t>(
      1, static_cast<std::size_t>(
             std::ceil(targeting_fraction * static_cast<double>(rows.size()))));
  result.metrics.auuc = auuc;
  result.metrics.qini_coefficient = qini_area;
  result.metrics.uplift_at_target =
      cumulative[std::min(target_count, rows.size()) - 1] /
      static_cast<double>(std::min(target_count, rows.size()));
  return result;
}

double bootstrap_target_uplift(std::vector<UpliftScoredRow> rows,
                               UpliftLearner learner,
                               double targeting_fraction) {
  return make_curve(rows, learner, targeting_fraction).metrics.uplift_at_target;
}

std::pair<double, double>
bootstrap_interval(const std::vector<UpliftScoredRow> &rows,
                   UpliftLearner learner, const UpliftParams &params) {
  if (rows.empty() || params.bootstrap_samples == 0) {
    return {0.0, 0.0};
  }
  std::mt19937_64 generator(params.seed);
  std::uniform_int_distribution<std::size_t> pick(0, rows.size() - 1);
  std::vector<double> estimates;
  estimates.reserve(params.bootstrap_samples);
  std::vector<UpliftScoredRow> sample(rows.size());
  for (std::size_t repetition = 0; repetition < params.bootstrap_samples;
       ++repetition) {
    for (UpliftScoredRow &row : sample) {
      row = rows[pick(generator)];
    }
    estimates.push_back(
        bootstrap_target_uplift(sample, learner, params.targeting_fraction));
  }
  std::sort(estimates.begin(), estimates.end());
  const std::size_t lower = static_cast<std::size_t>(
      std::floor(0.025 * static_cast<double>(estimates.size() - 1)));
  const std::size_t upper = static_cast<std::size_t>(
      std::ceil(0.975 * static_cast<double>(estimates.size() - 1)));
  return {estimates[lower], estimates[upper]};
}

PropensitySummary propensity_summary(const std::vector<UpliftScoredRow> &rows) {
  PropensitySummary summary;
  if (rows.empty()) {
    return summary;
  }
  summary.minimum = std::numeric_limits<double>::infinity();
  summary.maximum = -std::numeric_limits<double>::infinity();
  double sum = 0.0;
  for (const UpliftScoredRow &row : rows) {
    summary.minimum = std::min(summary.minimum, row.propensity);
    summary.maximum = std::max(summary.maximum, row.propensity);
    sum += row.propensity;
  }
  summary.mean = sum / static_cast<double>(rows.size());
  double squared = 0.0;
  for (const UpliftScoredRow &row : rows) {
    const double difference = row.propensity - summary.mean;
    squared += difference * difference;
  }
  summary.standard_deviation =
      std::sqrt(squared / static_cast<double>(rows.size()));
  return summary;
}

} // namespace

std::string_view to_string(UpliftLearner learner) {
  switch (learner) {
  case UpliftLearner::T:
    return "t_learner";
  case UpliftLearner::X:
    return "x_learner";
  }
  return "t_learner";
}

UpliftModelFit fit_uplift_models(const UpliftDataset &local,
                                 const UpliftParams &params,
                                 const AllReduce &allreduce,
                                 const std::string &round_tag) {
  UpliftModelFit result;
  if (const compute::ComputeStatus status = validate_dataset(local, true);
      !status.ok()) {
    result.status = status;
    return result;
  }
  if (const compute::ComputeStatus status = validate_params(params);
      !status.ok()) {
    result.status = status;
    return result;
  }

  std::vector<std::size_t> fit_all;
  std::vector<std::size_t> fit_treated;
  std::vector<std::size_t> fit_control;
  for (std::size_t row = 0; row < local.row_count(); ++row) {
    if (is_holdout(local.entities[row], local.treatment[row], params)) {
      continue;
    }
    fit_all.push_back(row);
    (local.treatment[row] == 1.0 ? fit_treated : fit_control).push_back(row);
  }

  GbdtParams binary = params.outcome_model;
  binary.objective = "binary_logistic";
  binary.holdout_fraction = 0.0;
  GbdtParams regression = params.outcome_model;
  regression.objective = "squared_error";
  regression.holdout_fraction = 0.0;

  const auto train = [&](const std::vector<std::size_t> &indices,
                         const std::vector<double> &labels,
                         const GbdtParams &model_params,
                         const std::string &name) {
    return train_gbdt(select_columns(local.features, indices),
                      local.feature_names, labels, model_params, allreduce,
                      round_tag + "." + name);
  };

  GbdtTrainResult propensity = train(
      fit_all, select_values(local.treatment, fit_all), binary, "propensity");
  if (!propensity.ok()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "uplift propensity model: " + propensity.status.message);
    return result;
  }
  GbdtTrainResult treated =
      train(fit_treated, select_values(local.outcome, fit_treated), binary,
            "treated_outcome");
  if (!treated.ok()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "uplift treated outcome model: " + treated.status.message);
    return result;
  }
  GbdtTrainResult control =
      train(fit_control, select_values(local.outcome, fit_control), binary,
            "control_outcome");
  if (!control.ok()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "uplift control outcome model: " + control.status.message);
    return result;
  }

  std::vector<double> treated_effects;
  treated_effects.reserve(fit_treated.size());
  for (const std::size_t row : fit_treated) {
    const std::vector<double> features = feature_row(local, row);
    treated_effects.push_back(local.outcome[row] -
                              control.model.predict_proba(features));
  }
  std::vector<double> control_effects;
  control_effects.reserve(fit_control.size());
  for (const std::size_t row : fit_control) {
    const std::vector<double> features = feature_row(local, row);
    control_effects.push_back(treated.model.predict_proba(features) -
                              local.outcome[row]);
  }

  GbdtTrainResult treated_effect =
      train(fit_treated, treated_effects, regression, "treated_effect");
  if (!treated_effect.ok()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "uplift treated effect model: " + treated_effect.status.message);
    return result;
  }
  GbdtTrainResult control_effect =
      train(fit_control, control_effects, regression, "control_effect");
  if (!control_effect.ok()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "uplift control effect model: " + control_effect.status.message);
    return result;
  }

  result.models.propensity = std::move(propensity.model);
  result.models.treated_outcome = std::move(treated.model);
  result.models.control_outcome = std::move(control.model);
  result.models.treated_effect = std::move(treated_effect.model);
  result.models.control_effect = std::move(control_effect.model);
  result.status = compute::ComputeStatus::success();
  return result;
}

std::vector<UpliftScoredRow> score_uplift_rows(const UpliftDataset &local,
                                               const UpliftModelBundle &models,
                                               const UpliftParams &params) {
  std::vector<UpliftScoredRow> rows;
  rows.reserve(local.row_count());
  for (std::size_t row = 0; row < local.row_count(); ++row) {
    const std::vector<double> features = feature_row(local, row);
    const double propensity = models.propensity.predict_proba(features);
    rows.push_back(UpliftScoredRow{
        local.entities[row], local.treatment[row], local.outcome[row],
        propensity, t_effect(models, features),
        x_effect(models, features, propensity),
        is_holdout(local.entities[row], local.treatment[row], params)});
  }
  return rows;
}

UpliftResult evaluate_uplift(const UpliftModelBundle &models,
                             std::vector<UpliftScoredRow> rows,
                             const UpliftParams &params) {
  UpliftResult result;
  result.models = models;
  if (const compute::ComputeStatus status = validate_params(params);
      !status.ok()) {
    result.status = status;
    return result;
  }
  if (rows.empty()) {
    result.status = compute::ComputeStatus::invalid_argument(
        "uplift: no gathered rows to evaluate");
    return result;
  }

  std::vector<UpliftScoredRow> held_out;
  for (const UpliftScoredRow &row : rows) {
    result.treated_rows += row.treatment == 1.0 ? 1 : 0;
    result.control_rows += row.treatment == 0.0 ? 1 : 0;
    if (!eligible(row, params)) {
      ++result.overlap_excluded;
      continue;
    }
    if (row.held_out) {
      held_out.push_back(row);
    }
  }
  result.held_out_rows = held_out.size();
  if (!held_out.empty()) {
    const CurveResult t_curve =
        make_curve(held_out, UpliftLearner::T, params.targeting_fraction);
    const CurveResult x_curve =
        make_curve(held_out, UpliftLearner::X, params.targeting_fraction);
    result.t_learner = t_curve.metrics;
    result.x_learner = x_curve.metrics;
    const bool x_wins =
        x_curve.metrics.qini_coefficient >
            t_curve.metrics.qini_coefficient + kTieTolerance ||
        (std::abs(x_curve.metrics.qini_coefficient -
                  t_curve.metrics.qini_coefficient) <= kTieTolerance &&
         x_curve.metrics.auuc > t_curve.metrics.auuc + kTieTolerance);
    result.selected_learner = x_wins ? UpliftLearner::X : UpliftLearner::T;
    result.qini_curve = x_wins ? x_curve.points : t_curve.points;

    const auto [lower, upper] =
        bootstrap_interval(held_out, result.selected_learner, params);
    result.uplift_lower = lower;
    result.uplift_upper = upper;
  }
  result.propensity = propensity_summary(rows);
  result.observational =
      result.propensity.standard_deviation > 0.15 ||
      result.propensity.maximum - result.propensity.minimum > 0.60;

  std::stable_sort(
      rows.begin(), rows.end(),
      [&](const UpliftScoredRow &left, const UpliftScoredRow &right) {
        const double left_score = learner_score(left, result.selected_learner);
        const double right_score =
            learner_score(right, result.selected_learner);
        if (left_score != right_score) {
          return left_score > right_score;
        }
        return left.entity < right.entity;
      });
  const std::size_t eligible_rows = rows.size() - result.overlap_excluded;
  const std::size_t recommendation_count =
      eligible_rows == 0
          ? 0
          : std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(
                                         params.targeting_fraction *
                                         static_cast<double>(eligible_rows))));
  std::size_t recommended = 0;
  for (const UpliftScoredRow &row : rows) {
    const bool row_eligible = eligible(row, params);
    const bool recommend = row_eligible && recommended < recommendation_count;
    if (recommend) {
      ++recommended;
    }
    result.treatment_effects.push_back(TreatmentEffect{
        row.entity, learner_score(row, result.selected_learner), row.propensity,
        row.treatment, row.outcome, row_eligible, recommend});
  }
  result.status = compute::ComputeStatus::success();
  return result;
}

UpliftResult train_uplift(const UpliftDataset &dataset,
                          const UpliftParams &params) {
  if (const compute::ComputeStatus status = validate_dataset(dataset);
      !status.ok()) {
    UpliftResult result;
    result.status = status;
    return result;
  }
  const AllReduce identity = [](const std::string &,
                                const std::vector<double> &local,
                                GbdtReduce) { return local; };
  UpliftModelFit fit =
      fit_uplift_models(dataset, params, identity, "uplift.single");
  if (!fit.ok()) {
    UpliftResult result;
    result.status = fit.status;
    return result;
  }
  return evaluate_uplift(
      fit.models, score_uplift_rows(dataset, fit.models, params), params);
}

} // namespace zetta::ml
