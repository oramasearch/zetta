// SPDX-License-Identifier: MIT

#include "zetta/ml/bayesian.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <numeric>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cluster/collectives.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr double kMinimumVariance = 1e-9;
constexpr double kPriorShape = 2.0;
constexpr double kPriorScale = 1.0;

double unit_hash(std::uint64_t value, std::uint64_t seed) {
    value += seed + 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    value ^= value >> 31;
    return static_cast<double>(value >> 11) /
           static_cast<double>(std::uint64_t{1} << 53);
}

double normal_cdf(double value) {
    return 0.5 * std::erfc(-value / std::sqrt(2.0));
}

double normal_quantile(double probability) {
    double lower = -10.0;
    double upper = 10.0;
    for (int iteration = 0; iteration < 100; ++iteration) {
        const double middle = (lower + upper) / 2.0;
        if (normal_cdf(middle) < probability) {
            lower = middle;
        } else {
            upper = middle;
        }
    }
    return (lower + upper) / 2.0;
}

double conformal_quantile(std::vector<double> scores, double coverage) {
    std::sort(scores.begin(), scores.end());
    const double rank =
        std::ceil((static_cast<double>(scores.size()) + 1.0) * coverage);
    const std::size_t index = static_cast<std::size_t>(
        std::clamp(rank - 1.0, 0.0, static_cast<double>(scores.size() - 1)));
    return scores[index];
}

double sample_normal(std::mt19937_64& generator) {
    const double first = std::max(
        std::generate_canonical<double, 53>(generator),
        std::numeric_limits<double>::min());
    const double second = std::generate_canonical<double, 53>(generator);
    return std::sqrt(-2.0 * std::log(first)) *
           std::cos(2.0 * std::numbers::pi * second);
}

double sample_gamma(std::mt19937_64& generator, double shape) {
    if (shape < 1.0) {
        const double uniform = std::max(
            std::generate_canonical<double, 53>(generator),
            std::numeric_limits<double>::min());
        return sample_gamma(generator, shape + 1.0) *
               std::pow(uniform, 1.0 / shape);
    }
    const double d = shape - 1.0 / 3.0;
    const double c = 1.0 / std::sqrt(9.0 * d);
    while (true) {
        const double normal = sample_normal(generator);
        const double base = 1.0 + c * normal;
        if (base <= 0.0) {
            continue;
        }
        const double candidate = base * base * base;
        const double uniform =
            std::generate_canonical<double, 53>(generator);
        if (uniform < 1.0 - 0.0331 * normal * normal * normal * normal) {
            return d * candidate;
        }
        if (std::log(std::max(uniform,
                              std::numeric_limits<double>::min())) <
            0.5 * normal * normal +
                d * (1.0 - candidate + std::log(candidate))) {
            return d * candidate;
        }
    }
}

double sample_inverse_gamma(std::mt19937_64& generator, double shape,
                            double scale) {
    const double gamma = sample_gamma(generator, shape) / scale;
    return 1.0 / std::max(gamma, kMinimumVariance);
}

struct GroupStatistics {
    std::vector<double> counts;
    std::vector<double> sums;
    std::vector<double> sum_squares;
};

GroupStatistics local_statistics(
    const std::vector<HierarchicalObservation>& observations,
    const std::vector<std::string>& group_order, const BayesianParams& params,
    std::vector<HierarchicalObservation>* holdout) {
    std::unordered_map<std::string, std::size_t> indexes;
    for (std::size_t index = 0; index < group_order.size(); ++index) {
        indexes[group_order[index]] = index;
    }
    GroupStatistics statistics;
    statistics.counts.assign(group_order.size(), 0.0);
    statistics.sums.assign(group_order.size(), 0.0);
    statistics.sum_squares.assign(group_order.size(), 0.0);
    for (const HierarchicalObservation& observation : observations) {
        const auto found = indexes.find(observation.group);
        if (found == indexes.end() || !std::isfinite(observation.value)) {
            continue;
        }
        if (unit_hash(observation.key, params.seed) < params.holdout_fraction) {
            if (holdout != nullptr) {
                holdout->push_back(observation);
            }
            continue;
        }
        const std::size_t index = found->second;
        statistics.counts[index] += 1.0;
        statistics.sums[index] += observation.value;
        statistics.sum_squares[index] +=
            observation.value * observation.value;
    }
    return statistics;
}

HierarchicalResult fit_from_statistics(
    const std::vector<std::string>& group_order,
    const GroupStatistics& statistics,
    const std::vector<HierarchicalObservation>& holdout,
    const BayesianParams& params) {
    HierarchicalResult result;
    result.nominal_coverage = params.nominal_coverage;
    if (group_order.empty()) {
        result.error = "bayesian: no groups";
        return result;
    }
    if (params.nominal_coverage <= 0.0 || params.nominal_coverage >= 1.0) {
        result.error = "bayesian: nominal_coverage must be between 0 and 1";
        return result;
    }
    if (params.posterior_draws == 0) {
        result.error = "bayesian: posterior_draws must be positive";
        return result;
    }
    const double total_count =
        std::accumulate(statistics.counts.begin(), statistics.counts.end(), 0.0);
    const double total_sum =
        std::accumulate(statistics.sums.begin(), statistics.sums.end(), 0.0);
    const double total_sum_squares = std::accumulate(
        statistics.sum_squares.begin(), statistics.sum_squares.end(), 0.0);
    if (total_count < 2.0) {
        result.error = "bayesian: fewer than two fitting rows remain after holdout";
        return result;
    }

    double population_mean = total_sum / total_count;
    double observation_variance = std::max(
        (total_sum_squares - total_sum * total_sum / total_count) /
            std::max(1.0, total_count - 1.0),
        kMinimumVariance);
    double group_variance = observation_variance;
    std::vector<double> group_means(group_order.size(), population_mean);
    for (std::size_t index = 0; index < group_order.size(); ++index) {
        if (statistics.counts[index] > 0.0) {
            group_means[index] =
                statistics.sums[index] / statistics.counts[index];
        }
    }

    std::vector<std::vector<double>> draws(group_order.size());
    std::vector<double> population_draws;
    std::vector<double> variance_draws;
    std::mt19937_64 generator(params.seed);
    const std::size_t iterations = params.burn_in + params.posterior_draws;
    for (std::size_t iteration = 0; iteration < iterations; ++iteration) {
        for (std::size_t index = 0; index < group_order.size(); ++index) {
            const double precision =
                statistics.counts[index] / observation_variance +
                1.0 / group_variance;
            const double mean =
                (statistics.sums[index] / observation_variance +
                 population_mean / group_variance) /
                precision;
            group_means[index] =
                mean + sample_normal(generator) / std::sqrt(precision);
        }

        const double population_precision =
            static_cast<double>(group_order.size()) / group_variance + 1e-6;
        const double population_location =
            std::accumulate(group_means.begin(), group_means.end(), 0.0) /
            group_variance / population_precision;
        population_mean = population_location +
                          sample_normal(generator) /
                              std::sqrt(population_precision);

        double residual_sum_squares = 0.0;
        for (std::size_t index = 0; index < group_order.size(); ++index) {
            residual_sum_squares += statistics.sum_squares[index] -
                                    2.0 * group_means[index] *
                                        statistics.sums[index] +
                                    statistics.counts[index] *
                                        group_means[index] * group_means[index];
        }
        observation_variance = sample_inverse_gamma(
            generator, kPriorShape + total_count / 2.0,
            kPriorScale + std::max(0.0, residual_sum_squares) / 2.0);

        double group_sum_squares = 0.0;
        for (const double group_mean : group_means) {
            const double difference = group_mean - population_mean;
            group_sum_squares += difference * difference;
        }
        group_variance = sample_inverse_gamma(
            generator,
            kPriorShape + static_cast<double>(group_order.size()) / 2.0,
            kPriorScale + group_sum_squares / 2.0);

        if (iteration < params.burn_in) {
            continue;
        }
        for (std::size_t index = 0; index < group_order.size(); ++index) {
            draws[index].push_back(group_means[index]);
        }
        population_draws.push_back(population_mean);
        variance_draws.push_back(observation_variance);
    }

    result.population_mean = std::accumulate(population_draws.begin(),
                                             population_draws.end(), 0.0) /
                             static_cast<double>(population_draws.size());
    const double mean_observation_variance =
        std::accumulate(variance_draws.begin(), variance_draws.end(), 0.0) /
        static_cast<double>(variance_draws.size());
    const double tail = (1.0 - params.nominal_coverage) / 2.0;
    const double predictive_z = normal_quantile(1.0 - tail);
    result.groups.reserve(group_order.size());
    std::unordered_map<std::string, std::size_t> result_indexes;
    for (std::size_t index = 0; index < group_order.size(); ++index) {
        std::vector<double>& group_draws = draws[index];
        std::sort(group_draws.begin(), group_draws.end());
        const std::size_t lower_index = static_cast<std::size_t>(
            std::floor(tail * static_cast<double>(group_draws.size() - 1)));
        const std::size_t upper_index = static_cast<std::size_t>(
            std::ceil((1.0 - tail) *
                      static_cast<double>(group_draws.size() - 1)));
        const double posterior_mean =
            std::accumulate(group_draws.begin(), group_draws.end(), 0.0) /
            static_cast<double>(group_draws.size());
        double posterior_variance = 0.0;
        for (const double draw : group_draws) {
            const double difference = draw - posterior_mean;
            posterior_variance += difference * difference;
        }
        posterior_variance /= static_cast<double>(group_draws.size());
        const double predictive_radius =
            predictive_z *
            std::sqrt(mean_observation_variance + posterior_variance);
        GroupPosterior posterior;
        posterior.group = group_order[index];
        posterior.train_rows =
            static_cast<std::uint64_t>(statistics.counts[index]);
        posterior.sample_mean = statistics.counts[index] > 0.0
                                    ? statistics.sums[index] /
                                          statistics.counts[index]
                                    : result.population_mean;
        posterior.posterior_mean = posterior_mean;
        posterior.credible_lower = group_draws[lower_index];
        posterior.credible_upper = group_draws[upper_index];
        posterior.predictive_lower = posterior_mean - predictive_radius;
        posterior.predictive_upper = posterior_mean + predictive_radius;
        result_indexes[posterior.group] = result.groups.size();
        result.groups.push_back(std::move(posterior));
    }

    std::uint64_t covered = 0;
    double width_sum = 0.0;
    for (const HierarchicalObservation& observation : holdout) {
        const auto found = result_indexes.find(observation.group);
        if (found == result_indexes.end()) {
            ++result.excluded_rows;
            continue;
        }
        const GroupPosterior& posterior = result.groups[found->second];
        if (observation.value >= posterior.predictive_lower &&
            observation.value <= posterior.predictive_upper) {
            ++covered;
        }
        width_sum +=
            posterior.predictive_upper - posterior.predictive_lower;
        ++result.holdout_rows;
    }
    result.train_rows = static_cast<std::uint64_t>(total_count);
    if (result.holdout_rows > 0) {
        result.empirical_coverage =
            static_cast<double>(covered) /
            static_cast<double>(result.holdout_rows);
        result.mean_interval_width =
            width_sum / static_cast<double>(result.holdout_rows);
    }
    return result;
}

std::string unique_table_name(const data::Database& database,
                              const std::string& run_id) {
    std::string name = "posterior_" + run_id.substr(0, 8);
    std::size_t suffix = 2;
    while (database.find_table(name)) {
        name = "posterior_" + run_id.substr(0, 8) + "_" +
               std::to_string(suffix++);
    }
    return name;
}

} // namespace

bool UncertaintySplit::valid(std::size_t row_count, std::string* error) const {
    std::vector<unsigned char> membership(row_count, 0);
    const auto add = [&](const std::vector<std::size_t>& indexes,
                         const char* name) {
        for (const std::size_t index : indexes) {
            if (index >= row_count) {
                if (error != nullptr) {
                    *error = std::string("bayesian: ") + name +
                             " index is outside the input";
                }
                return false;
            }
            if (membership[index] != 0) {
                if (error != nullptr) {
                    *error = "bayesian: fit, calibration, and holdout rows must "
                             "be disjoint";
                }
                return false;
            }
            membership[index] = 1;
        }
        return true;
    };
    if (!add(fit, "fit") || !add(calibration, "calibration") ||
        !add(holdout, "holdout")) {
        return false;
    }
    if (fit.empty() || calibration.empty() || holdout.empty()) {
        if (error != nullptr) {
            *error = "bayesian: fit, calibration, and holdout sets must all be "
                     "non-empty";
        }
        return false;
    }
    return true;
}

UncertaintySplit make_uncertainty_split(
    const std::vector<std::uint64_t>& row_keys, double calibration_fraction,
    double holdout_fraction, std::uint64_t seed) {
    UncertaintySplit split;
    for (std::size_t index = 0; index < row_keys.size(); ++index) {
        const double value = unit_hash(row_keys[index], seed);
        if (value < calibration_fraction) {
            split.calibration.push_back(index);
        } else if (value < calibration_fraction + holdout_fraction) {
            split.holdout.push_back(index);
        } else {
            split.fit.push_back(index);
        }
    }
    return split;
}

ConformalRegressionResult split_conformal_regression(
    const std::vector<double>& observations,
    const std::vector<double>& point_predictions,
    const UncertaintySplit& split, double nominal_coverage) {
    ConformalRegressionResult result;
    result.nominal_coverage = nominal_coverage;
    if (observations.size() != point_predictions.size()) {
        result.error = "bayesian: observations and predictions differ in size";
        return result;
    }
    if (nominal_coverage <= 0.0 || nominal_coverage >= 1.0) {
        result.error = "bayesian: nominal_coverage must be between 0 and 1";
        return result;
    }
    if (!split.valid(observations.size(), &result.error)) {
        return result;
    }
    std::vector<double> residuals;
    residuals.reserve(split.calibration.size());
    for (const std::size_t index : split.calibration) {
        residuals.push_back(
            std::abs(observations[index] - point_predictions[index]));
    }
    result.residual_quantile =
        conformal_quantile(std::move(residuals), nominal_coverage);
    result.calibration_rows = split.calibration.size();
    result.holdout_rows = split.holdout.size();
    std::vector<unsigned char> is_holdout(observations.size(), 0);
    for (const std::size_t index : split.holdout) {
        is_holdout[index] = 1;
    }
    std::size_t covered = 0;
    result.intervals.reserve(point_predictions.size());
    for (std::size_t index = 0; index < point_predictions.size(); ++index) {
        const ConformalInterval interval{
            point_predictions[index],
            point_predictions[index] - result.residual_quantile,
            point_predictions[index] + result.residual_quantile};
        result.intervals.push_back(interval);
        if (is_holdout[index] != 0 && observations[index] >= interval.lower &&
            observations[index] <= interval.upper) {
            ++covered;
        }
    }
    result.empirical_coverage =
        static_cast<double>(covered) /
        static_cast<double>(result.holdout_rows);
    result.mean_interval_width = 2.0 * result.residual_quantile;
    return result;
}

ConformalClassificationResult split_conformal_classification(
    const std::vector<std::size_t>& labels,
    const std::vector<std::vector<double>>& class_probabilities,
    const UncertaintySplit& split, double nominal_coverage) {
    ConformalClassificationResult result;
    result.nominal_coverage = nominal_coverage;
    if (labels.size() != class_probabilities.size() || labels.empty()) {
        result.error = "bayesian: labels and class probabilities differ in size";
        return result;
    }
    if (nominal_coverage <= 0.0 || nominal_coverage >= 1.0) {
        result.error = "bayesian: nominal_coverage must be between 0 and 1";
        return result;
    }
    if (!split.valid(labels.size(), &result.error)) {
        return result;
    }
    const std::size_t class_count = class_probabilities.front().size();
    if (class_count < 2) {
        result.error = "bayesian: classification needs at least two classes";
        return result;
    }
    for (std::size_t index = 0; index < labels.size(); ++index) {
        if (class_probabilities[index].size() != class_count ||
            labels[index] >= class_count) {
            result.error = "bayesian: malformed class probabilities or label";
            return result;
        }
    }
    std::vector<double> scores;
    scores.reserve(split.calibration.size());
    for (const std::size_t index : split.calibration) {
        scores.push_back(1.0 - class_probabilities[index][labels[index]]);
    }
    result.score_quantile =
        conformal_quantile(std::move(scores), nominal_coverage);
    result.calibration_rows = split.calibration.size();
    result.holdout_rows = split.holdout.size();
    std::vector<unsigned char> is_holdout(labels.size(), 0);
    for (const std::size_t index : split.holdout) {
        is_holdout[index] = 1;
    }
    std::size_t covered = 0;
    std::size_t total_set_size = 0;
    for (std::size_t row = 0; row < labels.size(); ++row) {
        ClassificationPrediction prediction;
        for (std::size_t label = 0; label < class_count; ++label) {
            if (1.0 - class_probabilities[row][label] <=
                result.score_quantile) {
                prediction.labels.push_back(label);
            }
        }
        if (is_holdout[row] != 0) {
            total_set_size += prediction.labels.size();
            if (std::find(prediction.labels.begin(), prediction.labels.end(),
                          labels[row]) != prediction.labels.end()) {
                ++covered;
            }
        }
        result.prediction_sets.push_back(std::move(prediction));
    }
    result.empirical_coverage =
        static_cast<double>(covered) /
        static_cast<double>(result.holdout_rows);
    result.mean_set_size =
        static_cast<double>(total_set_size) /
        static_cast<double>(result.holdout_rows);
    return result;
}

HierarchicalResult fit_hierarchical_means(
    const std::vector<HierarchicalObservation>& observations,
    const BayesianParams& params) {
    std::set<std::string> unique_groups;
    for (const HierarchicalObservation& observation : observations) {
        if (!observation.group.empty() && std::isfinite(observation.value)) {
            unique_groups.insert(observation.group);
        }
    }
    const std::vector<std::string> group_order(unique_groups.begin(),
                                                unique_groups.end());
    std::vector<HierarchicalObservation> holdout;
    const GroupStatistics statistics =
        local_statistics(observations, group_order, params, &holdout);
    HierarchicalResult result =
        fit_from_statistics(group_order, statistics, holdout, params);
    result.excluded_rows += observations.size() -
                            static_cast<std::size_t>(result.train_rows) -
                            static_cast<std::size_t>(result.holdout_rows);
    return result;
}

HierarchicalResult run_hierarchical_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const std::vector<HierarchicalObservation>& local_observations,
    const std::vector<std::string>& group_order, const BayesianParams& params,
    const std::string& round_tag, std::chrono::milliseconds timeout) {
    HierarchicalResult result;
    coordinator.run(job, [&](compute::JobRound& round) {
        std::vector<HierarchicalObservation> local_holdout;
        const GroupStatistics local = local_statistics(
            local_observations, group_order, params, &local_holdout);
        const auto reduce = [&](const std::string& name,
                                const std::vector<double>& values) {
            return round.allreduce(round_tag + "." + name, values,
                                   cluster::CollectiveOp::Sum, timeout);
        };
        const std::optional<std::vector<double>> counts =
            reduce("counts", local.counts);
        const std::optional<std::vector<double>> sums =
            reduce("sums", local.sums);
        const std::optional<std::vector<double>> sum_squares =
            reduce("sum_squares", local.sum_squares);
        if (!counts || !sums || !sum_squares) {
            result.error = "bayesian: sufficient-statistic allreduce timed out";
            return compute::JobCoordinator::Outcome{false, result.error};
        }
        const GroupStatistics global{*counts, *sums, *sum_squares};
        result =
            fit_from_statistics(group_order, global, local_holdout, params);
        if (!result.ok()) {
            return compute::JobCoordinator::Outcome{false, result.error};
        }
        const double local_covered =
            result.empirical_coverage * static_cast<double>(result.holdout_rows);
        const double local_width =
            result.mean_interval_width * static_cast<double>(result.holdout_rows);
        const std::optional<std::vector<double>> coverage = reduce(
            "coverage", {local_covered, static_cast<double>(result.holdout_rows),
                         local_width, static_cast<double>(result.excluded_rows)});
        if (!coverage || coverage->size() != 4) {
            result.error = "bayesian: coverage allreduce timed out";
            return compute::JobCoordinator::Outcome{false, result.error};
        }
        result.holdout_rows =
            static_cast<std::uint64_t>((*coverage)[1]);
        result.excluded_rows =
            static_cast<std::uint64_t>((*coverage)[3]);
        if (result.holdout_rows > 0) {
            result.empirical_coverage =
                (*coverage)[0] / static_cast<double>(result.holdout_rows);
            result.mean_interval_width =
                (*coverage)[2] / static_cast<double>(result.holdout_rows);
        }
        return compute::JobCoordinator::Outcome{
            true, "hierarchical posterior and holdout coverage complete"};
    });
    return result;
}

BayesianRunResult run_bayesian(
    Node& node, const data::ResolvedDataset& dataset,
    const BayesianParams& params, std::string project, std::string graph,
    std::string model_name, const cluster::Cluster* cluster) {
    BayesianRunResult result;
    const std::shared_ptr<pipeline::Run> run =
        node.create_run(std::move(project), std::move(graph));
    if (!run) {
        result.error = "bayesian: could not create the run record";
        return result;
    }
    // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
    run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
    result.run_id = run->id();
    run->set_status(pipeline::RunStatus::Running);
    const auto fail = [&](std::string reason) {
        run->add_log({util::now_iso8601(), "error", reason});
        run->set_status(pipeline::RunStatus::Failed);
        result.error = std::move(reason);
        return result;
    };
    if (!dataset.ok()) {
        return fail(dataset.error);
    }
    if (params.group_column.empty()) {
        return fail("bayesian: group_column is required");
    }

    query::QueryService service(node, cluster);
    const query::QueryService::Outcome queried = service.run(
        "SELECT * FROM " + dataset.database + "." + dataset.table,
        dataset.database);
    if (!queried.ok()) {
        return fail("bayesian: could not read dataset: " + queried.error);
    }
    std::vector<HierarchicalObservation> observations;
    observations.reserve(queried.rows.size());
    std::uint64_t excluded = 0;
    for (const nlohmann::json& row : queried.rows) {
        const auto entity = row.find(dataset.contract.entity_column);
        const auto group = row.find(params.group_column);
        const auto value = row.find(dataset.contract.label_column);
        if (entity == row.end() || !entity->is_number_integer() ||
            group == row.end() ||
            (!group->is_string() && !group->is_number_integer()) ||
            value == row.end() || !value->is_number()) {
            ++excluded;
            continue;
        }
        observations.push_back(
            {static_cast<std::uint64_t>(entity->get<std::int64_t>()),
             group->is_string() ? group->get<std::string>() : group->dump(),
             value->get<double>()});
    }
    result.summary = fit_hierarchical_means(observations, params);
    result.summary.excluded_rows += excluded;
    if (!result.summary.ok()) {
        return fail(result.summary.error);
    }
    if (result.summary.holdout_rows == 0) {
        return fail("bayesian: no holdout rows remain to measure coverage");
    }

    std::shared_ptr<data::Database> output_database =
        node.find_database("bayesian_results");
    if (!output_database) {
        output_database = node.create_database("bayesian_results");
    }
    if (!output_database) {
        return fail("bayesian: could not create the output database");
    }
    const std::string table_name =
        unique_table_name(*output_database, run->id());
    const std::shared_ptr<data::Table> output_table =
        output_database->create_table(table_name);
    if (!output_table) {
        return fail("bayesian: could not create the posterior output table");
    }
    for (const GroupPosterior& posterior : result.summary.groups) {
        output_table->add_row(
            {{"group", posterior.group},
             {"train_rows", posterior.train_rows},
             {"sample_mean", posterior.sample_mean},
             {"posterior_mean", posterior.posterior_mean},
             {"credible_lower", posterior.credible_lower},
             {"credible_upper", posterior.credible_upper},
             {"predictive_lower", posterior.predictive_lower},
             {"predictive_upper", posterior.predictive_upper}});
    }
    result.output_table = output_database->name() + "." + table_name;

    ml::ModelValue model_value;
    model_value.name = std::move(model_name);
    model_value.kind = "bayesian";
    model_value.project = run->project();
    model_value.run = run->id();
    model_value.feature_schema =
        {{"label", dataset.contract.label_column},
         {"group", params.group_column},
         {"method", "hierarchical_normal_gibbs"}};
    model_value.metrics =
        {{"nominal_coverage", result.summary.nominal_coverage},
         {"empirical_coverage", result.summary.empirical_coverage},
         {"mean_interval_width", result.summary.mean_interval_width},
         {"train_rows", result.summary.train_rows},
         {"held_out_rows", result.summary.holdout_rows},
         {"excluded_rows", result.summary.excluded_rows},
         {"groups", result.summary.groups.size()}};
    const std::shared_ptr<ml::Model> model = node.create_model(model_value);
    if (!model) {
        return fail("bayesian: could not register the model");
    }
    nlohmann::json weights;
    weights["population_mean"] = result.summary.population_mean;
    weights["groups"] = nlohmann::json::array();
    for (const GroupPosterior& posterior : result.summary.groups) {
        weights["groups"].push_back(
            {{"group", posterior.group},
             {"posterior_mean", posterior.posterior_mean},
             {"credible_lower", posterior.credible_lower},
             {"credible_upper", posterior.credible_upper}});
    }
    if (!model->set_weights(weights.dump())) {
        return fail("bayesian: could not persist the posterior weights");
    }
    result.model_id = model->id();

    for (const std::string& note : dataset.notes) {
        run->add_log({util::now_iso8601(), "info", "dataset: " + note});
    }
    run->add_log(
        {util::now_iso8601(), "info",
         "uncertainty: coverage is empirical on rows excluded from fitting; "
         "the interval guarantee assumes exchangeable observations"});
    run->set_lineage(
        {{"dataset:" + dataset.database + "." + dataset.table},
         {"resolve dataset", "hold out evaluation rows",
          "allreduce per-group statistics", "sample hierarchical posterior",
          "measure holdout coverage", "write posterior table"},
         util::now_iso8601(), 1.0});
    run->set_outputs(
        {{"dataset", "group posterior estimates", result.output_table},
         {"model", model->name(), model->id()},
         {"report", "uncertainty report", run->id()}});
    run->set_status(pipeline::RunStatus::Succeeded);
    return result;
}

} // namespace zetta::ml
