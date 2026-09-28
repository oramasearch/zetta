#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "zetta/api/background.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/label_efficiency_runner.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace zetta::ml {
namespace {

const nlohmann::json* model_attributes(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind == ir::StepKind::Model &&
            node->attributes.is_object()) {
            return &node->attributes;
        }
    }
    return nullptr;
}

std::string hinted_string(const nlohmann::json& hints,
                          const char* key, std::string fallback) {
    const auto found = hints.find(key);
    if (found != hints.end() && found->is_string() &&
        !found->get_ref<const std::string&>().empty()) {
        return found->get<std::string>();
    }
    return fallback;
}

ir::ExecutionGraph build_m19_shape(const ShapeRequest& request) {
    std::string source = hinted_string(request.hints, "source", "datasets");
    std::string table = hinted_string(request.hints, "table", "labels");
    if (const auto path = request.hints.find("dataset_path");
        path != request.hints.end() && path->is_string()) {
        const data::DatasetPath parsed =
            data::parse_dataset_path(path->get<std::string>());
        if (parsed.kind == data::DatasetPathKind::Table) {
            source = parsed.database;
            table = parsed.table;
        }
    }

    nlohmann::json model = {
        {"model_kind", "label_efficiency"},
        {"queue_size", 20},
        {"initial_labels", 4},
        {"gold_fraction", 0.2},
        {"seed", 1058},
        {"uncertainty", "margin"},
        {"diversity_weight", 0.25},
        {"labelling_functions", nlohmann::json::array()},
        {"weak_labels_are_gold", false},
        {"gold_split", "disjoint"},
    };
    for (const char* key : {"queue_size", "initial_labels", "gold_fraction",
                            "seed", "uncertainty", "diversity_weight",
                            "logistic_iterations",
                            "logistic_learning_rate",
                            "label_model_iterations",
                            "labelling_functions"}) {
        if (request.hints.contains(key)) {
            model[key] = request.hints.at(key);
        }
    }

    ir::ExecutionGraph graph;
    graph.add_node(ir::StepKind::Connect, "Load examples for labelling",
                   {{"source", source},
                    {"table", table},
                    {"note", "read the pipeline's own resolved table"}},
                   "load_label_examples");
    graph.add_node(ir::StepKind::Clean, "Prepare numeric label features",
                   {{"note", "retain entity, features, and binary gold label"}},
                   "prepare_label_features");
    graph.add_node(ir::StepKind::Model,
                   "Build active queue and probabilistic labels",
                   std::move(model), "train_label_efficiency");
    graph.add_node(ir::StepKind::Act, "Write label-efficiency tables",
                   {{"outputs", {"label_queue", "weak_labels"}},
                    {"note", "gold rows are excluded from both outputs"}},
                   "write_label_outputs");
    graph.add_edge("load_label_examples", "prepare_label_features");
    graph.add_edge("prepare_label_features", "train_label_efficiency");
    graph.add_edge("train_label_efficiency", "write_label_outputs");
    return graph;
}

bool m19_owns(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) {
            continue;
        }
        const std::string value = kind->get<std::string>();
        return value == "label_efficiency" || value == "active_learning" ||
               value == "weak_supervision";
    }
    return false;
}

bool read_positive_size(const nlohmann::json& value, const char* key,
                        std::size_t& output, std::string& error) {
    const auto found = value.find(key);
    if (found == value.end()) {
        return true;
    }
    if (!found->is_number_integer() || found->get<long long>() <= 0) {
        error = std::string(key) + " must be a positive integer";
        return false;
    }
    output = found->get<std::size_t>();
    return true;
}

bool apply_params(const nlohmann::json& value, LabelEfficiencyParams& params,
                  std::string& error) {
    if (!value.is_object()) {
        error = "label_efficiency must be an object";
        return false;
    }
    if (!read_positive_size(value, "queue_size", params.queue_size, error) ||
        !read_positive_size(value, "initial_labels", params.initial_labels,
                            error) ||
        !read_positive_size(value, "logistic_iterations",
                            params.logistic_iterations, error) ||
        !read_positive_size(value, "label_model_iterations",
                            params.label_model_iterations, error)) {
        return false;
    }
    if (const auto found = value.find("seed"); found != value.end()) {
        if (!found->is_number_unsigned() &&
            (!found->is_number_integer() || found->get<long long>() < 0)) {
            error = "seed must be a non-negative integer";
            return false;
        }
        params.seed = found->get<std::uint64_t>();
    }
    if (const auto found = value.find("gold_fraction"); found != value.end()) {
        if (!found->is_number()) {
            error = "gold_fraction must be a number";
            return false;
        }
        params.gold_fraction = found->get<double>();
    }
    if (const auto found = value.find("diversity_weight");
        found != value.end()) {
        if (!found->is_number()) {
            error = "diversity_weight must be a number";
            return false;
        }
        params.diversity_weight = found->get<double>();
    }
    if (const auto found = value.find("logistic_learning_rate");
        found != value.end()) {
        if (!found->is_number() || found->get<double>() <= 0.0) {
            error = "logistic_learning_rate must be positive";
            return false;
        }
        params.logistic_learning_rate = found->get<double>();
    }
    if (const auto found = value.find("uncertainty"); found != value.end()) {
        if (!found->is_string()) {
            error = "uncertainty must be margin or entropy";
            return false;
        }
        const std::optional<UncertaintyMeasure> parsed =
            uncertainty_measure_from_string(found->get<std::string>());
        if (!parsed) {
            error = "uncertainty must be margin or entropy";
            return false;
        }
        params.uncertainty = *parsed;
    }
    const auto functions = value.find("labelling_functions");
    if (functions == value.end()) {
        return true;
    }
    if (!functions->is_array()) {
        error = "labelling_functions must be an array";
        return false;
    }
    params.labelling_functions.clear();
    for (const nlohmann::json& item : *functions) {
        if (!item.is_object() || !item.contains("name") ||
            !item.at("name").is_string() ||
            item.at("name").get_ref<const std::string&>().empty() ||
            !item.contains("column") || !item.at("column").is_string() ||
            item.at("column").get_ref<const std::string&>().empty() ||
            !item.contains("value") || !item.at("value").is_number() ||
            !item.contains("label") || !item.at("label").is_number_integer()) {
            error = "each labelling function needs name, column, value, and label";
            return false;
        }
        LabellingFunction function;
        function.name = item.at("name").get<std::string>();
        function.column = item.at("column").get<std::string>();
        function.value = item.at("value").get<double>();
        function.label = item.at("label").get<int>();
        const std::string operation =
            item.value("operator", std::string("greater_equal"));
        const std::optional<RuleOperator> parsed =
            rule_operator_from_string(operation);
        if (!parsed || (function.label != 0 && function.label != 1)) {
            error = "labelling function operator or label is invalid";
            return false;
        }
        function.rule_operator = *parsed;
        if (const auto otherwise = item.find("otherwise_label");
            otherwise != item.end() && !otherwise->is_null()) {
            if (!otherwise->is_number_integer() ||
                (otherwise->get<int>() != 0 && otherwise->get<int>() != 1)) {
                error = "otherwise_label must be 0 or 1";
                return false;
            }
            function.otherwise_label = otherwise->get<int>();
        }
        params.labelling_functions.push_back(std::move(function));
    }
    return true;
}

std::optional<std::string> graph_dataset_path(
    const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Connect ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto source = node->attributes.find("source");
        const auto table = node->attributes.find("table");
        if (source != node->attributes.end() && source->is_string() &&
            table != node->attributes.end() && table->is_string()) {
            return source->get<std::string>() + "." +
                   table->get<std::string>();
        }
    }
    return std::nullopt;
}

api::ApiResult run_m19(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-19 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding has no background "
            "action owner");
    }

    std::string dataset_path;
    if (const auto input_path = request.input.find("dataset_path");
        input_path != request.input.end()) {
        dataset_path = input_path->get<std::string>();
    } else {
        const std::optional<std::string> pipeline_path =
            graph_dataset_path(request.graph->graph());
        if (!pipeline_path) {
            return api::ApiResult::failure(
                "invalid_input",
                "the M-19 graph has no connect step with source and table");
        }
        dataset_path = *pipeline_path;
    }
    const data::DatasetPath path = data::parse_dataset_path(dataset_path);
    if (path.kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure(
            "invalid_input", "M-19 requires a connector-landed table");
    }
    data::DatasetContract overrides;
    if (const auto given = request.input.find("dataset");
        given != request.input.end()) {
        std::string reason;
        const std::optional<data::DatasetContract> parsed =
            data::dataset_contract_from_json(*given, &reason);
        if (!parsed) {
            return api::ApiResult::failure("invalid_input", reason);
        }
        overrides = *parsed;
    }
    data::ResolvedDataset dataset = data::resolve_dataset(
        context.node, path, overrides,
        data::default_dataset_sampler(context.node, context.cluster));
    if (!dataset.ok()) {
        dataset.contract = overrides;
        return run_dataset_failure(request, "M-19", dataset);
    }

    LabelEfficiencyRunOptions options;
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.model_name =
        request.input.value("model_name", options.model_name);
    std::string params_error;
    if (const nlohmann::json* attributes =
            model_attributes(request.graph->graph());
        attributes != nullptr &&
        !apply_params(*attributes, options.params, params_error)) {
        return api::ApiResult::failure("invalid_input", params_error);
    }
    if (const auto supplied = request.input.find("label_efficiency");
        supplied != request.input.end() &&
        !apply_params(*supplied, options.params, params_error)) {
        return api::ApiResult::failure("invalid_input", params_error);
    }
    data::FeaturePlanOptions feature_options;
    for (const auto& source : dataset.contract.feature_columns) {
        feature_options.native_entries.push_back({{source}, {source},
            "native global mean/standard-deviation normalization for logistic scores and k-center diversity",
            "active learning uses comparable numeric inputs without using gold labels as predictors"});
    }
    for (const auto& function : options.params.labelling_functions) {
        feature_options.native_entries.push_back({{function.column}, {"weak_vote_" + function.name},
            "raw numeric threshold " + std::string(to_string(function.rule_operator)) + " " +
                std::to_string(function.value) + "; emit " + std::to_string(function.label) +
                ", otherwise " + (function.otherwise_label ? std::to_string(*function.otherwise_label) : "abstain") +
                "; generative weak-supervision label model",
            "declared labelling rules contribute votes independently of the held-out gold target"});
    }
    const auto feature_plan = plan_run_features(request, "M-19", dataset, feature_options);
    if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
        return api::ApiResult::failure("invalid_input", reason);
    }
    for (const auto& function : options.params.labelling_functions) {
        if (!std::ranges::contains(dataset.contract.feature_columns, function.column)) {
            return api::ApiResult::failure("invalid_input",
                "labelling function '" + function.name + "' needs a selected numeric input: " + function.column);
        }
    }
    if (options.params.labelling_functions.empty()) {
        return api::ApiResult::failure(
            "invalid_input",
            "M-19 needs at least one declared labelling function");
    }

    const nlohmann::json dataset_contract = data::to_json(dataset.contract);
    const nlohmann::json dataset_notes = dataset.notes;

    std::set<std::string> prior_runs;
    for (const std::shared_ptr<pipeline::Run>& existing :
         context.node.runs()) {
        if (existing) {
            prior_runs.insert(existing->id());
        }
    }
    const std::shared_ptr<AsyncLabelEfficiencyRun> handle =
        AsyncLabelEfficiencyRun::start(context.node, std::move(dataset),
                                       std::move(options));
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string run_id;
    while (std::chrono::steady_clock::now() < deadline) {
        context.node.drain_owner_work();
        for (const std::shared_ptr<pipeline::Run>& candidate :
             context.node.runs()) {
            if (!candidate || prior_runs.contains(candidate->id())) {
                continue;
            }
            if (candidate->project() == request.project->id() &&
                candidate->graph() == request.graph->id()) {
                run_id = candidate->id();
                break;
            }
        }
        if (!run_id.empty()) {
            break;
        }
        if (handle->done()) {
            if (handle->result().run) {
                run_id = handle->result().run->id();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (run_id.empty()) {
        if (handle->done() && !handle->result().ok()) {
            return api::ApiResult::failure("engine_error",
                                           handle->result().error);
        }
        return api::ApiResult::failure(
            "engine_error",
            "the M-19 run was submitted but its Run record did not appear");
    }

    record_run_features(context.node, run_id, feature_plan);
    return api::ApiResult::success(
        {{"run_id", run_id},
         {"engine", "label_efficiency"},
         {"status", "running"},
         {"poll", "get_run for status; get_run_report for metrics and tables"},
         {"dataset", dataset_contract},
         {"dataset_notes", dataset_notes}});
}

}  // namespace

Methodology make_m19_methodology() {
    Methodology methodology;
    methodology.id = "M-19";
    methodology.name = "Label efficiency";
    methodology.family = "The data supply chain";
    methodology.model_kinds = {
        "label_efficiency", "active_learning", "weak_supervision"};
    methodology.solutions = {
        "label-efficient-training", "support-ticket-triage"};
    methodology.keywords = {
        "active learning", "weak supervision", "label efficiency"};
    methodology.inputs = {
        {"entity", true, "row identifier"},
        {"feature", true, "numeric columns used by the learner and rules"},
        {"label", true, "binary gold labels reserved for evaluation"},
    };
    methodology.outputs = {"label queue table", "weak-label table"};
    methodology.metrics = {"accuracy at labelling budget", "labels saved",
                           "label-model gold accuracy"};
    methodology.runnable = true;
    methodology.build_shape = build_m19_shape;
    methodology.owns = m19_owns;
    methodology.run = run_m19;
    return methodology;
}

}  // namespace zetta::ml
