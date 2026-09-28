#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "zetta/api/background.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/ml/streaming.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

std::string string_hint(const nlohmann::json& hints, const char* name,
                        std::string fallback) {
    const auto found = hints.find(name);
    if (found != hints.end() && found->is_string() && !found->empty()) {
        return found->get<std::string>();
    }
    return fallback;
}

std::vector<std::string> feature_hints(const nlohmann::json& hints) {
    const auto found = hints.find("feature_columns");
    if (found == hints.end() || !found->is_array()) {
        return {};
    }
    std::vector<std::string> features;
    for (const nlohmann::json& feature : *found) {
        if (feature.is_string() && !feature.empty()) {
            features.push_back(feature.get<std::string>());
        }
    }
    return features;
}

double number_hint(const nlohmann::json& hints, const char* name,
                   double fallback) {
    const auto found = hints.find(name);
    return found != hints.end() && found->is_number()
               ? found->get<double>()
               : fallback;
}

ir::ExecutionGraph build_m21_shape(const ShapeRequest& request) {
    const nlohmann::json hints = request.hints.is_object()
                                     ? request.hints
                                     : nlohmann::json::object();
    const std::string source = string_hint(hints, "source", "streaming_data");
    const std::string table = string_hint(hints, "table", "events");
    const std::string entity = string_hint(hints, "entity_column", "entity_id");
    const std::string time = string_hint(hints, "time_column", "event_time");
    const std::string label = string_hint(hints, "label_column", "label");
    const std::string label_time =
        string_hint(hints, "label_time_column", "label_time");
    const std::string input_topic =
        string_hint(hints, "input_topic", "streaming.events");
    const std::string output_topic =
        string_hint(hints, "output_topic", "streaming.scores");

    nlohmann::json model_attributes{
        {"model_kind", "streaming"},
        {"algo", "online_logistic_regression"},
        {"entity_column", entity},
        {"time_column", time},
        {"label_column", label},
        {"label_time_column", label_time},
        {"input_topic", input_topic},
        {"output_topic", output_topic},
        {"learning_rate", number_hint(hints, "learning_rate", 0.05)},
        {"l2_regularization",
         number_hint(hints, "l2_regularization", 0.001)},
        {"window_duration", number_hint(hints, "window_duration", 60.0)},
        {"allowed_lateness", number_hint(hints, "allowed_lateness", 5.0)},
        {"temporal_validation", true},
        {"event_time_ordered", true},
        {"future_event_policy", "buffer_until_watermark"},
        {"label_time_policy", "not_before_event_time"},
        {"prequential", true}};
    const std::vector<std::string> features = feature_hints(hints);
    if (!features.empty()) {
        model_attributes["feature_columns"] = features;
    }

    ir::ExecutionGraph graph;
    graph.add_node(ir::StepKind::Connect, "Subscribe to event stream",
                   {{"source", source},
                    {"table", table},
                    {"topic", input_topic},
                    {"time_column", time}},
                   "subscribe_events");
    graph.add_node(ir::StepKind::Clean, "Compute event-time windows",
                   {{"entity_column", entity},
                    {"time_column", time},
                    {"window_duration", model_attributes["window_duration"]},
                    {"allowed_lateness", model_attributes["allowed_lateness"]}},
                   "window_features");
    graph.add_node(ir::StepKind::Model, "Test then train online model",
                   std::move(model_attributes), "train_streaming_model");
    graph.add_node(ir::StepKind::Serve, "Publish streaming scores",
                   {{"topic", output_topic}, {"delivery", "pubsub"}},
                   "publish_scores");
    graph.add_edge("subscribe_events", "window_features");
    graph.add_edge("window_features", "train_streaming_model");
    graph.add_edge("train_streaming_model", "publish_scores");
    return graph;
}

bool m21_owns(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (!node || node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const std::string model_kind =
            string_hint(node->attributes, "model_kind", {});
        if (model_kind == "streaming" || model_kind == "online_learning") {
            return true;
        }
    }
    return false;
}

const nlohmann::json* model_attributes(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node && node->kind == ir::StepKind::Model &&
            node->attributes.is_object()) {
            return &node->attributes;
        }
    }
    return nullptr;
}

std::optional<std::string> pipeline_dataset_path(
    const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (!node || node->kind != ir::StepKind::Connect ||
            !node->attributes.is_object()) {
            continue;
        }
        const std::string source =
            string_hint(node->attributes, "source", {});
        const std::string table =
            string_hint(node->attributes, "table", {});
        if (!source.empty() && !table.empty()) {
            return source + "." + table;
        }
    }
    return std::nullopt;
}

data::FeaturePlan streaming_feature_plan(RunRequest& request, data::ResolvedDataset& dataset,
                                         const data::DatasetContract& selected,
                                         const StreamingParams& params) {
    const bool selected_clock =
        std::ranges::contains(selected.feature_columns, dataset.contract.time_column) ||
        std::ranges::contains(selected.feature_columns, params.label_time_column);
    std::erase_if(dataset.contract.feature_columns, [&](const auto& column) {
        return column == dataset.contract.time_column || column == params.label_time_column;
    });
    const std::string window = "native per-entity event-time window of " +
        std::to_string(params.window_duration) + " event-time units, including the current event; lateness " +
        std::to_string(params.allowed_lateness) + " event-time units";
    data::FeaturePlanOptions feature_options;
    feature_options.native_entries.push_back({
        {dataset.contract.entity_column, dataset.contract.time_column}, {"window_count"},
        window + "; count accepted events",
        "window activity is available before the current label update"});
    for (const auto& source : dataset.contract.feature_columns) {
        feature_options.native_entries.push_back({
            {dataset.contract.entity_column, dataset.contract.time_column, source},
            {"window_sum_" + source}, window + "; sum numeric event values",
            "online logistic SGD scores the event before consuming its available label"});
    }
    auto plan = plan_run_features(request, "M-21", dataset, feature_options);
    if (!feature_plan_error(plan).empty()) {
        return plan;
    }
    if (selected_clock) {
        plan.error = dataset.contract.time_column + " and " + params.label_time_column +
            " are window/scheduling metadata, not prediction features";
        return plan;
    }
    if (dataset.contract.feature_columns.empty()) {
        plan.status = "transform_required";
        plan.transform_required = data::RequiredTransform{
            "create_eligible_features", "Streaming needs numeric event values, not only timestamps.",
            {}, "Add numeric event values in a clean step and rerun the plan."};
        return plan;
    }
    plan.excluded.push_back({dataset.contract.time_column,
        "event time bounds windows and watermarks; never a raw timestamp predictor"});
    for (auto& excluded : plan.excluded) {
        if (excluded.source_column == params.label_time_column) {
            excluded.reason = "label availability schedules updates; never a prediction feature";
        }
    }
    return plan;
}

class AsyncStreamingRun {
public:
    static std::shared_ptr<AsyncStreamingRun> start(
        Node& node, data::ResolvedDataset dataset, StreamingParams params,
        StreamingRunOptions options, const cluster::Cluster* cluster) {
        return std::shared_ptr<AsyncStreamingRun>(new AsyncStreamingRun(
            node, std::move(dataset), std::move(params), std::move(options),
            cluster));
    }

    ~AsyncStreamingRun() {
        while (!done_.load(std::memory_order_acquire)) {
            node_.drain_owner_work();
            std::this_thread::yield();
        }
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    bool done() const noexcept {
        return done_.load(std::memory_order_acquire);
    }

private:
    AsyncStreamingRun(Node& node, data::ResolvedDataset dataset,
                      StreamingParams params, StreamingRunOptions options,
                      const cluster::Cluster* cluster)
        : node_(node),
          dataset_(std::move(dataset)),
          params_(std::move(params)),
          options_(std::move(options)),
          cluster_(cluster),
          worker_([this] {
              result_ = run_streaming(node_, dataset_, params_, options_,
                                      cluster_);
              done_.store(true, std::memory_order_release);
          }) {}

    Node& node_;
    data::ResolvedDataset dataset_;
    StreamingParams params_;
    StreamingRunOptions options_;
    const cluster::Cluster* cluster_ = nullptr;
    StreamingRunResult result_;
    std::atomic<bool> done_{false};
    std::thread worker_;
};

api::ApiResult run_m21(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-21 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding wired no background "
            "action owner");
    }

    const nlohmann::json* attributes =
        model_attributes(request.graph->graph());
    if (attributes == nullptr) {
        return api::ApiResult::failure(
            "invalid_input", "M-21 graph has no streaming model step");
    }
    std::optional<std::string> path;
    if (const auto supplied = request.input.find("dataset_path");
        supplied != request.input.end()) {
        if (!supplied->is_string() || supplied->empty()) {
            return api::ApiResult::failure(
                "invalid_input", "dataset_path must be a non-empty string");
        }
        path = supplied->get<std::string>();
    } else {
        path = pipeline_dataset_path(request.graph->graph());
    }
    if (!path) {
        return api::ApiResult::failure(
            "invalid_input",
            "M-21 needs dataset_path or a connect step with source and table");
    }
    const data::DatasetPath parsed = data::parse_dataset_path(*path);
    if (parsed.kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure(
            "invalid_input", "M-21 reads a landed database.table dataset");
    }

    data::DatasetContract contract;
    contract.entity_column = string_hint(*attributes, "entity_column", {});
    contract.label_column = string_hint(*attributes, "label_column", {});
    contract.time_column = string_hint(*attributes, "time_column", {});
    contract.feature_columns = feature_hints(*attributes);
    if (const auto given = request.input.find("dataset");
        given != request.input.end()) {
        std::string reason;
        const std::optional<data::DatasetContract> explicit_contract =
            data::dataset_contract_from_json(*given, &reason);
        if (!explicit_contract) {
            return api::ApiResult::failure("invalid_input", reason);
        }
        contract = *explicit_contract;
    }
    if (contract.time_column.empty()) {
        return api::ApiResult::failure(
            "invalid_input", "M-21 requires an explicit event-time column");
    }
    data::ResolvedDataset dataset = data::resolve_dataset(
        context.node, parsed, contract,
        data::default_dataset_sampler(context.node, context.cluster));
    if (!dataset.ok()) {
        dataset.contract = contract;
        return run_dataset_failure(request, "M-21", dataset);
    }

    nlohmann::json parameter_json{
        {"learning_rate", number_hint(*attributes, "learning_rate", 0.05)},
        {"l2_regularization",
         number_hint(*attributes, "l2_regularization", 0.001)},
        {"window_duration",
         number_hint(*attributes, "window_duration", 60.0)},
        {"allowed_lateness",
         number_hint(*attributes, "allowed_lateness", 5.0)},
        {"label_time_column",
         string_hint(*attributes, "label_time_column", "label_time")},
        {"input_topic",
         string_hint(*attributes, "input_topic", "streaming.events")},
        {"output_topic",
         string_hint(*attributes, "output_topic", "streaming.scores")}};
    if (const auto supplied = request.input.find("streaming");
        supplied != request.input.end()) {
        if (!supplied->is_object()) {
            return api::ApiResult::failure(
                "invalid_input", "streaming parameters must be an object");
        }
        parameter_json.update(*supplied);
    }
    std::string reason;
    std::optional<StreamingParams> params =
        StreamingParams::from_json(parameter_json, &reason);
    if (!params) {
        return api::ApiResult::failure("invalid_input", reason);
    }

    const auto feature_plan = streaming_feature_plan(request, dataset, contract, *params);
    if (const auto plan_error = feature_plan_error(feature_plan); !plan_error.empty()) {
        return api::ApiResult::failure("invalid_input", plan_error);
    }
    std::shared_ptr<pipeline::Run> run = context.node.create_run(
        request.project->id(), request.graph->id());
    if (!run) {
        return api::ApiResult::failure("engine_error",
                                       "could not create the M-21 run record");
    }
    record_run_features(context.node, run->id(), feature_plan);
    run->set_status(pipeline::RunStatus::Running);
    run->add_log({util::now_iso8601(), "info",
                  "streaming: subscribed to " + params->input_topic});

    StreamingRunOptions options;
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.run_id = run->id();
    options.model_name =
        request.input.value("model_name", std::string("streaming_model"));
    const std::shared_ptr<AsyncStreamingRun> handle =
        AsyncStreamingRun::start(context.node, dataset, *params, options,
                                 context.cluster);
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });

    return api::ApiResult::success(
        {{"run_id", run->id()},
         {"engine", "streaming"},
         {"status", "running"},
         {"poll", "get_run for status; get_run_report for prequential metrics"},
         {"dataset", data::to_json(dataset.contract)},
         {"dataset_notes", dataset.notes}});
}

} // namespace

Methodology make_m21_methodology() {
    Methodology methodology;
    methodology.id = "M-21";
    methodology.name = "Online / streaming learning";
    methodology.family = "Time & vigilance";
    methodology.model_kinds = {"online_learning", "streaming"};
    methodology.solutions = {};
    methodology.keywords = {"online learning", "streaming",
                            "real time learning"};
    methodology.inputs = {
        {"entity", true, "entity whose event windows are maintained"},
        {"time", true, "event time and optional delayed-label time"},
        {"feature", true, "numeric event values accumulated per window"},
        {"label", false, "binary outcome released no earlier than its event"},
        {"group", false, "owned partition grouping"}};
    methodology.outputs = {"online model", "streaming scores",
                           "score table", "run report"};
    methodology.metrics = {"prequential log loss", "prequential accuracy",
                           "drift loss delta", "late events excluded"};
    methodology.issue =
        "https://github.com/oramasearch/zetta/issues/1060";
    methodology.runnable = true;
    methodology.build_shape = build_m21_shape;
    methodology.owns = m21_owns;
    methodology.run = run_m21;
    return methodology;
}

} // namespace zetta::ml
