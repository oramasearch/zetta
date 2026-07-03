#include "zetta/api/capabilities.hpp"

#include <string>
#include <utility>

#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/feedback.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/query/service.hpp"
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
        "backend, fields. No side effects.",
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
}

}  // namespace zetta::api
