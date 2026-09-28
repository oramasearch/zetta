#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/gbdt.hpp"

// Native privacy-preserving tabular learning (#1059, M-20).
//
// The Gaussian copula is fit from participant-local sufficient statistics. Numeric
// marginals use empirical histogram quantiles; categorical marginals use a stable
// ordered encoding. Normal-score correlations and DP GBDT gradient histograms are
// combined through the same injected collective reduction used by the native GBDT.
// No foreign runtime or hosted model is involved.
//
// See docs/ml/synthetic_private.md.
namespace zetta::ml {

enum class PrivacyNoise {
    Laplace,
    Gaussian,
};

struct SyntheticPrivateParams {
    double epsilon = 4.0;
    double delta = 1e-6;
    double maximum_epsilon = 8.0;
    double maximum_delta = 1e-5;
    PrivacyNoise noise = PrivacyNoise::Gaussian;
    double clip_min = 0.0;
    double clip_max = 1.0;
    std::size_t synthetic_rows = 0;
    std::uint64_t seed = 42;
    double minimum_dcr = 1e-9;
    double maximum_membership_auc = 0.65;
    GbdtParams gbdt;
};

struct PrivacyAccounting {
    double epsilon_spent = 0.0;
    double delta_spent = 0.0;
    std::size_t mechanisms = 0;
    std::string mechanism;
};

struct SyntheticPrivateMetrics {
    double marginal_distance = 0.0;
    double correlation_distance = 0.0;
    double distance_to_closest_record = 0.0;
    double membership_auc = 0.5;
    double tstr_auc = 0.5;
    PrivacyAccounting privacy;
    bool privacy_evaluation_passed = false;

    nlohmann::json to_json() const;
};

struct SyntheticPrivateResult {
    std::vector<nlohmann::json> synthetic_rows;
    GbdtModel private_model;
    SyntheticPrivateMetrics metrics;
    std::vector<std::string> feature_names;
    std::string error;

    bool ok() const noexcept {
        return error.empty();
    }
};

using PrivateGather = std::function<std::vector<std::vector<double>>(
    const std::string& tag, const std::vector<double>& local)>;
using PrivateBroadcast = std::function<std::vector<double>(
    const std::string& tag, const std::vector<double>& root_value)>;

struct PrivateCollectives {
    AllReduce allreduce;
    PrivateGather gather;
    PrivateBroadcast broadcast;
    bool is_root = true;
};

// Run M-20 over this participant's local rows. Every participant calls this in
// lock-step with the same contract, parameters, and collective callbacks. Raw rows
// stay local: only category dictionaries, marginal counts, normal-score moments,
// evaluation scores, and model histograms cross the collective plane.
SyntheticPrivateResult run_synthetic_private(
    const std::vector<nlohmann::json>& local_rows,
    const data::DatasetContract& contract,
    const SyntheticPrivateParams& params,
    const PrivateCollectives& collectives,
    const std::string& tag = "synthetic_private");

// Single-node convenience used by direct callers and unit tests.
SyntheticPrivateResult run_synthetic_private(
    const std::vector<nlohmann::json>& rows,
    const data::DatasetContract& contract,
    const SyntheticPrivateParams& params = {});

}  // namespace zetta::ml
