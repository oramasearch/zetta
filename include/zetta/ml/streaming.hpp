// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"

namespace zetta {
class Node;
}

namespace zetta::cluster {
class Cluster;
}

namespace zetta::compute {
class ComputeJob;
class JobCoordinator;
}

// Native online / streaming learning (#1060). Events are released in event-time
// order behind a watermark, transformed into per-entity window counts and sums,
// scored by online logistic regression, then updated only when their labels
// become available. Evaluation is prequential: every metric uses the score made
// before that label trained the model.
//
// The distributed form is synchronous SPMD. Each owned partition keeps its
// entity windows locally; participants agree the global action times and
// allreduce the per-time gradient and metric partials. This produces identical
// weights on every participant without moving source rows.
//
// See docs/ml/streaming.md.
namespace zetta::ml {

struct StreamingParams {
    double learning_rate = 0.05;
    double l2_regularization = 0.001;
    double window_duration = 60.0;
    double allowed_lateness = 5.0;
    std::size_t metric_window = 128;
    std::size_t persist_every = 32;
    std::uint64_t seed = 0;
    std::string label_time_column = "label_time";
    std::string input_topic = "streaming.events";
    std::string output_topic = "streaming.scores";

    nlohmann::json to_json() const;
    static std::optional<StreamingParams> from_json(
        const nlohmann::json& value, std::string* error = nullptr);
};

const nlohmann::json& streaming_params_schema();

struct StreamingEvent {
    std::string id;
    std::int64_t entity = 0;
    double event_time = 0.0;
    std::vector<double> values;
    std::optional<double> label;
    std::optional<double> label_time;
    std::size_t arrival_sequence = 0;
};

struct StreamingScore {
    std::string event_id;
    std::int64_t entity = 0;
    double event_time = 0.0;
    double probability = 0.0;
    std::uint64_t model_version = 0;
};

struct OnlineLogisticModel {
    std::vector<std::string> feature_names;
    std::vector<double> weights;
    double bias = 0.0;
    std::uint64_t version = 0;
    std::uint64_t seed = 0;

    double predict(const std::vector<double>& features) const;
    nlohmann::json to_json() const;
    static std::optional<OnlineLogisticModel> from_json(
        const nlohmann::json& value, std::string* error = nullptr);
};

const nlohmann::json& streaming_model_schema();

struct PrequentialMetrics {
    double log_loss = 0.0;
    double accuracy = 0.0;
    double drift_loss_delta = 0.0;
    std::uint64_t evaluated_labels = 0;
    std::uint64_t events_read = 0;
    std::uint64_t events_scored = 0;
    std::uint64_t late_events_excluded = 0;
    std::uint64_t early_labels_excluded = 0;
    double watermark = 0.0;

    nlohmann::json to_json() const;
};

using StreamingAllReduce =
    std::function<std::vector<double>(const std::string&,
                                      const std::vector<double>&)>;
using StreamingAllGather =
    std::function<std::vector<std::vector<double>>(
        const std::string&, const std::vector<double>&)>;

struct StreamingCallbacks {
    std::function<void(const StreamingScore&)> on_score;
    std::function<void(const OnlineLogisticModel&, std::uint64_t)> on_persist;
};

struct StreamingTrainResult {
    OnlineLogisticModel model;
    PrequentialMetrics metrics;
    std::vector<StreamingScore> scores;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

StreamingTrainResult train_streaming(
    const std::vector<StreamingEvent>& local_events,
    const std::vector<std::string>& feature_names,
    const StreamingParams& params,
    const StreamingAllReduce& allreduce = {},
    const StreamingAllGather& allgather = {},
    const StreamingCallbacks& callbacks = {});

struct DistributedStreamingResult {
    StreamingTrainResult training;
    bool is_root = false;
    bool succeeded = false;
    std::string detail;
};

DistributedStreamingResult run_streaming_train_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const std::vector<StreamingEvent>& local_events,
    const std::vector<std::string>& feature_names,
    const StreamingParams& params, const StreamingCallbacks& callbacks = {},
    const std::string& round_tag = "streaming",
    std::chrono::milliseconds timeout = std::chrono::seconds(10));

struct StreamingRunOptions {
    std::string project;
    std::string graph;
    std::string run_id;
    std::string model_name = "streaming_model";
};

struct StreamingRunResult {
    std::string model_id;
    std::string scores_table;
    std::string input_topic;
    std::string output_topic;
    StreamingTrainResult training;
    std::vector<std::string> excluded;
    std::string error;

    bool ok() const noexcept { return error.empty() && training.ok(); }
};

StreamingRunResult run_streaming(
    Node& node, const data::ResolvedDataset& dataset,
    const StreamingParams& params, const StreamingRunOptions& options,
    const cluster::Cluster* cluster = nullptr);

} // namespace zetta::ml
