#include "zetta/ml/ensemble_runner.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/compute/cpu_device.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::ml {
namespace {

std::string lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return value;
}

bool ensemble_kind(const std::string& value) {
    const std::string normalized = lower(value);
    return normalized == "ensemble" || normalized == "ensembling" ||
           normalized == "stacking";
}

std::string string_field(const nlohmann::json& value, const char* name,
                         std::string fallback = {}) {
    if (!value.is_object()) {
        return fallback;
    }
    const auto found = value.find(name);
    if (found == value.end() || !found->is_string()) {
        return fallback;
    }
    return found->get<std::string>();
}

std::vector<std::string> string_array_field(const nlohmann::json& value,
                                            const char* name) {
    std::vector<std::string> result;
    if (!value.is_object()) {
        return result;
    }
    const auto found = value.find(name);
    if (found == value.end() || !found->is_array()) {
        return result;
    }
    for (const nlohmann::json& entry : *found) {
        if (entry.is_string()) {
            result.push_back(entry.get<std::string>());
        }
    }
    return result;
}

template <class Number>
Number number_field(const nlohmann::json& value, const char* name,
                    Number fallback) {
    if (!value.is_object()) {
        return fallback;
    }
    const auto found = value.find(name);
    if (found == value.end() || !found->is_number()) {
        return fallback;
    }
    return found->get<Number>();
}

ir::ExecutionGraph build_m18_shape(const ShapeRequest& request) {
    std::string source = string_field(request.hints, "source", "datasets");
    source = string_field(request.hints, "database", std::move(source));
    std::string table = string_field(request.hints, "table", "training");
    if (const std::string path_value =
            string_field(request.hints, "dataset_path");
        !path_value.empty()) {
        const data::DatasetPath path = data::parse_dataset_path(path_value);
        if (path.kind == data::DatasetPathKind::Table) {
            source = path.database;
            table = path.table;
        }
    }

    const std::string entity =
        string_field(request.hints, "entity_column", "entity");
    const std::string label =
        string_field(request.hints, "label_column", "label");
    const std::string time = string_field(request.hints, "time_column");
    const std::vector<std::string> features =
        string_array_field(request.hints, "feature_columns");
    const std::size_t folds =
        number_field<std::size_t>(request.hints, "folds", 5);
    const double holdout =
        number_field<double>(request.hints, "holdout_fraction", 0.2);

    ir::ExecutionGraph graph;
    graph.add_node(
        ir::StepKind::Connect, "Load ensemble training table",
        nlohmann::json{{"source", source},
                       {"table", table},
                       {"note", "read the pipeline's resolved training table"}},
        "load_ensemble_table");
    graph.add_node(
        ir::StepKind::Clean, "Validate numeric stacking inputs",
        nlohmann::json{{"note", "project entity, label, time, and numeric features"}},
        "prepare_ensemble_inputs");
    nlohmann::json model = {
        {"model_kind", "ensemble"},
        {"entity_column", entity},
        {"label_column", label},
        {"feature_columns", features},
        {"time_column", time},
        {"folds", folds},
        {"holdout_fraction", holdout},
        {"stacking_predictions", "out_of_fold"},
        {"fold_strategy", time.empty() ? "entity_hash" : "time_ordered"},
        {"base_refit", "all_training"},
        {"note", "fit the stacker on OOF scores, then refit every base"}};
    if (!time.empty()) {
        model["time_cutoff"] =
            number_field<double>(request.hints, "as_of", 0.0);
    }
    graph.add_node(ir::StepKind::Model, "Train out-of-fold stack", model,
                   "train_ensemble");
    graph.add_node(
        ir::StepKind::Serve, "Write blended scores and meta-model weights",
        nlohmann::json{{"outputs", {"blended_scores", "meta_model_weights"}}},
        "write_ensemble_outputs");
    graph.add_edge("load_ensemble_table", "prepare_ensemble_inputs");
    graph.add_edge("prepare_ensemble_inputs", "train_ensemble");
    graph.add_edge("train_ensemble", "write_ensemble_outputs");
    return graph;
}

bool m18_owns(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const std::string kind =
            string_field(node->attributes, "model_kind");
        if (ensemble_kind(kind)) {
            return true;
        }
    }
    return false;
}

const ir::Node* first_step(const ir::ExecutionGraph& graph,
                           ir::StepKind kind) {
    const std::vector<std::shared_ptr<ir::Node>> nodes =
        graph.nodes_of_kind(kind);
    return nodes.empty() ? nullptr : nodes.front().get();
}

data::DatasetContract graph_contract(const ir::ExecutionGraph& graph,
                                     const nlohmann::json& input,
                                     std::string& error) {
    data::DatasetContract contract;
    if (const auto given = input.find("dataset"); given != input.end()) {
        const std::optional<data::DatasetContract> parsed =
            data::dataset_contract_from_json(*given, &error);
        if (!parsed) {
            return contract;
        }
        contract = *parsed;
    }
    const ir::Node* model = first_step(graph, ir::StepKind::Model);
    if (model == nullptr || !model->attributes.is_object()) {
        error = "M-18 graph has no model step with dataset roles";
        return contract;
    }
    const std::string entity =
        string_field(model->attributes, "entity_column");
    const std::string label =
        string_field(model->attributes, "label_column");
    const std::string time = string_field(model->attributes, "time_column");
    const std::vector<std::string> features =
        string_array_field(model->attributes, "feature_columns");
    if (!entity.empty()) {
        contract.entity_column = entity;
    }
    if (!label.empty()) {
        contract.label_column = label;
    }
    if (!features.empty()) {
        contract.feature_columns = features;
    }
    if (!time.empty()) {
        contract.time_column = time;
        contract.as_of =
            number_field<double>(model->attributes, "time_cutoff", 0.0);
    }
    return contract;
}

EnsembleParams graph_params(const ir::ExecutionGraph& graph,
                            const nlohmann::json& input) {
    EnsembleParams params;
    const ir::Node* model = first_step(graph, ir::StepKind::Model);
    if (model != nullptr) {
        params.folds =
            number_field<std::size_t>(model->attributes, "folds", params.folds);
        params.seed =
            number_field<std::uint64_t>(model->attributes, "seed", params.seed);
        params.holdout_fraction = number_field<double>(
            model->attributes, "holdout_fraction", params.holdout_fraction);
        if (string_field(model->attributes, "stacking_predictions") !=
            "out_of_fold") {
            params.prediction_source = StackingPredictionSource::InSample;
        }
    }
    params.holdout_fraction = number_field<double>(
        input, "holdout_fraction", params.holdout_fraction);
    return params;
}

api::ApiResult run_m18(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-18 run request is missing its engine context");
    }
    const ir::ExecutionGraph& graph = request.graph->graph();
    const ir::Node* connect = first_step(graph, ir::StepKind::Connect);
    if (connect == nullptr || !connect->attributes.is_object()) {
        return api::ApiResult::failure(
            "invalid_input", "M-18 graph has no table connect step");
    }
    const std::string database =
        string_field(connect->attributes, "source");
    const std::string table = string_field(connect->attributes, "table");
    if (database.empty() || table.empty()) {
        return api::ApiResult::failure(
            "invalid_input", "M-18 connect step needs source and table attributes");
    }

    const data::DatasetPath graph_path =
        data::parse_dataset_path(database + "." + table);
    if (const auto supplied = request.input.find("dataset_path");
        supplied != request.input.end()) {
        const data::DatasetPath supplied_path =
            data::parse_dataset_path(supplied->get<std::string>());
        if (supplied_path.kind != data::DatasetPathKind::Table ||
            lower(supplied_path.database) != lower(graph_path.database) ||
            lower(supplied_path.table) != lower(graph_path.table)) {
            return api::ApiResult::failure(
                "invalid_input",
                "M-18 trains on its pipeline table '" + database + "." + table +
                    "'; dataset_path may only repeat that table");
        }
    }

    std::string contract_error;
    const data::DatasetContract contract =
        graph_contract(graph, request.input, contract_error);
    if (!contract_error.empty()) {
        return api::ApiResult::failure("invalid_input", contract_error);
    }
    api::ApiContext& context = *request.context;
    data::ResolvedDataset dataset = data::resolve_dataset(
        context.node, graph_path, contract,
        data::default_dataset_sampler(context.node, context.cluster));
    if (!dataset.ok()) {
        dataset.contract = contract;
        return run_dataset_failure(request, "M-18", dataset);
    }

    auto feature_plan = plan_run_features(request, "M-18", dataset);
    if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
        return api::ApiResult::failure("invalid_input", reason);
    }

    EnsembleRunOptions options;
    options.params = graph_params(graph, request.input);
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.model_name =
        string_field(request.input, "model_name", options.model_name);
    compute::CpuDevice device;
    EnsembleRunResult result = run_ensemble_on_dataset(
        context.node, device, graph, dataset, options);
    if (!result.execution.run) {
        return api::ApiResult::failure(
            "engine_error", "M-18 could not create its Run record");
    }

    if (const auto model = context.node.find_model(result.model_id)) {
        for (const auto& base : model->feature_schema().at("base_models")) {
            feature_plan.entries.push_back({dataset.contract.feature_columns,
                {"oof_" + base.get<std::string>()},
                "native out-of-fold base prediction; regularized logistic stacking",
                "meta-model inputs are held out from each base fit, respecting temporal folds when configured"});
        }
    }
    record_run_features(context.node, result.execution.run->id(), feature_plan);

    nlohmann::json output;
    output["run_id"] = result.execution.run->id();
    output["engine"] = "ensemble";
    output["status"] =
        std::string(pipeline::to_string(result.execution.run->status()));
    output["dataset"] = data::to_json(dataset.contract);
    output["dataset_notes"] = dataset.notes;
    return api::ApiResult::success(std::move(output));
}

}  // namespace

Methodology make_m18_methodology() {
    Methodology methodology;
    methodology.id = "M-18";
    methodology.name = "Ensembling & stacking";
    methodology.family = "The tabular decision core";
    methodology.model_kinds = {"ensemble", "ensembling", "stacking"};
    methodology.solutions = {};
    methodology.keywords = {"ensemble", "stack", "blend models"};
    methodology.inputs = {
        {"entity", true, "entity identifier"},
        {"feature", true, "numeric base-model features"},
        {"label", true, "binary evaluation outcome"},
        {"time", false, "ordered observation time"}};
    methodology.outputs = {"ensemble model", "blended scores",
                           "meta-model weights"};
    methodology.metrics = {"held-out ROC-AUC", "log loss",
                           "lift over best single model"};
    methodology.runnable = true;
    methodology.build_shape = build_m18_shape;
    methodology.owns = m18_owns;
    methodology.run = run_m18;
    return methodology;
}

}  // namespace zetta::ml
