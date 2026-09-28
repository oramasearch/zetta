#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "zetta/api/background.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"

namespace zetta::ml {
namespace {

std::string lower(std::string_view value) {
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return result;
}

bool m1_model_kind(std::string_view value) {
    const std::string normalized = lower(value);
    return normalized == "gbdt" || normalized == "gradient_boosting" ||
           normalized == "classifier" || normalized == "classification" ||
           normalized == "regression" || normalized == "score" ||
           normalized == "tabular";
}

bool m1_owns(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        if (const auto kind = node->attributes.find("model_kind");
            kind != node->attributes.end() && kind->is_string()) {
            return m1_model_kind(kind->get<std::string>());
        }
        if (const auto algorithm = node->attributes.find("algo");
            algorithm != node->attributes.end() && algorithm->is_string()) {
            return m1_model_kind(algorithm->get<std::string>());
        }
    }
    return zetta::usecases::churn::is_churn_graph(graph);
}

ir::ExecutionGraph build_m1_shape(const ShapeRequest&) {
    ir::ExecutionGraph graph;

    graph.add_node(
        ir::StepKind::Connect, "Load customer accounts",
        nlohmann::json{{"source", "warehouse"},
                       {"table", "customers"},
                       {"note", "active accounts to score for churn risk"}},
        "load_customers");
    graph.add_node(
        ir::StepKind::Connect, "Load usage & billing activity",
        nlohmann::json{{"source", "warehouse"},
                       {"table", "events"},
                       {"note", "engagement + payment signals per account"}},
        "load_activity");
    graph.add_node(
        ir::StepKind::Clean, "Build churn features",
        nlohmann::json{
            {"note", "point-in-time features; keep out post-cutoff leakage"}},
        "build_features");
    graph.add_node(
        ir::StepKind::Model, "Train churn-risk model",
        nlohmann::json{{"model_kind", "gbdt"},
                       {"algo", "gradient_boosting"},
                       {"target", "churned"},
                       {"note", "predict probability each account churns"}},
        "train_churn_model");
    graph.add_node(
        ir::StepKind::Index, "Index churn scores",
        nlohmann::json{{"kind", "feature"},
                       {"note", "make scores fast to look up at serve time"}},
        "index_scores");
    graph.add_node(
        ir::StepKind::Serve, "Serve churn scores & reasons",
        nlohmann::json{{"note", "expose per-account risk + top drivers"}},
        "serve_scores");
    graph.add_node(
        ir::StepKind::Act, "Trigger retention outreach",
        nlohmann::json{{"note", "notify retention on high-risk accounts"}},
        "notify_retention");

    graph.add_edge("load_customers", "build_features");
    graph.add_edge("load_activity", "build_features");
    graph.add_edge("build_features", "train_churn_model");
    graph.add_edge("train_churn_model", "index_scores");
    graph.add_edge("index_scores", "serve_scores");
    graph.add_edge("serve_scores", "notify_retention");
    return graph;
}

api::ApiResult run_m1(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-1 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding wired no background "
            "action owner");
    }

    zetta::usecases::churn::ChurnDataset dataset;
    zetta::usecases::churn::ChurnTableInput table;
    nlohmann::json dataset_notes = nlohmann::json::array();
    nlohmann::json dataset_contract;
    if (const auto dataset_path = request.input.find("dataset_path");
        dataset_path != request.input.end()) {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind == zetta::data::DatasetPathKind::Table) {
            zetta::data::DatasetContract overrides;
            if (const auto given = request.input.find("dataset");
                given != request.input.end()) {
                std::string reason;
                const std::optional<zetta::data::DatasetContract> parsed =
                    zetta::data::dataset_contract_from_json(*given, &reason);
                if (!parsed) {
                    return api::ApiResult::failure("invalid_input", reason);
                }
                overrides = *parsed;
            }
            const zetta::data::ResolvedDataset resolved =
                zetta::data::resolve_dataset(
                    context.node, path, overrides,
                    zetta::data::default_dataset_sampler(context.node,
                                                         context.cluster));
            if (!resolved.ok()) {
                return api::ApiResult::failure("engine_error", resolved.error);
            }
            table.table_id = resolved.table_id;
            table.contract = resolved.contract;
            table.dataset_name = dataset_path->get<std::string>();
            table.notes = resolved.notes;
            dataset_notes = resolved.notes;
            dataset_contract = to_json(resolved.contract);
        } else {
            auto loaded =
                zetta::usecases::churn::load_churn_dataset(path.directory);
            if (!loaded.ok()) {
                return api::ApiResult::failure("engine_error", loaded.error);
            }
            dataset = std::move(loaded.dataset);
        }
    } else {
        // #1040: resolve the pipeline's connect-step table here when that work
        // lands; until then, preserve the built-in demo default.
        dataset = zetta::usecases::churn::demo_churn_dataset();
    }

    zetta::usecases::churn::ChurnSolutionOptions options;
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.model_name = request.input.value("model_name", options.model_name);
    options.top_k = request.input.value("top_k", options.top_k);
    options.coordinate = request.input.value("coordinate", true);
    options.params.holdout_fraction = request.input.value(
        "holdout_fraction", options.params.holdout_fraction);

    std::set<std::string> prior_runs;
    for (const auto& existing : context.node.runs()) {
        if (existing) {
            prior_runs.insert(existing->id());
        }
    }
    auto handle = table.empty() ? zetta::usecases::churn::AsyncChurnRun::start(
                                      context.node, request.graph->graph(),
                                      std::move(dataset), options)
                                : zetta::usecases::churn::AsyncChurnRun::start(
                                      context.node, request.graph->graph(),
                                      std::move(table), options);
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string run_id;
    while (std::chrono::steady_clock::now() < deadline) {
        context.node.drain_owner_work();
        for (const auto& candidate : context.node.runs()) {
            if (!candidate || prior_runs.contains(candidate->id())) {
                continue;
            }
            if (candidate->project() != request.project->id() ||
                candidate->graph() != request.graph->id()) {
                continue;
            }
            run_id = candidate->id();
            break;
        }
        if (!run_id.empty()) {
            break;
        }
        if (handle->done()) {
            if (handle->result().execution.run) {
                run_id = handle->result().execution.run->id();
            }
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (run_id.empty()) {
        if (handle->done() && !handle->result().ok()) {
            return api::ApiResult::failure("engine_error",
                                           handle->result().execution.error);
        }
        return api::ApiResult::failure(
            "engine_error",
            "the run was submitted but its Run record did not appear in time "
            "— poll list_runs for a run labelled project '" +
                request.project->id() + "'");
    }

    nlohmann::json output;
    output["run_id"] = run_id;
    output["engine"] = "churn";
    output["status"] = "running";
    output["poll"] =
        "get_run / system.runs for status+logs; system.jobs for the "
        "featurize/train/explain/score jobs";
    if (!dataset_contract.is_null()) {
        output["dataset"] = std::move(dataset_contract);
        output["dataset_notes"] = std::move(dataset_notes);
    }
    return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_m1_methodology() {
    Methodology methodology;
    methodology.id = "M-1";
    methodology.name = "Tabular supervised learning";
    methodology.family = "The tabular decision core";
    methodology.model_kinds = {"gbdt",       "gradient_boosting",
                               "classifier", "classification",
                               "regression", "score",
                               "tabular"};
    methodology.solutions = {"S-1", "S-2", "S-3", "S-4", "S-10"};
    methodology.keywords = {
        "churn",    "retention",       "retain",     "attrition", "cancel",
        "renew",    "renewal",         "subscriber", "lapse",     "downgrade",
        "win-back", "predict whether", "classify",   "score"};
    methodology.inputs = {
        {"entity", true, "entity identifier"},
        {"label", true, "outcome column"},
        {"time", false, "point-in-time cutoff column"},
    };
    methodology.outputs = {"model", "scores", "drivers", "ranked entities"};
    methodology.metrics = {"ROC-AUC", "PR-AUC", "log loss", "Brier score",
                           "lift"};
    methodology.runnable = true;
    methodology.build_shape = build_m1_shape;
    methodology.owns = m1_owns;
    methodology.run = run_m1;
    return methodology;
}

std::string unsupported_methodology_message(const Methodology& methodology) {
    return methodology.name + " (" + methodology.id +
           ") is not supported yet: " + methodology.issue;
}

std::string runnable_methodologies() {
    std::string result;
    for (const Methodology& methodology : methodologies()) {
        if (!methodology.runnable) {
            continue;
        }
        if (!result.empty()) {
            result += ", ";
        }
        result += methodology.name + " (" + methodology.id + ")";
    }
    return result;
}

nlohmann::json methodology_json(const Methodology& methodology) {
    nlohmann::json inputs = nlohmann::json::array();
    for (const MethodologyInput& input : methodology.inputs) {
        inputs.push_back({{"role", input.role},
                          {"required", input.required},
                          {"description", input.description}});
    }
    return nlohmann::json{{"id", methodology.id},
                          {"name", methodology.name},
                          {"family", methodology.family},
                          {"model_kinds", methodology.model_kinds},
                          {"solutions", methodology.solutions},
                          {"inputs", std::move(inputs)},
                          {"outputs", methodology.outputs},
                          {"metrics", methodology.metrics},
                          {"runnable", methodology.runnable},
                          {"partial", methodology.partial},
                          {"issue", methodology.issue}};
}

} // namespace zetta::ml
