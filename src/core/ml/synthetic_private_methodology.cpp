#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/table.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/synthetic_private.hpp"
#include "zetta/ml/synthetic_private_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::ml {
namespace {

bool m20_model_kind(std::string_view value) {
    return value == "synthetic_private" || value == "privacy_preserving" ||
           value == "synthetic_data";
}

bool m20_owns(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Model || !node->attributes.is_object()) {
            continue;
        }
        const auto kind = node->attributes.find("model_kind");
        if (kind != node->attributes.end() && kind->is_string() &&
            m20_model_kind(kind->get<std::string>())) {
            return true;
        }
    }
    return false;
}

ir::ExecutionGraph build_m20_shape(const ShapeRequest& request) {
    const std::string source = request.hints.value("source", "datasets");
    const std::string table = request.hints.value("table", "private_training");
    const double epsilon = request.hints.value("epsilon", 4.0);
    const double delta = request.hints.value("delta", 1e-6);
    const double maximum_epsilon = request.hints.value("maximum_epsilon", 8.0);
    const double maximum_delta = request.hints.value("maximum_delta", 1e-5);
    const double clip_min = request.hints.value("clip_min", 0.0);
    const double clip_max = request.hints.value("clip_max", 1.0);
    const std::string noise = request.hints.value("noise", "gaussian");

    ir::ExecutionGraph graph;
    graph.add_node(ir::StepKind::Connect, "Read protected training table",
                   {{"source", source},
                    {"table", table},
                    {"note", "read each owned partition near its owner"}},
                   "load_private_training");
    graph.add_node(ir::StepKind::Clean, "Bound and encode protected columns",
                   {{"clip_min", clip_min},
                    {"clip_max", clip_max},
                    {"note", "clip numeric features and order categorical values"}},
                   "bound_private_features");
    graph.add_node(
        ir::StepKind::Model, "Generate synthetic data and train private model",
        {{"model_kind", "synthetic_private"},
         {"epsilon", epsilon},
         {"delta", delta},
         {"maximum_epsilon", maximum_epsilon},
         {"maximum_delta", maximum_delta},
         {"noise", noise},
         {"clip_min", clip_min},
         {"clip_max", clip_max},
         {"privacy_evaluation", true},
         {"note", "Gaussian copula plus DP histogram GBDT"}},
        "train_synthetic_private");
    graph.add_node(ir::StepKind::Serve, "Publish evaluated privacy outputs",
                   {{"outputs", {"synthetic_table", "private_model", "guarantee_report"}},
                    {"note", "publish only after DCR and membership checks pass"}},
                   "publish_private_outputs");
    graph.add_edge("load_private_training", "bound_private_features");
    graph.add_edge("bound_private_features", "train_synthetic_private");
    graph.add_edge("train_synthetic_private", "publish_private_outputs");
    return graph;
}

const ir::Node* model_step(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind == ir::StepKind::Model && node->attributes.is_object() &&
            m20_model_kind(
                node->attributes.value("model_kind", std::string{}))) {
            return node.get();
        }
    }
    return nullptr;
}

std::string graph_dataset_path(const ir::ExecutionGraph& graph) {
    for (const auto& node : graph.nodes()) {
        if (node->kind != ir::StepKind::Connect || !node->attributes.is_object()) {
            continue;
        }
        const std::string source =
            node->attributes.value("source", std::string{});
        const std::string table =
            node->attributes.value("table", std::string{});
        if (!source.empty() && !table.empty()) {
            return source + "." + table;
        }
    }
    return {};
}

SyntheticPrivateParams params_from(const ir::Node& model,
                                   const nlohmann::json& input) {
    SyntheticPrivateParams params;
    const auto number = [&](const char* key, double fallback) {
        if (const auto value = input.find(key);
            value != input.end() && value->is_number()) {
            return value->get<double>();
        }
        return model.attributes.value(key, fallback);
    };
    params.epsilon = number("epsilon", params.epsilon);
    params.delta = number("delta", params.delta);
    params.maximum_epsilon =
        model.attributes.value("maximum_epsilon", params.maximum_epsilon);
    params.maximum_delta =
        model.attributes.value("maximum_delta", params.maximum_delta);
    const std::string noise = input.value(
        "noise", model.attributes.value("noise", std::string("gaussian")));
    params.noise = noise == "laplace" ? PrivacyNoise::Laplace
                                       : PrivacyNoise::Gaussian;
    params.clip_min = number("clip_min", params.clip_min);
    params.clip_max = number("clip_max", params.clip_max);
    params.synthetic_rows = input.value("synthetic_rows", params.synthetic_rows);
    params.seed = input.value("seed", params.seed);
    params.gbdt.num_trees = input.value("num_trees", 12);
    params.gbdt.max_depth = input.value("max_depth", 2);
    params.gbdt.max_bins = 16;
    params.gbdt.learning_rate = 0.15;
    params.gbdt.min_child_hessian = 0.0;
    return params;
}

api::ApiResult fail_run(const std::shared_ptr<pipeline::Run>& run,
                        std::string reason) {
    if (run) {
        run->add_log({util::now_iso8601(), "error", reason});
        run->set_status(pipeline::RunStatus::Failed);
    }
    return api::ApiResult::failure("engine_error", std::move(reason));
}

api::ApiResult run_m20(RunRequest& request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-20 run request is missing its engine context");
    }
    api::ApiContext& context = *request.context;
    const ir::Node* model = model_step(request.graph->graph());
    if (model == nullptr) {
        return api::ApiResult::failure(
            "invalid_input", "M-20 graph has no synthetic_private model step");
    }
    const std::string path_value = request.input.value(
        "dataset_path", graph_dataset_path(request.graph->graph()));
    if (path_value.empty()) {
        return api::ApiResult::failure(
            "invalid_input", "M-20 graph has no source/table connect step");
    }
    const data::DatasetPath path = data::parse_dataset_path(path_value);
    if (path.kind != data::DatasetPathKind::Table) {
        return api::ApiResult::failure(
            "invalid_input", "M-20 requires a landed database.table dataset");
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
    const data::DatasetSampler sampler =
        data::default_dataset_sampler(context.node, context.cluster);
    const data::ResolvedDataset resolved = data::resolve_dataset(
        context.node, path, overrides, sampler);
    if (!resolved.ok()) {
        return api::ApiResult::failure("engine_error", resolved.error);
    }
    const std::shared_ptr<pipeline::Run> run = context.node.create_run(
        request.project->id(), request.graph->id());
    if (!run) {
        return api::ApiResult::failure("engine_error",
                                       "could not create M-20 run record");
    }
    run->set_status(pipeline::RunStatus::Running);
    run->add_log({util::now_iso8601(), "info",
                  "dataset: read: " + path_value});
    for (const std::string& note : resolved.notes) {
        run->add_log({util::now_iso8601(), "info", "dataset: " + note});
    }

    compute::JobCoordinator* coordinator = context.node.job_coordinator();
    if (coordinator == nullptr) {
        return fail_run(run, "M-20 requires an attached compute coordinator");
    }
    const SyntheticPrivateParams params = params_from(*model, request.input);
    const std::string round_tag = "synthetic-private:" + util::new_uuid();
    const std::vector<std::string> roster =
        coordinator->collectives().participants();
    if (roster.size() > 1) {
        compute::CoordinatedLaunchListener* listener =
            context.node.coordinated_launch();
        if (listener == nullptr) {
            return fail_run(run, "M-20 cluster has no coordinated launch listener");
        }
        compute::CoordinatedLaunchRequest launch;
        launch.id = util::new_uuid();
        launch.kind = "synthetic_private";
        launch.initiator = listener->node_id();
        launch.participants = roster;
        launch.payload =
            {{"table_id", resolved.table_id},
             {"contract", data::to_json(resolved.contract)},
             {"round_tag", round_tag},
             {"params",
              {{"epsilon", params.epsilon},
               {"delta", params.delta},
               {"maximum_epsilon", params.maximum_epsilon},
               {"maximum_delta", params.maximum_delta},
               {"noise", params.noise == PrivacyNoise::Gaussian ? "gaussian"
                                                                  : "laplace"},
               {"clip_min", params.clip_min},
               {"clip_max", params.clip_max},
               {"synthetic_rows", params.synthetic_rows},
               {"seed", params.seed},
               {"minimum_dcr", params.minimum_dcr},
               {"maximum_membership_auc", params.maximum_membership_auc},
               {"gbdt", params.gbdt.to_json()}}}};
        listener->fan_out(launch);
    }
    const std::shared_ptr<compute::ComputeJob> job = context.node.create_job(
        compute::JobKind::Train, request.project->id(), request.graph->id());
    if (!job) {
        return fail_run(run, "could not create M-20 compute job");
    }
    DistributedSyntheticPrivateResult distributed = run_synthetic_private_job(
        *coordinator, context.node, *job, resolved.table_id, resolved.contract,
        params, round_tag);
    if (!distributed.ok()) {
        return fail_run(run, distributed.learned.error.empty()
                                 ? job->detail()
                                 : distributed.learned.error);
    }
    SyntheticPrivateResult& learned = distributed.learned;

    std::shared_ptr<data::Database> output_database =
        context.node.find_database("synthetic_private_results");
    if (!output_database) {
        output_database =
            context.node.create_database("synthetic_private_results");
    }
    if (!output_database) {
        return fail_run(run, "could not create M-20 output database");
    }
    const std::string table_name =
        "synthetic_" + request.graph->id().substr(0, 8);
    output_database->drop_table(table_name);
    const std::shared_ptr<data::Table> output_table =
        output_database->create_table(table_name);
    if (!output_table) {
        return fail_run(run, "could not create M-20 synthetic output table");
    }
    for (nlohmann::json& row : learned.synthetic_rows) {
        output_table->add_row(std::move(row));
    }

    ModelValue model_value;
    model_value.name = request.input.value(
        "model_name", std::string("synthetic_private_model"));
    model_value.kind = "synthetic_private";
    model_value.project = request.project->id();
    model_value.run = run->id();
    model_value.feature_schema = {{"features", learned.feature_names}};
    model_value.metrics = learned.metrics.to_json();
    const std::shared_ptr<Model> private_model =
        context.node.create_model(std::move(model_value));
    if (!private_model ||
        !private_model->set_weights(learned.private_model.to_json().dump())) {
        return fail_run(run, "could not persist M-20 private model");
    }

    const std::string output_ref =
        "synthetic_private_results." + table_name;
    run->set_outputs({{"dataset", "evaluated synthetic table", output_ref},
                      {"model", "differentially private model", private_model->id()},
                      {"report", "privacy guarantee report", run->id()}});
    pipeline::RunLineage lineage;
    lineage.sources = {"dataset:" + path_value};
    for (const auto& node : request.graph->graph().nodes()) {
        lineage.steps.push_back(node->id);
    }
    lineage.freshness = util::now_iso8601();
    lineage.confidence =
        std::clamp(1.0 - std::abs(learned.metrics.membership_auc - 0.5),
                   0.0, 1.0);
    run->set_lineage(std::move(lineage));
    run->add_log({util::now_iso8601(), "info",
                  "privacy evaluation passed; published synthetic table and DP model"});
    run->set_status(pipeline::RunStatus::Succeeded);

    return api::ApiResult::success(
        {{"run_id", run->id()},
         {"engine", "synthetic_private"},
         {"status", std::string(pipeline::to_string(run->status()))},
         {"dataset", data::to_json(resolved.contract)},
         {"dataset_notes", resolved.notes}});
}

}  // namespace

Methodology make_m20_methodology() {
    Methodology methodology;
    methodology.id = "M-20";
    methodology.name = "Privacy-preserving learning & synthetic data";
    methodology.family = "The data supply chain";
    methodology.model_kinds = {"synthetic_private", "privacy_preserving",
                               "synthetic_data"};
    methodology.solutions = {};
    methodology.keywords = {"differential privacy", "private model",
                            "synthetic data", "synthetic training data",
                            "privacy preserving", "privacy-preserving"};
    methodology.inputs = {{"feature", true, "protected training features"},
                          {"label", true, "binary utility target"},
                          {"group", false, "data boundary or participant"}};
    methodology.outputs = {"synthetic table", "DP model", "guarantee report"};
    methodology.metrics = {"marginal distance", "correlation distance", "DCR",
                           "membership AUC", "TSTR AUC", "epsilon", "delta"};
    methodology.issue = "https://github.com/oramasearch/zetta/issues/1059";
    methodology.runnable = true;
    methodology.build_shape = build_m20_shape;
    methodology.owns = m20_owns;
    methodology.run = run_m20;
    return methodology;
}

}  // namespace zetta::ml
