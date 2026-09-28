#include "zetta/ml/rank.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

using compute::ComputeStatus;

std::uint64_t mix_group(std::int64_t group, std::uint64_t seed) {
    std::uint64_t value = static_cast<std::uint64_t>(group) ^ seed;
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

bool validation_group(std::int64_t group, double fraction, std::uint64_t seed) {
    if (fraction <= 0.0) {
        return false;
    }
    if (fraction >= 1.0) {
        return true;
    }
    const std::uint64_t hash = mix_group(group, seed);
    const double unit = static_cast<double>(hash >> 11) / static_cast<double>(1ull << 53);
    return unit < fraction;
}

double relevance_gain(double label) {
    return std::exp2(std::min(label, 60.0)) - 1.0;
}

double rank_discount(std::size_t zero_based_rank) {
    return 1.0 / std::log2(static_cast<double>(zero_based_rank) + 2.0);
}

std::vector<std::size_t> score_order(const std::vector<std::size_t>& rows,
                                     const std::vector<double>& scores) {
    std::vector<std::size_t> order = rows;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        if (scores[left] != scores[right]) {
            return scores[left] > scores[right];
        }
        return left < right;
    });
    return order;
}

double dcg_at_k(const std::vector<std::size_t>& order, const std::vector<double>& labels,
                std::size_t cutoff) {
    const std::size_t count = std::min(cutoff, order.size());
    double result = 0.0;
    for (std::size_t position = 0; position < count; ++position) {
        result += relevance_gain(labels[order[position]]) * rank_discount(position);
    }
    return result;
}

std::map<std::int64_t, std::vector<std::size_t>>
group_rows(const std::vector<std::int64_t>& groups) {
    std::map<std::int64_t, std::vector<std::size_t>> result;
    for (std::size_t row = 0; row < groups.size(); ++row) {
        result[groups[row]].push_back(row);
    }
    return result;
}

RankDataset subset(const RankDataset& dataset, const std::vector<std::size_t>& rows) {
    RankDataset selected;
    selected.feature_names = dataset.feature_names;
    selected.label_name = dataset.label_name;
    selected.entity_name = dataset.entity_name;
    selected.features.assign(dataset.features.size(), {});
    for (std::vector<double>& column : selected.features) {
        column.reserve(rows.size());
    }
    selected.labels.reserve(rows.size());
    selected.groups.reserve(rows.size());
    selected.entities.reserve(rows.size());
    for (const std::size_t row : rows) {
        for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
            selected.features[feature].push_back(dataset.features[feature][row]);
        }
        selected.labels.push_back(dataset.labels[row]);
        selected.groups.push_back(dataset.groups[row]);
        selected.entities.push_back(dataset.entities[row]);
    }
    return selected;
}

ComputeStatus validate_dataset(const RankDataset& dataset) {
    const std::size_t rows = dataset.labels.size();
    if (dataset.feature_names.empty()) {
        return ComputeStatus::invalid_argument(
            "rank: at least one ranking-time feature is required");
    }
    if (dataset.features.size() != dataset.feature_names.size()) {
        return ComputeStatus::invalid_argument(
            "rank: feature column count does not match feature names");
    }
    if (dataset.groups.size() != rows || dataset.entities.size() != rows) {
        return ComputeStatus::invalid_argument(
            "rank: entity, group, label, and feature rows must align");
    }
    for (const std::vector<double>& feature : dataset.features) {
        if (feature.size() != rows) {
            return ComputeStatus::invalid_argument(
                "rank: feature columns have different row counts");
        }
    }
    for (const double label : dataset.labels) {
        if (!std::isfinite(label) || label < 0.0) {
            return ComputeStatus::invalid_argument(
                "rank: relevance labels must be finite and non-negative");
        }
    }
    return ComputeStatus::success();
}

ComputeStatus lambda_gradients(const std::map<std::int64_t, std::vector<std::size_t>>& groups,
                               std::size_t cutoff, const std::vector<double>& raw,
                               const std::vector<double>& labels, std::vector<double>& gradient,
                               std::vector<double>& hessian) {
    if (raw.size() != labels.size()) {
        return ComputeStatus::invalid_argument("rank: raw score and label rows do not align");
    }
    gradient.assign(raw.size(), 0.0);
    hessian.assign(raw.size(), 0.0);
    for (const auto& [_, rows] : groups) {
        std::vector<std::size_t> ideal = rows;
        std::stable_sort(ideal.begin(), ideal.end(), [&](std::size_t left, std::size_t right) {
            if (labels[left] != labels[right]) {
                return labels[left] > labels[right];
            }
            return left < right;
        });
        const double ideal_dcg = dcg_at_k(ideal, labels, cutoff);
        if (ideal_dcg <= 0.0) {
            continue;
        }
        const std::vector<std::size_t> ranked = score_order(rows, raw);
        std::vector<std::size_t> position(raw.size(), 0);
        for (std::size_t index = 0; index < ranked.size(); ++index) {
            position[ranked[index]] = index;
        }
        for (std::size_t first = 0; first < rows.size(); ++first) {
            for (std::size_t second = first + 1; second < rows.size(); ++second) {
                std::size_t higher = rows[first];
                std::size_t lower = rows[second];
                if (labels[higher] == labels[lower]) {
                    continue;
                }
                if (labels[higher] < labels[lower]) {
                    std::swap(higher, lower);
                }
                const std::size_t higher_position = position[higher];
                const std::size_t lower_position = position[lower];
                if (higher_position >= cutoff && lower_position >= cutoff) {
                    continue;
                }
                const double gain_difference =
                    relevance_gain(labels[higher]) - relevance_gain(labels[lower]);
                const double discount_difference =
                    std::abs(rank_discount(higher_position) - rank_discount(lower_position));
                const double delta_ndcg = gain_difference * discount_difference / ideal_dcg;
                const double score_difference = std::clamp(raw[higher] - raw[lower], -50.0, 50.0);
                const double wrong_order_probability = 1.0 / (1.0 + std::exp(score_difference));
                const double lambda = wrong_order_probability * delta_ndcg;
                const double curvature = std::max(
                    wrong_order_probability * (1.0 - wrong_order_probability) * delta_ndcg, 1e-12);
                gradient[higher] -= lambda;
                gradient[lower] += lambda;
                hessian[higher] += curvature;
                hessian[lower] += curvature;
            }
        }
    }
    return ComputeStatus::success();
}

} // namespace

RankParams::RankParams() {
    gbdt.num_trees = 60;
    gbdt.max_depth = 4;
    gbdt.max_bins = 32;
    gbdt.learning_rate = 0.1;
    gbdt.objective = "lambdamart";
    gbdt.holdout_fraction = 0.0;
}

RankGroupSplit split_rank_groups(const std::vector<std::int64_t>& groups, double holdout_fraction,
                                 std::uint64_t seed) {
    RankGroupSplit split;
    const auto grouped = group_rows(groups);
    for (const auto& [group, rows] : grouped) {
        if (validation_group(group, holdout_fraction, seed)) {
            split.validation_groups.push_back(group);
            split.validation_rows.insert(split.validation_rows.end(), rows.begin(), rows.end());
        } else {
            split.training_groups.push_back(group);
            split.training_rows.insert(split.training_rows.end(), rows.begin(), rows.end());
        }
    }
    std::sort(split.training_rows.begin(), split.training_rows.end());
    std::sort(split.validation_rows.begin(), split.validation_rows.end());
    return split;
}

RankMetrics evaluate_ranking(const std::vector<double>& scores, const std::vector<double>& labels,
                             const std::vector<std::int64_t>& groups, std::size_t cutoff) {
    RankMetrics metrics;
    if (cutoff == 0 || scores.size() != labels.size() || scores.size() != groups.size()) {
        return metrics;
    }
    const auto grouped = group_rows(groups);
    double ndcg_sum = 0.0;
    double yield_sum = 0.0;
    for (const auto& [_, rows] : grouped) {
        const std::vector<std::size_t> ranked = score_order(rows, scores);
        std::vector<std::size_t> ideal = rows;
        std::stable_sort(ideal.begin(), ideal.end(), [&](std::size_t left, std::size_t right) {
            if (labels[left] != labels[right]) {
                return labels[left] > labels[right];
            }
            return left < right;
        });
        const double ideal_dcg = dcg_at_k(ideal, labels, cutoff);
        const double actual_dcg = dcg_at_k(ranked, labels, cutoff);
        ndcg_sum += ideal_dcg > 0.0 ? actual_dcg / ideal_dcg : 1.0;
        const std::size_t count = std::min(cutoff, ranked.size());
        for (std::size_t position = 0; position < count; ++position) {
            yield_sum += labels[ranked[position]];
        }
    }
    metrics.groups = grouped.size();
    metrics.rows = labels.size();
    if (metrics.groups > 0) {
        metrics.ndcg_at_k = ndcg_sum / static_cast<double>(metrics.groups);
        metrics.yield_at_k = yield_sum / static_cast<double>(metrics.groups);
    }
    return metrics;
}

std::vector<RankedEntity> rank_entities(const GbdtModel& model, const RankDataset& dataset) {
    std::vector<double> scores(dataset.row_count(), 0.0);
    std::vector<double> row(dataset.feature_names.size(), 0.0);
    for (std::size_t index = 0; index < dataset.row_count(); ++index) {
        for (std::size_t feature = 0; feature < dataset.features.size(); ++feature) {
            row[feature] = dataset.features[feature][index];
        }
        scores[index] = model.predict_raw(row);
    }

    std::vector<RankedEntity> ranked;
    const auto grouped = group_rows(dataset.groups);
    ranked.reserve(dataset.row_count());
    for (const auto& [group, rows] : grouped) {
        std::vector<std::size_t> order = rows;
        std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
            if (scores[left] != scores[right]) {
                return scores[left] > scores[right];
            }
            return dataset.entities[left] < dataset.entities[right];
        });
        for (std::size_t position = 0; position < order.size(); ++position) {
            const std::size_t index = order[position];
            ranked.push_back(RankedEntity{group, dataset.entities[index], scores[index],
                                          dataset.labels[index], position + 1});
        }
    }
    return ranked;
}

LambdaMartTrainResult train_lambda_mart(const RankDataset& dataset, const RankParams& params,
                                        const AllReduce& allreduce, const std::string& tag) {
    LambdaMartTrainResult result;
    if (const ComputeStatus status = validate_dataset(dataset); !status.ok()) {
        result.status = status;
        return result;
    }
    if (params.cutoff == 0) {
        result.status = ComputeStatus::invalid_argument("rank: cutoff must be greater than zero");
        return result;
    }
    const auto grouped = group_rows(dataset.groups);
    GbdtParams gbdt = params.gbdt;
    gbdt.objective = "lambdamart";
    gbdt.holdout_fraction = 0.0;
    TrainHooks hooks;
    hooks.objective_gradient = [grouped, cutoff = params.cutoff](const std::vector<double>& raw,
                                                                 const std::vector<double>& labels,
                                                                 std::vector<double>& gradient,
                                                                 std::vector<double>& hessian) {
        return lambda_gradients(grouped, cutoff, raw, labels, gradient, hessian);
    };
    GbdtTrainResult trained = train_gbdt(dataset.features, dataset.feature_names, dataset.labels,
                                         gbdt, allreduce, tag, hooks);
    result.model = std::move(trained.model);
    result.feature_importances = std::move(trained.feature_importances);
    result.status = trained.status;
    return result;
}

LambdaMartTrainResult train_lambda_mart(const RankDataset& dataset, const RankParams& params) {
    const AllReduce identity = [](const std::string&, const std::vector<double>& local,
                                  GbdtReduce) {
        return local;
    };
    return train_lambda_mart(dataset, params, identity);
}

} // namespace zetta::ml
