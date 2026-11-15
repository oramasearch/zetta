// SPDX-License-Identifier: MIT

#include "zetta/ml/synthetic_private.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

constexpr std::size_t kBins = 64;
constexpr double kTail = 1e-9;

struct ColumnProfile {
    std::string name;
    bool categorical = false;
    double minimum = 0.0;
    double maximum = 0.0;
    std::vector<std::string> categories;
    std::vector<double> counts;
};

struct CopulaModel {
    std::vector<ColumnProfile> columns;
    std::vector<double> correlation;
    std::vector<double> cholesky;
    double rows = 0.0;
};

std::uint64_t mix_seed(std::uint64_t seed, std::string_view value,
                       std::size_t index = 0) {
    std::uint64_t hash = seed ^ 1469598103934665603ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    hash ^= static_cast<std::uint64_t>(index) + 0x9e3779b97f4a7c15ull +
            (hash << 6) + (hash >> 2);
    return hash;
}

double normal_cdf(double value) {
    return 0.5 * std::erfc(-value / std::sqrt(2.0));
}

// Peter Acklam's inverse-normal approximation. The input is clamped away from
// 0/1 so finite empirical tails always produce a finite normal score.
double normal_quantile(double probability) {
    const double p = std::clamp(probability, kTail, 1.0 - kTail);
    constexpr double a1 = -3.969683028665376e+01;
    constexpr double a2 = 2.209460984245205e+02;
    constexpr double a3 = -2.759285104469687e+02;
    constexpr double a4 = 1.383577518672690e+02;
    constexpr double a5 = -3.066479806614716e+01;
    constexpr double a6 = 2.506628277459239e+00;
    constexpr double b1 = -5.447609879822406e+01;
    constexpr double b2 = 1.615858368580409e+02;
    constexpr double b3 = -1.556989798598866e+02;
    constexpr double b4 = 6.680131188771972e+01;
    constexpr double b5 = -1.328068155288572e+01;
    constexpr double c1 = -7.784894002430293e-03;
    constexpr double c2 = -3.223964580411365e-01;
    constexpr double c3 = -2.400758277161838e+00;
    constexpr double c4 = -2.549732539343734e+00;
    constexpr double c5 = 4.374664141464968e+00;
    constexpr double c6 = 2.938163982698783e+00;
    constexpr double d1 = 7.784695709041462e-03;
    constexpr double d2 = 3.224671290700398e-01;
    constexpr double d3 = 2.445134137142996e+00;
    constexpr double d4 = 3.754408661907416e+00;
    constexpr double lower = 0.02425;
    constexpr double upper = 1.0 - lower;
    if (p < lower) {
        const double q = std::sqrt(-2.0 * std::log(p));
        return (((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
               ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    if (p > upper) {
        const double q = std::sqrt(-2.0 * std::log(1.0 - p));
        return -(((((c1 * q + c2) * q + c3) * q + c4) * q + c5) * q + c6) /
               ((((d1 * q + d2) * q + d3) * q + d4) * q + 1.0);
    }
    const double q = p - 0.5;
    const double r = q * q;
    return (((((a1 * r + a2) * r + a3) * r + a4) * r + a5) * r + a6) * q /
           (((((b1 * r + b2) * r + b3) * r + b4) * r + b5) * r + 1.0);
}

std::vector<double> encode_bytes(std::string_view value) {
    std::vector<double> encoded;
    encoded.reserve(value.size() + 1);
    encoded.push_back(static_cast<double>(value.size()));
    for (const unsigned char byte : value) {
        encoded.push_back(static_cast<double>(byte));
    }
    return encoded;
}

std::string decode_bytes(const std::vector<double>& value) {
    if (value.empty()) {
        return {};
    }
    const std::size_t size = std::min<std::size_t>(
        static_cast<std::size_t>(std::max(0.0, value[0])), value.size() - 1);
    std::string decoded;
    decoded.reserve(size);
    for (std::size_t index = 0; index < size; ++index) {
        decoded.push_back(static_cast<char>(std::clamp(value[index + 1], 0.0, 255.0)));
    }
    return decoded;
}

std::vector<double> reduce(const PrivateCollectives& collectives,
                           const std::string& tag,
                           const std::vector<double>& local, GbdtReduce op) {
    if (!collectives.allreduce) {
        return local;
    }
    return collectives.allreduce(tag, local, op);
}

std::vector<std::vector<double>> gather(const PrivateCollectives& collectives,
                                        const std::string& tag,
                                        const std::vector<double>& local) {
    if (!collectives.gather) {
        return {local};
    }
    return collectives.gather(tag, local);
}

std::vector<double> broadcast(const PrivateCollectives& collectives,
                              const std::string& tag,
                              const std::vector<double>& root_value) {
    if (!collectives.broadcast) {
        return root_value;
    }
    return collectives.broadcast(tag, root_value);
}

std::string scalar_key(const nlohmann::json& value) {
    return value.dump();
}

bool numeric_value(const nlohmann::json& row, const std::string& column,
                   double& value) {
    const auto found = row.find(column);
    if (found == row.end()) {
        return false;
    }
    if (found->is_boolean()) {
        value = found->get<bool>() ? 1.0 : 0.0;
        return true;
    }
    if (!found->is_number()) {
        return false;
    }
    value = found->get<double>();
    return std::isfinite(value);
}

std::vector<std::string> modeled_columns(const data::DatasetContract& contract) {
    std::vector<std::string> columns = contract.feature_columns;
    if (!contract.label_column.empty() &&
        std::find(columns.begin(), columns.end(), contract.label_column) ==
            columns.end()) {
        columns.push_back(contract.label_column);
    }
    return columns;
}

std::vector<std::string> global_categories(
    const std::vector<nlohmann::json>& rows, const std::string& column,
    const PrivateCollectives& collectives, const std::string& tag) {
    std::set<std::string> local;
    for (const nlohmann::json& row : rows) {
        const auto found = row.find(column);
        if (found != row.end() && !found->is_null()) {
            local.insert(scalar_key(*found));
        }
    }
    const nlohmann::json local_json = std::vector<std::string>(local.begin(), local.end());
    const std::vector<std::vector<double>> gathered =
        gather(collectives, tag + ".gather", encode_bytes(local_json.dump()));
    nlohmann::json merged = nlohmann::json::array();
    if (collectives.is_root) {
        std::set<std::string> values;
        for (const std::vector<double>& part : gathered) {
            const nlohmann::json decoded =
                nlohmann::json::parse(decode_bytes(part), nullptr, false);
            if (!decoded.is_array()) {
                continue;
            }
            for (const nlohmann::json& entry : decoded) {
                if (entry.is_string()) {
                    values.insert(entry.get<std::string>());
                }
            }
        }
        merged = std::vector<std::string>(values.begin(), values.end());
    }
    const std::vector<double> wire = broadcast(
        collectives, tag + ".broadcast",
        collectives.is_root ? encode_bytes(merged.dump()) : std::vector<double>{});
    const nlohmann::json decoded =
        nlohmann::json::parse(decode_bytes(wire), nullptr, false);
    if (!decoded.is_array()) {
        return {};
    }
    return decoded.get<std::vector<std::string>>();
}

std::size_t numeric_bin(const ColumnProfile& profile, double value) {
    if (profile.maximum <= profile.minimum) {
        return 0;
    }
    const double position = (value - profile.minimum) /
                            (profile.maximum - profile.minimum);
    return std::min(kBins - 1, static_cast<std::size_t>(
                                   std::clamp(position, 0.0, 1.0) * kBins));
}

std::size_t categorical_bin(const ColumnProfile& profile,
                            const nlohmann::json& value) {
    const std::string key = scalar_key(value);
    const auto found = std::lower_bound(profile.categories.begin(),
                                        profile.categories.end(), key);
    if (found == profile.categories.end() || *found != key) {
        return 0;
    }
    return static_cast<std::size_t>(found - profile.categories.begin());
}

double bin_probability(const ColumnProfile& profile, std::size_t bin) {
    const double total = std::accumulate(profile.counts.begin(),
                                         profile.counts.end(), 0.0);
    if (total <= 0.0) {
        return 0.5;
    }
    const double before = std::accumulate(profile.counts.begin(),
                                          profile.counts.begin() + bin, 0.0);
    return std::clamp((before + 0.5 * profile.counts[bin]) / total,
                      kTail, 1.0 - kTail);
}

std::vector<double> row_scores(const nlohmann::json& row,
                               const CopulaModel& model) {
    std::vector<double> scores(model.columns.size(), 0.0);
    for (std::size_t index = 0; index < model.columns.size(); ++index) {
        const ColumnProfile& profile = model.columns[index];
        const auto found = row.find(profile.name);
        if (found == row.end() || found->is_null()) {
            continue;
        }
        std::size_t bin = 0;
        if (profile.categorical) {
            bin = categorical_bin(profile, *found);
        } else {
            double value = 0.0;
            if (!numeric_value(row, profile.name, value)) {
                continue;
            }
            bin = numeric_bin(profile, value);
        }
        scores[index] = normal_quantile(bin_probability(profile, bin));
    }
    return scores;
}

std::vector<double> cholesky(const std::vector<double>& correlation,
                             std::size_t size) {
    std::vector<double> lower(size * size, 0.0);
    for (std::size_t row = 0; row < size; ++row) {
        for (std::size_t column = 0; column <= row; ++column) {
            double sum = correlation[row * size + column];
            for (std::size_t inner = 0; inner < column; ++inner) {
                sum -= lower[row * size + inner] * lower[column * size + inner];
            }
            if (row == column) {
                lower[row * size + column] =
                    std::sqrt(std::max(sum, 1e-8));
            } else {
                lower[row * size + column] =
                    sum / lower[column * size + column];
            }
        }
    }
    return lower;
}

CopulaModel fit_copula(const std::vector<nlohmann::json>& rows,
                       const std::vector<std::string>& columns,
                       const PrivateCollectives& collectives,
                       const std::string& tag, std::string& error) {
    CopulaModel model;
    const std::size_t column_count = columns.size();
    if (column_count == 0) {
        error = "synthetic_private: no modeled columns";
        return model;
    }
    const std::vector<double> row_count = reduce(
        collectives, tag + ".rows", {static_cast<double>(rows.size())},
        GbdtReduce::Sum);
    if (row_count.size() != 1 || row_count[0] < 4.0) {
        error = "synthetic_private: at least four rows are required";
        return model;
    }
    model.rows = row_count[0];

    model.columns.reserve(column_count);
    for (std::size_t index = 0; index < column_count; ++index) {
        ColumnProfile profile;
        profile.name = columns[index];
        bool local_numeric = true;
        double local_minimum = std::numeric_limits<double>::infinity();
        double local_maximum = -std::numeric_limits<double>::infinity();
        for (const nlohmann::json& row : rows) {
            const auto found = row.find(profile.name);
            if (found == row.end() || found->is_null()) {
                continue;
            }
            double value = 0.0;
            if (!numeric_value(row, profile.name, value)) {
                local_numeric = false;
                break;
            }
            local_minimum = std::min(local_minimum, value);
            local_maximum = std::max(local_maximum, value);
        }
        const std::vector<double> numeric_votes = reduce(
            collectives, tag + ".type." + std::to_string(index),
            {local_numeric ? 0.0 : 1.0}, GbdtReduce::Sum);
        profile.categorical = numeric_votes.empty() || numeric_votes[0] > 0.0;
        if (profile.categorical) {
            profile.categories = global_categories(
                rows, profile.name, collectives,
                tag + ".categories." + std::to_string(index));
            if (profile.categories.empty()) {
                error = "synthetic_private: column '" + profile.name +
                        "' has no values";
                return {};
            }
            profile.counts.assign(profile.categories.size(), 0.0);
            for (const nlohmann::json& row : rows) {
                const auto found = row.find(profile.name);
                if (found != row.end() && !found->is_null()) {
                    ++profile.counts[categorical_bin(profile, *found)];
                }
            }
        } else {
            const std::vector<double> global_minimum = reduce(
                collectives, tag + ".minimum." + std::to_string(index),
                {std::isfinite(local_minimum)
                     ? local_minimum
                     : std::numeric_limits<double>::max()},
                GbdtReduce::Min);
            const std::vector<double> global_maximum = reduce(
                collectives, tag + ".maximum." + std::to_string(index),
                {std::isfinite(local_maximum)
                     ? local_maximum
                     : std::numeric_limits<double>::lowest()},
                GbdtReduce::Max);
            if (global_minimum.size() != 1 || global_maximum.size() != 1 ||
                !std::isfinite(global_minimum[0]) ||
                !std::isfinite(global_maximum[0]) ||
                global_minimum[0] > global_maximum[0]) {
                error = "synthetic_private: numeric column '" + profile.name +
                        "' has no finite values";
                return {};
            }
            profile.minimum = global_minimum[0];
            profile.maximum = global_maximum[0];
            profile.counts.assign(kBins, 0.0);
            for (const nlohmann::json& row : rows) {
                double value = 0.0;
                if (numeric_value(row, profile.name, value)) {
                    ++profile.counts[numeric_bin(profile, value)];
                }
            }
        }
        profile.counts = reduce(
            collectives, tag + ".marginal." + std::to_string(index),
            profile.counts, GbdtReduce::Sum);
        model.columns.push_back(std::move(profile));
    }

    std::vector<double> local_moments(column_count * column_count + column_count,
                                      0.0);
    for (const nlohmann::json& row : rows) {
        const std::vector<double> scores = row_scores(row, model);
        for (std::size_t left = 0; left < column_count; ++left) {
            local_moments[column_count * column_count + left] += scores[left];
            for (std::size_t right = 0; right < column_count; ++right) {
                local_moments[left * column_count + right] +=
                    scores[left] * scores[right];
            }
        }
    }
    const std::vector<double> moments = reduce(
        collectives, tag + ".correlation", local_moments, GbdtReduce::Sum);
    if (moments.size() != local_moments.size()) {
        error = "synthetic_private: correlation allreduce failed";
        return {};
    }
    model.correlation.assign(column_count * column_count, 0.0);
    for (std::size_t left = 0; left < column_count; ++left) {
        const double left_mean =
            moments[column_count * column_count + left] / model.rows;
        for (std::size_t right = 0; right < column_count; ++right) {
            const double right_mean =
                moments[column_count * column_count + right] / model.rows;
            const double covariance =
                moments[left * column_count + right] / model.rows -
                left_mean * right_mean;
            const double left_variance = std::max(
                moments[left * column_count + left] / model.rows -
                    left_mean * left_mean,
                1e-8);
            const double right_variance = std::max(
                moments[right * column_count + right] / model.rows -
                    right_mean * right_mean,
                1e-8);
            model.correlation[left * column_count + right] =
                left == right
                    ? 1.0
                    : std::clamp(covariance /
                                     std::sqrt(left_variance * right_variance),
                                 -0.99, 0.99);
        }
    }
    model.cholesky = cholesky(model.correlation, column_count);
    return model;
}

nlohmann::json quantile_value(const ColumnProfile& profile, double probability) {
    const double total = std::accumulate(profile.counts.begin(),
                                         profile.counts.end(), 0.0);
    const double target = std::clamp(probability, 0.0, 1.0) * total;
    double cumulative = 0.0;
    std::size_t bin = profile.counts.empty() ? 0 : profile.counts.size() - 1;
    for (std::size_t index = 0; index < profile.counts.size(); ++index) {
        cumulative += profile.counts[index];
        if (target <= cumulative) {
            bin = index;
            break;
        }
    }
    if (profile.categorical) {
        return nlohmann::json::parse(profile.categories[bin], nullptr, false);
    }
    if (profile.maximum <= profile.minimum) {
        return profile.minimum;
    }
    const double lower = profile.minimum +
                         (profile.maximum - profile.minimum) *
                             static_cast<double>(bin) / kBins;
    const double upper = profile.minimum +
                         (profile.maximum - profile.minimum) *
                             static_cast<double>(bin + 1) / kBins;
    const double within = profile.counts[bin] > 0.0
                              ? std::clamp((target -
                                            (cumulative - profile.counts[bin])) /
                                               profile.counts[bin],
                                           0.0, 1.0)
                              : 0.5;
    return lower + (upper - lower) * within;
}

std::vector<nlohmann::json> sample_copula(const CopulaModel& model,
                                          std::size_t count,
                                          std::uint64_t seed) {
    std::mt19937_64 generator(seed);
    std::normal_distribution<double> normal(0.0, 1.0);
    const std::size_t width = model.columns.size();
    std::vector<nlohmann::json> rows;
    rows.reserve(count);
    for (std::size_t row_index = 0; row_index < count; ++row_index) {
        std::vector<double> independent(width, 0.0);
        for (double& value : independent) {
            value = normal(generator);
        }
        nlohmann::json row = nlohmann::json::object();
        for (std::size_t column = 0; column < width; ++column) {
            double correlated = 0.0;
            for (std::size_t inner = 0; inner <= column; ++inner) {
                correlated += model.cholesky[column * width + inner] *
                              independent[inner];
            }
            row[model.columns[column].name] =
                quantile_value(model.columns[column], normal_cdf(correlated));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

std::vector<nlohmann::json> broadcast_rows(
    const std::vector<nlohmann::json>& root_rows,
    const PrivateCollectives& collectives, const std::string& tag) {
    const std::vector<double> wire = broadcast(
        collectives, tag,
        collectives.is_root
            ? encode_bytes(nlohmann::json(root_rows).dump())
            : std::vector<double>{});
    const nlohmann::json decoded =
        nlohmann::json::parse(decode_bytes(wire), nullptr, false);
    if (!decoded.is_array()) {
        return {};
    }
    return decoded.get<std::vector<nlohmann::json>>();
}

double mixed_distance(const nlohmann::json& left, const nlohmann::json& right,
                      const CopulaModel& model) {
    double sum = 0.0;
    for (const ColumnProfile& profile : model.columns) {
        const auto left_value = left.find(profile.name);
        const auto right_value = right.find(profile.name);
        if (left_value == left.end() || right_value == right.end()) {
            sum += 1.0;
            continue;
        }
        if (profile.categorical) {
            sum += *left_value == *right_value ? 0.0 : 1.0;
            continue;
        }
        double left_number = 0.0;
        double right_number = 0.0;
        if (!numeric_value(left, profile.name, left_number) ||
            !numeric_value(right, profile.name, right_number)) {
            sum += 1.0;
            continue;
        }
        const double range = std::max(profile.maximum - profile.minimum, 1e-12);
        sum += std::min(1.0, std::abs(left_number - right_number) / range);
    }
    return sum / static_cast<double>(model.columns.size());
}

std::vector<double> closest_to_local(
    const std::vector<nlohmann::json>& queries,
    const std::vector<nlohmann::json>& local_rows, const CopulaModel& model) {
    std::vector<double> distances(queries.size(),
                                  std::numeric_limits<double>::infinity());
    for (std::size_t query = 0; query < queries.size(); ++query) {
        for (const nlohmann::json& row : local_rows) {
            distances[query] = std::min(
                distances[query], mixed_distance(queries[query], row, model));
        }
    }
    return distances;
}

double auc(const std::vector<double>& scores, const std::vector<double>& labels) {
    if (scores.size() != labels.size() || scores.empty()) {
        return 0.5;
    }
    std::vector<std::size_t> order(scores.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left,
                                                      std::size_t right) {
        return scores[left] < scores[right];
    });
    double positives = 0.0;
    double rank_sum = 0.0;
    std::size_t begin = 0;
    while (begin < order.size()) {
        std::size_t end = begin + 1;
        while (end < order.size() &&
               scores[order[end]] == scores[order[begin]]) {
            ++end;
        }
        const double average_rank =
            0.5 * static_cast<double>(begin + 1 + end);
        for (std::size_t rank = begin; rank < end; ++rank) {
            if (labels[order[rank]] >= 0.5) {
                positives += 1.0;
                rank_sum += average_rank;
            }
        }
        begin = end;
    }
    const double negatives = static_cast<double>(scores.size()) - positives;
    if (positives == 0.0 || negatives == 0.0) {
        return 0.5;
    }
    const double measured =
        (rank_sum - positives * (positives + 1.0) / 2.0) /
        (positives * negatives);
    return std::max(measured, 1.0 - measured);
}

bool evaluation_member(const nlohmann::json& row, std::uint64_t seed) {
    return (mix_seed(seed, row.dump()) % 4) != 0;
}

double membership_auc(const std::vector<nlohmann::json>& local_rows,
                      const std::vector<nlohmann::json>& synthetic,
                      const CopulaModel& model,
                      const PrivateCollectives& collectives,
                      std::uint64_t seed, const std::string& tag) {
    std::vector<double> pairs;
    for (const nlohmann::json& row : local_rows) {
        double distance = std::numeric_limits<double>::infinity();
        for (const nlohmann::json& candidate : synthetic) {
            distance = std::min(distance, mixed_distance(row, candidate, model));
        }
        pairs.push_back(-distance);
        pairs.push_back(evaluation_member(row, seed) ? 1.0 : 0.0);
    }
    const std::vector<std::vector<double>> parts =
        gather(collectives, tag + ".gather", pairs);
    std::vector<double> result;
    if (collectives.is_root) {
        std::vector<double> scores;
        std::vector<double> labels;
        for (const std::vector<double>& part : parts) {
            for (std::size_t index = 0; index + 1 < part.size(); index += 2) {
                scores.push_back(part[index]);
                labels.push_back(part[index + 1]);
            }
        }
        result = {auc(scores, labels)};
    }
    const std::vector<double> shared =
        broadcast(collectives, tag + ".broadcast", result);
    return shared.size() == 1 ? shared[0] : 0.5;
}

double laplace(std::mt19937_64& generator, double scale) {
    std::uniform_real_distribution<double> uniform(-0.5, 0.5);
    const double sample = uniform(generator);
    return -scale * std::copysign(std::log1p(-2.0 * std::abs(sample)), sample);
}

struct DpReducer {
    const PrivateCollectives& collectives;
    const SyntheticPrivateParams& params;
    std::size_t features = 0;
    std::size_t maximum_mechanisms = 0;
    std::size_t mechanisms = 0;

    double epsilon_per_mechanism() const {
        return std::min(
            params.epsilon / static_cast<double>(maximum_mechanisms), 1.0);
    }

    std::vector<double> operator()(const std::string& tag,
                                   const std::vector<double>& local,
                                   GbdtReduce op) {
        if (tag.ends_with("edges.min")) {
            return std::vector<double>(local.size(), params.clip_min);
        }
        if (tag.ends_with("edges.max")) {
            return std::vector<double>(local.size(), params.clip_max);
        }
        std::vector<double> global = reduce(collectives, tag, local, op);
        if (op != GbdtReduce::Sum ||
            (!tag.ends_with(".base") && tag.find(".t") == std::string::npos)) {
            return global;
        }
        ++mechanisms;
        const double epsilon = epsilon_per_mechanism();
        const double delta = params.noise == PrivacyNoise::Gaussian
                                 ? params.delta /
                                       static_cast<double>(maximum_mechanisms)
                                 : 0.0;
        const double gaussian_sensitivity =
            tag.ends_with(".base")
                ? std::sqrt(2.0)
                : std::sqrt(static_cast<double>(features) *
                            (1.0 + 0.25 * 0.25));
        const double laplace_sensitivity =
            tag.ends_with(".base")
                ? 2.0
                : static_cast<double>(features) * 1.25;
        std::mt19937_64 generator(mix_seed(params.seed, tag));
        for (std::size_t index = 0; index < global.size(); ++index) {
            if (params.noise == PrivacyNoise::Gaussian) {
                const double sigma = gaussian_sensitivity *
                                     std::sqrt(2.0 * std::log(1.25 / delta)) /
                                     epsilon;
                std::normal_distribution<double> normal(0.0, sigma);
                global[index] += normal(generator);
            } else {
                global[index] +=
                    laplace(generator, laplace_sensitivity / epsilon);
            }
        }
        if (tag.ends_with(".base") && global.size() == 2) {
            global[1] = std::max(global[1], 2.0);
            global[0] = std::clamp(global[0], 1e-6, global[1] - 1e-6);
        } else {
            for (std::size_t index = 1; index < global.size(); index += 2) {
                global[index] = std::max(global[index], 1e-6);
            }
        }
        return global;
    }
};

double encoded_feature(const nlohmann::json& row,
                       const ColumnProfile& profile) {
    const auto found = row.find(profile.name);
    if (found == row.end() || found->is_null()) {
        return 0.0;
    }
    if (profile.categorical) {
        return static_cast<double>(categorical_bin(profile, *found));
    }
    double value = 0.0;
    numeric_value(row, profile.name, value);
    return value;
}

void model_matrix(const std::vector<nlohmann::json>& rows,
                  const CopulaModel& copula,
                  const data::DatasetContract& contract,
                  std::vector<std::vector<double>>& columns,
                  std::vector<double>& labels,
                  std::vector<std::string>& feature_names) {
    std::map<std::string, const ColumnProfile*> profiles;
    for (const ColumnProfile& profile : copula.columns) {
        profiles.emplace(profile.name, &profile);
    }
    feature_names = contract.feature_columns;
    columns.assign(feature_names.size(), {});
    for (const nlohmann::json& row : rows) {
        const auto label = row.find(contract.label_column);
        if (label == row.end() || (!label->is_number() && !label->is_boolean())) {
            continue;
        }
        for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
            columns[feature].push_back(
                encoded_feature(row, *profiles.at(feature_names[feature])));
        }
        labels.push_back(label->is_boolean()
                             ? (label->get<bool>() ? 1.0 : 0.0)
                             : label->get<double>());
    }
}

double tstr_auc(const std::vector<nlohmann::json>& local_real,
                const std::vector<nlohmann::json>& synthetic,
                const CopulaModel& copula,
                const data::DatasetContract& contract,
                const SyntheticPrivateParams& params,
                const PrivateCollectives& collectives,
                const std::string& tag, std::string& error) {
    std::vector<std::vector<double>> synthetic_columns;
    std::vector<double> synthetic_labels;
    std::vector<std::string> feature_names;
    model_matrix(synthetic, copula, contract, synthetic_columns,
                 synthetic_labels, feature_names);
    GbdtParams utility_params = params.gbdt;
    utility_params.num_trees = std::min(utility_params.num_trees, 20);
    // Every participant has the same evaluated synthetic table. Train the
    // throwaway utility model locally on that table, then distribute only the
    // scores over each participant's real-data shard for the global TSTR AUC.
    const GbdtTrainResult trained = train_gbdt(
        synthetic_columns, feature_names, synthetic_labels, utility_params);
    if (!trained.ok()) {
        error = "synthetic_private: TSTR model failed: " +
                trained.status.message;
        return 0.5;
    }
    std::vector<std::vector<double>> real_columns;
    std::vector<double> real_labels;
    std::vector<std::string> ignored;
    model_matrix(local_real, copula, contract, real_columns, real_labels, ignored);
    std::vector<double> pairs;
    std::vector<double> row(feature_names.size(), 0.0);
    for (std::size_t index = 0; index < real_labels.size(); ++index) {
        for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
            row[feature] = real_columns[feature][index];
        }
        pairs.push_back(trained.model.predict_proba(row));
        pairs.push_back(real_labels[index]);
    }
    const std::vector<std::vector<double>> parts =
        gather(collectives, tag + ".metrics.gather", pairs);
    std::vector<double> result;
    if (collectives.is_root) {
        std::vector<double> scores;
        std::vector<double> labels;
        for (const std::vector<double>& part : parts) {
            for (std::size_t index = 0; index + 1 < part.size(); index += 2) {
                scores.push_back(part[index]);
                labels.push_back(part[index + 1]);
            }
        }
        result = {auc(scores, labels)};
    }
    const std::vector<double> shared =
        broadcast(collectives, tag + ".metrics.broadcast", result);
    return shared.size() == 1 ? shared[0] : 0.5;
}

double marginal_distance(const CopulaModel& source,
                         const CopulaModel& synthetic) {
    double total = 0.0;
    for (std::size_t column = 0; column < source.columns.size(); ++column) {
        const ColumnProfile& left = source.columns[column];
        const ColumnProfile& right = synthetic.columns[column];
        const double left_total =
            std::accumulate(left.counts.begin(), left.counts.end(), 0.0);
        const double right_total =
            std::accumulate(right.counts.begin(), right.counts.end(), 0.0);
        const std::size_t count = std::min(left.counts.size(), right.counts.size());
        double distance = 0.0;
        for (std::size_t bin = 0; bin < count; ++bin) {
            distance += std::abs(left.counts[bin] / left_total -
                                 right.counts[bin] / right_total);
        }
        total += 0.5 * distance;
    }
    return total / static_cast<double>(source.columns.size());
}

double correlation_distance(const CopulaModel& source,
                             const CopulaModel& synthetic) {
    double sum = 0.0;
    for (std::size_t index = 0; index < source.correlation.size(); ++index) {
        const double difference =
            source.correlation[index] - synthetic.correlation[index];
        sum += difference * difference;
    }
    return std::sqrt(sum / static_cast<double>(source.correlation.size()));
}

}  // namespace

nlohmann::json SyntheticPrivateMetrics::to_json() const {
    return nlohmann::json{
        {"marginal_distance", marginal_distance},
        {"correlation_distance", correlation_distance},
        {"distance_to_closest_record", distance_to_closest_record},
        {"membership_auc", membership_auc},
        {"tstr_auc", tstr_auc},
        {"epsilon_spent", privacy.epsilon_spent},
        {"delta_spent", privacy.delta_spent},
        {"privacy_mechanisms", privacy.mechanisms},
        {"privacy_evaluation_passed", privacy_evaluation_passed ? 1.0 : 0.0}};
}

SyntheticPrivateResult run_synthetic_private(
    const std::vector<nlohmann::json>& local_rows,
    const data::DatasetContract& contract,
    const SyntheticPrivateParams& params,
    const PrivateCollectives& collectives, const std::string& tag,
    const TrainHooks& hooks) {
    SyntheticPrivateResult result;
    if (!collectives.allreduce || !collectives.gather ||
        !collectives.broadcast) {
        result.error = "synthetic_private: collective callbacks are required";
        return result;
    }
    if (!(params.epsilon > 0.0) || !(params.delta >= 0.0) ||
        !(params.delta < 1.0) || !(params.maximum_delta < 1.0) ||
        params.epsilon > params.maximum_epsilon ||
        params.delta > params.maximum_delta) {
        result.error =
            "synthetic_private: requested privacy budget exceeds the enforced maximum";
        return result;
    }
    if (params.noise == PrivacyNoise::Gaussian && params.delta <= 0.0) {
        result.error = "synthetic_private: Gaussian noise requires delta > 0";
        return result;
    }
    if (!(params.clip_min < params.clip_max)) {
        result.error = "synthetic_private: clip_min must be below clip_max";
        return result;
    }
    if (contract.label_column.empty() || contract.feature_columns.empty()) {
        result.error =
            "synthetic_private: a label and at least one feature are required";
        return result;
    }

    const std::vector<std::string> columns = modeled_columns(contract);
    std::string error;
    std::vector<nlohmann::json> privacy_train;
    for (const nlohmann::json& row : local_rows) {
        if (evaluation_member(row, params.seed)) {
            privacy_train.push_back(row);
        }
    }
    const CopulaModel privacy_model = fit_copula(
        privacy_train, columns, collectives, tag + ".privacy.fit", error);
    if (!error.empty()) {
        result.error = std::move(error);
        return result;
    }
    const CopulaModel source = fit_copula(local_rows, columns, collectives,
                                          tag + ".source", error);
    if (!error.empty()) {
        result.error = std::move(error);
        return result;
    }
    const std::size_t output_rows =
        params.synthetic_rows == 0
            ? static_cast<std::size_t>(source.rows)
            : params.synthetic_rows;
    std::vector<nlohmann::json> root_rows;
    if (collectives.is_root) {
        root_rows = sample_copula(source, output_rows, params.seed);
    }
    result.synthetic_rows = broadcast_rows(
        root_rows, collectives, tag + ".synthetic.broadcast");
    if (result.synthetic_rows.size() != output_rows) {
        result.error = "synthetic_private: synthetic-row broadcast failed";
        return result;
    }

    const std::vector<double> local_distances =
        closest_to_local(result.synthetic_rows, local_rows, source);
    const std::vector<double> distances = reduce(
        collectives, tag + ".privacy.dcr", local_distances, GbdtReduce::Min);
    result.metrics.distance_to_closest_record = distances.empty()
                                                    ? 0.0
                                                    : *std::min_element(
                                                          distances.begin(),
                                                          distances.end());
    std::vector<nlohmann::json> privacy_root_rows;
    if (collectives.is_root) {
        privacy_root_rows = sample_copula(
            privacy_model, output_rows, mix_seed(params.seed, "membership"));
    }
    const std::vector<nlohmann::json> privacy_rows = broadcast_rows(
        privacy_root_rows, collectives, tag + ".privacy.synthetic.broadcast");
    result.metrics.membership_auc = membership_auc(
        local_rows, privacy_rows, privacy_model, collectives, params.seed,
        tag + ".privacy.membership");

    const CopulaModel synthetic_profile = fit_copula(
        collectives.is_root ? result.synthetic_rows
                            : std::vector<nlohmann::json>{},
        columns, collectives, tag + ".synthetic.profile", error);
    if (!error.empty()) {
        result.error = std::move(error);
        return result;
    }
    result.metrics.marginal_distance =
        marginal_distance(source, synthetic_profile);
    result.metrics.correlation_distance =
        correlation_distance(source, synthetic_profile);
    result.metrics.tstr_auc = tstr_auc(
        local_rows, result.synthetic_rows, source, contract, params, collectives,
        tag + ".utility", error);
    if (!error.empty()) {
        result.error = std::move(error);
        return result;
    }

    result.metrics.privacy_evaluation_passed =
        result.metrics.distance_to_closest_record > params.minimum_dcr &&
        result.metrics.membership_auc <= params.maximum_membership_auc;
    if (!result.metrics.privacy_evaluation_passed) {
        result.synthetic_rows.clear();
        result.error =
            "synthetic_private: privacy evaluation failed; no synthetic table was published";
        return result;
    }

    std::vector<std::vector<double>> private_columns;
    std::vector<double> private_labels;
    model_matrix(local_rows, source, contract, private_columns, private_labels,
                 result.feature_names);
    for (std::vector<double>& column : private_columns) {
        for (double& value : column) {
            value = std::clamp(value, params.clip_min, params.clip_max);
        }
    }
    DpReducer reducer{collectives, params, result.feature_names.size(),
                      1 + static_cast<std::size_t>(params.gbdt.num_trees) *
                              static_cast<std::size_t>(params.gbdt.max_depth + 1)};
    const AllReduce private_reduce =
        [&](const std::string& round, const std::vector<double>& local,
            GbdtReduce operation) {
            return reducer(round, local, operation);
        };
    // Training-data metrics are additional private releases, not telemetry.
    // Suppress before computing or reducing them, even with a hook installed.
    TrainHooks private_hooks = hooks;
    private_hooks.suppress_metrics = true;
    const GbdtTrainResult private_model = train_gbdt(
        private_columns, result.feature_names, private_labels, params.gbdt,
        private_reduce, tag + ".dp_gbdt", private_hooks);
    if (!private_model.ok()) {
        result.synthetic_rows.clear();
        result.error = "synthetic_private: DP GBDT failed: " +
                       private_model.status.message;
        return result;
    }
    result.private_model = private_model.model;
    result.metrics.privacy.mechanisms = reducer.mechanisms;
    result.metrics.privacy.epsilon_spent =
        reducer.epsilon_per_mechanism() *
        static_cast<double>(reducer.mechanisms);
    result.metrics.privacy.delta_spent =
        params.noise == PrivacyNoise::Gaussian
            ? params.delta * static_cast<double>(reducer.mechanisms) /
                  static_cast<double>(reducer.maximum_mechanisms)
            : 0.0;
    result.metrics.privacy.mechanism =
        params.noise == PrivacyNoise::Gaussian ? "Gaussian" : "Laplace";
    return result;
}

SyntheticPrivateResult run_synthetic_private(
    const std::vector<nlohmann::json>& rows,
    const data::DatasetContract& contract,
    const SyntheticPrivateParams& params, const TrainHooks& hooks) {
    const PrivateCollectives collectives{
        .allreduce = [](const std::string&, const std::vector<double>& local,
                        GbdtReduce) {
            return local;
        },
        .gather = [](const std::string&, const std::vector<double>& local) {
            return std::vector<std::vector<double>>{local};
        },
        .broadcast = [](const std::string&,
                        const std::vector<double>& root_value) {
            return root_value;
        },
        .is_root = true};
    return run_synthetic_private(rows, contract, params, collectives,
                                 "synthetic_private", hooks);
}

}  // namespace zetta::ml
