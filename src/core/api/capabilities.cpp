#include "zetta/api/capabilities.hpp"

#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "zetta/api/background.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/feedback.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/n8n_compile.hpp"
#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/project_input.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/query/service.hpp"
#include "zetta/testkit/async_case_run.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/usecases/churn/churn_dataset.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/version.hpp"

namespace zetta::api {

namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json empty_object_schema() {
    return nlohmann::json{{"type", "object"}, {"additionalProperties", false}};
}

// --- server_info -------------------------------------------------------------

const nlohmann::json& server_info_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

ApiResult handle_server_info(const nlohmann::json&, ApiContext& context) {
    nlohmann::json output;
    output["version"] = zetta::kVersion;
    output["node_id"] = context.node.node_id();
    output["cluster_members"] = context.node.cluster_members();
    output["databases"] = context.node.database_count();
    output["projects"] = context.node.project_count();
    output["runs"] = context.node.runs().size();
    output["models"] = context.node.models().size();
    output["jobs"] = context.node.jobs().size();
    return ApiResult::success(std::move(output));
}

// --- sql ---------------------------------------------------------------------

const nlohmann::json& sql_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"statement"}},
        {"additionalProperties", false},
        {"properties",
         {{"statement",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "one SQL statement (Postgres subset; the system.* "
                            "views are all readable here)"}}},
          {"database",
           {{"type", "string"},
            {"description",
             "default database for unqualified table names (optional)"}}}}}};
    return schema;
}

ApiResult handle_sql(const nlohmann::json& input, ApiContext& context) {
    zetta::query::QueryService service(context.node, context.cluster);
    const std::string database = input.value("database", "");
    auto outcome = service.run(input.at("statement").get<std::string>(), database);
    if (!outcome.ok()) {
        return ApiResult::failure("engine_error", outcome.error);
    }
    nlohmann::json output;
    output["messages"] = outcome.messages;
    output["columns"] = outcome.columns;
    output["rows"] = outcome.rows;
    return ApiResult::success(std::move(output));
}

// --- search --------------------------------------------------------------------

const nlohmann::json& search_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"target", "query"}},
        {"additionalProperties", false},
        {"properties",
         {{"target",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "an index id, or a table name with a Search index over it"}}},
          {"query",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the free-text query"}}},
          {"field",
           {{"type", "string"},
            {"description",
             "restrict matching to one indexed field (optional)"}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 1000},
            {"description", "cap on the global ranking (default 10)"}}}}}};
    return schema;
}

// The thin caller of THE search entry point (Node::search, #641) — the same
// path the GUI drives, so MCP/REST and the GUI can never diverge.
ApiResult handle_search(const nlohmann::json& input, ApiContext& context) {
    zetta::index::SearchRequest request;
    request.target = input.at("target").get<std::string>();
    request.query = input.at("query").get<std::string>();
    request.field = input.value("field", std::string{});
    request.top_k = input.value("top_k", 10);

    const zetta::index::SearchResult result =
        context.node.search(request, context.recorder, &context.trace);
    if (!result.ok()) {
        return ApiResult::failure("engine_error", result.status.message);
    }

    nlohmann::json hits = nlohmann::json::array();
    for (const zetta::index::SearchHit& hit : result.hits) {
        hits.push_back({{"id", hit.row_id}, {"score", hit.score}});
    }
    // Per-node provenance: which node answered, and at what cost (#641).
    nlohmann::json nodes = nlohmann::json::array();
    for (const zetta::index::NodeContribution& contribution : result.nodes) {
        nodes.push_back({{"node_id", contribution.node_id},
                         {"local", contribution.local},
                         {"hits", contribution.hits},
                         {"sub_indexes", contribution.sub_indexes},
                         {"latency_ms", contribution.latency_ms},
                         {"wire_bytes", contribution.wire_bytes}});
    }
    nlohmann::json output;
    output["index_id"] = result.index_id;
    output["table"] = result.spec.table;
    output["path"] = zetta::index::to_string(result.path);
    output["hits"] = std::move(hits);
    output["nodes"] = std::move(nodes);
    output["wire_bytes_moved"] = result.wire_bytes_moved;
    output["duration_ms"] = result.duration_ms;
    return ApiResult::success(std::move(output));
}

// --- list_projects -----------------------------------------------------------

const nlohmann::json& list_projects_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

ApiResult handle_list_projects(const nlohmann::json&, ApiContext& context) {
    nlohmann::json output;
    output["projects"] = context.node.project_names();
    return ApiResult::success(std::move(output));
}

// --- typed detail reads (#616 read parity) -----------------------------------
//
// Each get_* returns the entity's already-validated value JSON — the exact
// shape `metadata()` serializes (= the CRDT value the boundary schema
// validated) — with the id added, so MCP, the on-disk projection, and the
// system.* SQL views can never drift (RULE-7: same live engine source).

nlohmann::json id_input_schema(std::string_view field,
                               std::string_view description) {
    return nlohmann::json{
        {"type", "object"},
        {"required", {field}},
        {"additionalProperties", false},
        {"properties",
         {{field, {{"type", "string"},
                   {"minLength", 1},
                   {"description", description}}}}}};
}

const nlohmann::json& get_project_schema() {
    static const nlohmann::json schema =
        id_input_schema("name", "the project's name, or its uuid");
    return schema;
}

ApiResult handle_get_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    auto project = context.node.find_project(name);
    if (!project) {
        // Entity refs elsewhere (runs, jobs, models) carry the project *uuid*,
        // so accept it as the handle too.
        for (const std::string& candidate : context.node.project_names()) {
            auto by_name = context.node.find_project(candidate);
            if (by_name && by_name->id() == name) {
                project = std::move(by_name);
                break;
            }
        }
    }
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    nlohmann::json output = project->metadata();
    output["id"] = project->id();
    nlohmann::json inputs = nlohmann::json::array();
    for (const auto& project_input : project->inputs()) {
        nlohmann::json entry = project_input->metadata();
        entry["id"] = project_input->id();
        inputs.push_back(std::move(entry));
    }
    output["inputs"] = std::move(inputs);
    nlohmann::json graphs = nlohmann::json::array();
    for (const auto& graph : project->graphs()) {
        graphs.push_back({{"id", graph->id()}, {"name", graph->name()}});
    }
    output["graphs"] = std::move(graphs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_run_schema() {
    static const nlohmann::json schema =
        id_input_schema("run_id", "the run's uuid (system.runs.run_id)");
    return schema;
}

ApiResult handle_get_run(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("run_id").get<std::string>();
    const auto run = context.node.find_run(id);
    if (!run) {
        return ApiResult::failure("not_found", "no run with id '" + id + "'");
    }
    nlohmann::json output = run->metadata();
    output["id"] = run->id();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_model_schema() {
    static const nlohmann::json schema =
        id_input_schema("model_id", "the model's uuid (system.models.model_id)");
    return schema;
}

ApiResult handle_get_model(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) {
        return ApiResult::failure("not_found", "no model with id '" + id + "'");
    }
    nlohmann::json output = model->metadata();
    output["id"] = model->id();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_job_schema() {
    static const nlohmann::json schema =
        id_input_schema("job_id", "the compute job's uuid (system.jobs.job_id)");
    return schema;
}

ApiResult handle_get_job(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("job_id").get<std::string>();
    const auto job = context.node.find_job(id);
    if (!job) {
        return ApiResult::failure("not_found", "no job with id '" + id + "'");
    }
    nlohmann::json output = job->metadata();
    output["id"] = job->id();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_index_schema() {
    static const nlohmann::json schema =
        id_input_schema("index_id", "the index's uuid (system.indexes.index_id)");
    return schema;
}

ApiResult handle_get_index(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("index_id").get<std::string>();
    const auto index = context.node.find_index(id);
    if (!index) {
        return ApiResult::failure("not_found", "no index with id '" + id + "'");
    }
    nlohmann::json output = index->metadata();
    output["id"] = index->id();
    return ApiResult::success(std::move(output));
}

// --- get_dataset / list_datasets ----------------------------------------------

// A DatasetSummary as JSON — the full computed shape, with the per-partition
// placement kept structured (the SQL view flattens it to strings for tabular
// reading; the tool keeps the arrays an agent can walk).
nlohmann::json dataset_summary_json(const zetta::data::DatasetSummary& dataset) {
    nlohmann::json partitions = nlohmann::json::array();
    for (const zetta::data::DatasetPlacement& placement : dataset.partitions) {
        partitions.push_back({{"partition_id", placement.partition_id},
                              {"blob_ref", placement.blob_ref},
                              {"row_start", placement.row_start},
                              {"row_end", placement.row_end},
                              {"owner_nodes", placement.owner_nodes}});
    }
    return nlohmann::json{{"name", dataset.name},
                          {"database", dataset.database},
                          {"table_id", dataset.table_id},
                          {"format", dataset.format},
                          {"replication", dataset.replication},
                          {"rows", dataset.rows},
                          {"size_bytes", dataset.size_bytes},
                          {"columns", dataset.columns},
                          {"partitions", std::move(partitions)}};
}

const nlohmann::json& get_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"dataset"}},
        {"additionalProperties", false},
        {"properties",
         {{"dataset",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the dataset name (= the table name)"}}},
          {"database",
           {{"type", "string"},
            {"description", "the owning database — required only when the "
                            "same dataset name exists in several databases"}}}}}};
    return schema;
}

ApiResult handle_get_dataset(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("dataset").get<std::string>();
    const std::string database = input.value("database", "");
    std::vector<zetta::data::DatasetSummary> matches;
    for (zetta::data::DatasetSummary& dataset : context.node.datasets()) {
        if (dataset.name != name) continue;
        if (!database.empty() && dataset.database != database) continue;
        matches.push_back(std::move(dataset));
    }
    if (matches.empty()) {
        return ApiResult::failure(
            "not_found", "no dataset named '" + name + "'" +
                             (database.empty() ? "" : " in database '" + database + "'"));
    }
    if (matches.size() > 1) {
        std::string databases;
        for (const zetta::data::DatasetSummary& match : matches) {
            if (!databases.empty()) databases += ", ";
            databases += match.database;
        }
        return ApiResult::failure(
            "engine_error", "dataset '" + name + "' exists in several databases (" +
                                databases + ") — pass \"database\" to pick one");
    }
    return ApiResult::success(dataset_summary_json(matches.front()));
}

// --- list_* siblings (#616) ----------------------------------------------------
//
// Compact structured listings over the same live Node registries the system.*
// SQL views compute from — for the places sql's flattened tabular rows aren't
// ergonomic for an agent (structured arrays, stable ids to feed the get_* reads).

const nlohmann::json& list_runs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

ApiResult handle_list_runs(const nlohmann::json&, ApiContext& context) {
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& run : context.node.runs()) {
        runs.push_back({{"id", run->id()},
                        {"project", run->project()},
                        {"graph", run->graph()},
                        {"deployment", run->deployment()},
                        {"status", std::string(to_string(run->status()))},
                        {"created", run->created()},
                        {"updated", run->updated()},
                        {"log_count", run->logs().size()},
                        {"output_count", run->outputs().size()}});
    }
    nlohmann::json output;
    output["runs"] = std::move(runs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_models_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

ApiResult handle_list_models(const nlohmann::json&, ApiContext& context) {
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : context.node.models()) {
        models.push_back({{"id", model->id()},
                          {"name", model->name()},
                          {"kind", model->kind()},
                          {"project", model->project()},
                          {"run", model->run()},
                          {"created", model->created()},
                          {"has_weights", model->has_weights()}});
    }
    nlohmann::json output;
    output["models"] = std::move(models);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_jobs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

ApiResult handle_list_jobs(const nlohmann::json&, ApiContext& context) {
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& job : context.node.jobs()) {
        jobs.push_back({{"id", job->id()},
                        {"kind", std::string(to_string(job->kind()))},
                        {"status", std::string(to_string(job->status()))},
                        {"project", job->project()},
                        {"graph", job->graph()},
                        {"run", job->run()},
                        {"created", job->created()},
                        {"updated", job->updated()},
                        {"detail", job->detail()}});
    }
    nlohmann::json output;
    output["jobs"] = std::move(jobs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_indexes_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

// The index registry as a tool (#642) — read from the same live source
// (Node::indexes()) the system.indexes SQL view computes from (RULE-7), so an
// agent can discover what `search` can target before searching.
ApiResult handle_list_indexes(const nlohmann::json&, ApiContext& context) {
    nlohmann::json indexes = nlohmann::json::array();
    for (const auto& index : context.node.indexes()) {
        indexes.push_back({{"id", index->id()},
                           {"table", index->table()},
                           {"kind", std::string(index->kind_name())},
                           {"backend", index->backend()},
                           {"fields", index->fields()},
                           {"created", index->created()}});
    }
    nlohmann::json output;
    output["indexes"] = std::move(indexes);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_datasets_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

ApiResult handle_list_datasets(const nlohmann::json&, ApiContext& context) {
    nlohmann::json datasets = nlohmann::json::array();
    for (const zetta::data::DatasetSummary& dataset : context.node.datasets()) {
        datasets.push_back(dataset_summary_json(dataset));
    }
    nlohmann::json output;
    output["datasets"] = std::move(datasets);
    return ApiResult::success(std::move(output));
}

// --- create_project ----------------------------------------------------------

const nlohmann::json& create_project_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1}}},
          {"owner",
           {{"type", "string"},
            {"description", "owner key id recorded on the project (optional)"}}}}}};
    return schema;
}

ApiResult handle_create_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::string owner =
        input.value("owner", context.acting_key.empty() ? std::string("local")
                                                        : context.acting_key);
    auto project = context.node.create_project(name, owner);
    if (!project) {
        return ApiResult::failure(
            "engine_error",
            "create_project rejected '" + name + "' (empty name or name clash)");
    }
    nlohmann::json output;
    output["name"] = name;
    output["owner"] = owner;
    return ApiResult::success(std::move(output));
}

// --- list_traces / get_trace ---------------------------------------------

const nlohmann::json& list_traces_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"limit",
           {{"type", "integer"}, {"minimum", 1}, {"maximum", 500},
            {"description", "most recent spans to return (default 50)"}}}}}};
    return schema;
}

ApiResult handle_list_traces(const nlohmann::json& input, ApiContext& context) {
    if (!context.recorder) {
        return ApiResult::failure("engine_error",
                                  "no span recorder is wired on this node");
    }
    const std::size_t limit = input.value("limit", 50);
    nlohmann::json spans = nlohmann::json::array();
    for (const auto& span : context.recorder->recent(limit)) {
        spans.push_back(zetta::observability::to_json(span));
    }
    nlohmann::json output;
    output["spans"] = std::move(spans);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_trace_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"trace_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"trace_id",
           {{"type", "string"}, {"minLength", 32}, {"maxLength", 32}}}}}};
    return schema;
}

ApiResult handle_get_trace(const nlohmann::json& input, ApiContext& context) {
    if (!context.recorder) {
        return ApiResult::failure("engine_error",
                                  "no span recorder is wired on this node");
    }
    const auto trace_spans =
        context.recorder->trace(input.at("trace_id").get<std::string>());
    nlohmann::json spans = nlohmann::json::array();
    for (const auto& span : trace_spans) {
        spans.push_back(zetta::observability::to_json(span));
    }
    nlohmann::json output;
    output["trace_id"] = input.at("trace_id");
    output["spans"] = std::move(spans);
    return ApiResult::success(std::move(output));
}

// --- submit_feedback -----------------------------------------------------------

const nlohmann::json& submit_feedback_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"description"}},
        {"additionalProperties", false},
        {"properties",
         {{"description",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "what happened / what you wish for, verbatim"}}},
          {"trace_ids",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "traces to attach (optional; default = the most "
                            "recent traces on this node)"}}},
          {"export_path",
           {{"type", "string"},
            {"description",
             "local file path to ALSO write the bundle to — the explicit "
             "user export; nothing leaves the cluster on its own "
             "(optional)"}}}}}};
    return schema;
}

// The database that holds feedback bundles as large objects (replicated
// metadata + peer-transferred body, like every large object).
constexpr std::string_view kFeedbackDatabase = "feedback";

ApiResult handle_submit_feedback(const nlohmann::json& input,
                                 ApiContext& context) {
    zetta::observability::FeedbackBundleOptions options;
    options.description = input.at("description").get<std::string>();
    if (const auto trace_ids = input.find("trace_ids");
        trace_ids != input.end()) {
        options.trace_ids = trace_ids->get<std::vector<std::string>>();
    }
    options.actor =
        context.acting_key.empty() ? std::string("local") : context.acting_key;

    std::string reason;
    const auto bundle =
        zetta::observability::build_feedback_bundle(context.node, options, &reason);
    if (!bundle) {
        return ApiResult::failure("engine_error", reason);
    }

    auto database = context.node.find_database(std::string(kFeedbackDatabase));
    if (!database) {
        database = context.node.create_database(std::string(kFeedbackDatabase));
    }
    if (!database) {
        return ApiResult::failure(
            "engine_error", "submit_feedback: could not open the '" +
                                std::string(kFeedbackDatabase) + "' database");
    }
    const std::string name =
        "feedback-" + bundle->at("submitted_at").get<std::string>() + ".json";
    const auto stored =
        database->large_objects().import_bytes(name, bundle->dump(2));
    if (!stored) {
        return ApiResult::failure(
            "engine_error", "submit_feedback: could not store the bundle");
    }

    nlohmann::json output;
    output["feedback_id"] = stored->id;
    output["name"] = stored->name;
    output["database"] = std::string(kFeedbackDatabase);
    output["size"] = stored->size;
    output["sha256"] = stored->sha256;
    output["traces_attached"] = bundle->at("traces").size();
    output["errors_attached"] = bundle->at("recent_errors").size();
    output["crashes_attached"] = bundle->at("crashes").size();

    // The explicit export (and the ONLY way bytes leave the store): the user
    // asked for a copy at a path of their choosing.
    if (const auto export_path = input.find("export_path");
        export_path != input.end()) {
        const std::string destination = export_path->get<std::string>();
        if (!database->large_objects().export_file(stored->id, destination)) {
            return ApiResult::failure(
                "engine_error",
                "submit_feedback: bundle stored as " + stored->id +
                    " but the export to '" + destination + "' failed");
        }
        output["exported_to"] = destination;
    }
    return ApiResult::success(std::move(output));
}

// --- governed action parity (#617) ---------------------------------------------
//
// The mutating half of the product loop, each a thin caller of the SAME engine
// entry point the GUI drives (RULE-1/RULE-3 cannot be bypassed by construction:
// authorize + audit happen in Registry::call, the gate inside the engine seams).
// Long work is `kind: job`: submit, return the engine's durable Run id, poll
// progress through the read surface (get_run / system.runs) — no request-scoped
// blocking, no new job infrastructure. Errors carry the engine's path-qualified
// reason verbatim. See docs/api/governed-actions.md.

std::string actor_of(const ApiContext& context) {
    return context.acting_key.empty() ? std::string("local") : context.acting_key;
}

// Resolve a project by name or uuid (the get_project convention: entity refs
// elsewhere carry the project uuid, so accept it as the handle too).
std::shared_ptr<zetta::pipeline::Project> resolve_project(
    ApiContext& context, const std::string& name) {
    auto project = context.node.find_project(name);
    if (project) return project;
    for (const std::string& candidate : context.node.project_names()) {
        auto by_name = context.node.find_project(candidate);
        if (by_name && by_name->id() == name) return by_name;
    }
    return nullptr;
}

// Resolve a stored graph on a project by name or uuid.
std::shared_ptr<zetta::pipeline::ProjectGraph> resolve_graph(
    const zetta::pipeline::Project& project, const std::string& name) {
    auto graph = project.find_graph(name);
    if (graph) return graph;
    return project.find_graph_by_id(name);
}

nlohmann::json input_json(const zetta::pipeline::ProjectInput& input) {
    return nlohmann::json{{"input_id", input.id()},
                          {"kind", std::string(to_string(input.kind()))},
                          {"filename", input.filename()},
                          {"content_hash", input.content_hash()},
                          {"size", input.size()}};
}

nlohmann::json graph_json(const zetta::pipeline::ProjectGraph& graph) {
    return nlohmann::json{{"graph_id", graph.id()},
                          {"name", graph.name()},
                          {"steps", graph.graph().node_count()},
                          {"edges", graph.graph().edge_count()}};
}

// The first failing check's name + reason — the line the Deploy panel shows
// when the gate blocks (RULE-1), surfaced verbatim over MCP.
std::string first_gate_failure(const zetta::pipeline::ValidationReport& report) {
    for (const zetta::pipeline::CheckResult& check : report.checks) {
        if (check.status == zetta::pipeline::CheckStatus::Fail) {
            return check.name + ": " + check.reason;
        }
    }
    return "gate blocked deploy";
}

// --- capture_intent ------------------------------------------------------------

const nlohmann::json& capture_intent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "text"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"text",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the natural-language intent, verbatim"}}}}}};
    return schema;
}

ApiResult handle_capture_intent(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    auto captured = project->capture_intent(input.at("text").get<std::string>(),
                                            actor_of(context));
    if (!captured) {
        return ApiResult::failure(
            "engine_error",
            "capture_intent rejected the text (empty/whitespace-only intent, "
            "or the input folder could not be created)");
    }
    nlohmann::json output = input_json(*captured);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- import_n8n ------------------------------------------------------------------

const nlohmann::json& import_n8n_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "content"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"content",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the n8n workflow export JSON, verbatim (stored "
                            "byte-for-byte after schema validation)"}}},
          {"filename",
           {{"type", "string"},
            {"description",
             "recorded as the artifact's filename (default workflow.json)"}}}}}};
    return schema;
}

ApiResult handle_import_n8n(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const zetta::pipeline::N8nImportResult imported =
        zetta::pipeline::import_n8n_workflow(
            *project, input.at("content").get<std::string>(),
            input.value("filename", "workflow.json"), actor_of(context));
    if (!imported.ok()) {
        return ApiResult::failure("engine_error", imported.error);
    }
    nlohmann::json output = input_json(*imported.input);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- compile_intent / compile_n8n / compile_python -------------------------------
//
// Three thin compilers onto the same destination: a named, editable, replicated
// ProjectGraph (Project::create_graph — which audits the graph change). Each
// mirrors its engine entry point: Project::compile_intent_to_graph (#311),
// compile_n8n_workflow (#308), input::PythonFrontend (#520/#522).

const nlohmann::json& compile_intent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "input_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"input_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the captured intent input's uuid (capture_intent)"}}},
          {"name",
           {{"type", "string"},
            {"description",
             "the stored graph's name (default '<shape> skeleton')"}}}}}};
    return schema;
}

ApiResult handle_compile_intent(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string input_id = input.at("input_id").get<std::string>();
    auto intent_input = project->find_input(input_id);
    if (!intent_input) {
        return ApiResult::failure("not_found", "no input with id '" + input_id +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    if (intent_input->kind() != zetta::pipeline::ProjectInputKind::Intent) {
        return ApiResult::failure(
            "engine_error", "input '" + input_id +
                                "' is an artifact, not a captured intent — "
                                "compile it with compile_n8n");
    }
    auto graph =
        project->compile_intent_to_graph(input_id, input.value("name", ""));
    if (!graph) {
        return ApiResult::failure(
            "engine_error",
            "compile_intent could not store a graph from input '" + input_id +
                "' (payload body not present locally, empty intent, or the "
                "graph name collides with an existing graph)");
    }
    nlohmann::json output = graph_json(*graph);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& compile_n8n_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "input_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"input_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored n8n artifact input's uuid (import_n8n)"}}},
          {"name",
           {{"type", "string"},
            {"description", "the stored graph's name (default 'n8n workflow')"}}}}}};
    return schema;
}

ApiResult handle_compile_n8n(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string input_id = input.at("input_id").get<std::string>();
    auto artifact = project->find_input(input_id);
    if (!artifact) {
        return ApiResult::failure("not_found", "no input with id '" + input_id +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    std::string reason;
    auto compiled = zetta::pipeline::compile_n8n_workflow(*artifact, &reason);
    if (!compiled) {
        return ApiResult::failure("engine_error", reason);
    }
    auto graph = project->create_graph(input.value("name", "n8n workflow"),
                                       std::move(*compiled));
    if (!graph) {
        return ApiResult::failure(
            "engine_error",
            "the graph name collides with an existing graph on project '" +
                project->name() + "' (or is empty)");
    }
    nlohmann::json output = graph_json(*graph);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& compile_python_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "source"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"source",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the simulated-Python pipeline source (zetta lowers "
                            "it to the IR; it never runs Python)"}}},
          {"name",
           {{"type", "string"},
            {"description",
             "the stored graph's name (default 'python program')"}}}}}};
    return schema;
}

ApiResult handle_compile_python(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const zetta::input::FrontendResult lowered =
        zetta::input::PythonFrontend{}.compile(
            input.at("source").get<std::string>());
    if (!lowered.ok()) {
        return ApiResult::failure("engine_error", lowered.error);
    }
    auto graph = project->create_graph(input.value("name", "python program"),
                                       lowered.graph);
    if (!graph) {
        return ApiResult::failure(
            "engine_error",
            "the graph name collides with an existing graph on project '" +
                project->name() + "' (or is empty)");
    }
    nlohmann::json output = graph_json(*graph);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- gate ------------------------------------------------------------------------

const nlohmann::json& gate_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"graph",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored graph's name, or its uuid"}}}}}};
    return schema;
}

ApiResult handle_gate(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    // A blocked graph is a successful gate *evaluation* — the report carries
    // the verdict; deploy/run are where a blocked graph becomes an error.
    nlohmann::json output = report;  // ADL to_json: checks + verdict + block_deploy
    output["project"] = project->name();
    output["graph"] = graph->name();
    return ApiResult::success(std::move(output));
}

// --- deploy ----------------------------------------------------------------------

const nlohmann::json& deploy_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"graph",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored graph's name, or its uuid"}}},
          {"target",
           {{"type", "string"},
            {"enum", {"local", "kubernetes", "vpc", "cloud"}},
            {"description", "the placement dial (default local; only local is "
                            "functional in R1)"}}}}}};
    return schema;
}

ApiResult handle_deploy(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name +
                                                   "' on project '" +
                                                   project->name() + "'");
    }
    const auto kind = zetta::pipeline::deployment_kind_from_name(
        input.value("target", "local"));
    if (!kind) {
        return ApiResult::failure("invalid_input", "unknown deployment target",
                                  "/target");
    }
    // The gate report first, for the failing check's reason; the engine's own
    // deploy_graph re-runs the gate and audits BOTH outcomes (RULE-1 lives in
    // the engine seam — this handler cannot bypass it by construction).
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    auto deployment = context.node.deploy_graph(
        graph->graph(), zetta::pipeline::DeploymentTarget{*kind, {}, {}},
        graph->name());
    if (!deployment) {
        if (report.block_deploy) {
            return ApiResult::failure("engine_error",
                                      "gate blocked deploy: " +
                                          first_gate_failure(report));
        }
        return ApiResult::failure(
            "engine_error",
            *kind == zetta::pipeline::DeploymentKind::Local
                ? "deploy_graph refused the graph (empty graph)"
                : "deploy_graph refused the target ('" +
                      zetta::pipeline::to_string(*kind) +
                      "' is planned but not functional in R1 — use 'local')");
    }
    nlohmann::json output;
    output["deployment_id"] = deployment->id();
    output["graph"] = graph->name();
    output["target"] = zetta::pipeline::to_string(*kind);
    output["gate"] = report;
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- run (kind: job — THE canonical runner, #637) ---------------------------------

const nlohmann::json& run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"graph",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the stored graph's name, or its uuid"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "a churn dataset directory on the node's filesystem (default: the "
             "built-in demo dataset)"}}},
          {"model_name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model's name (default churn_model)"}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "at-risk list / playbook cap (0 = all)"}}},
          {"coordinate",
           {{"type", "boolean"},
            {"description", "fan the run out to the live membership (default "
                            "true; false is only correct on a single-member "
                            "cluster)"}}}}}};
    return schema;
}

ApiResult handle_run(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no graph named '" + graph_name +
                                                   "' on project '" +
                                                   project->name() + "'");
    }

    // The gate, where the GUI's run paths have it (the governed console gates
    // every run; the coordinated launch re-checks it on every member): RULE-1
    // holds over MCP exactly as everywhere else.
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    if (report.block_deploy) {
        return ApiResult::failure(
            "engine_error", "blocked by gate: " + first_gate_failure(report));
    }

    // Non-churn graphs run structurally (DefaultStepExecutor) — quick, within
    // the call, same as the governed console's fallback path.
    if (!zetta::usecases::churn::is_churn_graph(graph->graph())) {
        const zetta::pipeline::ExecutionResult executed =
            context.node.run_graph(graph->graph(), project->id(), graph->id());
        if (!executed.run) {
            return ApiResult::failure("engine_error",
                                      "the run record could not be created");
        }
        nlohmann::json output;
        output["run_id"] = executed.run->id();
        output["engine"] = "structural";
        output["status"] = std::string(to_string(executed.run->status()));
        if (!executed.ok) {
            output["failed_step"] = executed.failed_step;
            output["error"] = executed.error;
        }
        return ApiResult::success(std::move(output));
    }

    // Churn-shaped: THE canonical runner over the node's live coordinator
    // (#637/#580, SPMD across the membership via the coordinated launch #601),
    // driven off-thread (AsyncChurnRun, #624). kind: job — return the durable
    // Run id as soon as the record exists; progress/outcome ride system.runs /
    // system.jobs / get_run.
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "run is a job capability and this embedding wired no background "
            "action owner");
    }
    zetta::usecases::churn::ChurnDataset dataset;
    if (const auto dataset_path = input.find("dataset_path");
        dataset_path != input.end()) {
        auto loaded = zetta::usecases::churn::load_churn_dataset(
            dataset_path->get<std::string>());
        if (!loaded.ok()) {
            return ApiResult::failure("engine_error", loaded.error);
        }
        dataset = std::move(loaded.dataset);
    } else {
        dataset = zetta::usecases::churn::demo_churn_dataset();
    }
    zetta::usecases::churn::ChurnSolutionOptions options;
    options.project = project->id();
    options.graph = graph->id();
    options.model_name = input.value("model_name", options.model_name);
    options.top_k = input.value("top_k", options.top_k);
    options.coordinate = input.value("coordinate", true);

    std::set<std::string> prior;
    for (const auto& existing : context.node.runs()) {
        if (existing) prior.insert(existing->id());
    }
    auto handle = zetta::usecases::churn::AsyncChurnRun::start(
        context.node, graph->graph(), std::move(dataset), options);
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });

    // The Run record is created by the worker through the owner gate; this
    // handler IS on the owner thread (the bridge contract), so pump the gate
    // until the durable handle exists — milliseconds for the demo dataset,
    // bounded for a large custom one. An early-failing run (no live
    // coordinator, empty dataset) surfaces its engine reason instead.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    std::string run_id;
    while (std::chrono::steady_clock::now() < deadline) {
        context.node.drain_owner_work();
        for (const auto& candidate : context.node.runs()) {
            if (!candidate || prior.contains(candidate->id())) continue;
            if (candidate->project() != project->id()) continue;
            if (candidate->graph() != graph->id()) continue;
            run_id = candidate->id();
            break;
        }
        if (!run_id.empty()) break;
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
            return ApiResult::failure("engine_error",
                                      handle->result().execution.error);
        }
        return ApiResult::failure(
            "engine_error",
            "the run was submitted but its Run record did not appear in time "
            "— poll list_runs for a run labelled project '" + project->id() +
                "'");
    }
    nlohmann::json output;
    output["run_id"] = run_id;
    output["engine"] = "churn";
    output["status"] = "running";
    output["poll"] = "get_run / system.runs for status+logs; system.jobs for "
                     "the featurize/train/explain/score jobs";
    return ApiResult::success(std::move(output));
}

// --- add_dataset -------------------------------------------------------------------

const nlohmann::json& add_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"path"}},
        {"additionalProperties", false},
        {"properties",
         {{"path",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a parquet/jsonl/json/csv file on the node's filesystem"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "target database, created if absent (default "
                            "datasets)"}}},
          {"table_name",
           {{"type", "string"},
            {"description", "override the table name derived from the file"}}},
          {"partitioned",
           {{"type", "boolean"},
            {"description", "shard + place the rows across the cluster "
                            "(default true)"}}},
          {"partition_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 4096},
            {"description", "partitions to write (default 8)"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "owners per partition (default 1)"}}}}}};
    return schema;
}

ApiResult handle_add_dataset(const nlohmann::json& input, ApiContext& context) {
    zetta::data::AddDatasetOptions options;
    options.database = input.value("database", options.database);
    options.table_name = input.value("table_name", options.table_name);
    options.partitioned = input.value("partitioned", options.partitioned);
    options.partition_count =
        input.value("partition_count", options.partition_count);
    options.replication_factor =
        input.value("replication_factor", options.replication_factor);
    options.members = context.node.cluster_members();  // live placement roster

    const zetta::data::AddDatasetResult added = zetta::data::add_dataset_file(
        context.node, input.at("path").get<std::string>(), options);
    if (!added.ok()) {
        return ApiResult::failure("engine_error", added.error);
    }
    nlohmann::json partitions = nlohmann::json::array();
    for (const auto& partition : added.partitions) {
        if (!partition) continue;
        partitions.push_back({{"partition_id", partition->id()},
                              {"owner_nodes", partition->owner_nodes()}});
    }
    nlohmann::json output;
    output["database"] = added.database ? added.database->name() : options.database;
    output["table"] = added.table ? added.table->name() : "";
    output["format"] = zetta::data::dataset_file::format_name(added.format);
    output["rows"] = added.rows;
    output["partitions"] = std::move(partitions);
    return ApiResult::success(std::move(output));
}

// --- testkit_run (kind: job) --------------------------------------------------------

const nlohmann::json& testkit_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"case"}},
        {"additionalProperties", false},
        {"properties",
         {{"case",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "a testkit case id (system.test_cases / the Test "
                            "suite panel list them)"}}},
          {"options",
           {{"type", "object"},
            {"description", "per-case option overrides keyed by option key; "
                            "values typed per the case's declaration"}}},
          {"custom_data_path",
           {{"type", "string"},
            {"description", "a churn dataset directory for cases that accept "
                            "one (default: the built-in synthetic dataset)"}}}}}};
    return schema;
}

// Apply the caller's option overrides onto a case's declared defaults,
// validating each key + value type against the declaration — the same
// path-qualified rejection discipline as the schema boundary.
std::optional<ApiResult> apply_case_options(
    const zetta::testkit::TestCase& test_case, const nlohmann::json& overrides,
    zetta::testkit::TestOptions& options) {
    for (const auto& [key, value] : overrides.items()) {
        const zetta::testkit::TestOption* declared = nullptr;
        for (const zetta::testkit::TestOption& option : test_case.options) {
            if (option.key == key) {
                declared = &option;
                break;
            }
        }
        if (!declared) {
            return ApiResult::failure(
                "invalid_input",
                "'" + key + "' is not an option of case '" + test_case.id + "'",
                "/options/" + key);
        }
        const std::string pointer = "/options/" + key;
        switch (declared->type) {
            case zetta::testkit::OptionType::Bool:
                if (!value.is_boolean()) {
                    return ApiResult::failure("invalid_input",
                                              "expected a boolean", pointer);
                }
                options.set_bool(key, value.get<bool>());
                break;
            case zetta::testkit::OptionType::Int:
                if (!value.is_number_integer()) {
                    return ApiResult::failure("invalid_input",
                                              "expected an integer", pointer);
                }
                options.set_int(key, value.get<long>());
                break;
            case zetta::testkit::OptionType::Double:
                if (!value.is_number()) {
                    return ApiResult::failure("invalid_input",
                                              "expected a number", pointer);
                }
                options.set_double(key, value.get<double>());
                break;
            case zetta::testkit::OptionType::Choice: {
                int index = -1;
                if (value.is_number_integer()) {
                    index = value.get<int>();
                } else if (value.is_string()) {
                    const std::string label = value.get<std::string>();
                    for (std::size_t i = 0; i < declared->choices.size(); ++i) {
                        if (declared->choices[i] == label) {
                            index = static_cast<int>(i);
                            break;
                        }
                    }
                }
                if (index < 0 ||
                    index >= static_cast<int>(declared->choices.size())) {
                    return ApiResult::failure(
                        "invalid_input",
                        "expected one of the declared choices (by index or "
                        "label)",
                        pointer);
                }
                options.set_choice(key, index);
                break;
            }
            case zetta::testkit::OptionType::Text:
                if (!value.is_string()) {
                    return ApiResult::failure("invalid_input",
                                              "expected a string", pointer);
                }
                options.set_text(key, value.get<std::string>());
                break;
        }
    }
    return std::nullopt;
}

ApiResult handle_testkit_run(const nlohmann::json& input, ApiContext& context) {
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "testkit_run is a job capability and this embedding wired no "
            "background action owner");
    }
    const std::string case_id = input.at("case").get<std::string>();
    const zetta::testkit::TestCase* test_case =
        zetta::testkit::TestSuite::builtin().find(case_id);
    if (!test_case) {
        return ApiResult::failure("not_found",
                                  "no testkit case with id '" + case_id + "'");
    }
    zetta::testkit::TestOptions options = test_case->defaults();
    if (const auto overrides = input.find("options");
        overrides != input.end()) {
        if (auto rejected = apply_case_options(*test_case, *overrides, options)) {
            return *rejected;
        }
    }
    auto handle = zetta::testkit::AsyncCaseRun::start(
        context.node, *test_case, std::move(options),
        input.value("custom_data_path", ""), context.recorder, context.trace);
    if (!handle) {
        return ApiResult::failure("engine_error",
                                  "the run record could not be created");
    }
    context.background->adopt(
        handle, [worker = handle.get()] { return worker->done(); });
    nlohmann::json output;
    output["run_id"] = handle->run_id();
    output["case"] = test_case->id;
    output["status"] = "running";
    output["poll"] = "get_run for status; the case's log lines land with the "
                     "terminal status";
    return ApiResult::success(std::move(output));
}

}  // namespace

void register_core_capabilities(Registry& registry) {
    registry.add(Capability{
        "server_info",
        "Server info",
        "Read this node's identity, version, entity counts, and cluster "
        "membership. No side effects.",
        server_info_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_server_info});

    registry.add(Capability{
        "sql",
        "Run SQL",
        "Run one governed SQL statement (Postgres subset). Every system.* "
        "observability view is readable here. DDL/INSERT mutate the database.",
        sql_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_sql});

    registry.add(Capability{
        "search",
        "Search",
        "Search a registered index by free text — distributed scatter-gather "
        "across the cluster when the table is partitioned — returning ranked "
        "hits with per-node provenance and a trace id. No side effects.",
        search_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_search});

    registry.add(Capability{
        "list_projects",
        "List projects",
        "Read the names of every AI project on this node. No side effects.",
        list_projects_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_projects});

    registry.add(Capability{
        "list_traces",
        "List recent traces",
        "Read the most recent observability spans on this node (intent, "
        "timings, result shapes). No side effects.",
        list_traces_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_traces});

    registry.add(Capability{
        "get_trace",
        "Get one trace",
        "Read every recorded span of one trace by its 32-hex trace id. No "
        "side effects.",
        get_trace_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_trace});

    registry.add(Capability{
        "get_project",
        "Get one project",
        "Read one AI project's full detail — metadata plus its inputs and "
        "graphs — by name or uuid. No side effects.",
        get_project_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_project});

    registry.add(Capability{
        "get_run",
        "Get one run",
        "Read one run's full validated record — status, logs, outputs, "
        "lineage — by its uuid. No side effects.",
        get_run_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_run});

    registry.add(Capability{
        "get_model",
        "Get one model",
        "Read one model's full validated record — kind, lineage, feature "
        "schema, metrics, weights fingerprint — by its uuid. No side effects.",
        get_model_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_model});

    registry.add(Capability{
        "get_job",
        "Get one job",
        "Read one compute job's full validated record — kind, status, "
        "lineage refs, detail — by its uuid. No side effects.",
        get_job_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_job});

    registry.add(Capability{
        "get_index",
        "Get one index",
        "Read one registered index's full validated record — table, kind, "
        "backend, fields — by its uuid. No side effects.",
        get_index_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_index});

    registry.add(Capability{
        "get_dataset",
        "Get one dataset",
        "Read one dataset's computed summary — schema, row count, size, "
        "format, replication, partition placement — by name (plus database "
        "when ambiguous). No side effects.",
        get_dataset_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_dataset});

    registry.add(Capability{
        "list_runs",
        "List runs",
        "Read a compact listing of every run — id, lineage refs, status, "
        "stamps, log/output counts. No side effects.",
        list_runs_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_runs});

    registry.add(Capability{
        "list_models",
        "List models",
        "Read a compact listing of every registered model — id, name, kind, "
        "lineage refs, weights presence. No side effects.",
        list_models_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_models});

    registry.add(Capability{
        "list_jobs",
        "List jobs",
        "Read a compact listing of every compute job — id, kind, status, "
        "lineage refs, stamps, detail. No side effects.",
        list_jobs_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_jobs});

    registry.add(Capability{
        "list_indexes",
        "List indexes",
        "Read a compact listing of every registered index — id, table, kind, "
        "backend, fields — to discover what `search` can target. No side "
        "effects.",
        list_indexes_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_indexes});

    registry.add(Capability{
        "list_datasets",
        "List datasets",
        "Read the computed summary of every dataset — schema, row count, "
        "size, format, replication, partition placement. No side effects.",
        list_datasets_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_datasets});

    registry.add(Capability{
        "submit_feedback",
        "Submit feedback",
        "Snapshot a feedback bundle — your description plus the relevant "
        "traces, recent errors, crash reports, and versions — stored as a "
        "large object you explicitly export; nothing leaves the cluster on "
        "its own.",
        submit_feedback_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_submit_feedback});

    registry.add(Capability{
        "create_project",
        "Create project",
        "Create a new AI project (the central user-managed artifact); the "
        "project replicates to every node.",
        create_project_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_project"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_project});

    // --- governed action parity (#617): the rest of the product loop ---------

    registry.add(Capability{
        "capture_intent",
        "Capture intent",
        "Capture a natural-language intent as a replicated project input with "
        "provenance; compile it into a graph with compile_intent.",
        capture_intent_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "capture_intent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_capture_intent});

    registry.add(Capability{
        "import_n8n",
        "Import n8n workflow",
        "Validate an n8n workflow export against its schema and store it "
        "byte-for-byte as a replicated project artifact input; compile it "
        "with compile_n8n.",
        import_n8n_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "import_n8n"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_import_n8n});

    registry.add(Capability{
        "compile_intent",
        "Compile intent to graph",
        "Compile a captured intent input into a named, editable execution "
        "graph stored on the project (replicated; audited as a graph change).",
        compile_intent_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_intent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_intent});

    registry.add(Capability{
        "compile_n8n",
        "Compile n8n workflow to graph",
        "Compile a stored n8n workflow input into a named, editable execution "
        "graph stored on the project (replicated; audited as a graph change).",
        compile_n8n_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_n8n"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_n8n});

    registry.add(Capability{
        "compile_python",
        "Compile simulated Python to graph",
        "Lower a simulated-Python pipeline source to a named, editable "
        "execution graph stored on the project (replicated; audited as a "
        "graph change); zetta re-implements the pipeline — it never runs "
        "Python.",
        compile_python_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_python"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_python});

    registry.add(Capability{
        "gate",
        "Run the correctness gate",
        "Run the correctness gate over a stored project graph and return the "
        "full validation report (per-check status + reason, verdict, "
        "block_deploy). Evaluates only — no state change.",
        gate_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "gate"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_gate});

    registry.add(Capability{
        "deploy",
        "Deploy a graph",
        "Place a stored project graph onto a target through the correctness "
        "gate (RULE-1: a failing gate blocks with the failing check's "
        "reason); the recorded deployment replicates and is audited.",
        deploy_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "deploy"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_deploy});

    registry.add(Capability{
        "run",
        "Run a graph",
        "Execute a stored, gate-passing project graph: a churn-shaped graph "
        "runs the real engine over the live cluster and returns its run id "
        "immediately (poll get_run / system.runs); other graphs run "
        "structurally within the call.",
        run_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_run});

    registry.add(Capability{
        "add_dataset",
        "Add a dataset",
        "Load a local parquet/jsonl/json/csv file into a new partitioned "
        "table placed across the cluster (audited as dataset_added; visible "
        "in system.datasets).",
        add_dataset_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "add_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_add_dataset});

    registry.add(Capability{
        "testkit_run",
        "Run a testkit case",
        "Start one in-app test-suite case in the background and return its "
        "run id immediately (poll get_run — the case's log lines land with "
        "the terminal status).",
        testkit_run_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "testkit_run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_testkit_run});
}

}  // namespace zetta::api
