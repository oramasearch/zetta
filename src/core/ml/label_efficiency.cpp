// SPDX-License-Identifier: MIT

#include "zetta/ml/label_efficiency.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

constexpr double kProbabilityFloor = 1e-9;

struct SelectedRow {
    LabelEfficiencyRow row;
    std::size_t owner = 0;
};

struct Normalization {
    std::vector<double> means;
    std::vector<double> scales;
};

struct Candidate {
    bool present = false;
    std::size_t owner = 0;
    LabelEfficiencyRow row;
    double uncertainty = 0.0;
    double diversity = 0.0;
    double score = 0.0;
    double random_key = 0.0;
};

std::uint64_t mix_hash(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

double unit_hash(std::int64_t entity, std::uint64_t seed,
                 std::uint64_t stream) {
    const std::uint64_t raw = static_cast<std::uint64_t>(entity);
    const std::uint64_t hashed = mix_hash(raw ^ mix_hash(seed + stream));
    return static_cast<double>(hashed >> 11) /
           static_cast<double>(1ull << 53);
}

bool is_gold(const LabelEfficiencyRow& row,
             const LabelEfficiencyParams& params) {
    return unit_hash(row.entity, params.seed, 0x676f6c64ull) <
           params.gold_fraction;
}

double clamp_probability(double value) {
    return std::clamp(value, kProbabilityFloor,
                      1.0 - kProbabilityFloor);
}

double sigmoid(double value) {
    if (value >= 0.0) {
        const double exponent = std::exp(-value);
        return 1.0 / (1.0 + exponent);
    }
    const double exponent = std::exp(value);
    return exponent / (1.0 + exponent);
}

std::vector<double> normalized_features(const LabelEfficiencyRow& row,
                                        const Normalization& normalization) {
    std::vector<double> normalized(row.features.size(), 0.0);
    for (std::size_t index = 0; index < row.features.size(); ++index) {
        normalized[index] =
            (row.features[index] - normalization.means[index]) /
            normalization.scales[index];
    }
    return normalized;
}

double predict_probability(const std::vector<double>& weights,
                           const LabelEfficiencyRow& row,
                           const Normalization& normalization) {
    double linear = weights.empty() ? 0.0 : weights.front();
    const std::vector<double> features =
        normalized_features(row, normalization);
    for (std::size_t index = 0; index < features.size(); ++index) {
        linear += weights[index + 1] * features[index];
    }
    return sigmoid(linear);
}

double uncertainty_score(double probability, UncertaintyMeasure measure) {
    const double bounded = clamp_probability(probability);
    if (measure == UncertaintyMeasure::Margin) {
        return 1.0 - 2.0 * std::abs(bounded - 0.5);
    }
    const double entropy =
        -bounded * std::log(bounded) -
        (1.0 - bounded) * std::log(1.0 - bounded);
    return entropy / std::log(2.0);
}

double diversity_score(const LabelEfficiencyRow& row,
                       const std::vector<SelectedRow>& selected,
                       const Normalization& normalization) {
    if (selected.empty()) {
        return 1.0;
    }
    const std::vector<double> candidate =
        normalized_features(row, normalization);
    double minimum = std::numeric_limits<double>::infinity();
    for (const SelectedRow& known : selected) {
        const std::vector<double> existing =
            normalized_features(known.row, normalization);
        double squared = 0.0;
        for (std::size_t index = 0; index < candidate.size(); ++index) {
            const double difference = candidate[index] - existing[index];
            squared += difference * difference;
        }
        minimum = std::min(minimum, std::sqrt(squared));
    }
    return minimum / (1.0 + minimum);
}

std::vector<double> encode_candidate(const Candidate& candidate,
                                     std::size_t feature_count) {
    std::vector<double> wire(9 + feature_count, 0.0);
    if (!candidate.present) {
        return wire;
    }
    wire[0] = 1.0;
    wire[1] = static_cast<double>(candidate.owner);
    const std::uint64_t entity =
        std::bit_cast<std::uint64_t>(candidate.row.entity);
    wire[2] = static_cast<double>(entity & 0xffffffffull);
    wire[3] = static_cast<double>(entity >> 32);
    wire[4] = static_cast<double>(candidate.row.gold_label);
    wire[5] = candidate.uncertainty;
    wire[6] = candidate.diversity;
    wire[7] = candidate.score;
    wire[8] = candidate.random_key;
    for (std::size_t index = 0; index < feature_count; ++index) {
        wire[index + 9] = candidate.row.features[index];
    }
    return wire;
}

std::optional<Candidate> decode_candidate(const std::vector<double>& wire,
                                          std::size_t feature_count) {
    if (wire.size() != 9 + feature_count || wire[0] == 0.0) {
        return std::nullopt;
    }
    Candidate candidate;
    candidate.present = true;
    candidate.owner = static_cast<std::size_t>(wire[1]);
    const std::uint64_t entity =
        static_cast<std::uint64_t>(wire[2]) |
        (static_cast<std::uint64_t>(wire[3]) << 32);
    candidate.row.entity = std::bit_cast<std::int64_t>(entity);
    candidate.row.gold_label = static_cast<int>(wire[4]);
    candidate.uncertainty = wire[5];
    candidate.diversity = wire[6];
    candidate.score = wire[7];
    candidate.random_key = wire[8];
    candidate.row.features.assign(wire.begin() + 9, wire.end());
    return candidate;
}

std::optional<SelectedRow> select_candidate(
    compute::JobRound& round, const std::string& tag,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const std::unordered_set<std::int64_t>& excluded,
    const std::vector<double>& weights,
    const std::vector<SelectedRow>& selected,
    const Normalization& normalization,
    const LabelEfficiencyParams& params, bool random,
    std::size_t local_owner, std::chrono::milliseconds timeout,
    LabelQueueEntry* queue_entry) {
    Candidate local;
    for (const LabelEfficiencyRow& row : local_rows) {
        if (is_gold(row, params) || excluded.contains(row.entity)) {
            continue;
        }
        Candidate candidate;
        candidate.present = true;
        candidate.owner = local_owner;
        candidate.row = row;
        candidate.random_key =
            unit_hash(row.entity, params.seed, random ? 0x72616e64ull
                                                      : 0x61637469ull);
        const double probability =
            predict_probability(weights, row, normalization);
        candidate.uncertainty =
            uncertainty_score(probability, params.uncertainty);
        candidate.diversity =
            diversity_score(row, selected, normalization);
        candidate.score =
            (1.0 - params.diversity_weight) * candidate.uncertainty +
            params.diversity_weight * candidate.diversity;
        const bool better_random =
            !local.present || candidate.random_key < local.random_key ||
            (candidate.random_key == local.random_key &&
             candidate.row.entity < local.row.entity);
        const bool better_active =
            !local.present || candidate.score > local.score ||
            (candidate.score == local.score &&
             candidate.row.entity < local.row.entity);
        if ((random && better_random) || (!random && better_active)) {
            local = std::move(candidate);
        }
    }

    const std::size_t feature_count = normalization.means.size();
    const std::optional<std::vector<std::vector<double>>> gathered =
        round.gather(tag + ".candidates",
                     encode_candidate(local, feature_count), timeout);
    if (!gathered) {
        return std::nullopt;
    }

    Candidate chosen;
    if (!gathered->empty()) {
        for (const std::vector<double>& wire : *gathered) {
            const std::optional<Candidate> candidate =
                decode_candidate(wire, feature_count);
            if (!candidate) {
                continue;
            }
            const bool better_random =
                !chosen.present || candidate->random_key < chosen.random_key ||
                (candidate->random_key == chosen.random_key &&
                 candidate->row.entity < chosen.row.entity);
            const bool better_active =
                !chosen.present || candidate->score > chosen.score ||
                (candidate->score == chosen.score &&
                 candidate->row.entity < chosen.row.entity);
            if ((random && better_random) || (!random && better_active)) {
                chosen = *candidate;
            }
        }
    }
    const std::optional<std::vector<double>> broadcast =
        round.broadcast(tag + ".chosen",
                        encode_candidate(chosen, feature_count), timeout);
    if (!broadcast) {
        return std::nullopt;
    }
    const std::optional<Candidate> decoded =
        decode_candidate(*broadcast, feature_count);
    if (!decoded) {
        return SelectedRow{};
    }
    if (queue_entry != nullptr) {
        queue_entry->entity = decoded->row.entity;
        queue_entry->uncertainty = decoded->uncertainty;
        queue_entry->diversity = decoded->diversity;
        queue_entry->score = decoded->score;
    }
    return SelectedRow{decoded->row, decoded->owner};
}

std::optional<std::vector<double>> fit_logistic(
    compute::JobRound& round, const std::string& tag,
    const std::vector<SelectedRow>& selected,
    const Normalization& normalization,
    const LabelEfficiencyParams& params, std::size_t local_owner,
    std::chrono::milliseconds timeout) {
    const std::size_t feature_count = normalization.means.size();
    std::vector<double> weights(feature_count + 1, 0.0);
    for (std::size_t iteration = 0;
         iteration < params.logistic_iterations; ++iteration) {
        std::vector<double> local_gradient(feature_count + 2, 0.0);
        for (const SelectedRow& selected_row : selected) {
            if (selected_row.owner != local_owner) {
                continue;
            }
            const double probability =
                predict_probability(weights, selected_row.row, normalization);
            const double error =
                probability - static_cast<double>(selected_row.row.gold_label);
            const std::vector<double> features =
                normalized_features(selected_row.row, normalization);
            local_gradient[0] += error;
            for (std::size_t index = 0; index < feature_count; ++index) {
                local_gradient[index + 1] += error * features[index];
            }
            local_gradient.back() += 1.0;
        }
        const std::optional<std::vector<double>> global = round.allreduce(
            tag + ".gradient." + std::to_string(iteration), local_gradient,
            zetta::cluster::CollectiveOp::Sum, timeout);
        if (!global || global->size() != local_gradient.size()) {
            return std::nullopt;
        }
        const double count = std::max(1.0, global->back());
        const double rate = params.logistic_learning_rate /
                            std::sqrt(static_cast<double>(iteration + 1));
        for (std::size_t index = 0; index < weights.size(); ++index) {
            weights[index] -= rate * (*global)[index] / count;
        }
    }
    return weights;
}

std::optional<double> evaluate_accuracy(
    compute::JobRound& round, const std::string& tag,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const std::vector<double>& weights, const Normalization& normalization,
    const LabelEfficiencyParams& params,
    std::chrono::milliseconds timeout) {
    std::vector<double> local(2, 0.0);
    for (const LabelEfficiencyRow& row : local_rows) {
        if (!is_gold(row, params)) {
            continue;
        }
        const int prediction =
            predict_probability(weights, row, normalization) >= 0.5 ? 1 : 0;
        local[0] += prediction == row.gold_label ? 1.0 : 0.0;
        local[1] += 1.0;
    }
    const std::optional<std::vector<double>> global = round.allreduce(
        tag, local, zetta::cluster::CollectiveOp::Sum, timeout);
    if (!global || global->size() != 2 || (*global)[1] == 0.0) {
        return std::nullopt;
    }
    return (*global)[0] / (*global)[1];
}

bool predicate_matches(double feature, const LabellingFunction& function) {
    switch (function.rule_operator) {
        case RuleOperator::Less:
            return feature < function.value;
        case RuleOperator::LessEqual:
            return feature <= function.value;
        case RuleOperator::Greater:
            return feature > function.value;
        case RuleOperator::GreaterEqual:
            return feature >= function.value;
        case RuleOperator::Equal:
            return feature == function.value;
    }
    return false;
}

std::optional<int> vote_of(
    const LabelEfficiencyRow& row, const LabellingFunction& function,
    const std::unordered_map<std::string, std::size_t>& feature_indexes) {
    const auto found = feature_indexes.find(function.column);
    if (found == feature_indexes.end() || found->second >= row.features.size()) {
        return std::nullopt;
    }
    if (predicate_matches(row.features[found->second], function)) {
        return function.label;
    }
    return function.otherwise_label;
}

double weak_probability(const std::vector<std::optional<int>>& votes,
                        const std::vector<double>& accuracies,
                        double prior) {
    double positive = std::log(clamp_probability(prior));
    double negative = std::log(clamp_probability(1.0 - prior));
    for (std::size_t index = 0; index < votes.size(); ++index) {
        if (!votes[index]) {
            continue;
        }
        const double accuracy = clamp_probability(accuracies[index]);
        positive += std::log(*votes[index] == 1 ? accuracy
                                               : 1.0 - accuracy);
        negative += std::log(*votes[index] == 0 ? accuracy
                                               : 1.0 - accuracy);
    }
    return sigmoid(positive - negative);
}

using FeatureIndexes = std::unordered_map<std::string, std::size_t>;

std::optional<std::string> validate_inputs(
    const std::vector<LabelEfficiencyRow>& local_rows,
    const std::vector<std::string>& feature_columns,
    const LabelEfficiencyParams& params, FeatureIndexes& feature_indexes) {
    if (feature_columns.empty()) {
        return "label efficiency: no feature columns";
    }
    if (params.queue_size == 0 || params.initial_labels == 0) {
        return "label efficiency: queue_size and initial_labels must be positive";
    }
    if (params.logistic_iterations == 0 ||
        params.label_model_iterations == 0) {
        return "label efficiency: iteration counts must be positive";
    }
    if (!(params.gold_fraction > 0.0 && params.gold_fraction < 1.0)) {
        return "label efficiency: gold_fraction must be between 0 and 1";
    }
    if (params.diversity_weight < 0.0 ||
        params.diversity_weight > 1.0) {
        return "label efficiency: diversity_weight must be in [0,1]";
    }
    if (params.labelling_functions.empty()) {
        return "label efficiency: no labelling functions";
    }
    for (const LabelEfficiencyRow& row : local_rows) {
        if (row.features.size() != feature_columns.size()) {
            return "label efficiency: row feature count does not match columns";
        }
        if (row.gold_label != 0 && row.gold_label != 1) {
            return "label efficiency: gold labels must be binary";
        }
    }
    for (std::size_t index = 0; index < feature_columns.size(); ++index) {
        feature_indexes.emplace(feature_columns[index], index);
    }
    for (const LabellingFunction& function : params.labelling_functions) {
        if (function.name.empty() ||
            !feature_indexes.contains(function.column) ||
            (function.label != 0 && function.label != 1) ||
            (function.otherwise_label &&
             *function.otherwise_label != 0 &&
             *function.otherwise_label != 1)) {
            return "label efficiency: invalid labelling function '" +
                   function.name + "'";
        }
    }
    return std::nullopt;
}

std::optional<Normalization> compute_normalization(
    compute::JobRound& round, const std::string& tag,
    const std::vector<LabelEfficiencyRow>& local_rows,
    std::size_t feature_count, std::chrono::milliseconds timeout) {
    std::vector<double> local_stats(1 + 2 * feature_count, 0.0);
    for (const LabelEfficiencyRow& row : local_rows) {
        local_stats[0] += 1.0;
        for (std::size_t index = 0; index < row.features.size(); ++index) {
            local_stats[index + 1] += row.features[index];
            local_stats[index + 1 + feature_count] +=
                row.features[index] * row.features[index];
        }
    }
    const std::optional<std::vector<double>> global_stats = round.allreduce(
        tag + ".normalization", local_stats,
        zetta::cluster::CollectiveOp::Sum, timeout);
    if (!global_stats || global_stats->size() != local_stats.size() ||
        global_stats->front() == 0.0) {
        return std::nullopt;
    }
    Normalization normalization;
    normalization.means.resize(feature_count, 0.0);
    normalization.scales.resize(feature_count, 1.0);
    const double total_rows = global_stats->front();
    for (std::size_t index = 0; index < feature_count; ++index) {
        const double mean = (*global_stats)[index + 1] / total_rows;
        const double mean_square =
            (*global_stats)[index + 1 + feature_count] / total_rows;
        normalization.means[index] = mean;
        normalization.scales[index] =
            std::sqrt(std::max(1e-12, mean_square - mean * mean));
    }
    return normalization;
}

bool count_splits(compute::JobRound& round, const std::string& tag,
                  const std::vector<LabelEfficiencyRow>& local_rows,
                  const LabelEfficiencyParams& params,
                  std::chrono::milliseconds timeout,
                  LabelEfficiencyMetrics& metrics) {
    std::vector<double> local_counts(2, 0.0);
    for (const LabelEfficiencyRow& row : local_rows) {
        local_counts[is_gold(row, params) ? 0 : 1] += 1.0;
    }
    const std::optional<std::vector<double>> global_counts = round.allreduce(
        tag + ".row_counts", local_counts,
        zetta::cluster::CollectiveOp::Sum, timeout);
    if (!global_counts || global_counts->size() != 2 ||
        (*global_counts)[0] == 0.0 || (*global_counts)[1] == 0.0) {
        return false;
    }
    metrics.gold_rows = static_cast<std::size_t>((*global_counts)[0]);
    metrics.unlabelled_rows =
        static_cast<std::size_t>((*global_counts)[1]);
    return true;
}

void record_active_metrics(const std::vector<double>& active_curve,
                           const std::vector<double>& random_curve,
                           std::size_t selected_count,
                           const LabelEfficiencyParams& params,
                           LabelEfficiencyMetrics& metrics) {
    metrics.labelling_budget = selected_count;
    metrics.active_accuracy_at_budget = active_curve.back();
    metrics.random_accuracy_at_budget = random_curve.back();
    metrics.accuracy_gain_at_budget =
        active_curve.back() - random_curve.back();
    const double target = active_curve.back();
    const auto reaches_target = [target](double accuracy) {
        return accuracy + 1e-12 >= target;
    };
    const auto active_first =
        std::find_if(active_curve.begin(), active_curve.end(), reaches_target);
    const auto random_first =
        std::find_if(random_curve.begin(), random_curve.end(), reaches_target);
    const std::size_t active_budget =
        static_cast<std::size_t>(
            std::distance(active_curve.begin(), active_first)) +
        params.initial_labels + 1;
    const std::size_t random_budget =
        random_first == random_curve.end()
            ? params.initial_labels + random_curve.size() + 1
            : static_cast<std::size_t>(
                  std::distance(random_curve.begin(), random_first)) +
                  params.initial_labels + 1;
    metrics.labels_saved =
        random_budget > active_budget ? random_budget - active_budget : 0;
}

bool run_active_learning(
    compute::JobRound& round, const std::string& tag,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const Normalization& normalization, const LabelEfficiencyParams& params,
    std::size_t local_owner, std::chrono::milliseconds timeout,
    LabelEfficiencyResult& result) {
    std::vector<SelectedRow> active_selected;
    std::vector<SelectedRow> random_selected;
    std::unordered_set<std::int64_t> active_entities;
    std::unordered_set<std::int64_t> random_entities;
    const std::vector<double> empty_weights(
        normalization.means.size() + 1, 0.0);
    for (std::size_t index = 0; index < params.initial_labels; ++index) {
        const std::optional<SelectedRow> selected = select_candidate(
            round, tag + ".initial." + std::to_string(index), local_rows,
            active_entities, empty_weights, active_selected, normalization,
            params, true, local_owner, timeout, nullptr);
        if (!selected || selected->row.features.empty()) {
            break;
        }
        active_selected.push_back(*selected);
        random_selected.push_back(*selected);
        active_entities.insert(selected->row.entity);
        random_entities.insert(selected->row.entity);
    }
    if (active_selected.empty()) {
        result.error =
            "label efficiency: no rows available for an initial label";
        return false;
    }

    std::vector<double> active_curve;
    std::vector<double> random_curve;
    for (std::size_t rank = 0; rank < params.queue_size; ++rank) {
        const std::optional<std::vector<double>> active_weights = fit_logistic(
            round, tag + ".active.fit." + std::to_string(rank),
            active_selected, normalization, params, local_owner, timeout);
        if (!active_weights) {
            result.error = "label efficiency: active-model allreduce failed";
            return false;
        }
        LabelQueueEntry queue_entry;
        queue_entry.rank = rank + 1;
        const std::optional<SelectedRow> active = select_candidate(
            round, tag + ".active.select." + std::to_string(rank), local_rows,
            active_entities, *active_weights, active_selected, normalization,
            params, false, local_owner, timeout, &queue_entry);
        const std::optional<SelectedRow> random = select_candidate(
            round, tag + ".random.select." + std::to_string(rank), local_rows,
            random_entities, empty_weights, random_selected, normalization,
            params, true, local_owner, timeout, nullptr);
        if (!active || !random || active->row.features.empty() ||
            random->row.features.empty()) {
            break;
        }
        active_selected.push_back(*active);
        random_selected.push_back(*random);
        active_entities.insert(active->row.entity);
        random_entities.insert(random->row.entity);
        result.label_queue.push_back(queue_entry);

        const std::optional<std::vector<double>> updated_active = fit_logistic(
            round, tag + ".active.updated." + std::to_string(rank),
            active_selected, normalization, params, local_owner, timeout);
        const std::optional<std::vector<double>> updated_random = fit_logistic(
            round, tag + ".random.updated." + std::to_string(rank),
            random_selected, normalization, params, local_owner, timeout);
        if (!updated_active || !updated_random) {
            result.error =
                "label efficiency: benchmark model allreduce failed";
            return false;
        }
        const std::optional<double> active_accuracy = evaluate_accuracy(
            round, tag + ".active.accuracy." + std::to_string(rank),
            local_rows, *updated_active, normalization, params, timeout);
        const std::optional<double> random_accuracy = evaluate_accuracy(
            round, tag + ".random.accuracy." + std::to_string(rank),
            local_rows, *updated_random, normalization, params, timeout);
        if (!active_accuracy || !random_accuracy) {
            result.error =
                "label efficiency: gold evaluation allreduce failed";
            return false;
        }
        active_curve.push_back(*active_accuracy);
        random_curve.push_back(*random_accuracy);
    }
    if (result.label_queue.empty()) {
        result.error =
            "label efficiency: no candidates remain for the label queue";
        return false;
    }
    record_active_metrics(active_curve, random_curve, active_selected.size(),
                          params, result.metrics);
    return true;
}

struct LabelModelFit {
    std::vector<double> accuracies;
    double prior = 0.5;
};

std::optional<LabelModelFit> fit_label_model(
    compute::JobRound& round, const std::string& tag,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const FeatureIndexes& feature_indexes,
    const LabelEfficiencyParams& params,
    std::chrono::milliseconds timeout) {
    const std::size_t function_count = params.labelling_functions.size();
    LabelModelFit fit;
    fit.accuracies.assign(function_count, 0.7);
    for (std::size_t iteration = 0;
         iteration < params.label_model_iterations; ++iteration) {
        std::vector<double> local(2 + 2 * function_count, 0.0);
        for (const LabelEfficiencyRow& row : local_rows) {
            if (is_gold(row, params)) {
                continue;
            }
            std::vector<std::optional<int>> votes;
            votes.reserve(function_count);
            bool any_vote = false;
            for (const LabellingFunction& function :
                 params.labelling_functions) {
                votes.push_back(vote_of(row, function, feature_indexes));
                any_vote = any_vote || votes.back().has_value();
            }
            if (!any_vote) {
                continue;
            }
            const double probability =
                weak_probability(votes, fit.accuracies, fit.prior);
            local[0] += probability;
            local[1] += 1.0;
            for (std::size_t index = 0; index < function_count; ++index) {
                if (!votes[index]) {
                    continue;
                }
                local[2 + 2 * index] +=
                    *votes[index] == 1 ? probability : 1.0 - probability;
                local[3 + 2 * index] += 1.0;
            }
        }
        const std::optional<std::vector<double>> global = round.allreduce(
            tag + ".label_model." + std::to_string(iteration), local,
            zetta::cluster::CollectiveOp::Sum, timeout);
        if (!global || global->size() != local.size() ||
            (*global)[1] == 0.0) {
            return std::nullopt;
        }
        fit.prior = std::clamp((*global)[0] / (*global)[1], 0.05, 0.95);
        for (std::size_t index = 0; index < function_count; ++index) {
            const double coverage = (*global)[3 + 2 * index];
            if (coverage > 0.0) {
                fit.accuracies[index] = std::clamp(
                    (*global)[2 + 2 * index] / coverage, 0.5001, 0.9999);
            }
        }
    }
    return fit;
}

bool collect_weak_labels(
    compute::JobRound& round, const std::string& tag,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const FeatureIndexes& feature_indexes,
    const LabelEfficiencyParams& params, const LabelModelFit& fit,
    std::chrono::milliseconds timeout, LabelEfficiencyResult& result) {
    const std::size_t function_count = params.labelling_functions.size();
    for (std::size_t index = 0; index < function_count; ++index) {
        result.labelling_function_accuracies.push_back(
            {params.labelling_functions[index].name, fit.accuracies[index]});
    }
    std::vector<double> local_gold_accuracy(2, 0.0);
    for (const LabelEfficiencyRow& row : local_rows) {
        std::vector<std::optional<int>> votes;
        votes.reserve(function_count);
        std::vector<std::string> contributors;
        for (std::size_t index = 0; index < function_count; ++index) {
            const std::optional<int> vote = vote_of(
                row, params.labelling_functions[index], feature_indexes);
            votes.push_back(vote);
            if (vote) {
                contributors.push_back(params.labelling_functions[index].name);
            }
        }
        if (contributors.empty()) {
            continue;
        }
        const double probability =
            weak_probability(votes, fit.accuracies, fit.prior);
        if (is_gold(row, params)) {
            const int prediction = probability >= 0.5 ? 1 : 0;
            local_gold_accuracy[0] +=
                prediction == row.gold_label ? 1.0 : 0.0;
            local_gold_accuracy[1] += 1.0;
            continue;
        }
        result.local_weak_labels.push_back(
            {row.entity, probability, std::move(contributors)});
    }
    const std::optional<std::vector<double>> label_accuracy = round.allreduce(
        tag + ".label_model.gold_accuracy", local_gold_accuracy,
        zetta::cluster::CollectiveOp::Sum, timeout);
    if (!label_accuracy || label_accuracy->size() != 2 ||
        (*label_accuracy)[1] == 0.0) {
        result.error =
            "label efficiency: no gold rows received labelling-function votes";
        return false;
    }
    result.metrics.label_model_gold_accuracy =
        (*label_accuracy)[0] / (*label_accuracy)[1];
    return true;
}

} // namespace

bool is_label_efficiency_gold(
    std::int64_t entity, const LabelEfficiencyParams& params) {
    LabelEfficiencyRow row;
    row.entity = entity;
    return is_gold(row, params);
}

std::string_view to_string(UncertaintyMeasure measure) {
    switch (measure) {
        case UncertaintyMeasure::Margin:
            return "margin";
        case UncertaintyMeasure::Entropy:
            return "entropy";
    }
    return "margin";
}

std::optional<UncertaintyMeasure> uncertainty_measure_from_string(
    std::string_view name) {
    if (name == "margin") {
        return UncertaintyMeasure::Margin;
    }
    if (name == "entropy") {
        return UncertaintyMeasure::Entropy;
    }
    return std::nullopt;
}

std::string_view to_string(RuleOperator rule_operator) {
    switch (rule_operator) {
        case RuleOperator::Less:
            return "less";
        case RuleOperator::LessEqual:
            return "less_equal";
        case RuleOperator::Greater:
            return "greater";
        case RuleOperator::GreaterEqual:
            return "greater_equal";
        case RuleOperator::Equal:
            return "equal";
    }
    return "greater_equal";
}

std::optional<RuleOperator> rule_operator_from_string(std::string_view name) {
    if (name == "less") {
        return RuleOperator::Less;
    }
    if (name == "less_equal") {
        return RuleOperator::LessEqual;
    }
    if (name == "greater") {
        return RuleOperator::Greater;
    }
    if (name == "greater_equal") {
        return RuleOperator::GreaterEqual;
    }
    if (name == "equal") {
        return RuleOperator::Equal;
    }
    return std::nullopt;
}

LabelEfficiencyResult run_label_efficiency_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const std::vector<std::string>& feature_columns,
    const LabelEfficiencyParams& params, const std::string& round_tag,
    std::chrono::milliseconds timeout) {
    LabelEfficiencyResult result;
    const std::string tag =
        round_tag.empty() ? "ml.label_efficiency" : round_tag;

    result.status = coordinator.run(
        job, [&](compute::JobRound& round) -> compute::JobCoordinator::Outcome {
            result.is_root = round.is_root();
            FeatureIndexes feature_indexes;
            if (const std::optional<std::string> invalid = validate_inputs(
                    local_rows, feature_columns, params, feature_indexes)) {
                result.error = *invalid;
                return {.ok = false, .detail = result.error};
            }

            const std::vector<std::string> participants = round.participants();
            const auto owner_found = std::find(participants.begin(),
                                               participants.end(),
                                               round.node_id());
            const std::size_t local_owner = static_cast<std::size_t>(
                std::distance(participants.begin(), owner_found));
            const std::optional<Normalization> normalization =
                compute_normalization(round, tag, local_rows,
                                      feature_columns.size(), timeout);
            if (!normalization) {
                result.error =
                    "label efficiency: feature-statistics allreduce failed";
                return {.ok = false, .detail = result.error};
            }
            if (!count_splits(round, tag, local_rows, params, timeout,
                              result.metrics)) {
                result.error =
                    "label efficiency: deterministic split needs gold and unlabeled rows";
                return {.ok = false, .detail = result.error};
            }

            if (!run_active_learning(round, tag, local_rows, *normalization,
                                     params, local_owner, timeout, result)) {
                return {.ok = false, .detail = result.error};
            }

            const std::optional<LabelModelFit> label_model = fit_label_model(
                round, tag, local_rows, feature_indexes, params, timeout);
            if (!label_model) {
                result.error =
                    "label efficiency: label-model allreduce failed";
                return {.ok = false, .detail = result.error};
            }
            if (!collect_weak_labels(round, tag, local_rows, feature_indexes,
                                     params, *label_model, timeout, result)) {
                return {.ok = false, .detail = result.error};
            }

            return {
                .ok = true,
                .detail =
                    "label efficiency over " +
                    std::to_string(participants.size()) + " nodes produced " +
                    std::to_string(result.label_queue.size()) +
                    " review rows and " +
                    std::to_string(result.metrics.unlabelled_rows) +
                    " probabilistic-label candidates",
            };
        });
    if (result.status != compute::JobStatus::Succeeded &&
        result.error.empty()) {
        result.error = job.detail();
    }
    return result;
}

} // namespace zetta::ml
