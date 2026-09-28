#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/bayesian.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

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

std::string hint_string(const nlohmann::json& hints, const char* key,
                        std::string fallback) {
    const auto found = hints.find(key);
    if (found == hints.end() || !found->is_string() || found->empty()) {
        return fallback;
    }
    return found->get<std::string>();
}

bool bayesian_model_kind(std::string_view value) {
    const std::string normalized = lower(value);
    return normalized == "bayesian" || normalized == "probabilistic" ||
           normalized == "uncertainty";
}

bool owns_bayesian(const ir::ExecutionGraph& graph) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model ||
            !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind != node->attributes.end() && kind->is_string() &&
            bayesian_model_kind(kind->get<std::string>())) {
            return true;
        }
    }
    return false;
}

ir::ExecutionGraph build_bayesian_shape(const ShapeRequest& request) {
    const std::string database =
        hint_string(request.hints, "database", "datasets");
    const std::string table =
        hint_string(request.hints, "table", "observations");
    const std::string entity =
        hint_string(request.hints, "entity_column", "id");
    const std::string label =
        hint_string(request.hints, "label_column", "value");
    const std::string group =
        hint_string(request.hints, "group_column", "group");
    const double nominal_coverage =
        request.hints.value("nominal_coverage", 0.9);

    ir::ExecutionGraph graph;
    graph.add_node(
        ir::StepKind::Connect, "Read grouped observations",
        {{"source", database},
         {"table", table},
         {"entity_column", entity},
         {"label_column", label},
         {"group_column", group}},
        "read_grouped_observations");
    graph.add_node(
        ir::StepKind::Clean, "Reserve a disjoint coverage holdout",
        {{"split", "content_hash"},
         {"holdout_fraction", request.hints.value("holdout_fraction", 0.2)},
         {"note", "holdout rows never contribute to posterior fitting"}},
        "reserve_coverage_holdout");
    graph.add_node(
        ir::StepKind::Model, "Fit partial-pooling posterior",
        {{"model_kind", "bayesian"},
         {"uncertainty_kind", "hierarchical"},
         {"inference", "gibbs"},
         {"label", label},
         {"group_column", group},
         {"nominal_coverage", nominal_coverage},
         {"coverage_checked", true},
         {"exchangeability_assumption",
          "observations are exchangeable within the declared groups"}},
        "fit_hierarchical_posterior");
    graph.add_node(
        ir::StepKind::Serve, "Write posterior estimates and intervals",
        {{"output", "table"},
         {"columns",
          {"group", "posterior_mean", "credible_lower", "credible_upper",
           "predictive_lower", "predictive_upper"}}},
        "write_posterior_table");
    graph.add_edge("read_grouped_observations", "reserve_coverage_holdout");
    graph.add_edge("reserve_coverage_holdout", "fit_hierarchical_posterior");
    graph.add_edge("fit_hierarchical_posterior", "write_posterior_table");
    return graph;
}

std::optional<std::string> attribute_string(const ir::ExecutionGraph& graph,
                                            ir::StepKind kind,
                                            const char* attribute) {
    for (const std::shared_ptr<ir::Node>& node : graph.nodes_of_kind(kind)) {
        if (!node->attributes.is_object()) {
            continue;
        }
        const auto found = node->attributes.find(attribute);
        if (found != node->attributes.end() && found->is_string() &&
            !found->empty()) {
            return found->get<std::string>();
        }
    }
    return std::nullopt;
}

api::ApiResult run_bayesian_methodology(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-17 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    const ir::ExecutionGraph& graph = request.graph->graph();

    std::string dataset_path;
    if (const auto given = request.input.find("dataset_path");
        given != request.input.end()) {
        dataset_path = given->get<std::string>();
    } else {
        const std::optional<std::string> source =
            attribute_string(graph, ir::StepKind::Connect, "source");
        const std::optional<std::string> table =
            attribute_string(graph, ir::StepKind::Connect, "table");
        if (!source || !table) {
            return api::ApiResult::failure(
                "invalid_input",
                "M-17 needs dataset_path or a connect step with source and table");
        }
        dataset_path = *source + "." + *table;
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
    if (overrides.entity_column.empty()) {
        overrides.entity_column =
            attribute_string(graph, ir::StepKind::Connect, "entity_column")
                .value_or("id");
    }
    if (overrides.label_column.empty()) {
        overrides.label_column =
            attribute_string(graph, ir::StepKind::Model, "label")
                .value_or("value");
    }

    BayesianParams params;
    params.group_column = request.input.value(
        "group_column",
        attribute_string(graph, ir::StepKind::Model, "group_column")
            .value_or("group"));
    params.nominal_coverage = request.input.value(
        "nominal_coverage",
        [&graph] {
            for (const std::shared_ptr<ir::Node>& node :
                 graph.nodes_of_kind(ir::StepKind::Model)) {
                if (node->attributes.is_object()) {
                    return node->attributes.value("nominal_coverage", 0.9);
                }
            }
            return 0.9;
        }());
    params.holdout_fraction =
        request.input.value("holdout_fraction", params.holdout_fraction);
    params.seed = request.input.value("seed", params.seed);
    params.burn_in = request.input.value("burn_in", params.burn_in);
    params.posterior_draws =
        request.input.value("posterior_draws", params.posterior_draws);

    if (overrides.feature_columns.empty()) {
        overrides.feature_columns = {params.group_column};
    }
    const data::DatasetPath parsed_path = data::parse_dataset_path(dataset_path);
    if (parsed_path.kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure(
            "invalid_input", "M-17 requires a database.table dataset");
    }
    data::ResolvedDataset resolved = data::resolve_dataset(
        context.node, parsed_path, overrides,
        data::default_dataset_sampler(context.node, context.cluster));
    if (!resolved.ok()) {
        resolved.contract = overrides;
        return run_dataset_failure(request, "M-17", resolved);
    }

    data::FeaturePlanOptions feature_options;
    feature_options.group_column = params.group_column;
    feature_options.native_entries.push_back({
        {params.group_column, resolved.contract.label_column},
        {"hierarchical_group_statistics"},
        "fit-row group count, sum and sum of squares; native Gibbs partial pooling",
        "grouped observations fit posterior estimates; separate holdout rows measure coverage"});
    const auto feature_plan = plan_run_features(request, "M-17", resolved, feature_options);
    if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
        return api::ApiResult::failure("invalid_input", reason);
    }

    BayesianRunResult run_result = run_bayesian(
        context.node, resolved, params, request.project->id(),
        request.graph->id(),
        request.input.value("model_name", "bayesian_hierarchical_model"),
        context.cluster);
    if (!run_result.ok()) {
        return api::ApiResult::failure("engine_error", run_result.error);
    }
    record_run_features(context.node, run_result.run_id, feature_plan);
    return api::ApiResult::success(
        {{"run_id", run_result.run_id},
         {"engine", "bayesian"},
         {"status", "succeeded"},
         {"dataset", data::to_json(resolved.contract)},
         {"dataset_notes", resolved.notes},
         {"output_table", run_result.output_table}});
}

} // namespace

Methodology make_m17_methodology() {
    Methodology methodology;
    methodology.id = "M-17";
    methodology.name =
        "Bayesian / probabilistic modeling & uncertainty quantification";
    methodology.family = "The tabular decision core";
    methodology.model_kinds = {"bayesian", "probabilistic", "uncertainty"};
    methodology.keywords = {"bayesian", "uncertainty", "credible interval",
                            "probabilistic", "partial pooling",
                            "prediction interval"};
    methodology.inputs = {
        {"entity", true, "stable row identifier"},
        {"label", true, "numeric observation"},
        {"group", true, "hierarchical group"},
        {"time", false, "ordered observation time"},
        {"prediction", false, "point prediction or class probabilities"},
    };
    methodology.outputs = {"posterior estimates", "credible intervals",
                           "prediction intervals", "prediction sets"};
    methodology.metrics = {"empirical coverage", "nominal coverage",
                           "mean interval width", "mean prediction-set size"};
    methodology.issue = "https://github.com/oramasearch/zetta/issues/1056";
    methodology.runnable = true;
    methodology.build_shape = build_bayesian_shape;
    methodology.owns = owns_bayesian;
    methodology.run = run_bayesian_methodology;
    return methodology;
}

} // namespace zetta::ml
