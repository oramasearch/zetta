#include "zetta/ml/segmentation_methodology.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/api/background.hpp"
#include "zetta/cluster/collectives.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {
using Json = nlohmann::json;
constexpr const char *kLaunchKind = "segmentation";

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return text;
}

Json default_config() {
    return {{"scaling", "global_auto"},
            {"encoding", "bounded_one_hot"},
            {"selection", "silhouette_then_davies_bouldin"}};
}

ir::ExecutionGraph build_shape(const ShapeRequest &request) {
    Json config = default_config();
    const Json hints =
        request.hints.is_object() ? request.hints : Json::object();
    if (hints.is_object()) {
        for (const auto &[key, value] : hints.items()) {
            if (key != "source" && key != "table") {
                config[key] = value;
            }
        }
    }
    ir::ExecutionGraph graph;
    graph.add_node(ir::StepKind::Connect, "Load entities to segment",
                   {{"source", hints.value("source", Json("datasets"))},
                    {"table", hints.value("table", Json("customers"))}},
                   "load_entities");
    graph.add_node(ir::StepKind::Clean,
                   "Scale numeric and encode categorical features",
                   {{"segmentation", config}, {"entity_key", "kept_aside"}},
                   "prepare_segments");
    graph.add_node(ir::StepKind::Model, "Choose k and train entity segments",
                   {{"model_kind", "segmentation"}, {"segmentation", config}},
                   "train_segments");
    graph.add_node(ir::StepKind::Act, "Write one segment assignment per entity",
                   {{"output", "assignments"}}, "write_assignments");
    graph.add_node(ir::StepKind::Act, "Write descriptive segment profiles",
                   {{"output", "profiles"}}, "write_profiles");
    graph.add_edge("load_entities", "prepare_segments");
    graph.add_edge("prepare_segments", "train_segments");
    graph.add_edge("train_segments", "write_assignments");
    graph.add_edge("train_segments", "write_profiles");
    return graph;
}

bool owns(const ir::ExecutionGraph &graph) {
    for (const auto &step : graph.nodes()) {
        if (step->kind != ir::StepKind::Model) {
            continue;
        }
        const auto kind = step->attributes.find("model_kind");
        if (kind != step->attributes.end() && kind->is_string() &&
            is_segmentation_kind(kind->get<std::string>())) {
            return true;
        }
    }
    return false;
}

std::string actual_name(const data::DatasetSummary &summary,
                        const std::string &name) {
    for (const auto &column : summary.columns) {
        if (lower(column) == lower(name)) {
            return column;
        }
    }
    return {};
}

bool resolve_roles(const data::DatasetSummary &summary, Json &config,
                   SegmentationParams &params,
                   data::ResolvedDataset &resolved) {
    if (params.entity_column.empty()) {
        for (const auto &candidate : data::entity_column_candidates()) {
            params.entity_column = actual_name(summary, candidate);
            if (!params.entity_column.empty()) {
                resolved.notes.push_back("entity column inferred from name: " +
                                         params.entity_column);
                break;
            }
        }
    }
    params.entity_column = actual_name(summary, params.entity_column);
    if (params.entity_column.empty()) {
        resolved.error = "segmentation: name an existing integer entity_column "
                         "(for example customer_id)";
        return false;
    }
    for (const auto *role : {"label_column", "time_column"}) {
        const std::string named = config.value(role, "");
        const auto actual = actual_name(summary, named);
        if (!named.empty() && actual.empty()) {
            resolved.error =
                "segmentation: no column '" + named + "' for " + role;
            return false;
        }
        if (!actual.empty()) {
            config[role] = actual;
        }
    }
    params.label_column = config.value("label_column", "");
    for (const auto &candidate : data::label_column_candidates()) {
        const auto named = actual_name(summary, candidate);
        if (!named.empty()) {
            params.excluded_columns.push_back(named);
        }
    }
    const auto time = config.value("time_column", "");
    if (!time.empty()) {
        params.excluded_columns.push_back(time);
    }
    return true;
}

bool resolve_features(const data::DatasetSummary &summary,
                      SegmentationParams &params,
                      data::ResolvedDataset &resolved) {
    const bool inferred = params.feature_columns.empty();
    std::vector<std::string> features;
    for (const auto &column :
         inferred ? summary.columns : params.feature_columns) {
        if (segmentation_excludes_column(column, params)) {
            if (!inferred) {
                resolved.error = "segmentation: feature '" + column +
                                 "' is an entity key or label/outcome";
                return false;
            }
            params.excluded_columns.push_back(column);
            continue;
        }
        const auto actual = actual_name(summary, column);
        if (actual.empty()) {
            resolved.error = "segmentation: no feature column '" + column + "'";
            return false;
        }
        features.push_back(actual);
    }
    if (features.empty()) {
        resolved.error =
            "segmentation: the table has no eligible feature columns";
        return false;
    }
    if (inferred) {
        resolved.notes.push_back(
            "numeric features inferred from non-role columns; "
            "text is excluded unless declared categorical");
    }
    params.feature_columns = std::move(features);
    return true;
}

bool resolve_categories(const data::DatasetSummary &summary, const Json &config,
                        SegmentationParams &params,
                        data::ResolvedDataset &resolved) {
    // Only conventional categorical names are inferred. Arbitrary strings
    // (including repeated free text) need an explicit categorical role.
    if (!config.contains("categorical_columns")) {
        for (const auto &column : params.feature_columns) {
            const auto name = lower(column);
            if (name == "region" || name == "country" || name == "channel" ||
                name == "plan" || name == "tier" || name == "category") {
                params.categorical_columns.push_back(column);
            }
        }
        if (!params.categorical_columns.empty()) {
            resolved.notes.push_back("categorical roles inferred by name; "
                                     "levels are bounded globally");
        }
    }
    for (auto &column : params.categorical_columns) {
        column = actual_name(summary, column);
        if (column.empty() || std::find(params.feature_columns.begin(),
                                        params.feature_columns.end(), column) ==
                                  params.feature_columns.end()) {
            resolved.error = "segmentation: each categorical column must "
                             "be an eligible feature";
            return false;
        }
    }
    return true;
}

// Unlike the supervised resolver, M-3 never needs to inspect a label value.
// Catalog names select candidates; the trainer checks types and cardinality
// globally over owned partitions, not from a possibly biased local sample.
data::ResolvedDataset resolve_table(Node &node, const data::DatasetPath &path,
                                    Json &config) {
    data::ResolvedDataset resolved;
    resolved.database = path.database;
    resolved.table = path.table;
    if (path.kind != data::DatasetPathKind::Table) {
        resolved.error =
            "segmentation requires a landed table, not a dataset directory";
        return resolved;
    }
    for (const auto &summary : node.datasets()) {
        if (summary.database != path.database || summary.name != path.table) {
            continue;
        }
        if (summary.degraded) {
            resolved.error = summary.degraded_reason;
            return resolved;
        }
        auto params = segmentation_params(config);
        if (!resolve_roles(summary, config, params, resolved) ||
            !resolve_features(summary, params, resolved) ||
            !resolve_categories(summary, config, params, resolved)) {
            return resolved;
        }
        std::sort(params.excluded_columns.begin(),
                  params.excluded_columns.end());
        params.excluded_columns.erase(
            std::unique(params.excluded_columns.begin(),
                        params.excluded_columns.end()),
            params.excluded_columns.end());
        config["entity_column"] = params.entity_column;
        config["feature_columns"] = params.feature_columns;
        config["categorical_columns"] = params.categorical_columns;
        config["excluded_columns"] = params.excluded_columns;
        resolved.table_id = summary.table_id;
        resolved.rows = summary.rows;
        resolved.contract.entity_column = params.entity_column;
        resolved.contract.label_column = params.label_column;
        resolved.contract.time_column = config.value("time_column", "");
        resolved.contract.as_of = config.value("as_of", 0.0);
        resolved.contract.feature_columns = params.feature_columns;
        return resolved;
    }
    resolved.error = "segmentation: pipeline table '" + path.database + "." +
                     path.table +
                     "' does not exist; land it or edit the connect step";
    return resolved;
}

Json launch_payload(const data::ResolvedDataset &dataset, const Json &config,
                    const SegmentationRunOptions &options) {
    return {{"database", dataset.database},
            {"table", dataset.table},
            {"table_id", dataset.table_id},
            {"rows", dataset.rows},
            {"notes", dataset.notes},
            {"config", config},
            {"project", options.project},
            {"graph", options.graph},
            {"model_name", options.model_name},
            {"run_id", options.run_id},
            {"output_node", options.output_node},
            {"round_tag", options.round_tag}};
}

struct BackgroundSegmentation {
    Node &node;
    std::atomic<bool> finished{false};

    explicit BackgroundSegmentation(Node &owner) : node(owner) {
    }

    ~BackgroundSegmentation() {
        while (!finished.load(std::memory_order_acquire)) {
            node.drain_owner_work();
            std::this_thread::yield();
        }
    }
};

runtime::task<> execute(BackgroundSegmentation *worker,
                        data::ResolvedDataset dataset, Json config,
                        SegmentationRunOptions options,
                        compute::CoordinatedLaunchRequest launch) {
    Node &node = worker->node;
    node.run_on_owner([&] {
        if (launch.participants.size() > 1) {
            node.coordinated_launch()->fan_out(launch);
        }
    });
    const auto result =
        run_segmentation(node, dataset, segmentation_params(config), options);
    node.run_on_owner([&] {
        if (auto run = node.find_run(options.run_id)) {
            if (!result.ok()) {
                run->add_log({util::now_iso8601(), "error", result.error});
            }
            run->set_status(result.ok() ? pipeline::RunStatus::Succeeded
                                        : pipeline::RunStatus::Failed);
        }
    });
    co_return;
}

api::ApiResult submit_run(RunRequest &request,
                          const data::ResolvedDataset &dataset,
                          const Json &config,
                          const std::vector<std::string> &roster) {
    auto &context = *request.context;
    auto &node = context.node;
    SegmentationRunOptions options;
    options.project = request.project->id();
    options.graph = request.graph->id();
    options.model_name = request.input.value("model_name", options.model_name);
    const auto run = node.create_run(options.project, options.graph);
    if (!run) {
        return api::ApiResult::failure("engine_error",
                                       "segmentation could not create its Run");
    }
    options.run_id = run->id();
    options.output_node = node.coordinated_launch()->node_id();
    options.round_tag = "ml.segmentation:" + run->id();
    run->set_status(pipeline::RunStatus::Running);
    run->add_log(
        {util::now_iso8601(), "info",
         "reading pipeline table " + dataset.database + "." + dataset.table});
    compute::CoordinatedLaunchRequest launch;
    launch.id = run->id();
    launch.kind = kLaunchKind;
    launch.initiator = options.output_node;
    launch.participants = roster;
    launch.payload = launch_payload(dataset, config, options);
    auto worker = std::make_shared<BackgroundSegmentation>(node);
    auto *state = worker.get();
    context.background->adopt(worker, [state] {
        return state->finished.load(std::memory_order_acquire);
    });
    runtime::spawn(execute(state, dataset, config, options, std::move(launch)),
                   [state] {
                       state->finished.store(true, std::memory_order_release);
                   });
    return api::ApiResult::success(
        {{"run_id", run->id()},
         {"engine", "segmentation"},
         {"status", "running"},
         {"dataset", data::to_json(dataset.contract)},
         {"dataset_notes", dataset.notes},
         {"poll", "get_run, then get_run_report"}});
}

api::ApiResult run_methodology(RunRequest &request) {
    if (request.context == nullptr || request.project == nullptr ||
        request.graph == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "M-3 requires its engine, project and graph");
    }
    auto &context = *request.context;
    auto &node = context.node;
    if (request.input.contains("holdout_fraction") ||
        request.input.contains("top_k")) {
        return api::ApiResult::failure(
            "invalid_input", "segmentation has no supervised holdout or ranked "
                             "top_k; configure its K range in the graph hints");
    }
    if (context.background == nullptr || node.job_coordinator() == nullptr ||
        node.coordinated_launch() == nullptr) {
        return api::ApiResult::failure(
            "engine_error", "segmentation requires a background action "
                            "owner and attached compute cluster");
    }
    // Embedders may call the registry before their first tick. Adopt the owner
    // before a background worker can enqueue any Node work.
    if (!node.on_owner_thread()) {
        node.tick();
    }
    const auto &graph = request.graph->graph();
    const auto check = pipeline::SegmentationCheck{}.evaluate(graph);
    if (check.status == pipeline::CheckStatus::Fail || !owns(graph)) {
        return api::ApiResult::failure("engine_error",
                                       "blocked by gate: " + check.reason);
    }
    Json config = graph.nodes_of_kind(ir::StepKind::Model)
                      .front()
                      ->attributes.at("segmentation");
    if (const auto overrides = request.input.find("dataset");
        overrides != request.input.end()) {
        for (const auto &[key, value] : overrides->items()) {
            config[key] = value;
        }
    }
    for (const auto &step : graph.nodes()) {
        for (const auto *role :
             {"label", "target", "outcome", "entity_column"}) {
            const auto named = step->attributes.find(role);
            if (named != step->attributes.end() && named->is_string()) {
                if (!config.contains("excluded_columns")) {
                    config["excluded_columns"] = Json::array();
                }
                auto &excluded = config["excluded_columns"];
                if (std::find(excluded.begin(), excluded.end(), *named) ==
                    excluded.end()) {
                    excluded.push_back(*named);
                }
            }
        }
    }
    std::string error;
    if (!validate_segmentation_config(config, error)) {
        return api::ApiResult::failure("invalid_input",
                                       "segmentation: " + error);
    }
    const auto &source =
        graph.nodes_of_kind(ir::StepKind::Connect).front()->attributes;
    const std::string path = request.input.value(
        "dataset_path", source.at("source").get<std::string>() + "." +
                            source.at("table").get<std::string>());
    auto dataset = resolve_table(node, data::parse_dataset_path(path), config);
    if (!dataset.ok()) {
        return api::ApiResult::failure("engine_error", dataset.error);
    }
    data::FeaturePlanOptions feature_options;
    for (const auto& feature : dataset.contract.feature_columns) {
        feature_options.native_entries.push_back({{feature}, {feature},
            "native global numeric scaling or bounded one-hot encoding; fitted columns reported by the model",
            "selected as an eligible clustering input"});
    }
    const auto feature_plan = plan_run_features(request, "M-3", dataset, feature_options);
    if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
        return api::ApiResult::failure("invalid_input", reason);
    }
    if (!validate_segmentation_config(config, error)) {
        return api::ApiResult::failure("invalid_input",
                                       "segmentation: " + error);
    }
    const auto roster = node.job_coordinator()->collectives().participants();
    if (roster.size() > 1 && !request.input.value("coordinate", true)) {
        return api::ApiResult::failure(
            "invalid_input",
            "segmentation requires coordinate=true when the cluster has peers");
    }
    auto submitted = submit_run(request, dataset, config, roster);
    if (submitted.ok) {
        record_run_features(node, submitted.value.at("run_id"), feature_plan);
    }
    return submitted;
}

class LaunchErrors : public nlohmann::json_schema::error_handler {
  public:
    std::string reason;
    void error(const Json::json_pointer &path, const Json &,
               const std::string &message) override {
        if (reason.empty()) {
            reason = path.to_string() + ": " + message;
        }
    }
};

compute::CoordinatedLaunchListener::LaunchOutcome
run_peer(Node &node, compute::CoordinatedLaunchListener &listener,
         const compute::CoordinatedLaunchRequest &launch) {
    static const nlohmann::json_schema::json_validator validator(
        segmentation_launch_schema());
    LaunchErrors errors;
    validator.validate(launch.payload, errors);
    if (!errors.reason.empty()) {
        return {false, "segmentation launch: " + errors.reason};
    }
    const auto &payload = launch.payload;
    if (!compute::participant_rank(launch, listener.node_id()) ||
        !node.job_coordinator() ||
        launch.participants !=
            node.job_coordinator()->collectives().participants() ||
        payload.at("output_node") != launch.initiator ||
        payload.at("run_id") != launch.id) {
        return {false, "segmentation launch roster or writer no longer matches "
                       "the cluster"};
    }
    const auto &config = payload.at("config");
    std::string error;
    if (!validate_segmentation_config(config, error)) {
        return {false, "segmentation launch: " + error};
    }
    data::ResolvedDataset dataset;
    dataset.database = payload.at("database");
    dataset.table = payload.at("table");
    dataset.table_id = payload.at("table_id");
    dataset.rows = payload.at("rows");
    dataset.notes = payload.at("notes").get<std::vector<std::string>>();
    dataset.contract.entity_column = config.at("entity_column");
    dataset.contract.label_column = config.value("label_column", "");
    dataset.contract.time_column = config.value("time_column", "");
    dataset.contract.as_of = config.value("as_of", 0.0);
    dataset.contract.feature_columns =
        config.at("feature_columns").get<std::vector<std::string>>();
    SegmentationRunOptions options;
    options.project = payload.at("project");
    options.graph = payload.at("graph");
    options.model_name = payload.at("model_name");
    options.run_id = payload.at("run_id");
    options.output_node = payload.at("output_node");
    options.round_tag = payload.at("round_tag");
    // The existing coordinated-launch listener invokes peer bodies on their
    // owning loop, never on the network thread that services collectives.
    const auto result =
        run_segmentation(node, dataset, segmentation_params(config), options);
    return {result.ok(),
            result.ok() ? "segmentation peer completed" : result.error};
}
} // namespace

Methodology make_m3_methodology() {
    Methodology methodology;
    methodology.id = "M-3";
    methodology.name = "Segmentation / clustering";
    methodology.family = "Identity & structure";
    methodology.model_kinds = {"segmentation", "clustering", "cluster"};
    methodology.solutions = {"S-9"};
    methodology.keywords = {"segment", "cluster", "group", "persona", "cohort"};
    methodology.inputs = {
        {"entity", true, "integer entity key kept out of features"},
        {"feature", true, "numeric attributes and bounded categorical roles"},
        {"label", false, "label/outcome column to exclude, never a target"},
        {"time", false, "explicit numeric time cutoff; never inferred"}};
    methodology.outputs = {"model", "segment assignments", "segment profiles",
                           "run report"};
    methodology.metrics = {"chosen k",       "silhouette",
                           "Davies-Bouldin", "stability (ARI)",
                           "inertia per k",  "segment size/share"};
    methodology.issue = "https://github.com/oramasearch/zetta/issues/1042";
    methodology.runnable = true;
    methodology.build_shape = build_shape;
    methodology.owns = owns;
    methodology.run = run_methodology;
    return methodology;
}

const Json &segmentation_launch_schema() {
    static const Json schema = [] {
        Json result = {{"$schema", "http://json-schema.org/draft-07/schema#"},
                       {"title", "zetta.SegmentationLaunch"},
                       {"type", "object"},
                       {"additionalProperties", false},
                       {"required",
                        {"database", "table", "table_id", "rows", "notes",
                         "config", "project", "graph", "model_name", "run_id",
                         "output_node", "round_tag"}}};
        for (const auto *key :
             {"database", "table", "table_id", "project", "graph", "model_name",
              "run_id", "output_node", "round_tag"}) {
            result["properties"][key] = {{"type", "string"}, {"minLength", 1}};
        }
        result["properties"]["rows"] = {{"type", "integer"}, {"minimum", 0}};
        result["properties"]["notes"] = {{"type", "array"},
                                         {"items", {{"type", "string"}}}};
        Json config = segmentation_config_schema();
        config["required"].push_back("entity_column");
        config["required"].push_back("feature_columns");
        config["properties"]["entity_column"]["minLength"] = 1;
        config["properties"]["feature_columns"]["minItems"] = 1;
        result["properties"]["config"] = std::move(config);
        return result;
    }();
    return schema;
}

void install_segmentation_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node) {
    listener.set_runner(kLaunchKind, [&node, &listener](const auto &launch) {
        return run_peer(node, listener, launch);
    });
}

Json segmentation_run_report(Node &node, const pipeline::Run &run,
                             const Model &model, std::size_t limit) {
    const Json &schema = model.feature_schema();
    Json report = {
        {"run",
         {{"id", run.id()},
          {"project", run.project()},
          {"graph", run.graph()},
          {"status", to_string(run.status())},
          {"finished", run.updated()}}},
        {"methodology", "M-3"},
        {"model",
         {{"id", model.id()},
          {"name", model.name()},
          {"kind", model.kind()},
          {"metrics", model.metrics()},
          {"chosen_k_reason", schema.value("chosen_k_reason", Json{})},
          {"k_candidates", schema.value("k_candidates", Json::array())}}},
        {"dataset", schema.value("dataset", Json::object())},
        {"output_tables", Json::array()},
        {"segments", Json::array()},
        {"missing", Json::array()},
        {"caveats",
         {"Segment differences and generated labels are descriptive, not "
          "causal.",
          "Silhouette uses a deterministically preprocessed sample; these are "
          "clustering diagnostics, not predictive accuracy."}}};
    report["dataset"]["features"] = schema.value("features", Json::array());
    report["dataset"]["excluded_columns"] =
        schema.value("excluded_columns", Json::array());
    for (const auto *key : {"read_rows", "clustered_rows", "excluded_rows"}) {
        report["dataset"][key] = model.metrics().value(key, Json{});
    }
    for (const auto &output : run.outputs()) {
        if (output.kind != "dataset") {
            continue;
        }
        const auto path = data::parse_dataset_path(output.ref);
        const auto database = node.find_database(path.database);
        const auto table =
            database ? database->find_table(path.table) : nullptr;
        report["output_tables"].push_back(
            {{"name", output.name},
             {"ref", output.ref},
             {"available", table != nullptr},
             {"rows", table ? Json(table->row_count()) : Json{}}});
        if (!table) {
            report["missing"].push_back(
                {{"field", output.name},
                 {"why", "output table is not available on this member"}});
            continue;
        }
        if (output.name != "segment profiles") {
            continue;
        }
        for (std::size_t index = 0;
             index < std::min({limit, table->row_count(), std::size_t{64}});
             ++index) {
            const auto row = table->store().at(index);
            if (row) {
                report["segments"].push_back(*row);
            } else {
                report["missing"].push_back(
                    {{"field", "segment profile"}, {"why", row.error()}});
            }
        }
    }
    return report;
}
} // namespace zetta::ml
