// SPDX-License-Identifier: MIT

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"

#include <algorithm>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "zetta/api/background.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/rank.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace zetta::ml {
namespace {

std::string hint_string(const nlohmann::json& hints, const char* key, std::string fallback) {
    const auto value = hints.find(key);
    if (value == hints.end() || !value->is_string() || value->empty()) {
        return fallback;
    }
    return value->get<std::string>();
}

std::vector<std::string> hint_features(const nlohmann::json& hints) {
    std::vector<std::string> features;
    const auto value = hints.find("features");
    if (value == hints.end() || !value->is_array()) {
        return features;
    }
    for (const nlohmann::json& feature : *value) {
        if (feature.is_string() && !feature.empty()) {
            features.push_back(feature.get<std::string>());
        }
    }
    return features;
}

ir::ExecutionGraph build_m9_shape(const ShapeRequest& request) {
    const std::string source = hint_string(request.hints, "source", "datasets");
    const std::string table = hint_string(request.hints, "table", "ranking_candidates");
    const std::string entity = hint_string(request.hints, "entity", "entity_id");
    const std::string group = hint_string(request.hints, "group", "group_id");
    const std::string label = hint_string(request.hints, "label", "relevance");
    const std::vector<std::string> features = hint_features(request.hints);
    const std::size_t cutoff = request.hints.value("cutoff", std::size_t{5});
    const double holdout = request.hints.value("holdout_fraction", 0.25);

    ir::ExecutionGraph graph;
    graph.add_node(ir::StepKind::Connect, "Load ranking candidates",
                   {{"source", source}, {"table", table}}, "load_candidates");
    graph.add_node(ir::StepKind::Clean, "Select ranking-time features",
                   {{"note", "retain only features available when the queue is ranked"}},
                   "ranking_features");
    graph.add_node(ir::StepKind::Model, "Train LambdaMART ranker",
                   {{"model_kind", "rank"},
                    {"algorithm", "lambdamart"},
                    {"entity_column", entity},
                    {"group_column", group},
                    {"label", label},
                    {"feature_columns", features},
                    {"feature_availability", "ranking_time"},
                    {"split_strategy", "group"},
                    {"holdout_fraction", holdout},
                    {"cutoff", cutoff}},
                   "train_ranker");
    graph.add_node(ir::StepKind::Act, "Materialize ranked queue",
                   {{"output", "group_entity_score_rank"}}, "ranked_queue");
    graph.add_edge("load_candidates", "ranking_features");
    graph.add_edge("ranking_features", "train_ranker");
    graph.add_edge("train_ranker", "ranked_queue");
    return graph;
}

bool rank_kind(std::string_view kind) {
    return kind == "rank" || kind == "ranking" || kind == "learning_to_rank";
}

bool m9_owns(const ir::ExecutionGraph& graph) {
    for (const auto& step : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (!step->attributes.is_object()) {
            continue;
        }
        const auto kind = step->attributes.find("model_kind");
        if (kind != step->attributes.end() && kind->is_string() &&
            rank_kind(kind->get<std::string>())) {
            return true;
        }
    }
    return false;
}

const ir::Node* rank_model(const ir::ExecutionGraph& graph) {
    for (const auto& step : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (step && step->attributes.is_object()) {
            return step.get();
        }
    }
    return nullptr;
}

std::optional<data::DatasetPath> graph_dataset_path(const ir::ExecutionGraph& graph) {
    for (const auto& step : graph.nodes_of_kind(ir::StepKind::Connect)) {
        if (!step || !step->attributes.is_object()) {
            continue;
        }
        const std::string source = step->attributes.value("source", std::string{});
        const std::string table = step->attributes.value("table", std::string{});
        if (!source.empty() && !table.empty()) {
            return data::parse_dataset_path(source + "." + table);
        }
    }
    return std::nullopt;
}

api::ApiResult run_m9(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr || request.graph == nullptr) {
        return api::ApiResult::failure("engine_error",
                                       "M-9 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "run is a job capability and this embedding wired no background "
                            "action owner");
    }
    const ir::Node* model = rank_model(request.graph->graph());
    if (model == nullptr) {
        return api::ApiResult::failure("invalid_input", "M-9 graph has no model step");
    }

    data::DatasetPath path;
    if (const auto supplied = request.input.find("dataset_path"); supplied != request.input.end()) {
        path = data::parse_dataset_path(supplied->get<std::string>());
    } else if (const std::optional<data::DatasetPath> connected =
                   graph_dataset_path(request.graph->graph())) {
        path = *connected;
    } else {
        return api::ApiResult::failure(
            "invalid_input", "M-9 needs dataset_path or a connect step with source and table");
    }
    if (path.kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure("invalid_input",
                                       "M-9 reads a partitioned database.table dataset");
    }

    data::DatasetContract overrides;
    if (const auto supplied = request.input.find("dataset"); supplied != request.input.end()) {
        std::string error;
        const std::optional<data::DatasetContract> parsed =
            data::dataset_contract_from_json(*supplied, &error);
        if (!parsed) {
            return api::ApiResult::failure("invalid_input", error);
        }
        overrides = *parsed;
    }
    const nlohmann::json& attributes = model->attributes;
    if (overrides.entity_column.empty()) {
        overrides.entity_column = attributes.value("entity_column", std::string{});
    }
    if (overrides.label_column.empty()) {
        overrides.label_column = attributes.value("label", std::string{});
    }
    if (overrides.feature_columns.empty()) {
        const auto features = attributes.find("feature_columns");
        if (features != attributes.end() && features->is_array()) {
            for (const nlohmann::json& feature : *features) {
                if (feature.is_string()) {
                    overrides.feature_columns.push_back(feature.get<std::string>());
                }
            }
        }
    }

    RankParams params;
    params.group_column = attributes.value("group_column", std::string{});
    params.cutoff = attributes.value("cutoff", std::size_t{5});
    params.holdout_fraction = request.input.value(
        "holdout_fraction", attributes.value("holdout_fraction", params.holdout_fraction));
    if (params.group_column.empty()) {
        return api::ApiResult::failure("invalid_input", "M-9 requires a group column");
    }
    if (std::find(overrides.feature_columns.begin(), overrides.feature_columns.end(),
                  params.group_column) == overrides.feature_columns.end()) {
        overrides.feature_columns.push_back(params.group_column);
    }
    data::ResolvedDataset resolved =
        data::resolve_dataset(context.node, path, overrides,
                              data::default_dataset_sampler(context.node, context.cluster));
    if (!resolved.ok()) {
        return run_dataset_failure(request, "M-9", resolved);
    }
    std::erase(resolved.contract.feature_columns, params.group_column);
    if (resolved.contract.feature_columns.empty()) {
        return api::ApiResult::failure("invalid_input",
                                       "M-9 requires at least one ranking-time feature");
    }

    RankRunOptions options;
    data::FeaturePlanOptions feature_options;
    feature_options.group_column = params.group_column;
    const auto feature_plan = plan_run_features(request, "M-9", resolved, feature_options);
    if (const auto error = feature_plan_error(feature_plan); !error.empty()) {
        return api::ApiResult::failure("invalid_input", error);
    }
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.model_name = request.input.value("model_name", std::string("rank_model"));
    std::set<std::string> prior_runs;
    for (const auto& existing : context.node.runs()) {
        if (existing) {
            prior_runs.insert(existing->id());
        }
    }
    const auto handle =
        AsyncRankRun::start(context.node, request.graph->graph(), resolved, params, options);
    context.background->adopt(handle, [worker = handle.get()] {
        return worker->done();
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string run_id;
    while (std::chrono::steady_clock::now() < deadline) {
        context.node.drain_owner_work();
        for (const auto& candidate : context.node.runs()) {
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
        return api::ApiResult::failure("engine_error", handle->done()
                                                           ? handle->result().error
                                                           : "M-9 run record did not appear");
    }

    nlohmann::json output;
    record_run_features(context.node, run_id, feature_plan);
    output["run_id"] = run_id;
    output["engine"] = "rank";
    output["status"] = "running";
    output["poll"] = "get_run for status; get_run_report for NDCG and yield";
    output["dataset"] = data::to_json(resolved.contract);
    output["dataset_notes"] = resolved.notes;
    return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_m9_methodology() {
    Methodology methodology;
    methodology.id = "M-9";
    methodology.name = "Learning-to-rank";
    methodology.family = "Action & decision";
    methodology.model_kinds = {"ranking", "learning_to_rank", "rank"};
    methodology.solutions = {};
    methodology.keywords = {"rank", "prioritise", "prioritize", "queue"};
    methodology.inputs = {{"entity", true, "items to order"},
                          {"group", true, "query or queue identifier"},
                          {"label", true, "graded relevance"},
                          {"time", false, "entity or observation hold-out time"},
                          {"feature", true, "values available at ranking time"}};
    methodology.outputs = {"ranked-queue table", "priority scores", "model"};
    methodology.metrics = {"NDCG@k", "yield at budget"};
    methodology.issue = "https://github.com/oramasearch/zetta/issues/1048";
    methodology.runnable = true;
    methodology.build_shape = build_m9_shape;
    methodology.owns = m9_owns;
    methodology.run = run_m9;
    return methodology;
}

} // namespace zetta::ml
