// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/bayesian.hpp"

namespace {

using namespace std::chrono_literals;

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::compute::ComputeJob;
using zetta::compute::ComputeJobValue;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::ml::BayesianParams;
using zetta::ml::HierarchicalObservation;
using zetta::test::TempDir;

std::vector<std::uint64_t> keys(std::size_t count) {
    std::vector<std::uint64_t> values(count);
    for (std::size_t index = 0; index < count; ++index) {
        values[index] = index + 1;
    }
    return values;
}

bool wait_for_cluster(Cluster& first, Cluster& second) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (first.peer_count() == 1 && second.peer_count() == 1) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

ComputeJob make_job(const TempDir& directory, const std::string& id) {
    ComputeJobValue value;
    value.kind = JobKind::Train;
    return ComputeJob(id, value,
                      std::filesystem::path(directory.path()) / id);
}

} // namespace

TEST(ConformalRegression, HoldoutCoverageTracksNominalCoverage) {
    constexpr std::size_t row_count = 2000;
    std::vector<double> observations;
    std::vector<double> predictions;
    observations.reserve(row_count);
    predictions.reserve(row_count);
    std::mt19937_64 generator(81);
    std::normal_distribution<double> noise(0.0, 1.0);
    for (std::size_t index = 0; index < row_count; ++index) {
        const double point = static_cast<double>(index % 37) / 5.0;
        predictions.push_back(point);
        observations.push_back(point + noise(generator));
    }
    const zetta::ml::UncertaintySplit split =
        zetta::ml::make_uncertainty_split(keys(row_count), 0.25, 0.25, 19);
    const zetta::ml::ConformalRegressionResult result =
        zetta::ml::split_conformal_regression(observations, predictions, split,
                                              0.9);

    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_NEAR(result.empirical_coverage, 0.9, 0.04);
    EXPECT_GT(result.mean_interval_width, 0.0);
    EXPECT_EQ(result.calibration_rows, split.calibration.size());
    EXPECT_EQ(result.holdout_rows, split.holdout.size());
}

TEST(ConformalClassification, PredictionSetsReachNominalCoverage) {
    constexpr std::size_t row_count = 1600;
    std::vector<std::size_t> labels;
    std::vector<std::vector<double>> probabilities;
    labels.reserve(row_count);
    probabilities.reserve(row_count);
    std::mt19937_64 generator(92);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (std::size_t index = 0; index < row_count; ++index) {
        const bool positive = index % 2 == 0;
        const double confidence = 0.55 + 0.4 * uniform(generator);
        labels.push_back(positive ? 1u : 0u);
        probabilities.push_back(positive
                                    ? std::vector<double>{1.0 - confidence,
                                                          confidence}
                                    : std::vector<double>{confidence,
                                                          1.0 - confidence});
    }
    const zetta::ml::UncertaintySplit split =
        zetta::ml::make_uncertainty_split(keys(row_count), 0.25, 0.25, 31);
    const zetta::ml::ConformalClassificationResult result =
        zetta::ml::split_conformal_classification(labels, probabilities, split,
                                                  0.9);

    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_GE(result.empirical_coverage, 0.88);
    EXPECT_LE(result.empirical_coverage, 1.0);
    EXPECT_GT(result.mean_set_size, 0.0);
}

TEST(ConformalCoverage, CalibrationAndFittingRowsMustBeDisjoint) {
    zetta::ml::UncertaintySplit split;
    split.fit = {0, 1};
    split.calibration = {1, 2};
    split.holdout = {3};
    const zetta::ml::ConformalRegressionResult result =
        zetta::ml::split_conformal_regression(
            {0.0, 1.0, 2.0, 3.0}, {0.0, 1.0, 2.0, 3.0}, split, 0.9);

    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("disjoint"), std::string::npos);
}

TEST(BayesianHierarchical, SmallGroupsShrinkMoreTowardThePopulationMean) {
    std::vector<HierarchicalObservation> observations;
    std::uint64_t key = 1;
    for (int index = 0; index < 200; ++index) {
        observations.push_back(
            {key++, "large", 8.0 + static_cast<double>(index % 7) * 0.1});
    }
    for (int index = 0; index < 4; ++index) {
        observations.push_back(
            {key++, "small", 14.0 + static_cast<double>(index) * 0.1});
    }
    BayesianParams params;
    params.holdout_fraction = 0.15;
    params.seed = 44;
    params.burn_in = 150;
    params.posterior_draws = 500;
    const zetta::ml::HierarchicalResult result =
        zetta::ml::fit_hierarchical_means(observations, params);

    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.groups.size(), 2u);
    const auto small = std::find_if(
        result.groups.begin(), result.groups.end(),
        [](const zetta::ml::GroupPosterior& group) {
            return group.group == "small";
        });
    const auto large = std::find_if(
        result.groups.begin(), result.groups.end(),
        [](const zetta::ml::GroupPosterior& group) {
            return group.group == "large";
        });
    ASSERT_NE(small, result.groups.end());
    ASSERT_NE(large, result.groups.end());
    const double small_shrinkage =
        std::abs(small->sample_mean - small->posterior_mean);
    const double large_shrinkage =
        std::abs(large->sample_mean - large->posterior_mean);
    EXPECT_GT(small_shrinkage, large_shrinkage);
    EXPECT_LT(std::abs(small->posterior_mean - result.population_mean),
              std::abs(small->sample_mean - result.population_mean));
    EXPECT_LT(small->credible_lower, small->posterior_mean);
    EXPECT_GT(small->credible_upper, small->posterior_mean);
}

TEST(BayesianHierarchical, DistributedStatisticsMatchSingleMemberResult) {
    std::vector<HierarchicalObservation> all;
    for (std::uint64_t index = 0; index < 240; ++index) {
        const std::string group = "group-" + std::to_string(index % 3);
        all.push_back({index + 1, group,
                       2.0 * static_cast<double>(index % 3) +
                           static_cast<double>(index % 11) / 10.0});
    }
    std::vector<HierarchicalObservation> first_rows;
    std::vector<HierarchicalObservation> second_rows;
    for (std::size_t index = 0; index < all.size(); ++index) {
        (index % 2 == 0 ? first_rows : second_rows).push_back(all[index]);
    }

    ClusterConfig first_config;
    first_config.listen_host = "127.0.0.1";
    first_config.listen_port = 47880;
    first_config.port_range = 2;
    ClusterConfig second_config = first_config;
    second_config.listen_port = 47881;
    second_config.seeds = {"127.0.0.1:47880"};
    Cluster first_cluster("bayes-first", first_config);
    Cluster second_cluster("bayes-second", second_config);
    first_cluster.start();
    second_cluster.start();
    ASSERT_TRUE(wait_for_cluster(first_cluster, second_cluster));

    zetta::compute::JobCoordinator first_coordinator(first_cluster, 1);
    zetta::compute::JobCoordinator second_coordinator(second_cluster, 1);
    first_coordinator.start();
    second_coordinator.start();
    TempDir first_directory;
    TempDir second_directory;
    ComputeJob first_job = make_job(first_directory, "first-job");
    ComputeJob second_job = make_job(second_directory, "second-job");
    BayesianParams params;
    params.seed = 73;
    params.holdout_fraction = 0.2;
    params.burn_in = 100;
    params.posterior_draws = 300;
    const std::vector<std::string> groups = {"group-0", "group-1", "group-2"};
    zetta::ml::HierarchicalResult first_result;
    zetta::ml::HierarchicalResult second_result;
    std::thread first_thread([&] {
        first_result = zetta::ml::run_hierarchical_job(
            first_coordinator, first_job, first_rows, groups, params,
            "bayesian-distributed-test", 8s);
    });
    std::thread second_thread([&] {
        second_result = zetta::ml::run_hierarchical_job(
            second_coordinator, second_job, second_rows, groups, params,
            "bayesian-distributed-test", 8s);
    });
    first_thread.join();
    second_thread.join();

    ASSERT_TRUE(first_result.ok()) << first_result.error;
    ASSERT_TRUE(second_result.ok()) << second_result.error;
    ASSERT_EQ(first_result.groups.size(), second_result.groups.size());
    for (std::size_t index = 0; index < first_result.groups.size(); ++index) {
        EXPECT_DOUBLE_EQ(first_result.groups[index].posterior_mean,
                         second_result.groups[index].posterior_mean);
        EXPECT_DOUBLE_EQ(first_result.groups[index].credible_lower,
                         second_result.groups[index].credible_lower);
        EXPECT_DOUBLE_EQ(first_result.groups[index].credible_upper,
                         second_result.groups[index].credible_upper);
    }
    EXPECT_DOUBLE_EQ(first_result.empirical_coverage,
                     second_result.empirical_coverage);
    EXPECT_EQ(first_job.status(), JobStatus::Succeeded);
    EXPECT_EQ(second_job.status(), JobStatus::Succeeded);

    first_coordinator.stop();
    second_coordinator.stop();
    first_cluster.stop();
    second_cluster.stop();
}
