// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"

// Native label-efficiency training (#1058): active learning plus a generative
// weak-supervision label model. The distributed entry consumes one row shard per
// participant. Model gradients and label-model sufficient statistics are combined
// with JobRound collectives, so single-node and multi-node runs use the same math.
//
// See docs/ml/label-efficiency.md.
namespace zetta::ml {

enum class UncertaintyMeasure {
    Margin,
    Entropy,
};

std::string_view to_string(UncertaintyMeasure measure);
std::optional<UncertaintyMeasure> uncertainty_measure_from_string(
    std::string_view name);

enum class RuleOperator {
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
    Equal,
};

std::string_view to_string(RuleOperator rule_operator);
std::optional<RuleOperator> rule_operator_from_string(std::string_view name);

struct LabellingFunction {
    std::string name;
    std::string column;
    RuleOperator rule_operator = RuleOperator::GreaterEqual;
    double value = 0.0;
    int label = 1;
    std::optional<int> otherwise_label;
};

struct LabelEfficiencyParams {
    std::size_t queue_size = 20;
    std::size_t initial_labels = 4;
    double gold_fraction = 0.2;
    std::uint64_t seed = 1058;
    UncertaintyMeasure uncertainty = UncertaintyMeasure::Margin;
    double diversity_weight = 0.25;
    std::size_t logistic_iterations = 80;
    double logistic_learning_rate = 0.15;
    std::size_t label_model_iterations = 40;
    std::vector<LabellingFunction> labelling_functions;
};

struct LabelEfficiencyRow {
    std::int64_t entity = 0;
    std::vector<double> features;
    int gold_label = 0;
};

struct LabelQueueEntry {
    std::size_t rank = 0;
    std::int64_t entity = 0;
    double uncertainty = 0.0;
    double diversity = 0.0;
    double score = 0.0;

    bool operator==(const LabelQueueEntry&) const = default;
};

struct WeakLabel {
    std::int64_t entity = 0;
    double probability = 0.0;
    std::vector<std::string> contributing_functions;
};

struct LabellingFunctionAccuracy {
    std::string name;
    double estimated = 0.0;
};

struct LabelEfficiencyMetrics {
    double active_accuracy_at_budget = 0.0;
    double random_accuracy_at_budget = 0.0;
    double label_model_gold_accuracy = 0.0;
    double accuracy_gain_at_budget = 0.0;
    std::size_t labelling_budget = 0;
    std::size_t labels_saved = 0;
    std::size_t gold_rows = 0;
    std::size_t unlabelled_rows = 0;
};

struct LabelEfficiencyResult {
    std::vector<LabelQueueEntry> label_queue;
    // Each participant receives weak labels for only its owned rows. This keeps
    // the output path compute-near-data and avoids broadcasting the unlabeled mass.
    std::vector<WeakLabel> local_weak_labels;
    std::vector<LabellingFunctionAccuracy> labelling_function_accuracies;
    LabelEfficiencyMetrics metrics;
    zetta::compute::JobStatus status = zetta::compute::JobStatus::Failed;
    bool is_root = false;
    std::string error;

    bool ok() const noexcept {
        return status == zetta::compute::JobStatus::Succeeded && error.empty();
    }
};

// The deterministic, seed-controlled membership rule used for the evaluation
// holdout. Output writers and tests use the same rule as the trainer.
bool is_label_efficiency_gold(
    std::int64_t entity, const LabelEfficiencyParams& params);

inline constexpr std::chrono::milliseconds kDefaultLabelEfficiencyTimeout{10000};

// Run M-19 over this node's owned row shard. Call concurrently on every
// participant using the same parameters and feature-column ordering.
LabelEfficiencyResult run_label_efficiency_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const std::vector<LabelEfficiencyRow>& local_rows,
    const std::vector<std::string>& feature_columns,
    const LabelEfficiencyParams& params,
    const std::string& round_tag = {},
    std::chrono::milliseconds timeout = kDefaultLabelEfficiencyTimeout);

} // namespace zetta::ml
