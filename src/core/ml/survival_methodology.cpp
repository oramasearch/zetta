#include "zetta/ml/methodology.hpp"

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <thread>

#include "zetta/api/background.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/survival.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace zetta::ml {
namespace {

bool survival_owns(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind == node->attributes.end() || !kind->is_string()) {
            continue;
        }
        const std::string value = kind->get<std::string>();
        return value == "survival" || value == "time_to_event";
    }
    return false;
}

ir::ExecutionGraph build_survival_shape(const ShapeRequest& request) {
    const std::string source = request.hints.value("source", "datasets");
    const std::string table = request.hints.value("table", "survival_subjects");
    const std::string duration =
        request.hints.value("duration_column", "duration");
    const std::string event = request.hints.value("event_indicator", "event");

    ir::ExecutionGraph graph;
    graph.add_node(
        ir::StepKind::Connect, "Load survival cohort",
        nlohmann::json{{"source", source},
                       {"table", table},
                       {"note", "one row per subject at its start cutoff"}},
        "load_survival_cohort");
    graph.add_node(
        ir::StepKind::Clean, "Validate survival observations",
        nlohmann::json{
            {"note", "refuse post-cutoff features and invalid event flags"}},
        "validate_survival_observations");
    graph.add_node(
        ir::StepKind::Model, "Train discrete-time hazards",
        nlohmann::json{
            {"model_kind", "survival"},
            {"duration_column", duration},
            {"event_indicator", event},
            {"censoring_handled", true},
            {"stop_after_event", true},
            {"point_in_time_features", true},
            {"temporal_validation", true},
            {"note", "expand person-period rows and learn interval hazards"}},
        "train_survival_model");
    graph.add_node(
        ir::StepKind::Serve, "Write survival table",
        nlohmann::json{
            {"output", "survival_table"},
            {"note", "hazard and survival probability at every horizon"}},
        "write_survival_table");
    graph.add_node(
        ir::StepKind::Act, "Write interval calibration",
        nlohmann::json{
            {"output", "interval_calibration"},
            {"note", "predicted hazard against observed event rate"}},
        "write_survival_calibration");
    graph.add_edge("load_survival_cohort", "validate_survival_observations");
    graph.add_edge("validate_survival_observations", "train_survival_model");
    graph.add_edge("train_survival_model", "write_survival_table");
    graph.add_edge("write_survival_table", "write_survival_calibration");
    return graph;
}

std::optional<std::string> graph_dataset_path(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Connect ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto source = node->attributes.find("source");
        const auto table = node->attributes.find("table");
        if (source == node->attributes.end() || !source->is_string() ||
            table == node->attributes.end() || !table->is_string()) {
            continue;
        }
        return source->get<std::string>() + "." + table->get<std::string>();
    }
    return std::nullopt;
}

api::ApiResult run_survival_methodology(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-5 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    if (context.background == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "run is a job capability and this embedding "
                            "wired no background action owner");
    }

    std::optional<std::string> dataset_path;
    if (const auto given = request.input.find("dataset_path");
        given != request.input.end()) {
        dataset_path = given->get<std::string>();
    } else {
        dataset_path = graph_dataset_path(request.graph->graph());
    }
    if (!dataset_path) {
        return api::ApiResult::failure("invalid_input",
                                       "M-5 requires a connect step with "
                                       "string source and table attributes");
    }

    data::DatasetContract contract;
    if (const auto given = request.input.find("dataset");
        given != request.input.end()) {
        std::string reason;
        const std::optional<data::DatasetContract> parsed =
            data::dataset_contract_from_json(*given, &reason);
        if (!parsed) {
            return api::ApiResult::failure("invalid_input", reason);
        }
        contract = *parsed;
    }
    const data::DatasetPath parsed_path =
        data::parse_dataset_path(*dataset_path);
    if (parsed_path.kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure(
            "invalid_input", "M-5 runs on a resolved database.table dataset");
    }
    const data::ResolvedDataset resolved = data::resolve_dataset(
        context.node, parsed_path, contract,
        data::default_dataset_sampler(context.node, context.cluster));
    if (!resolved.ok()) {
        return api::ApiResult::failure("engine_error", resolved.error);
    }

    SurvivalDatasetParams params;
    params.project = request.project->id();
    params.graph = request.graph->id();
    params.model_name = request.input.value("model_name", params.model_name);
    params.duration_column =
        request.input.value("duration_column", params.duration_column);
    params.feature_time_column =
        request.input.value("feature_time_column", params.feature_time_column);
    params.training.interval_width =
        request.input.value("interval_width", params.training.interval_width);
    params.training.max_intervals =
        request.input.value("max_intervals", params.training.max_intervals);
    params.training.validation_fraction = request.input.value(
        "validation_fraction", params.training.validation_fraction);
    params.dataset_notes = resolved.notes;

    std::set<std::string> prior_runs;
    for (const auto& existing : context.node.runs()) {
        if (existing) {
            prior_runs.insert(existing->id());
        }
    }
    auto handle = AsyncSurvivalRun::start(context.node, request.graph->graph(),
                                          resolved, params);
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
            "engine_error", "the survival Run record did not appear in time");
    }

    return api::ApiResult::success(
        {{"run_id", run_id},
         {"engine", "survival"},
         {"status", "running"},
         {"dataset", data::to_json(resolved.contract)},
         {"dataset_notes", resolved.notes},
         {"poll",
          "get_run for status; get_run_report for metrics and output tables"}});
}

} // namespace

Methodology make_m5_methodology() {
    Methodology methodology;
    methodology.id = "M-5";
    methodology.name = "Survival / time-to-event";
    methodology.family = "The tabular decision core";
    methodology.model_kinds = {"survival", "time_to_event"};
    methodology.solutions = {};
    methodology.keywords = {"time to", "when will", "survival",
                            "remaining useful life"};
    methodology.inputs = {
        {"entity", true, "entity whose event time is modeled"},
        {"time", true, "start cutoff and duration"},
        {"label", true, "binary event indicator"},
        {"feature_time", true, "feature observation cutoff"},
        {"feature", true, "point-in-time predictors"},
    };
    methodology.outputs = {"survival table", "interval hazards",
                           "expected time to event", "median survival",
                           "interval calibration"};
    methodology.metrics = {"censoring-aware concordance",
                           "IPCW integrated Brier score"};
    methodology.runnable = true;
    methodology.build_shape = build_survival_shape;
    methodology.owns = survival_owns;
    methodology.run = run_survival_methodology;
    return methodology;
}

} // namespace zetta::ml
