// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/dataset_path.hpp"

namespace zetta {
class Node;
}

namespace zetta::cluster {
class Cluster;
}

namespace zetta::ml {

struct UncertaintySplit {
    std::vector<std::size_t> fit;
    std::vector<std::size_t> calibration;
    std::vector<std::size_t> holdout;

    bool valid(std::size_t row_count, std::string* error = nullptr) const;
};

UncertaintySplit make_uncertainty_split(
    const std::vector<std::uint64_t>& row_keys, double calibration_fraction,
    double holdout_fraction, std::uint64_t seed);

struct ConformalInterval {
    double point = 0.0;
    double lower = 0.0;
    double upper = 0.0;
};

struct ConformalRegressionResult {
    std::vector<ConformalInterval> intervals;
    double nominal_coverage = 0.0;
    double empirical_coverage = 0.0;
    double mean_interval_width = 0.0;
    double residual_quantile = 0.0;
    std::size_t calibration_rows = 0;
    std::size_t holdout_rows = 0;
    std::string error;

    bool ok() const { return error.empty(); }
};

ConformalRegressionResult split_conformal_regression(
    const std::vector<double>& observations,
    const std::vector<double>& point_predictions,
    const UncertaintySplit& split, double nominal_coverage);

struct ClassificationPrediction {
    std::vector<std::size_t> labels;
};

struct ConformalClassificationResult {
    std::vector<ClassificationPrediction> prediction_sets;
    double nominal_coverage = 0.0;
    double empirical_coverage = 0.0;
    double mean_set_size = 0.0;
    double score_quantile = 0.0;
    std::size_t calibration_rows = 0;
    std::size_t holdout_rows = 0;
    std::string error;

    bool ok() const { return error.empty(); }
};

ConformalClassificationResult split_conformal_classification(
    const std::vector<std::size_t>& labels,
    const std::vector<std::vector<double>>& class_probabilities,
    const UncertaintySplit& split, double nominal_coverage);

struct HierarchicalObservation {
    std::uint64_t key = 0;
    std::string group;
    double value = 0.0;
};

struct BayesianParams {
    double nominal_coverage = 0.9;
    double holdout_fraction = 0.2;
    std::uint64_t seed = 17;
    std::size_t burn_in = 200;
    std::size_t posterior_draws = 600;
    std::string group_column;
};

struct GroupPosterior {
    std::string group;
    std::uint64_t train_rows = 0;
    double sample_mean = 0.0;
    double posterior_mean = 0.0;
    double credible_lower = 0.0;
    double credible_upper = 0.0;
    double predictive_lower = 0.0;
    double predictive_upper = 0.0;
};

struct HierarchicalResult {
    std::vector<GroupPosterior> groups;
    double population_mean = 0.0;
    double nominal_coverage = 0.0;
    double empirical_coverage = 0.0;
    double mean_interval_width = 0.0;
    std::uint64_t train_rows = 0;
    std::uint64_t holdout_rows = 0;
    std::uint64_t excluded_rows = 0;
    std::string error;

    bool ok() const { return error.empty(); }
};

HierarchicalResult fit_hierarchical_means(
    const std::vector<HierarchicalObservation>& observations,
    const BayesianParams& params = {});

inline constexpr std::chrono::milliseconds kDefaultBayesianTimeout{10000};

HierarchicalResult run_hierarchical_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const std::vector<HierarchicalObservation>& local_observations,
    const std::vector<std::string>& group_order, const BayesianParams& params,
    const std::string& round_tag = "ml.bayesian.hierarchical",
    std::chrono::milliseconds timeout = kDefaultBayesianTimeout);

struct BayesianRunResult {
    std::string run_id;
    std::string model_id;
    std::string output_table;
    HierarchicalResult summary;
    std::string error;

    bool ok() const { return error.empty(); }
};

BayesianRunResult run_bayesian(
    Node& node, const data::ResolvedDataset& dataset,
    const BayesianParams& params, std::string project, std::string graph,
    std::string model_name = "bayesian_hierarchical_model",
    const cluster::Cluster* cluster = nullptr);

} // namespace zetta::ml
