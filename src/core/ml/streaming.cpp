#include "zetta/ml/streaming.hpp"

#include <algorithm>
#include <cmath>
#include <deque>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

constexpr double kProbabilityFloor = 1e-12;

double sigmoid(double value) {
    if (value >= 0.0) {
        const double exponent = std::exp(-value);
        return 1.0 / (1.0 + exponent);
    }
    const double exponent = std::exp(value);
    return exponent / (1.0 + exponent);
}

std::optional<double> number(const nlohmann::json& row,
                             const std::string& column) {
    const auto found = row.find(column);
    if (found == row.end() || found->is_null() || !found->is_number()) {
        return std::nullopt;
    }
    return found->get<double>();
}

std::optional<std::int64_t> integer(const nlohmann::json& row,
                                    const std::string& column) {
    const std::optional<double> value = number(row, column);
    if (!value || !std::isfinite(*value) || std::floor(*value) != *value) {
        return std::nullopt;
    }
    return static_cast<std::int64_t>(*value);
}

std::string event_id(const nlohmann::json& row, std::size_t sequence) {
    const auto found = row.find("event_id");
    if (found != row.end() && found->is_string() &&
        !found->get_ref<const std::string&>().empty()) {
        return found->get<std::string>();
    }
    return "event-" + std::to_string(sequence);
}

std::vector<double> identity_reduce(const std::string&,
                                    const std::vector<double>& local) {
    return local;
}

std::vector<std::vector<double>> identity_gather(
    const std::string&, const std::vector<double>& local) {
    return {local};
}

struct AcceptedEvent {
    StreamingEvent event;
    std::vector<double> streaming_features;
    double probability = 0.0;
    bool scored = false;
    bool label_allowed = false;
};

struct WindowEvent {
    double time = 0.0;
    std::vector<double> values;
};

struct MetricSample {
    double time = 0.0;
    double loss = 0.0;
    double correct = 0.0;
};

std::vector<std::string> streaming_feature_names(
    const std::vector<std::string>& feature_names) {
    std::vector<std::string> names;
    names.reserve(feature_names.size() + 1);
    names.push_back("window_count");
    for (const std::string& feature : feature_names) {
        names.push_back("window_sum_" + feature);
    }
    return names;
}

std::vector<double> flatten_metric_samples(
    const std::vector<MetricSample>& samples) {
    std::vector<double> flattened;
    flattened.reserve(samples.size() * 3);
    for (const MetricSample& sample : samples) {
        flattened.push_back(sample.time);
        flattened.push_back(sample.loss);
        flattened.push_back(sample.correct);
    }
    return flattened;
}

std::vector<MetricSample> decode_metric_samples(
    const std::vector<std::vector<double>>& gathered) {
    std::vector<MetricSample> samples;
    for (const std::vector<double>& values : gathered) {
        for (std::size_t index = 0; index + 2 < values.size(); index += 3) {
            samples.push_back(MetricSample{values[index], values[index + 1],
                                           values[index + 2]});
        }
    }
    std::stable_sort(samples.begin(), samples.end(),
                     [](const MetricSample& left, const MetricSample& right) {
                         return left.time < right.time;
                     });
    return samples;
}

std::string fail_reason(std::string_view field, std::string_view reason) {
    return "streaming_params." + std::string(field) + ": " +
           std::string(reason);
}

} // namespace

nlohmann::json StreamingParams::to_json() const {
    return nlohmann::json{{"learning_rate", learning_rate},
                          {"l2_regularization", l2_regularization},
                          {"window_duration", window_duration},
                          {"allowed_lateness", allowed_lateness},
                          {"metric_window", metric_window},
                          {"persist_every", persist_every},
                          {"seed", seed},
                          {"label_time_column", label_time_column},
                          {"input_topic", input_topic},
                          {"output_topic", output_topic}};
}

std::optional<StreamingParams> StreamingParams::from_json(
    const nlohmann::json& value, std::string* error) {
    const auto fail = [error](std::string reason)
        -> std::optional<StreamingParams> {
        if (error != nullptr) {
            *error = std::move(reason);
        }
        return std::nullopt;
    };
    if (!value.is_object()) {
        return fail("streaming_params: must be an object");
    }
    static const std::set<std::string> allowed = {
        "learning_rate",     "l2_regularization", "window_duration",
        "allowed_lateness",  "metric_window",     "persist_every",
        "seed",              "label_time_column", "input_topic",
        "output_topic"};
    for (const auto& [key, ignored] : value.items()) {
        if (!allowed.contains(key)) {
            return fail("streaming_params." + key + ": unknown property");
        }
    }

    StreamingParams params;
    const auto read_number = [&](const char* key, double& target,
                                 bool strictly_positive) -> bool {
        const auto found = value.find(key);
        if (found == value.end()) {
            return true;
        }
        if (!found->is_number()) {
            if (error != nullptr) {
                *error = fail_reason(key, "must be a number");
            }
            return false;
        }
        const double parsed = found->get<double>();
        if (!std::isfinite(parsed) ||
            (strictly_positive ? parsed <= 0.0 : parsed < 0.0)) {
            if (error != nullptr) {
                *error = fail_reason(
                    key, strictly_positive ? "must be greater than zero"
                                           : "must be non-negative");
            }
            return false;
        }
        target = parsed;
        return true;
    };
    if (!read_number("learning_rate", params.learning_rate, true) ||
        !read_number("l2_regularization", params.l2_regularization, false) ||
        !read_number("window_duration", params.window_duration, true) ||
        !read_number("allowed_lateness", params.allowed_lateness, false)) {
        return std::nullopt;
    }

    const auto read_size = [&](const char* key, std::size_t& target) -> bool {
        const auto found = value.find(key);
        if (found == value.end()) {
            return true;
        }
        if (!found->is_number_unsigned() && !found->is_number_integer()) {
            if (error != nullptr) {
                *error = fail_reason(key, "must be a positive integer");
            }
            return false;
        }
        std::uint64_t parsed = 0;
        if (found->is_number_unsigned()) {
            parsed = found->get<std::uint64_t>();
        } else {
            const std::int64_t signed_value = found->get<std::int64_t>();
            if (signed_value <= 0) {
                if (error != nullptr) {
                    *error = fail_reason(key, "must be a positive integer");
                }
                return false;
            }
            parsed = static_cast<std::uint64_t>(signed_value);
        }
        if (parsed == 0 || parsed > std::numeric_limits<std::size_t>::max()) {
            if (error != nullptr) {
                *error = fail_reason(key, "must be a positive integer");
            }
            return false;
        }
        target = static_cast<std::size_t>(parsed);
        return true;
    };
    if (!read_size("metric_window", params.metric_window) ||
        !read_size("persist_every", params.persist_every)) {
        return std::nullopt;
    }
    if (const auto found = value.find("seed"); found != value.end()) {
        if (!found->is_number_unsigned() && !found->is_number_integer()) {
            return fail(fail_reason("seed", "must be a non-negative integer"));
        }
        if (found->is_number_unsigned()) {
            params.seed = found->get<std::uint64_t>();
        } else {
            const std::int64_t parsed = found->get<std::int64_t>();
            if (parsed < 0) {
                return fail(
                    fail_reason("seed", "must be a non-negative integer"));
            }
            params.seed = static_cast<std::uint64_t>(parsed);
        }
    }

    for (const auto& [key, target] :
         {std::pair<const char*, std::string*>{"label_time_column",
                                               &params.label_time_column},
          {"input_topic", &params.input_topic},
          {"output_topic", &params.output_topic}}) {
        const auto found = value.find(key);
        if (found == value.end()) {
            continue;
        }
        if (!found->is_string() || found->get_ref<const std::string&>().empty()) {
            return fail(fail_reason(key, "must be a non-empty string"));
        }
        *target = found->get<std::string>();
    }
    return params;
}

const nlohmann::json& streaming_params_schema() {
    static const nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.StreamingParams"},
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"learning_rate", {{"type", "number"}, {"exclusiveMinimum", 0}}},
          {"l2_regularization", {{"type", "number"}, {"minimum", 0}}},
          {"window_duration", {{"type", "number"}, {"exclusiveMinimum", 0}}},
          {"allowed_lateness", {{"type", "number"}, {"minimum", 0}}},
          {"metric_window", {{"type", "integer"}, {"minimum", 1}}},
          {"persist_every", {{"type", "integer"}, {"minimum", 1}}},
          {"seed", {{"type", "integer"}, {"minimum", 0}}},
          {"label_time_column", {{"type", "string"}, {"minLength", 1}}},
          {"input_topic", {{"type", "string"}, {"minLength", 1}}},
          {"output_topic", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

double OnlineLogisticModel::predict(
    const std::vector<double>& features) const {
    if (features.size() != weights.size()) {
        return 0.5;
    }
    const double raw = std::inner_product(
        features.begin(), features.end(), weights.begin(), bias);
    return sigmoid(std::clamp(raw, -40.0, 40.0));
}

nlohmann::json OnlineLogisticModel::to_json() const {
    return nlohmann::json{{"kind", "online_logistic_regression"},
                          {"feature_names", feature_names},
                          {"weights", weights},
                          {"bias", bias},
                          {"version", version},
                          {"seed", seed}};
}

std::optional<OnlineLogisticModel> OnlineLogisticModel::from_json(
    const nlohmann::json& value, std::string* error) {
    const auto fail = [error](std::string reason)
        -> std::optional<OnlineLogisticModel> {
        if (error != nullptr) {
            *error = std::move(reason);
        }
        return std::nullopt;
    };
    if (!value.is_object()) {
        return fail("streaming_model: must be an object");
    }
    if (value.value("kind", std::string{}) !=
        "online_logistic_regression") {
        return fail("streaming_model.kind: must be online_logistic_regression");
    }
    const auto names = value.find("feature_names");
    const auto weights = value.find("weights");
    const auto bias = value.find("bias");
    const auto version = value.find("version");
    const auto seed = value.find("seed");
    if (names == value.end() || !names->is_array() ||
        weights == value.end() || !weights->is_array() ||
        bias == value.end() || !bias->is_number() ||
        version == value.end() || !version->is_number_unsigned() ||
        seed == value.end() || !seed->is_number_unsigned()) {
        return fail("streaming_model: missing or mistyped required field");
    }
    OnlineLogisticModel model;
    for (const nlohmann::json& name : *names) {
        if (!name.is_string() || name.get_ref<const std::string&>().empty()) {
            return fail("streaming_model.feature_names[]: must be non-empty strings");
        }
        model.feature_names.push_back(name.get<std::string>());
    }
    for (const nlohmann::json& weight : *weights) {
        if (!weight.is_number()) {
            return fail("streaming_model.weights[]: must be numbers");
        }
        model.weights.push_back(weight.get<double>());
    }
    if (model.weights.size() != model.feature_names.size()) {
        return fail("streaming_model.weights: must match feature_names length");
    }
    model.bias = bias->get<double>();
    model.version = version->get<std::uint64_t>();
    model.seed = seed->get<std::uint64_t>();
    return model;
}

const nlohmann::json& streaming_model_schema() {
    static const nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.OnlineLogisticModel"},
        {"type", "object"},
        {"required",
         {"kind", "feature_names", "weights", "bias", "version", "seed"}},
        {"additionalProperties", false},
        {"properties",
         {{"kind", {{"const", "online_logistic_regression"}}},
          {"feature_names",
           {{"type", "array"},
            {"items", {{"type", "string"}, {"minLength", 1}}}}},
          {"weights",
           {{"type", "array"}, {"items", {{"type", "number"}}}}},
          {"bias", {{"type", "number"}}},
          {"version", {{"type", "integer"}, {"minimum", 0}}},
          {"seed", {{"type", "integer"}, {"minimum", 0}}}}}};
    return schema;
}

nlohmann::json PrequentialMetrics::to_json() const {
    return nlohmann::json{{"prequential_log_loss", log_loss},
                          {"prequential_accuracy", accuracy},
                          {"drift_loss_delta", drift_loss_delta},
                          {"evaluated_labels", evaluated_labels},
                          {"events_read", events_read},
                          {"events_scored", events_scored},
                          {"late_events_excluded", late_events_excluded},
                          {"early_labels_excluded", early_labels_excluded},
                          {"watermark", watermark}};
}

StreamingTrainResult train_streaming(
    const std::vector<StreamingEvent>& local_events,
    const std::vector<std::string>& feature_names,
    const StreamingParams& params, const StreamingAllReduce& allreduce,
    const StreamingAllGather& allgather,
    const StreamingCallbacks& callbacks) {
    StreamingTrainResult result;
    if (feature_names.empty()) {
        result.error = "streaming: no feature columns";
        return result;
    }
    if (params.learning_rate <= 0.0 || params.l2_regularization < 0.0 ||
        params.window_duration <= 0.0 || params.allowed_lateness < 0.0 ||
        params.metric_window == 0 || params.persist_every == 0) {
        result.error = "streaming: invalid parameters";
        return result;
    }
    const StreamingAllReduce reduce = allreduce ? allreduce : identity_reduce;
    const StreamingAllGather gather = allgather ? allgather : identity_gather;

    std::vector<AcceptedEvent> accepted;
    accepted.reserve(local_events.size());
    double maximum_seen = -std::numeric_limits<double>::infinity();
    std::uint64_t late_events = 0;
    std::uint64_t early_labels = 0;
    for (const StreamingEvent& source : local_events) {
        if (source.values.size() != feature_names.size() ||
            !std::isfinite(source.event_time) ||
            !std::ranges::all_of(source.values,
                                 [](double value) { return std::isfinite(value); })) {
            result.error = "streaming: event '" + source.id +
                           "' has a malformed time or feature vector";
            return result;
        }
        const double prior_watermark =
            std::isfinite(maximum_seen)
                ? maximum_seen - params.allowed_lateness
                : -std::numeric_limits<double>::infinity();
        if (source.event_time < prior_watermark) {
            ++late_events;
            continue;
        }
        maximum_seen = std::max(maximum_seen, source.event_time);
        AcceptedEvent item;
        item.event = source;
        if (item.event.id.empty()) {
            item.event.id = "event-" +
                            std::to_string(item.event.arrival_sequence);
        }
        if (item.event.label) {
            if (*item.event.label != 0.0 && *item.event.label != 1.0) {
                result.error = "streaming: label for event '" + item.event.id +
                               "' must be 0 or 1";
                return result;
            }
            if (!item.event.label_time) {
                item.event.label_time = item.event.event_time;
            }
            item.label_allowed =
                *item.event.label_time >= item.event.event_time;
            if (!item.label_allowed) {
                ++early_labels;
            }
        }
        accepted.push_back(std::move(item));
    }

    std::vector<double> local_times;
    local_times.reserve(accepted.size() * 2);
    for (const AcceptedEvent& item : accepted) {
        local_times.push_back(item.event.event_time);
        if (item.event.label && item.label_allowed) {
            local_times.push_back(*item.event.label_time);
        }
    }
    const std::vector<std::vector<double>> gathered_times =
        gather("streaming.action_times", local_times);
    std::vector<double> action_times;
    for (const std::vector<double>& times : gathered_times) {
        action_times.insert(action_times.end(), times.begin(), times.end());
    }
    std::sort(action_times.begin(), action_times.end());
    action_times.erase(std::unique(action_times.begin(), action_times.end()),
                       action_times.end());
    if (action_times.empty()) {
        result.error = "streaming: no events remained after watermark filtering";
        return result;
    }

    result.model.feature_names = streaming_feature_names(feature_names);
    result.model.seed = params.seed;
    std::mt19937_64 generator(params.seed);
    std::uniform_real_distribution<double> initial(-1e-4, 1e-4);
    result.model.weights.resize(result.model.feature_names.size());
    std::generate(result.model.weights.begin(), result.model.weights.end(),
                  [&] { return initial(generator); });

    std::map<std::int64_t, std::deque<WindowEvent>> windows;
    std::vector<MetricSample> local_metrics;
    std::uint64_t persisted_labels = 0;
    std::uint64_t total_labels = 0;
    for (std::size_t time_index = 0; time_index < action_times.size();
         ++time_index) {
        const double time = action_times[time_index];
        for (AcceptedEvent& item : accepted) {
            if (item.event.event_time != time) {
                continue;
            }
            std::deque<WindowEvent>& window = windows[item.event.entity];
            while (!window.empty() &&
                   time - window.front().time > params.window_duration) {
                window.pop_front();
            }
            window.push_back(WindowEvent{time, item.event.values});
            item.streaming_features.assign(feature_names.size() + 1, 0.0);
            item.streaming_features[0] = static_cast<double>(window.size());
            for (const WindowEvent& event : window) {
                for (std::size_t feature = 0;
                     feature < event.values.size(); ++feature) {
                    item.streaming_features[feature + 1] +=
                        event.values[feature];
                }
            }
            item.probability = result.model.predict(item.streaming_features);
            item.scored = true;
            StreamingScore score{item.event.id,
                                 item.event.entity,
                                 item.event.event_time,
                                 item.probability,
                                 result.model.version};
            result.scores.push_back(score);
            if (callbacks.on_score) {
                callbacks.on_score(score);
            }
        }

        std::vector<double> local_gradient(result.model.weights.size() + 2,
                                           0.0);
        for (const AcceptedEvent& item : accepted) {
            if (!item.event.label || !item.label_allowed ||
                *item.event.label_time != time || !item.scored) {
                continue;
            }
            const double label = *item.event.label;
            const double probability = std::clamp(
                item.probability, kProbabilityFloor, 1.0 - kProbabilityFloor);
            const double residual = probability - label;
            local_gradient[0] += 1.0;
            local_gradient[1] += residual;
            for (std::size_t feature = 0;
                 feature < result.model.weights.size(); ++feature) {
                local_gradient[feature + 2] +=
                    residual * item.streaming_features[feature];
            }
            const double loss =
                -(label * std::log(probability) +
                  (1.0 - label) * std::log(1.0 - probability));
            const double correct =
                ((probability >= 0.5) == (label >= 0.5)) ? 1.0 : 0.0;
            local_metrics.push_back(MetricSample{time, loss, correct});
        }
        const std::vector<double> global_gradient = reduce(
            "streaming.gradient." + std::to_string(time_index),
            local_gradient);
        if (global_gradient.size() != local_gradient.size()) {
            result.error = "streaming: gradient allreduce failed";
            return result;
        }
        const double labels = global_gradient[0];
        if (labels <= 0.0) {
            continue;
        }
        result.model.bias -=
            params.learning_rate * (global_gradient[1] / labels);
        for (std::size_t feature = 0;
             feature < result.model.weights.size(); ++feature) {
            const double gradient =
                global_gradient[feature + 2] / labels +
                params.l2_regularization * result.model.weights[feature];
            result.model.weights[feature] -= params.learning_rate * gradient;
        }
        ++result.model.version;
        total_labels += static_cast<std::uint64_t>(labels);
        if (callbacks.on_persist &&
            total_labels - persisted_labels >= params.persist_every) {
            callbacks.on_persist(result.model, total_labels);
            persisted_labels = total_labels;
        }
    }
    if (callbacks.on_persist && persisted_labels != total_labels) {
        callbacks.on_persist(result.model, total_labels);
    }

    const std::vector<std::vector<double>> gathered_metrics = gather(
        "streaming.prequential", flatten_metric_samples(local_metrics));
    std::vector<MetricSample> metrics =
        decode_metric_samples(gathered_metrics);
    if (metrics.size() > params.metric_window) {
        metrics.erase(metrics.begin(),
                      metrics.end() -
                          static_cast<std::ptrdiff_t>(params.metric_window));
    }
    result.metrics.evaluated_labels = total_labels;
    if (!metrics.empty()) {
        for (const MetricSample& sample : metrics) {
            result.metrics.log_loss += sample.loss;
            result.metrics.accuracy += sample.correct;
        }
        result.metrics.log_loss /= static_cast<double>(metrics.size());
        result.metrics.accuracy /= static_cast<double>(metrics.size());
        if (metrics.size() >= 4) {
            const std::size_t middle = metrics.size() / 2;
            double older = 0.0;
            double newer = 0.0;
            for (std::size_t index = 0; index < middle; ++index) {
                older += metrics[index].loss;
            }
            for (std::size_t index = middle; index < metrics.size(); ++index) {
                newer += metrics[index].loss;
            }
            older /= static_cast<double>(middle);
            newer /= static_cast<double>(metrics.size() - middle);
            result.metrics.drift_loss_delta = newer - older;
        }
    }

    const std::vector<double> counts = reduce(
        "streaming.counts",
        {static_cast<double>(local_events.size()),
         static_cast<double>(result.scores.size()),
         static_cast<double>(late_events), static_cast<double>(early_labels)});
    if (counts.size() != 4) {
        result.error = "streaming: metric allreduce failed";
        return result;
    }
    result.metrics.events_read = static_cast<std::uint64_t>(counts[0]);
    result.metrics.events_scored = static_cast<std::uint64_t>(counts[1]);
    result.metrics.late_events_excluded =
        static_cast<std::uint64_t>(counts[2]);
    result.metrics.early_labels_excluded =
        static_cast<std::uint64_t>(counts[3]);

    const double local_watermark =
        std::isfinite(maximum_seen)
            ? maximum_seen - params.allowed_lateness
            : -std::numeric_limits<double>::infinity();
    const std::vector<std::vector<double>> watermarks =
        gather("streaming.watermarks", {local_watermark});
    result.metrics.watermark = std::numeric_limits<double>::infinity();
    for (const std::vector<double>& values : watermarks) {
        if (!values.empty() && std::isfinite(values.front())) {
            result.metrics.watermark =
                std::min(result.metrics.watermark, values.front());
        }
    }
    if (!std::isfinite(result.metrics.watermark)) {
        result.metrics.watermark = 0.0;
    }
    return result;
}

DistributedStreamingResult run_streaming_train_job(
    compute::JobCoordinator& coordinator, compute::ComputeJob& job,
    const std::vector<StreamingEvent>& local_events,
    const std::vector<std::string>& feature_names,
    const StreamingParams& params, const StreamingCallbacks& callbacks,
    const std::string& round_tag, std::chrono::milliseconds timeout) {
    DistributedStreamingResult result;
    result.succeeded =
        coordinator.run(job, [&](compute::JobRound& round) {
            result.is_root = round.is_root();
            bool collective_failed = false;
            const StreamingAllReduce reduce =
                [&](const std::string& tag,
                    const std::vector<double>& local) -> std::vector<double> {
                const auto reduced = round.allreduce(
                    round_tag + "." + tag, local,
                    cluster::CollectiveOp::Sum, timeout);
                if (!reduced) {
                    collective_failed = true;
                    return {};
                }
                return *reduced;
            };
            const StreamingAllGather gather =
                [&](const std::string& tag,
                    const std::vector<double>& local)
                    -> std::vector<std::vector<double>> {
                const auto gathered = round.gather(
                    round_tag + "." + tag + ".gather", local, timeout);
                if (!gathered) {
                    collective_failed = true;
                    return {};
                }
                std::vector<double> packed;
                if (round.is_root()) {
                    packed.push_back(static_cast<double>(gathered->size()));
                    for (const std::vector<double>& values : *gathered) {
                        packed.push_back(static_cast<double>(values.size()));
                        packed.insert(packed.end(), values.begin(), values.end());
                    }
                }
                const auto shared = round.broadcast(
                    round_tag + "." + tag + ".broadcast", packed, timeout);
                if (!shared || shared->empty()) {
                    collective_failed = true;
                    return {};
                }
                std::vector<std::vector<double>> decoded;
                std::size_t offset = 1;
                const std::size_t count =
                    static_cast<std::size_t>((*shared)[0]);
                for (std::size_t index = 0; index < count; ++index) {
                    if (offset >= shared->size()) {
                        collective_failed = true;
                        return {};
                    }
                    const std::size_t length =
                        static_cast<std::size_t>((*shared)[offset++]);
                    if (offset + length > shared->size()) {
                        collective_failed = true;
                        return {};
                    }
                    decoded.emplace_back(shared->begin() +
                                             static_cast<std::ptrdiff_t>(offset),
                                         shared->begin() +
                                             static_cast<std::ptrdiff_t>(offset +
                                                                         length));
                    offset += length;
                }
                return decoded;
            };
            result.training = train_streaming(local_events, feature_names,
                                              params, reduce, gather, callbacks);
            if (collective_failed && result.training.error.empty()) {
                result.training.error = "streaming: collective failed";
            }
            result.detail = result.training.ok()
                                ? "streaming: trained on " +
                                      std::to_string(
                                          result.training.metrics.events_scored) +
                                      " event(s) with " +
                                      std::to_string(result.training.metrics
                                                         .evaluated_labels) +
                                      " prequential label(s)"
                                : result.training.error;
            return compute::JobCoordinator::Outcome{
                result.training.ok(), result.detail};
        }) == compute::JobStatus::Succeeded;
    return result;
}

StreamingRunResult run_streaming(
    Node& node, const data::ResolvedDataset& dataset,
    const StreamingParams& params, const StreamingRunOptions& options,
    const cluster::Cluster* cluster) {
    StreamingRunResult result;
    result.input_topic = params.input_topic;
    result.output_topic = params.output_topic;
    std::shared_ptr<pipeline::Run> run;
    pubsub::PubSub* broker = nullptr;
    node.run_on_owner([&] {
        run = node.find_run(options.run_id);
        broker = node.pubsub();
    });
    if (!run) {
        result.error = "streaming: no run '" + options.run_id + "'";
        return result;
    }
    const auto fail_run = [&](std::string message) {
        result.error = std::move(message);
        node.run_on_owner([&] {
            run->add_log({util::now_iso8601(), "error", result.error});
            run->set_status(pipeline::RunStatus::Failed);
        });
    };

    query::QueryService::Outcome query_result;
    node.run_on_owner([&] {
        query::QueryService service(node, cluster);
        query_result = service.run("SELECT * FROM " + dataset.database + "." +
                                       dataset.table,
                                   dataset.database);
    });
    if (!query_result.ok()) {
        fail_run("streaming: could not read " + dataset.database + "." +
                 dataset.table + ": " + query_result.error);
        return result;
    }

    if (broker == nullptr) {
        fail_run("streaming: pub/sub is disabled on this node");
        return result;
    }
    const pubsub::Identity service{{"service", "streaming"}, {}};
    const pubsub::Address input_address{"topic", params.input_topic};
    const pubsub::Address output_address{"topic", params.output_topic};
    std::string topic_reason;
    if (!broker->topic_settings(input_address) &&
        !broker->create_topic(service, input_address, {}, &topic_reason)) {
        fail_run("streaming: " + topic_reason);
        return result;
    }
    pubsub::TopicSettings output_settings;
    output_settings.exposed = true;
    const std::optional<pubsub::TopicSettings> existing_output =
        broker->topic_settings(output_address);
    if (existing_output && !existing_output->exposed) {
        fail_run("streaming: output topic '" + output_address.to_string() +
                 "' exists but is not exposed");
        return result;
    }
    if (!existing_output &&
        !broker->create_topic(service, output_address, output_settings,
                              &topic_reason)) {
        fail_run("streaming: " + topic_reason);
        return result;
    }
    pubsub::SubscribeOptions subscribe_options;
    subscribe_options.start = pubsub::StartPosition::Next;
    std::unique_ptr<pubsub::Subscription> subscription = broker->subscribe(
        service,
        pubsub::Address{"service", "streaming-run-" + options.run_id},
        input_address, subscribe_options, &topic_reason);
    if (!subscription) {
        fail_run("streaming: " + topic_reason);
        return result;
    }

    for (const nlohmann::json& row : query_result.rows) {
        pubsub::PublishRequest request;
        request.to = {input_address};
        request.encoding = pubsub::Encoding::Json;
        request.mime_type = "application/json";
        request.payload = row.dump();
        const pubsub::PublishResult published =
            broker->publish(service, std::move(request));
        if (!published.accepted) {
            fail_run("streaming: could not publish an input event: " +
                     published.error);
            return result;
        }
    }
    broker->drain_dispatch();

    std::vector<StreamingEvent> events;
    events.reserve(query_result.rows.size());
    for (std::size_t sequence = 0; sequence < query_result.rows.size();
         ++sequence) {
        const std::optional<pubsub::Delivery> delivery =
            subscription->receive(std::chrono::seconds(1));
        if (!delivery) {
            fail_run("streaming: input topic ended before every table row was "
                     "received");
            return result;
        }
        const nlohmann::json row = nlohmann::json::parse(
            delivery->event.payload.bytes(), nullptr, false);
        if (!row.is_object()) {
            fail_run("streaming: input topic event is not a JSON object");
            return result;
        }
        StreamingEvent event;
        event.id = event_id(row, sequence);
        event.arrival_sequence = sequence;
        const std::optional<std::int64_t> entity =
            integer(row, dataset.contract.entity_column);
        const std::optional<double> time =
            number(row, dataset.contract.time_column);
        if (!entity || !time) {
            fail_run("streaming: event '" + event.id +
                     "' needs numeric entity and event-time columns");
            return result;
        }
        event.entity = *entity;
        event.event_time = *time;
        for (const std::string& feature : dataset.contract.feature_columns) {
            const std::optional<double> value = number(row, feature);
            if (!value) {
                fail_run("streaming: event '" + event.id +
                         "' has no numeric feature '" + feature + "'");
                return result;
            }
            event.values.push_back(*value);
        }
        event.label = number(row, dataset.contract.label_column);
        if (event.label) {
            event.label_time = number(row, params.label_time_column);
            if (!event.label_time) {
                event.label_time = event.event_time;
            }
        }
        events.push_back(std::move(event));
    }

    std::shared_ptr<Model> registered;
    node.run_on_owner([&] {
        ModelValue value;
        value.name = options.model_name;
        value.kind = "streaming";
        value.project = options.project;
        value.run = options.run_id;
        value.feature_schema = {
            {"event_features", dataset.contract.feature_columns},
            {"streaming_features",
             streaming_feature_names(dataset.contract.feature_columns)},
            {"entity_column", dataset.contract.entity_column},
            {"time_column", dataset.contract.time_column},
            {"label_column", dataset.contract.label_column},
            {"label_time_column", params.label_time_column},
            {"input_topic", params.input_topic},
            {"output_topic", params.output_topic},
            {"params", params.to_json()}};
        registered = node.create_model(std::move(value));
    });
    if (!registered) {
        fail_run("streaming: could not register the online model");
        return result;
    }
    node.run_on_owner([&] { result.model_id = registered->id(); });

    std::string publish_error;
    StreamingCallbacks callbacks;
    callbacks.on_score = [&](const StreamingScore& score) {
        nlohmann::json payload{{"event_id", score.event_id},
                               {"entity", score.entity},
                               {"event_time", score.event_time},
                               {"score", score.probability},
                               {"model_version", score.model_version}};
        pubsub::PublishRequest request;
        request.to = {output_address};
        request.encoding = pubsub::Encoding::Json;
        request.mime_type = "application/json";
        request.payload = payload.dump();
        const pubsub::PublishResult published =
            broker->publish(service, std::move(request));
        if (!published.accepted && publish_error.empty()) {
            publish_error = published.error;
        }
    };
    callbacks.on_persist = [&](const OnlineLogisticModel& model,
                               std::uint64_t labels) {
        const std::string weights = model.to_json().dump();
        node.run_on_owner([&] {
            registered->set_weights(weights);
            run->add_log({util::now_iso8601(), "info",
                          "streaming: persisted model version " +
                              std::to_string(model.version) + " after " +
                              std::to_string(labels) + " label(s)"});
        });
    };
    result.training = train_streaming(events, dataset.contract.feature_columns,
                                      params, {}, {}, callbacks);
    broker->drain_dispatch();
    if (!publish_error.empty() && result.training.error.empty()) {
        result.training.error =
            "streaming: could not publish a score: " + publish_error;
    }
    if (!result.training.ok()) {
        result.error = result.training.error;
        node.run_on_owner([&] {
            run->add_log({util::now_iso8601(), "error", result.error});
            run->set_status(pipeline::RunStatus::Failed);
        });
        return result;
    }

    node.run_on_owner([&] {
        registered->set_weights(result.training.model.to_json().dump());
        nlohmann::json metrics = result.training.metrics.to_json();
        metrics["allowed_lateness"] = params.allowed_lateness;
        metrics["window_duration"] = params.window_duration;
        metrics["learning_rate"] = params.learning_rate;
        metrics["l2_regularization"] = params.l2_regularization;
        registered->set_metrics(std::move(metrics));

        std::shared_ptr<data::Database> database =
            node.find_database("streaming_results");
        if (!database) {
            database = node.create_database("streaming_results");
        }
        if (!database) {
            result.error =
                "streaming: could not create the result database";
            run->add_log({util::now_iso8601(), "error", result.error});
            run->set_status(pipeline::RunStatus::Failed);
            return;
        }
        const std::string table_name =
            "scores_" + options.run_id.substr(0, 8);
        database->drop_table(table_name);
        const std::shared_ptr<data::Table> table =
            database->create_table(table_name);
        if (!table) {
            result.error = "streaming: could not create the score table";
            run->add_log({util::now_iso8601(), "error", result.error});
            run->set_status(pipeline::RunStatus::Failed);
            return;
        }
        for (const StreamingScore& score : result.training.scores) {
            table->add_row({{"event_id", score.event_id},
                            {"entity", score.entity},
                            {"event_time", score.event_time},
                            {"score", score.probability},
                            {"model_version", score.model_version}});
        }
        result.scores_table = "streaming_results." + table_name;

        for (const std::string& note : dataset.notes) {
            run->add_log({util::now_iso8601(), "info", "dataset: " + note});
            if (note.starts_with("not used:")) {
                result.excluded.push_back(note.substr(10));
            }
        }
        std::set<std::string> used_columns = {
            dataset.contract.entity_column, dataset.contract.label_column,
            dataset.contract.time_column, params.label_time_column, "event_id",
            "id"};
        used_columns.insert(dataset.contract.feature_columns.begin(),
                            dataset.contract.feature_columns.end());
        std::set<std::string> available_columns(query_result.columns.begin(),
                                                query_result.columns.end());
        for (const nlohmann::json& row : query_result.rows) {
            if (!row.is_object()) {
                continue;
            }
            for (const auto& [column, ignored] : row.items()) {
                available_columns.insert(column);
            }
        }
        for (const std::string& column : available_columns) {
            if (used_columns.contains(column) ||
                std::find(result.excluded.begin(), result.excluded.end(),
                          column) != result.excluded.end()) {
                continue;
            }
            result.excluded.push_back(column);
            run->add_log({util::now_iso8601(), "info",
                          "dataset: not used: " + column});
        }
        run->set_outputs({{"model", options.model_name, registered->id()},
                          {"dataset", "streaming scores",
                           result.scores_table},
                          {"report", "run report", run->id()}});
        pipeline::RunLineage lineage;
        lineage.sources = {"dataset:" + dataset.database + "." +
                               dataset.table,
                           "topic:" + params.input_topic};
        lineage.steps = {"subscribe", "window", "score", "test-then-train",
                         "publish", "persist"};
        lineage.freshness = util::now_iso8601();
        lineage.confidence = result.training.metrics.accuracy;
        run->set_lineage(std::move(lineage));
        run->add_log(
            {util::now_iso8601(), "info",
             "streaming: scored " +
                 std::to_string(result.training.metrics.events_scored) +
                 " event(s); evaluated " +
                 std::to_string(result.training.metrics.evaluated_labels) +
                 " label(s) test-then-train"});
        run->set_status(pipeline::RunStatus::Succeeded);
    });
    return result;
}

} // namespace zetta::ml
