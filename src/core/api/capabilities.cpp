#include "zetta/api/capabilities.hpp"
#include "object_routes.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <array>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "zetta/agent/agent_store.hpp"
#include "zetta/agent/chat.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/autograd/qwen.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/featurize_plan.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/data/promote_dataset.hpp"
#include "zetta/data/table.hpp"
#include "zetta/dataset/dataset_generator_factory.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ml/anomaly_methodology.hpp"
#include "zetta/ml/explain_row.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/model_import.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/streaming.hpp"
#include "zetta/ml/score_table.hpp"
#include "zetta/ml/segmentation_methodology.hpp"
#include "zetta/ml/workspace_methodology.hpp"
#include "zetta/ml/workspace_study.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/feedback.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/pipeline/executor.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/pipeline/n8n_compile.hpp"
#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/project_input.hpp"
#include "zetta/pipeline/step_authority.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/pipeline/validation.hpp"
#include "zetta/query/service.hpp"
#include "zetta/solutions/experiment.hpp"
#include "zetta/solutions/snapshot.hpp"
#include "zetta/solutions/roi_readout.hpp"
#include "zetta/storage/engine.hpp"
#include "zetta/testkit/async_case_run.hpp"
#include "zetta/testkit/test_suite.hpp"
#include "zetta/tokenizer/bpe.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/version.hpp"

#include "../base64.hpp"  // zetta::detail::base64_decode (record_methodology_run)

namespace zetta::api {

namespace {

// zetta#1261: a graph's methodology and whether it can run, workspace
// methodologies included (defined with the workspace methodology capabilities).
std::pair<nlohmann::json, bool> graph_methodology_fields(const ApiContext& context,
                                                         const zetta::ir::ExecutionGraph& graph);

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json empty_object_schema() {
    return nlohmann::json{{"type", "object"}, {"additionalProperties", false}};
}

// Output schemas (#618) are draft-07 like the input schemas (schema-as-code):
// each documents its handler's success value — rendered as the MCP tool's
// outputSchema and as the REST/OpenAPI 200 response body's `value`. The
// handler stays the source of the value; journeys assert the two agree.

// The typed detail reads return exactly the entity's already-validated value
// JSON (its embedded *_value_schema()) plus the id — compose the output
// schema from the SAME embedded schema so the two can never drift.
nlohmann::json entity_value_with_id_schema(nlohmann::json schema) {
    schema["properties"]["id"] = {{"type", "string"},
                                  {"description", "the entity's uuid"}};
    nlohmann::json required = schema.value("required", nlohmann::json::array());
    required.push_back("id");
    schema["required"] = std::move(required);
    return schema;
}

// --- server_info -------------------------------------------------------------

const nlohmann::json& server_info_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& server_info_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"version", "node_id", "cluster_members", "databases", "projects",
          "runs", "models", "jobs", "failed_object_logs", "object_log_prefix_durability"}},
        {"additionalProperties", false},
        {"properties",
         {{"version", {{"type", "string"}}},
          {"node_id", {{"type", "string"}}},
          {"cluster_members", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"databases", {{"type", "integer"}}},
          {"projects", {{"type", "integer"}}},
          {"runs", {{"type", "integer"}}},
          {"models", {{"type", "integer"}}},
          {"jobs", {{"type", "integer"}}},
          {"object_log_prefix_durability",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "the durability format of this build's object bucket logs: 1 keeps a prefix of a "
             "bucket's writes, up to the last barrier, after any crash, power loss included "
             "(#1235). Absent on a build from before it; the object lease and barrier answers "
             "carry it too (lake#543)"}}},
          {"incarnation",
           {{"type", "string"},
            {"description",
             "a random id chosen when this node's object store started; it changes at every "
             "start. The object answers carry it per bucket (lake#543). Absent on a node "
             "without an object store"}}},
          {"failed_object_logs",
           {{"type", "array"},
            {"description",
             "buckets whose object log failed an fsync on this node and serve nothing until it "
             "is reopened from disk; `stopped`: reopening gave up, restart the node"},
            {"items",
             {{"type", "object"},
              {"required", {"bucket", "stopped", "reopen_attempts"}},
              {"additionalProperties", false},
              {"properties",
               {{"bucket", {{"type", "string"}}},
                {"stopped", {{"type", "boolean"}}},
                {"reopen_attempts", {{"type", "integer"}, {"minimum", 0}}}}}}}}}}}};
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
    output["object_log_prefix_durability"] = zetta::data::ObjectStore::kPrefixDurability;
    nlohmann::json failed_object_logs = nlohmann::json::array();
    if (auto* objects = context.node.object_store()) {
        output["incarnation"] = objects->incarnation();
        for (const auto& failure : objects->failed_logs()) {
            failed_object_logs.push_back({{"bucket", failure.bucket},
                                          {"stopped", failure.stopped},
                                          {"reopen_attempts", failure.reopen_attempts}});
        }
    }
    output["failed_object_logs"] = std::move(failed_object_logs);
    return ApiResult::success(std::move(output));
}

// --- node_memory -------------------------------------------------------------

const nlohmann::json& node_memory_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& node_memory_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"sampled_at_ms", "allocator_available", "rss_available",
          "allocator_in_use_bytes", "allocator_free_held_bytes", "rss_bytes",
          "peak_rss_bytes", "anonymous_bytes", "attributed_bytes",
          "unattributed_bytes", "over_attributed_bytes", "subsystems"}},
        {"additionalProperties", false},
        {"properties",
         {{"sampled_at_ms", {{"type", "integer"}}},
          {"allocator_available", {{"type", "boolean"}}},
          {"rss_available", {{"type", "boolean"}}},
          {"allocator_in_use_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"allocator_free_held_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"rss_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"peak_rss_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"anonymous_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"attributed_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"unattributed_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"over_attributed_bytes", {{"type", "integer"}, {"minimum", 0}}},
          {"subsystems",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"name", "bytes", "items", "reserved_bytes",
                            "share_of_in_use"}},
              {"additionalProperties", false},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"bytes", {{"type", "integer"}, {"minimum", 0}}},
                {"items", {{"type", "integer"}, {"minimum", 0}}},
                {"reserved_bytes", {{"type", "integer"}, {"minimum", 0}}},
                {"share_of_in_use", {{"type", "number"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_node_memory(const nlohmann::json&, ApiContext& context) {
    const auto snapshot = context.node.memory_snapshot();
    nlohmann::json output;
    output["sampled_at_ms"] = snapshot.sampled_at_ms;
    output["allocator_available"] = snapshot.allocator.allocator_available;
    output["rss_available"] = snapshot.allocator.rss_available;
    output["allocator_in_use_bytes"] = snapshot.allocator.in_use_bytes;
    output["allocator_free_held_bytes"] = snapshot.allocator.free_held_bytes;
    output["rss_bytes"] = snapshot.allocator.rss_bytes;
    output["peak_rss_bytes"] = snapshot.allocator.peak_rss_bytes;
    output["anonymous_bytes"] = snapshot.allocator.anonymous_bytes;
    output["attributed_bytes"] = snapshot.attributed_bytes;
    output["unattributed_bytes"] = snapshot.unattributed_bytes;
    output["over_attributed_bytes"] = snapshot.over_attributed_bytes;
    output["subsystems"] = nlohmann::json::array();
    for (const auto& subsystem : snapshot.subsystems) {
        output["subsystems"].push_back(
            {{"name", subsystem.name},
             {"bytes", subsystem.bytes},
             {"items", subsystem.items},
             {"reserved_bytes", subsystem.reserved_bytes},
             {"share_of_in_use", subsystem.share_of_in_use}});
    }
    return ApiResult::success(std::move(output));
}

// --- versions ----------------------------------------------------------------

// "Which version am I actually talking to" — asked of the SOFTWARE and of the
// MODEL, and the two answers come from different places. Four checkpoints were
// served side by side for comparison once and the Ask Orama rail answered from
// the oldest; the replies read as a model regression for an hour before anyone
// suspected the wrong agent was answering. `server_info` reports the binary and
// nothing about the agents, so it could not have settled that. This can.
//
// `assist_agent` is the one the rail picks, and it is deliberately reported the
// same way the rail picks it (first served) rather than described, so this
// cannot drift away from the truth it exists to report — see #981 for making
// that choice explicit rather than positional.

const nlohmann::json& versions_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& versions_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"software", "agents"}},
        {"additionalProperties", false},
        {"properties",
         {{"software",
           {{"type", "object"},
            {"required", {"zetta", "node_id"}},
            {"properties",
             {{"zetta", {{"type", "string"}}}, {"node_id", {{"type", "string"}}}}}}},
          {"agents",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "name", "model", "checkpoint"}},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"model", {{"type", "string"}}},
                {"checkpoint", {{"type", "string"}}}}}}}}},
          {"assist_agent", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_versions(const nlohmann::json&, ApiContext& context) {
    nlohmann::json output;
    output["software"] = {{"zetta", zetta::kVersion}, {"node_id", context.node.node_id()}};

    nlohmann::json agents = nlohmann::json::array();
    std::string assist;
    if (context.node.replication() != nullptr) {
        zetta::agent::AgentStore store(*context.node.replication());
        for (const zetta::agent::AgentRecord& agent : store.list()) {
            // The checkpoint is the last path element of `model` — that is what
            // distinguishes v12 from v13 when both are named "Base agent".
            std::string checkpoint = agent.model;
            if (const std::size_t slash = checkpoint.find_last_of('/');
                slash != std::string::npos && slash + 1 < checkpoint.size()) {
                checkpoint = checkpoint.substr(slash + 1);
            }
            agents.push_back({{"id", agent.id},
                              {"name", agent.name},
                              {"model", agent.model},
                              {"checkpoint", std::move(checkpoint)}});
            if (assist.empty()) assist = agent.id;
        }
    }
    output["agents"] = std::move(agents);
    if (!assist.empty()) output["assist_agent"] = assist;
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

const nlohmann::json& sql_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"messages", "columns", "rows"}},
        {"additionalProperties", false},
        {"properties",
         {{"messages", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"rows",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description", "one result row keyed by column name"}}}}}}}};
    return schema;
}

ApiResult handle_sql(const nlohmann::json& input, ApiContext& context) {
    // Thread the gate-resolved identity (#745) into the engine so role-management
    // DDL is gated on roles:manage (#746). Read SQL still only needs the sql
    // capability's required op (mcp:call); the finer gate is per-statement inside.
    zetta::query::QueryService service(context.node, context.cluster,
                                       context.acting_key, context.acting_is_user);
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
          {"fields",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "the row fields each hit carries back (default: the index's own "
             "fields; [] = ids and scores only). Each must be one of the "
             "index's fields."}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 1000},
            {"description", "cap on the global ranking (default 10)"}}}}}};
    return schema;
}

// The search success value's shape is declared ONCE, beside the served
// route's rendering (serving.hpp, #648): search_response_schema() /
// search_result_to_json() are shared by this capability and serve_search.

// The thin caller of THE search entry point (Node::search, #641) — the same
// path the GUI drives, so MCP/REST and the GUI can never diverge.
ApiResult handle_search(const nlohmann::json& input, ApiContext& context) {
    zetta::index::SearchRequest request;
    request.target = input.at("target").get<std::string>();
    request.query = input.at("query").get<std::string>();
    request.field = input.value("field", std::string{});
    request.top_k = input.value("top_k", 10);
    if (input.contains("fields")) {
        request.fields = input.at("fields").get<std::vector<std::string>>();
    }

    const zetta::index::SearchResult result =
        context.node.search(request, context.recorder, &context.trace);
    if (!result.ok()) {
        return ApiResult::failure("engine_error", result.status.message);
    }
    return ApiResult::success(search_result_to_json(result));
}

// --- list_projects -----------------------------------------------------------

const nlohmann::json& list_projects_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_projects_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"projects"}},
        {"additionalProperties", false},
        {"properties",
         {{"projects",
           {{"type", "array"},
            {"description", "the project names"},
            {"items", {{"type", "string"}}}}},
          {"summaries",
           {{"type", "array"},
            {"description",
             "one entry per name in `projects`, in the same order (#1185): the "
             "project's id and name and each graph's id, name and approval, "
             "so a list page needs no get_project per project. Present only "
             "when the caller may also call get_project"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "name", "graphs"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"graphs",
                 {{"type", "array"},
                  {"items",
                   {{"type", "object"},
                    {"required", {"id", "name", "approval"}},
                    {"additionalProperties", false},
                    {"properties",
                     {{"id", {{"type", "string"}}},
                      {"name", {{"type", "string"}}},
                      {"approval",
                       {{"type", "string"},
                        {"enum", {"proposed", "approved"}},
                        {"description",
                         "plan-approval state (#885), as get_project"}}}}}}}}}}}}}}}}}};
    return schema;
}

// `projects` stays the list of names existing callers read; `summaries`
// (#1185) carries what a list page shows, from the same in-memory projects
// get_project reads. Internal projects (a standalone model's, lake#654) are left
// out of both, as project_names() does.
//
// A summary is part of get_project's answer, so it is read with get_project's
// right: a user whose mcp:call is scoped to list_projects alone (#747) gets the
// names, as before, and no summaries. AccessKeys and the local session are
// authorized on api:query for both tools, so for them nothing differs.
bool may_read_project_detail(const ApiContext& context) {
    return !context.acting_is_user ||
           context.node.may_call_tool(context.acting_key, /*acting_is_user=*/true,
                                      "get_project");
}

ApiResult handle_list_projects(const nlohmann::json&, ApiContext& context) {
    const bool with_summaries = may_read_project_detail(context);
    nlohmann::json names = nlohmann::json::array();
    nlohmann::json summaries = nlohmann::json::array();
    for (const auto& project : context.node.projects()) {
        if (project->internal()) continue;
        names.push_back(project->name());
        if (!with_summaries) continue;
        nlohmann::json graphs = nlohmann::json::array();
        for (const auto& graph : project->graphs()) {
            graphs.push_back({{"id", graph->id()},
                              {"name", graph->name()},
                              {"approval", graph->approval_status()}});
        }
        summaries.push_back({{"id", project->id()},
                             {"name", project->name()},
                             {"graphs", std::move(graphs)}});
    }
    nlohmann::json output;
    output["projects"] = std::move(names);
    if (with_summaries) output["summaries"] = std::move(summaries);
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

const nlohmann::json& get_project_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"id", "name", "created", "updated", "owner", "description", "inputs",
          "graphs"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"created", {{"type", "string"}}},
          {"updated", {{"type", "string"}}},
          {"owner", {{"type", "string"}}},
          {"description", {{"type", "string"}}},
          {"inputs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description",
               "one project input's validated metadata plus its id"},
              {"required", {"id", "kind"}},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"kind", {{"type", "string"}}}}}}}}},
          {"graphs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "name"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"approval",
                 {{"type", "string"},
                  {"enum", {"proposed", "approved"}},
                  {"description", "plan-approval state (#885)"}}},
                {"methodology",
                 {{"type", {"string", "null"}},
                  {"description", "the registry methodology id, when known"}}},
                {"runnable", {{"type", "boolean"}}},
                {"approved_by", {{"type", "string"}}},
                {"approved_at", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_get_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    auto project = context.node.find_project(name);
    if (!project) {
        // Entity refs elsewhere (runs, jobs, models) carry the project *uuid*,
        // so accept it as the handle too (a standalone model's internal project
        // is found this way only, lake#654).
        project = context.node.project_by_id(name);
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
        const auto [methodology, runnable] = graph_methodology_fields(context, graph->graph());
        graphs.push_back(
            {{"id", graph->id()},
             {"name", graph->name()},
             {"approval", graph->approval_status()},
             {"approved_by", graph->approved_by()},
             {"approved_at", graph->approved_at()},
             {"methodology", methodology},
             {"runnable", runnable}});
    }
    output["graphs"] = std::move(graphs);
    return ApiResult::success(std::move(output));
}

// An id input plus the opt-in for an entity-key mapping's entries (#1202).
nlohmann::json id_input_with_entity_keys_schema(std::string_view field,
                                                std::string_view description) {
    nlohmann::json schema = id_input_schema(field, description);
    schema["properties"]["include_entity_keys"] = {
        {"type", "boolean"},
        {"description",
         "also return entity_key_mapping.entries, every distinct text id and "
         "its derived key (default false: a count, entry_count — the list "
         "grows with the dataset's row count)"}};
    return schema;
}

const nlohmann::json& get_run_schema() {
    static const nlohmann::json schema = id_input_with_entity_keys_schema(
        "run_id", "the run's uuid (system.runs.run_id)");
    return schema;
}

const nlohmann::json& get_run_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json value =
            entity_value_with_id_schema(zetta::pipeline::run_value_schema());
        // Summarized by default (#1202); the full form on include_entity_keys.
        nlohmann::json& mapping = value["properties"]["entity_key_mapping"];
        mapping = {{"anyOf",
                    {mapping, zetta::data::entity_key_mapping_summary_schema()}}};
        return value;
    }();
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
    if (!input.value("include_entity_keys", false)) {
        zetta::data::summarize_entity_key_mappings(output);
    }
    return ApiResult::success(std::move(output));
}

// ---------------------------------------------------------------------------
// get_run_report (#958) — the facts a run FOUND, assembled at query time.
//
// The artifact a report is written from. Deliberately NOT a stored document: every
// number here already lives in replicated state — the Model record carries the
// metrics and what the model was trained from, the at-risk table carries the
// ranked list with its evidence — so assembling it on read means there is no
// second copy to drift from the first, and a report generated a month later
// reflects the same facts as one generated at the end of the run.
//
// The rule this serves: **the engine computes every number and the narrator only
// arranges them.** A field a writer has to work out is a field this artifact is
// missing. `majority_class_accuracy` is here because exactly that happened once.
// ---------------------------------------------------------------------------

const nlohmann::json& get_run_report_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the run's uuid (system.runs.run_id, or the "
                            "`report` output's ref)"}}},
          {"limit",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "maximum rows returned (default 20): ranked entities, anomalies, segment profiles, "
             "or forecast rows by series then timestamp. Limit applies after "
             "ordering"}}}}}};
    return schema;
}

const nlohmann::json& get_run_report_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run", "model", "features"}},
        {"properties",
         {{"features", zetta::pipeline::run_features_schema()},
          {"run",
           {{"type", "object"},
            {"description", "run id, graph, status, and when it finished"}}},
          {"dataset",
           {{"type", "object"},
            {"description",
             "what the model was built from: accounts, events, churn_rate, "
             "majority_class_accuracy (the accuracy a constant answer scores), "
             "and "
             "the feature columns. Segmentation reports the source table, "
             "read/clustered/excluded rows, fitted features and excluded "
             "columns. Anomaly detection records source, rows read/excluded, "
             "roles and unused columns. Forecasting records selected/excluded "
             "columns and rows/partitions/bytes read. Absent when the model records no dataset facts"}}},
          {"model",
           {{"type", "object"},
            {"description",
             "id, name, and the metrics. `held_out_*` is present only when the "
             "run "
             "asked for a holdout; when it is absent the only figures "
             "available are "
             "in-sample and they flatter (#953). Segmentation instead carries "
             "clustering metrics, chosen_k_reason and k_candidates. Anomaly "
             "detection reports threshold, budget, alert rate and optional precision at k. "
             "Forecast models carry rolling-origin errors, seasonal-naive comparison "
             "and interval coverage. Next-best-action models return hit rate, IPS/DR "
             "and importance-weight diagnostics. Language reports expose recall_at_k, MRR, "
             "groundedness, and embedder_version"}}},
          {"recommendations",
           {{"type", "array"},
            {"description",
             "M-8 entity-action rows ordered by entity then rank, including "
             "expected value, chosen action and why"}}},
          {"policy",
           {{"type", "object"},
            {"description",
             "M-8 policy metrics, chronological ALS holdout facts, offline "
             "evaluation refusal when applicable, and causal warning"}}},
          {"graph",
           {{"type", "object"},
            {"description",
             "graph runs: component sizes, degree distribution, PageRank top "
             "nodes, edge cutoff counts, and held-out node-task accuracy"}}},
          {"metrics",
           {{"type", "object"},
            {"description", "bandit reward, regret, exploration, and row metrics"}}},
          {"policy_table",
           {{"type", "string"},
            {"description", "bandit allocation-policy table reference"}}},
          {"decisions_table",
           {{"type", "string"},
            {"description", "bandit propensity-log table reference"}}},
          {"uncertainty",
           {{"type", "object"},
            {"description",
             "M-17 nominal and empirical coverage, interval width, hold-out row "
             "count, and the exchangeability assumption"}}},
          {"drivers",
           {{"type", "array"},
            {"description",
             "feature -> weight, strongest first. A weight is what "
             "the model leaned on, not a cause"}}},
          {"outputs",
           {{"oneOf", {{{"type", "array"}}, {{"type", "object"}}}},
            {"description",
             "forecast, recommendation, language, privacy and streaming output references; "
             "survival uses an object of horizon and interval-calibration table references"}}},
          {"forecast",
           {{"type", "array"},
            {"description",
             "forecast-table rows with series, timestamp, "
             "point_forecast, lower and upper; ordered by series "
             "then timestamp before applying limit"}}},
          {"baseline_comparison",
           {{"type", "string"},
            {"description", "whether the forecast beat seasonal-naive in "
                            "rolling-origin backtests"}}},
          {"meta_model_weights",
           {{"type", "array"},
            {"description",
             "M-18 base-model weights in the regularized logistic stacker"}}},
          {"privacy",
           {{"type", "object"},
            {"description",
             "M-20 fidelity, DCR, membership, TSTR, and epsilon/delta spend"}}},
          {"at_risk",
           {{"type", "array"},
            {"description",
             "the ranked entities with their risk and their evidence values, "
             "capped by `limit`"}}},
          {"topics",
           {{"type", "object"},
            {"description", "streaming input and score-output topic addresses"}}},
          {"transcripts",
           {{"type", "array"},
            {"description",
             "redacted speech segments with recording, time bounds, text, and "
             "confidence"}}},
          {"fields",
           {{"type", "array"},
            {"description",
             "M-11 extracted fields with confidence, page, box, and review status"}}},
          {"review_queue",
           {{"type", "object"},
            {"description",
             "M-11 review-table reference and routed-field count"}}},
          {"predictions",
           {{"type", "array"},
            {"description", "vision prediction rows, capped by `limit`"}}},
          {"confusion_matrix",
           {{"type", "array"},
            {"description",
             "vision held-out counts indexed by actual then predicted class"}}},
          {"methodology",
           {{"type", "string"},
            {"description", "methodology id for typed reports"}}},
          {"output_tables",
           {{"type", "array"},
            {"description", "named result table references, "
                            "availability and row counts"}}},
          {"anomalies",
           {{"type", "array"},
            {"description", "M-7 rows ranked by anomaly score, with flags, threshold "
                            "and robust contributors"}}},
          {"segments",
           {{"type", "array"},
            {"description", "segment sizes, shares, original-unit centers and "
                            "descriptive labels"}}},
          {"ranked_queue",
           {{"type", "array"},
            {"description",
             "learning-to-rank rows: group, entity, score, rank, and observed "
             "relevance, capped by `limit`"}}},
          {"caveats",
           {{"type", "array"},
            {"description", "figures that are PRESENT but must not be read as "
                            "results — a holdout "
                            "too small to mean anything, a perfect score. "
                            "`missing` says what the "
                            "run cannot tell you; this says what it can and "
                            "you should not believe"}}},
          {"missing",
           {{"type", "array"},
            {"description",
             "fields a report would want that this run cannot supply, each "
             "saying "
             "why. Stated rather than omitted so a writer does not quietly "
             "substitute its own number"}}}}}};
    return schema;
}

// The result table this run wrote, by the run's own `dataset` outputs. Prefer
// the known churn and ranking result databases over the input-table fallback.
std::shared_ptr<zetta::data::Table> run_result_table(
    Node& node, const zetta::pipeline::Run& run) {
    std::shared_ptr<zetta::data::Table> fallback;
    for (const zetta::pipeline::RunOutput& output : run.outputs()) {
        const std::size_t dot = output.ref.find('.');
        if (output.kind != "dataset" || dot == std::string::npos) continue;
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(output.ref.substr(0, dot));
        if (!database) continue;
        if (auto table = database->find_table(output.ref.substr(dot + 1))) {
            if (output.ref.starts_with("churn_results.") ||
                output.ref.starts_with("rank_results.")) {
                return table;
            }
            fallback = std::move(table);
        }
    }
    return fallback;
}

std::shared_ptr<zetta::data::Table> run_named_table(
    Node& node, const zetta::pipeline::Run& run, std::string_view name) {
    for (const zetta::pipeline::RunOutput& output : run.outputs()) {
        const std::size_t dot = output.ref.find('.');
        if (output.kind != "dataset" || output.name != name ||
            dot == std::string::npos) {
            continue;
        }
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(output.ref.substr(0, dot));
        if (database) {
            return database->find_table(output.ref.substr(dot + 1));
        }
    }
    return nullptr;
}

nlohmann::json table_rows(
    const std::shared_ptr<zetta::data::Table>& table,
    std::size_t limit = std::numeric_limits<std::size_t>::max()) {
    nlohmann::json rows = nlohmann::json::array();
    if (!table) {
        return rows;
    }
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size() && rows.size() < limit;
         ++index) {
        if (const auto row = store.at(index)) {
            rows.push_back(*row);
        }
    }
    return rows;
}

// A number from an at-risk row, or `fallback` when the column is absent or not a
// number. nlohmann's `value<T>` THROWS when the key is present with another type
// (a null rank, a string account id from a customer's dataset), and a throw from
// a read is an answer nobody receives (#1007). The report reads a table a run
// wrote — replicated state — so it tolerates a column of an unexpected type.
double row_number(const nlohmann::json& row, const char* key, double fallback) {
    const auto found = row.find(key);
    if (found == row.end() || !found->is_number()) return fallback;
    return found->get<double>();
}

// The entity key of an at-risk row: an int64 the whole way through (#1000), or
// whatever the row holds verbatim when it is not an integer (a business id that
// was never derived to a key, #957) — reported as-is rather than zeroed.
nlohmann::json row_entity(const nlohmann::json& row) {
    const auto found = row.find("account");
    if (found == row.end() || found->is_null()) return std::int64_t{0};
    if (found->is_number_integer()) return found->get<std::int64_t>();
    return *found;
}

std::shared_ptr<zetta::data::Table> run_output_table(
    Node& node, const zetta::pipeline::Run& run, std::string_view name) {
    for (const zetta::pipeline::RunOutput& output : run.outputs()) {
        if (output.kind != "dataset" || output.name != name) {
            continue;
        }
        const std::size_t dot = output.ref.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(output.ref.substr(0, dot));
        if (database) {
            return database->find_table(output.ref.substr(dot + 1));
        }
    }
    return nullptr;
}

nlohmann::json document_extraction_report(
    Node& node, const zetta::pipeline::Run& run,
    const std::shared_ptr<zetta::ml::Model>& model, std::size_t limit) {
    nlohmann::json report;
    report["run"] = {{"id", run.id()},
                     {"graph", run.graph()},
                     {"project", run.project()},
                     {"status", to_string(run.status())},
                     {"finished", run.updated()}};
    report["model"] = {{"id", model->id()},
                       {"name", model->name()},
                       {"kind", model->kind()},
                       {"metrics", model->metrics()}};
    nlohmann::json dataset = nlohmann::json::object();
    for (const std::string& source : run.lineage().sources) {
        if (source.starts_with("dataset:")) {
            dataset["source"] = source.substr(8);
        }
    }
    const nlohmann::json& metrics = model->metrics();
    if (metrics.contains("documents_read")) {
        dataset["documents_read"] = metrics.at("documents_read");
    }
    dataset["documents_excluded"] =
        metrics.value("documents_excluded", 0.0);
    report["dataset"] = std::move(dataset);

    nlohmann::json fields = nlohmann::json::array();
    if (const std::shared_ptr<zetta::data::Table> table =
            run_output_table(node, run, "extracted fields")) {
        const zetta::storage::StorageEngine& store = table->store();
        for (std::size_t index = 0;
             index < store.size() && fields.size() < limit; ++index) {
            const zetta::storage::ParquetLazyPtr row = store.at(index);
            if (row) {
                fields.push_back(*row);
            }
        }
    }
    report["fields"] = std::move(fields);

    nlohmann::json review = nlohmann::json::object();
    for (const zetta::pipeline::RunOutput& output : run.outputs()) {
        if (output.kind == "dataset" && output.name == "review queue") {
            review["table"] = output.ref;
            break;
        }
    }
    review["fields"] = metrics.value("fields_reviewed", 0.0);
    review["share"] = metrics.value("review_share", 0.0);
    report["review_queue"] = std::move(review);
    return report;
}

// A row count as a plain integer string — "4", not "4.000000".
std::string fixed_rows(double value) {
    return std::to_string(static_cast<long long>(value));
}

// A held-out figure that is PRESENT but must not be read as a result (#948 step 8).
// `check` names the test that fired; `why` is the sentence a reader is shown.
struct HeldOutCaveat {
    std::string check;
    std::string why;
};

// The one set of held-out checks. The run report lists these as `caveats` and
// model_health turns each into a finding that makes the model untrustworthy (#1224), so
// the two answers about the same model cannot disagree.
//
// The case that forced them: the built-in demo dataset produces a held-out AUC of 1.000
// on FOUR rows. Nothing was wrong — the pipeline worked and the data is trivially
// separable — but a perfect score presented without objection is a number that invites
// a false conclusion, which is worse than a missing one.
//
// `ranking` is a `rank` model; the row-count check has never applied to one (its
// report measures ranking quality per query group, not AUC on held-out rows).
std::vector<HeldOutCaveat> held_out_caveats(std::optional<double> held_out_rows,
                                            std::optional<double> held_out_auc,
                                            bool ranking) {
    std::vector<HeldOutCaveat> caveats;
    if (!ranking && held_out_rows) {
        // Below this, a ranking metric is noise: one row changing side moves AUC by
        // a visible amount, so the figure says more about the split than the model.
        constexpr double kMeaningfulHoldout = 30.0;
        if (*held_out_rows > 0.0 && *held_out_rows < kMeaningfulHoldout) {
            caveats.push_back(
                {"small_holdout",
                 "measured on only " + fixed_rows(*held_out_rows) +
                     " held-out rows. Below about 30 a ranking metric is "
                     "noise — one row changing side moves it visibly — so "
                     "this figure describes the split more than the model. "
                     "Do not quote it as a result"});
        }
    }
    if (held_out_auc && *held_out_auc >= 0.999) {
        caveats.push_back(
            {"perfect_auc",
             "a perfect score. On real data this is almost always a "
             "trivially separable demo set or a leaked feature, not a "
             "model that solved the problem. Check what the top driver "
             "is before showing this to anyone"});
    }
    return caveats;
}

nlohmann::json next_best_action_report(Node& node,
                                       const zetta::pipeline::Run& run,
                                       const zetta::ml::Model& model,
                                       std::size_t limit) {
    nlohmann::json report;
    report["run"] = {{"id", run.id()},
                     {"graph", run.graph()},
                     {"project", run.project()},
                     {"status", to_string(run.status())},
                     {"finished", run.updated()}};
    report["model"] = {{"id", model.id()},
                       {"name", model.name()},
                       {"kind", model.kind()},
                       {"metrics", model.metrics()}};
    const nlohmann::json& facts = model.feature_schema();
    if (const auto dataset = facts.find("dataset"); dataset != facts.end()) {
        report["dataset"] = *dataset;
    }
    report["outputs"] = nlohmann::json::array();
    std::vector<nlohmann::json> recommendations;
    for (const zetta::pipeline::RunOutput& output : run.outputs()) {
        report["outputs"].push_back(
            {{"kind", output.kind}, {"name", output.name}, {"ref", output.ref}});
        if (output.kind != "dataset") {
            continue;
        }
        const std::size_t dot = output.ref.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        const auto database = node.find_database(output.ref.substr(0, dot));
        const auto table =
            database ? database->find_table(output.ref.substr(dot + 1)) : nullptr;
        if (!table) {
            continue;
        }
        const zetta::storage::StorageEngine& store = table->store();
        if (output.name == "policy report") {
            if (store.size() > 0) {
                if (const auto row = store.at(0)) {
                    report["policy"] = *row;
                }
            }
            continue;
        }
        if (output.name != "recommendation table") {
            continue;
        }
        for (std::size_t index = 0; index < store.size(); ++index) {
            if (const auto row = store.at(index)) {
                recommendations.push_back(*row);
            }
        }
    }
    std::ranges::sort(recommendations,
                      [](const nlohmann::json& left,
                         const nlohmann::json& right) {
                          const double left_entity = row_number(left, "entity", 0.0);
                          const double right_entity = row_number(right, "entity", 0.0);
                          if (left_entity != right_entity) {
                              return left_entity < right_entity;
                          }
                          return row_number(left, "rank", 0.0) <
                                 row_number(right, "rank", 0.0);
                      });
    if (recommendations.size() > limit) {
        recommendations.resize(limit);
    }
    report["recommendations"] = std::move(recommendations);
    report["caveats"] = {
        {{"field", "propensity"},
         {"why", "propensity is not causal lift; use M-6 to decide whether "
                 "to treat"}}};
    if (!report.contains("policy")) {
        report["missing"] = {
            {{"field", "policy"},
             {"why", "the policy report table is not visible on this member"}}};
    }
    return report;
}

nlohmann::json chronological_forecast_rows(const zetta::data::Table &table,
                                           std::size_t limit) {
    std::vector<nlohmann::json> rows;
    const auto &store = table.store();
    rows.reserve(store.size());
    for (std::size_t index = 0; index < store.size(); ++index) {
        if (const auto row = store.at(index)) {
            rows.push_back(*row);
        }
    }
    // Replicated storage order is not temporal order. Sort the full result
    // before truncating so every member returns the same earliest forecasts.
    std::ranges::sort(rows, [](const auto &left, const auto &right) {
        const double left_series = row_number(left, "series", 0.0);
        const double right_series = row_number(right, "series", 0.0);
        if (left_series != right_series) {
            return left_series < right_series;
        }
        return row_number(left, "timestamp", 0.0) <
               row_number(right, "timestamp", 0.0);
    });
    if (rows.size() > limit) {
        rows.resize(limit);
    }
    return rows;
}

nlohmann::json forecast_report(Node &node, const zetta::pipeline::Run &run,
                               const zetta::ml::Model &model,
                               std::size_t limit) {
    nlohmann::json report;
    report["run"] = {{"id", run.id()},
                     {"graph", run.graph()},
                     {"project", run.project()},
                     {"status", to_string(run.status())},
                     {"finished", run.updated()}};
    report["model"] = {{"id", model.id()},
                       {"name", model.name()},
                       {"kind", model.kind()},
                       {"metrics", model.metrics()}};
    const nlohmann::json &facts = model.feature_schema();
    for (const char *key : {"dataset", "baseline_comparison"}) {
        if (const auto found = facts.find(key); found != facts.end()) {
            report[key] = *found;
        }
    }
    report["outputs"] = nlohmann::json::array();
    report["forecast"] = nlohmann::json::array();
    for (const auto &output : run.outputs()) {
        report["outputs"].push_back({{"kind", output.kind},
                                     {"name", output.name},
                                     {"ref", output.ref}});
        if (output.kind != "dataset") {
            continue;
        }
        const auto dot = output.ref.find('.');
        if (dot == std::string::npos) {
            continue;
        }
        const auto database = node.find_database(output.ref.substr(0, dot));
        const auto table =
            database ? database->find_table(output.ref.substr(dot + 1))
                     : nullptr;
        if (!table) {
            report["missing"] = {
                {{"field", "forecast"},
                 {"why",
                  "forecast output table is not yet visible on this member"}}};
            continue;
        }
        report["forecast"] = chronological_forecast_rows(*table, limit);
    }
    report["caveats"] = {
        {{"field", "interval_coverage"},
         {"why", "empirical coverage on rolling-origin residuals; not a "
                 "guarantee for future observations"}}};
    return report;
}

ApiResult assemble_run_report(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("run_id").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Run> run = context.node.find_run(id);
    if (!run) {
        return ApiResult::failure("not_found", "no run with id '" + id + "'");
    }
    const std::size_t limit = input.value("limit", std::size_t{20});

    nlohmann::json report;
    nlohmann::json missing = nlohmann::json::array();
    report["run"] = {{"id", run->id()},
                     {"graph", run->graph()},
                     {"project", run->project()},
                     {"status", to_string(run->status())},
                     {"finished", run->updated()}};

    // The model output names the Model this run trained; everything numeric hangs
    // off it.
    std::shared_ptr<zetta::ml::Model> model;
    for (const zetta::pipeline::RunOutput& output : run->outputs()) {
        if (output.kind == "model") model = context.node.find_model(output.ref);
        if (model) break;
    }
    if (!model) {
        return ApiResult::failure(
            "not_found",
            "run '" + id +
                "' has no resolvable model output, so there are no measured "
                "numbers to report");
    }

    if (model->kind() == "segmentation") {
        return ApiResult::success(zetta::ml::segmentation_run_report(
            context.node, *run, *model, limit));
    }
    if (model->kind() == "anomaly") {
        return ApiResult::success(
            zetta::ml::anomaly_run_report(context.node, *run, *model, limit));
    }

    if (model->kind() == "forecast") {
        return ApiResult::success(
            forecast_report(context.node, *run, *model, limit));
    }
    if (model->kind() == "next_best_action") {
        return ApiResult::success(
            next_best_action_report(context.node, *run, *model, limit));
    }
    if (model->kind() == "document_extraction") {
        return ApiResult::success(
            document_extraction_report(context.node, *run, model, limit));
    }
    const nlohmann::json& metrics = model->metrics();
    const auto number = [&metrics](const char* key) -> std::optional<double> {
        const auto found = metrics.find(key);
        if (found == metrics.end() || !found->is_number()) return std::nullopt;
        return found->get<double>();
    };

    if (model->kind() == "streaming") {
        nlohmann::json model_facts = {
            {"id", model->id()}, {"name", model->name()}, {"kind", model->kind()}};
        for (const auto& [key, value] : metrics.items()) {
            if (value.is_number()) {
                model_facts[key] = value;
            }
        }
        report["model"] = std::move(model_facts);

        nlohmann::json dataset = nlohmann::json::object();
        nlohmann::json topics = nlohmann::json::object();
        const nlohmann::json& feature_schema = model->feature_schema();
        if (feature_schema.is_object()) {
            if (const auto found = feature_schema.find("event_features");
                found != feature_schema.end()) {
                dataset["features"] = *found;
            }
            if (const auto found = feature_schema.find("entity_column");
                found != feature_schema.end()) {
                dataset["entity_column"] = *found;
            }
            if (const auto found = feature_schema.find("time_column");
                found != feature_schema.end()) {
                dataset["time_column"] = *found;
            }
            if (const auto found = feature_schema.find("label_column");
                found != feature_schema.end()) {
                dataset["label_column"] = *found;
            }
            if (const auto found = feature_schema.find("input_topic");
                found != feature_schema.end()) {
                topics["input"] = *found;
            }
            if (const auto found = feature_schema.find("output_topic");
                found != feature_schema.end()) {
                topics["scores"] = *found;
            }
        }
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        nlohmann::json excluded = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                excluded.push_back(log.message.substr(18));
            }
        }
        if (!excluded.empty()) {
            dataset["not_used"] = std::move(excluded);
        }
        report["dataset"] = std::move(dataset);
        if (!topics.empty()) {
            report["topics"] = std::move(topics);
        }
        nlohmann::json outputs = nlohmann::json::array();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            outputs.push_back({{"kind", output.kind},
                               {"name", output.name},
                               {"ref", output.ref}});
        }
        report["outputs"] = std::move(outputs);
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "synthetic_private") {
        report["model"] = {{"id", model->id()},
                           {"name", model->name()},
                           {"kind", model->kind()}};
        nlohmann::json privacy = nlohmann::json::object();
        for (const char* key :
             {"marginal_distance", "correlation_distance",
              "distance_to_closest_record", "membership_auc", "tstr_auc",
              "epsilon_spent", "delta_spent", "privacy_mechanisms",
              "privacy_evaluation_passed"}) {
            if (const auto value = number(key)) {
                privacy[key] = *value;
            }
        }
        report["privacy"] = std::move(privacy);
        nlohmann::json outputs = nlohmann::json::array();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            outputs.push_back({{"kind", output.kind},
                               {"name", output.name},
                               {"ref", output.ref}});
        }
        report["outputs"] = std::move(outputs);
        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        nlohmann::json excluded = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                excluded.push_back(log.message.substr(18));
            }
        }
        if (!excluded.empty()) {
            dataset["not_used"] = std::move(excluded);
        }
        if (!dataset.empty()) {
            report["dataset"] = std::move(dataset);
        }
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "label_efficiency") {
        report["model"] = {
            {"id", model->id()},
            {"name", model->name()},
            {"kind", model->kind()},
            {"metrics", metrics},
            {"labelling_functions",
             model->feature_schema().value("labelling_functions",
                                           nlohmann::json::array())},
        };
        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        if (const auto gold = number("gold_rows")) {
            dataset["excluded_gold_rows"] = *gold;
        }
        nlohmann::json not_used = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                not_used.push_back(log.message.substr(9));
            }
        }
        if (!not_used.empty()) {
            dataset["not_used"] = std::move(not_used);
        }
        report["dataset"] = std::move(dataset);
        nlohmann::json tables = nlohmann::json::array();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind == "dataset") {
                tables.push_back(
                    {{"name", output.name}, {"table", output.ref}});
            }
        }
        report["output_tables"] = std::move(tables);
        report["correctness"] = {
            {"weak_labels_are_gold", false},
            {"gold_split", "disjoint"},
        };
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "bayesian") {
        nlohmann::json model_facts = {
            {"id", model->id()}, {"name", model->name()}, {"kind", model->kind()}};
        for (const auto& [name, value] : metrics.items()) {
            if (value.is_number()) {
                model_facts[name] = value;
            }
        }
        report["model"] = std::move(model_facts);

        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        nlohmann::json notes = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                notes.push_back(log.message.substr(9));
            }
        }
        if (!notes.empty()) {
            dataset["not_used"] = std::move(notes);
        }
        if (const std::optional<double> excluded = number("excluded_rows")) {
            dataset["excluded_rows"] = *excluded;
        }
        report["dataset"] = std::move(dataset);

        nlohmann::json uncertainty = {
            {"assumption",
             "coverage interpretation assumes exchangeable observations within "
             "the declared groups"}};
        if (const std::optional<double> nominal = number("nominal_coverage")) {
            uncertainty["nominal_coverage"] = *nominal;
        }
        if (const std::optional<double> empirical =
                number("empirical_coverage")) {
            uncertainty["empirical_coverage"] = *empirical;
        }
        if (const std::optional<double> width =
                number("mean_interval_width")) {
            uncertainty["mean_interval_width"] = *width;
        }
        if (const std::optional<double> rows = number("held_out_rows")) {
            uncertainty["held_out_rows"] = *rows;
        }
        report["uncertainty"] = std::move(uncertainty);

        nlohmann::json output_tables = nlohmann::json::array();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind == "dataset") {
                output_tables.push_back(
                    {{"name", output.name}, {"ref", output.ref}});
            }
        }
        report["output_tables"] = std::move(output_tables);
        return ApiResult::success(std::move(report));
    }


    if (model->kind() == "optimization") {
        nlohmann::json model_facts = {
            {"id", model->id()},
            {"name", model->name()},
            {"solver_status",
             model->feature_schema().value("status", std::string("unknown"))}};
        for (const char* key :
             {"objective_value", "current_plan_objective", "objective_delta",
              "feasible", "optimality_gap", "constraint_violations", "iterations",
              "explored_nodes"}) {
            if (const std::optional<double> value = number(key)) {
                model_facts[key] = *value;
            }
        }
        report["model"] = std::move(model_facts);
        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        if (const std::optional<double> input_rows = number("input_rows")) {
            dataset["rows"] = *input_rows;
        }
        nlohmann::json excluded = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                excluded.push_back(log.message.substr(9));
            }
        }
        if (!excluded.empty()) {
            dataset["not_used"] = std::move(excluded);
        }
        if (!dataset.empty()) {
            report["dataset"] = std::move(dataset);
        }
        if (const std::shared_ptr<zetta::data::Table> plan =
                run_named_table(context.node, *run, "optimization plan")) {
            report["plan"] = table_rows(plan);
        }
        if (const std::shared_ptr<zetta::data::Table> constraints =
                run_named_table(context.node, *run, "constraint trace")) {
            report["constraints"] = table_rows(constraints);
        }
        return ApiResult::success(std::move(report));
    }


    if (model->kind() == "bandit") {
        nlohmann::json metric_facts = nlohmann::json::object();
        for (const char* key :
             {"cumulative_reward", "baseline_reward", "regret",
              "exploration_share", "decisions", "rows_read",
              "rows_excluded"}) {
            if (const auto value = number(key)) {
                metric_facts[key] = *value;
            }
        }
        report["model"] = {{"id", model->id()},
                           {"name", model->name()},
                           {"kind", "bandit"},
                           {"metrics", metric_facts}};
        report["metrics"] = metric_facts;
        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        if (const auto value = number("rows_read")) {
            dataset["rows_read"] = *value;
        }
        if (const auto value = number("rows_excluded")) {
            dataset["rows_excluded"] = *value;
        }
        nlohmann::json excluded = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                excluded.push_back(log.message.substr(9));
            }
        }
        if (!excluded.empty()) {
            dataset["not_used"] = std::move(excluded);
        }
        report["dataset"] = std::move(dataset);

        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind != "dataset") {
                continue;
            }
            if (output.name == "allocation policy") {
                report["policy_table"] = output.ref;
            }
            if (output.name == "decision propensities") {
                report["decisions_table"] = output.ref;
            }
        }
        report["drivers"] = nlohmann::json::array();
        report["at_risk"] = nlohmann::json::array();
        report["caveats"] = nlohmann::json::array();
        report["missing"] = nlohmann::json::array();
        return ApiResult::success(std::move(report));
    }


    if (model->kind() == "speech") {
        nlohmann::json model_facts = {
            {"id", model->id()}, {"name", model->name()}};
        for (const char* key :
             {"word_error_rate", "word_edits", "reference_words",
              "recordings_read", "recordings_excluded", "segments"}) {
            if (const auto value = number(key)) {
                model_facts[key] = *value;
            }
        }
        report["model"] = std::move(model_facts);

        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        if (const auto value = number("recordings_read")) {
            dataset["recordings_read"] = *value;
        }
        if (const auto value = number("recordings_excluded")) {
            dataset["recordings_excluded"] = *value;
        }
        report["dataset"] = std::move(dataset);

        nlohmann::json outputs = nlohmann::json::array();
        for (const auto& output : run->outputs()) {
            if (output.kind == "dataset") {
                outputs.push_back(output.ref);
            }
        }
        nlohmann::json transcripts = nlohmann::json::array();
        if (const auto table = run_named_table(context.node, *run, "redacted transcripts")) {
            const zetta::storage::StorageEngine& store = table->store();
            for (std::size_t index = 0;
                 index < store.size() && transcripts.size() < limit; ++index) {
                if (const auto row = store.at(index)) {
                    transcripts.push_back(*row);
                }
            }
        }
        report["output_tables"] = std::move(outputs);
        report["transcripts"] = std::move(transcripts);
        return ApiResult::success(std::move(report));
    }


    if (model->kind() == "language") {
        nlohmann::json language_model = {
            {"id", model->id()}, {"name", model->name()}};
        for (const char* key : {"recall_at_k", "mrr", "groundedness",
                                "queries", "answers", "train_rows"}) {
            if (const std::optional<double> value = number(key)) {
                language_model[key] = *value;
            }
        }
        if (const auto version =
                model->feature_schema().find("embedder_version");
            version != model->feature_schema().end() && version->is_string()) {
            language_model["embedder_version"] = *version;
        }
        report["model"] = std::move(language_model);

        nlohmann::json language_dataset = nlohmann::json::object();
        if (const std::optional<double> value = number("dataset_rows")) {
            language_dataset["rows_read"] = *value;
        }
        if (const std::optional<double> value = number("excluded_rows")) {
            language_dataset["rows_excluded"] = *value;
        }
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                language_dataset["source"] = source.substr(8);
            }
        }
        nlohmann::json notes = nlohmann::json::array();
        for (const pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: ")) {
                notes.push_back(log.message.substr(9));
            }
        }
        if (!notes.empty()) {
            language_dataset["notes"] = std::move(notes);
        }
        report["dataset"] = std::move(language_dataset);

        nlohmann::json outputs = nlohmann::json::array();
        for (const pipeline::RunOutput& output : run->outputs()) {
            if (output.kind == "dataset") {
                outputs.push_back(
                    {{"name", output.name}, {"table", output.ref}});
            }
        }
        report["outputs"] = std::move(outputs);
        if (const std::optional<double> value = number("groundedness");
            !value.has_value() || *value < 1.0) {
            report["caveats"] = nlohmann::json::array(
                {{{"field", "groundedness"},
                  {"why", "not every non-refused answer sentence cites a retrieved chunk"}}});
        }
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "vision") {
        nlohmann::json model_facts = {{"id", model->id()},
                                      {"name", model->name()}};
        for (const char* key :
             {"accuracy", "macro_f1", "train_rows", "held_out_rows"}) {
            if (const std::optional<double> value = number(key)) {
                model_facts[key] = *value;
            }
        }
        report["model"] = std::move(model_facts);

        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
                break;
            }
        }
        if (const std::optional<double> value = number("images_read")) {
            dataset["images_read"] = *value;
        }
        if (const std::optional<double> value = number("images_excluded")) {
            dataset["images_excluded"] = *value;
        }
        report["dataset"] = std::move(dataset);

        const nlohmann::json& feature_schema = model->feature_schema();
        if (const auto matrix = feature_schema.find("confusion_matrix");
            matrix != feature_schema.end() && matrix->is_array()) {
            report["confusion_matrix"] = *matrix;
        }

        nlohmann::json output_tables = nlohmann::json::array();
        if (const std::shared_ptr<zetta::data::Table> table =
                run_named_table(context.node, *run, "image predictions")) {
            for (const zetta::pipeline::RunOutput& output : run->outputs()) {
                if (output.kind == "dataset") {
                    output_tables.push_back(output.ref);
                }
            }
            nlohmann::json predictions = nlohmann::json::array();
            const zetta::storage::StorageEngine& store = table->store();
            for (std::size_t index = 0;
                 index < store.size() && predictions.size() < limit; ++index) {
                if (const auto row = store.at(index)) {
                    predictions.push_back(*row);
                }
            }
            report["predictions"] = std::move(predictions);
        }
        report["output_tables"] = std::move(output_tables);
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "graph") {
        nlohmann::json model_facts = {{"id", model->id()},
                                      {"name", model->name()},
                                      {"kind", model->kind()}};
        for (const auto& [key, value] : metrics.items()) {
            if (value.is_number()) model_facts[key] = value;
        }
        report["model"] = std::move(model_facts);

        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) dataset["source"] = source.substr(8);
        }
        if (const auto value = number("edges_read")) dataset["edges_read"] = *value;
        if (const auto value = number("edges_excluded")) {
            dataset["edges_excluded"] = *value;
        }
        if (!dataset.empty()) report["dataset"] = std::move(dataset);

        nlohmann::json tables = nlohmann::json::array();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind != "dataset") continue;
            tables.push_back(output.ref);
            if (output.ref.find(".summary_") == std::string::npos) continue;
            const std::size_t dot = output.ref.find('.');
            if (dot == std::string::npos) continue;
            const std::shared_ptr<zetta::data::Database> database =
                context.node.find_database(output.ref.substr(0, dot));
            if (!database) continue;
            const std::shared_ptr<zetta::data::Table> table =
                database->find_table(output.ref.substr(dot + 1));
            if (!table || table->store().size() == 0) continue;
            if (const auto row = table->store().at(0)) report["graph"] = *row;
        }
        report["output_tables"] = std::move(tables);
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "entity_resolution") {
        nlohmann::json model_facts = {
            {"id", model->id()},
            {"name", model->name()},
            {"kind", model->kind()},
            {"metrics", metrics},
        };
        if (const auto method = model->feature_schema().find("scoring_method");
            method != model->feature_schema().end() && method->is_string()) {
            model_facts["scoring_method"] = *method;
        }
        report["model"] = std::move(model_facts);
        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        if (const auto rows = number("input_rows")) {
            dataset["rows_read"] = *rows;
        }
        if (const auto rows = number("excluded_rows")) {
            dataset["rows_excluded"] = *rows;
        }
        nlohmann::json not_used = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: not used:")) {
                not_used.push_back(log.message.substr(18));
            }
        }
        if (!not_used.empty()) {
            dataset["not_used"] = std::move(not_used);
        }
        report["dataset"] = std::move(dataset);

        nlohmann::json output_tables = nlohmann::json::array();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind == "dataset") {
                output_tables.push_back(
                    {{"name", output.name}, {"table", output.ref}});
            }
        }
        report["output_tables"] = std::move(output_tables);
        report["thresholds"] =
            {{"match", metrics.value("match_threshold", 0.0)},
             {"review", metrics.value("review_threshold", 0.0)},
             {"non_match", "below review"}};
        return ApiResult::success(std::move(report));
    }

    if (model->kind() == "uplift") {
        const bool selected_x = number("selected_learner").value_or(0.0) != 0.0;
        const std::string prefix = selected_x ? "x_" : "t_";
        nlohmann::json model_facts = {
            {"id", model->id()},
            {"name", model->name()},
            {"selected_learner", selected_x ? "x_learner" : "t_learner"},
            {"metrics", metrics}};
        report["model"] = std::move(model_facts);
        report["arm_sizes"] = {
            {"treated", number("treated_rows").value_or(0.0)},
            {"control", number("control_rows").value_or(0.0)},
            {"held_out", number("held_out_rows").value_or(0.0)}};
        report["propensity"] = {
            {"minimum", number("propensity_min").value_or(0.0)},
            {"maximum", number("propensity_max").value_or(0.0)},
            {"mean", number("propensity_mean").value_or(0.0)},
            {"standard_deviation", number("propensity_stddev").value_or(0.0)}};
        report["targeting"] = {
            {"fraction", number("targeting_fraction").value_or(0.0)},
            {"uplift",
             number((prefix + "uplift_at_target").c_str()).value_or(0.0)},
            {"uncertainty_lower", number("uplift_lower").value_or(0.0)},
            {"uncertainty_upper", number("uplift_upper").value_or(0.0)},
            {"overlap_excluded", number("overlap_excluded").value_or(0.0)}};
        report["treatment_effects"] = table_rows(
            run_named_table(context.node, *run, "treatment effects"), limit);
        report["qini_curve"] =
            table_rows(run_named_table(context.node, *run, "Qini curve"));
        nlohmann::json dataset = nlohmann::json::object();
        for (const std::string& source : run->lineage().sources) {
            if (source.starts_with("dataset:")) {
                dataset["source"] = source.substr(8);
            }
        }
        nlohmann::json notes = nlohmann::json::array();
        for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
            if (log.message.starts_with("dataset: ")) {
                notes.push_back(log.message.substr(9));
            }
        }
        if (!notes.empty()) {
            dataset["notes"] = std::move(notes);
        }
        if (!dataset.empty()) {
            report["dataset"] = std::move(dataset);
        }
        if (number("observational").value_or(0.0) != 0.0) {
            report["assumptions"] = nlohmann::json::array(
                {"causal claims assume no unmeasured confounding"});
        }
        return ApiResult::success(std::move(report));
    }

    const bool ranking = model->kind() == "rank";
    const bool is_survival = model->kind() == "survival";
    nlohmann::json model_facts = {{"id", model->id()},
                                  {"name", model->name()},
                                  {"kind", model->kind()}};
    if (is_survival) {
        for (const char* metric :
             {"concordance", "integrated_brier_score", "training_subjects",
              "validation_subjects", "event_subjects", "censored_subjects",
              "interval_width"}) {
            if (const auto value = number(metric)) {
                model_facts[metric] = *value;
            }
        }
    } else if (ranking) {
        if (const auto value = number("ndcg_at_k")) model_facts["ndcg_at_k"] = *value;
        if (const auto value = number("yield_at_k")) model_facts["yield_at_k"] = *value;
        if (const auto value = number("cutoff")) model_facts["cutoff"] = *value;
        if (const auto value = number("held_out_groups")) model_facts["held_out_groups"] = *value;
        if (const auto value = number("held_out_rows")) model_facts["held_out_rows"] = *value;
        if (const auto value = number("train_groups")) model_facts["train_groups"] = *value;
        if (const auto value = number("train_rows")) model_facts["train_rows"] = *value;
    } else if (const auto held = number("held_out_auc")) {
        model_facts["held_out_auc"] = *held;
        if (const auto value = number("held_out_log_loss")) model_facts["held_out_log_loss"] = *value;
        if (const auto value = number("held_out_accuracy")) model_facts["held_out_accuracy"] = *value;
        if (const auto value = number("held_out_rows")) model_facts["held_out_rows"] = *value;
    } else {
        // Said out loud rather than left blank. A report that leads with a
        // held-out figure has to know when there isn't one, or it leads with the
        // in-sample number instead and calls it quality (#953).
        missing.push_back(
            {{"field", "held_out_auc"},
             {"why", "this run set no holdout_fraction, so nothing was measured on "
                     "rows the model had not seen. Every figure below is in-sample "
                     "and flatters. Re-run with holdout_fraction to get one"}});
    }
    if (!ranking && !is_survival) {
        if (const auto value = number("in_sample_auc")) model_facts["in_sample_auc"] = *value;
        else if (const auto legacy = number("auc")) model_facts["in_sample_auc"] = *legacy;
        if (const auto value = number("in_sample_log_loss")) model_facts["in_sample_log_loss"] = *value;
        else if (const auto legacy = number("log_loss")) model_facts["in_sample_log_loss"] = *legacy;
        if (const auto value = number("in_sample_accuracy")) model_facts["in_sample_accuracy"] = *value;
        else if (const auto legacy = number("accuracy")) model_facts["in_sample_accuracy"] = *legacy;
        if (const auto value = number("in_sample_rows")) model_facts["train_rows"] = *value;
        else if (const auto value = number("train_rows")) {
            model_facts["train_rows"] = *value;
        }
    }
    if (const auto value = number("oof_auc")) model_facts["oof_auc"] = *value;
    if (const auto value = number("oof_log_loss")) model_facts["oof_log_loss"] = *value;
    if (const auto value = number("oof_accuracy")) model_facts["oof_accuracy"] = *value;
    if (const auto value = number("oof_rows")) model_facts["oof_rows"] = *value;
    if (const auto value = number("best_single_held_out_auc")) {
        model_facts["best_single_held_out_auc"] = *value;
    }
    if (const auto value = number("held_out_auc_lift_over_best_single")) {
        model_facts["held_out_auc_lift_over_best_single"] = *value;
    }
    if (const auto value = number("meta_model_intercept")) {
        model_facts["meta_model_intercept"] = *value;
    }
    report["model"] = std::move(model_facts);

    nlohmann::json dataset = nlohmann::json::object();
    if (const auto value = number("training_subjects")) {
        dataset["training_subjects"] = *value;
    }
    if (const auto value = number("validation_subjects")) {
        dataset["validation_subjects"] = *value;
    }
    if (const auto value = number("dataset_accounts")) dataset["accounts"] = *value;
    if (const auto value = number("dataset_events")) dataset["events"] = *value;
    if (const auto value = number("dataset_churn_rate")) dataset["churn_rate"] = *value;
    if (const auto value = number("dataset_majority_class_accuracy")) {
        dataset["majority_class_accuracy"] = *value;
    }
    nlohmann::json features = nlohmann::json::array();
    nlohmann::json drivers = nlohmann::json::array();
    for (const zetta::ml::ModelDriver& driver : model->drivers()) {
        drivers.push_back({{"feature", driver.feature}, {"weight", driver.importance}});
        features.push_back(driver.feature);
    }
    if (model->kind() == "ensemble" &&
        model->feature_schema().contains("features") &&
        model->feature_schema().at("features").is_array()) {
        features = model->feature_schema().at("features");
    }
    if (!features.empty()) dataset["features"] = std::move(features);
    if (ranking) {
        const nlohmann::json& schema = model->feature_schema();
        const auto excluded = schema.find("excluded_from_features");
        if (excluded != schema.end() && excluded->is_array()) {
            dataset["excluded"] = *excluded;
        }
    }
    // What the run actually read and what it could not use, from the run itself: its
    // lineage sources and its "dataset: " log notes (a table-backed run, #947).
    const zetta::pipeline::RunLineage& lineage = run->lineage();
    for (const std::string& source : lineage.sources) {
        if (source.starts_with("dataset:")) {
            dataset["source"] = source.substr(8);
            dataset.erase("events");  // a table-backed run has no events shard
        }
    }
    nlohmann::json notes = nlohmann::json::array();
    for (const zetta::pipeline::RunLogEntry& log : run->logs()) {
        if (log.message.starts_with("dataset: not used:")) {
            notes.push_back(log.message.substr(9));
        }
    }
    if (!notes.empty()) dataset["not_used"] = std::move(notes);
    if (!dataset.empty()) report["dataset"] = std::move(dataset);
    else {
        missing.push_back({{"field", "dataset"},
                           {"why", "this run's model records no dataset facts; it "
                                   "predates them being written (#958)"}});
    }
    if (!drivers.empty()) {
        if (model->kind() == "ensemble") {
            report["meta_model_weights"] = drivers;
        }
        report["drivers"] = std::move(drivers);
    }

    nlohmann::json output_tables = nlohmann::json::array();
    for (const zetta::pipeline::RunOutput& output : run->outputs()) {
        if (output.kind == "dataset" && output.ref.contains('.')) {
            output_tables.push_back(output.ref);
        }
    }
    if (!output_tables.empty()) {
        report["output_tables"] = std::move(output_tables);
    }

    if (is_survival) {
        nlohmann::json outputs = nlohmann::json::object();
        for (const zetta::pipeline::RunOutput& output : run->outputs()) {
            if (output.kind != "dataset") {
                continue;
            }
            if (output.name == "survival table") {
                outputs["survival_table"] = output.ref;
            } else if (output.name == "interval calibration") {
                outputs["interval_calibration"] = output.ref;
            }
        }
        report["outputs"] = std::move(outputs);
        if (!missing.empty()) {
            report["missing"] = std::move(missing);
        }
        return ApiResult::success(std::move(report));
    }

    // The ranked list, read back from the table the run materialized — so the
    // report and anything querying `churn_results` cannot disagree.
    if (is_survival) {
        // Survival runs expose their horizon tables above, not a churn ranking.
    } else if (const std::shared_ptr<zetta::data::Table> table =
            model->kind() == "ensemble"
                ? run_named_table(context.node, *run, "blended scores")
                : run_result_table(context.node, *run)) {
        std::vector<nlohmann::json> rows;
        const zetta::storage::StorageEngine& store = table->store();
        for (std::size_t index = 0; index < store.size(); ++index) {
            if (const auto row = store.at(index)) rows.push_back(*row);
        }
        std::sort(rows.begin(), rows.end(),
                  [](const nlohmann::json& left, const nlohmann::json& right) {
                      return row_number(left, "rank", 0.0) <
                             row_number(right, "rank", 0.0);
                  });
        nlohmann::json ranked_queue = nlohmann::json::array();
        nlohmann::json at_risk = nlohmann::json::array();
        for (const nlohmann::json& row : rows) {
            if (ranking) {
                if (ranked_queue.size() >= limit) break;
                ranked_queue.push_back(
                    {{"group", row.value("group", std::int64_t{0})},
                     {"entity", row.value("entity", std::int64_t{0})},
                     {"score", row_number(row, "score", 0.0)},
                     {"rank", static_cast<std::int64_t>(row_number(row, "rank", 0.0))},
                     {"relevance", row_number(row, "relevance", 0.0)}});
                continue;
            }
            if (at_risk.size() >= limit) break;
            // `value<T>` deduces T from the DEFAULT, so a bare `0` picks `int`
            // and silently narrows the entity key to 32 bits (#1000). Entity
            // keys are int64 the whole way through, and a derived key — the
            // documented remedy for a string business id (#957) — is above
            // 2^31 about half the time, which made the ranked list a customer
            // is shown disagree with this node's own SQL table. `row_entity`
            // keeps the int64, and reads without throwing (#1007).
            nlohmann::json entry = {
                {"rank", static_cast<std::int64_t>(row_number(row, "rank", 0.0))},
                {"entity", row_entity(row)},
                {"risk", row_number(row, "risk", 0.0)}};
            // Evidence is whatever columns beyond the fixed ones the run wrote —
            // the population's strongest drivers, the same columns on every row.
            nlohmann::json evidence = nlohmann::json::object();
            for (const auto& [key, value] : row.items()) {
                if (key == "rank" || key == "account" || key == "risk" ||
                    key == "dominant_driver" || key == "driver_effect" || key == "id") {
                    continue;
                }
                evidence[key] = value;
            }
            if (!evidence.empty()) entry["evidence"] = std::move(evidence);
            at_risk.push_back(std::move(entry));
        }
        if (ranking) {
            report["ranked_queue"] = std::move(ranked_queue);
        } else {
            report["at_risk"] = std::move(at_risk);
        }
    } else {
        if (ranking) {
            missing.push_back({{"field", "ranked_queue"},
                               {"why", "this run materialized no ranked-queue table"}});
        } else {
            missing.push_back({{"field", "at_risk"},
                               {"why", "this run materialized no at-risk table"}});
        }
    }

    // Figures that are PRESENT but must not be read as results (#948 step 8's rule,
    // applied to the report itself). `missing` says what the run cannot tell you;
    // this says what it can tell you and you should not believe. The checks are
    // held_out_caveats(), shared with model_health (#1224).
    nlohmann::json caveats = nlohmann::json::array();
    for (const HeldOutCaveat& caveat :
         held_out_caveats(number("held_out_rows"), number("held_out_auc"), ranking)) {
        caveats.push_back({{"field", "held_out_auc"}, {"why", caveat.why}});
    }
    if (!caveats.empty()) report["caveats"] = std::move(caveats);
    if (!missing.empty()) report["missing"] = std::move(missing);
    return ApiResult::success(std::move(report));
}

const nlohmann::json& get_model_schema() {
    static const nlohmann::json schema = id_input_with_entity_keys_schema(
        "model_id", "the model's uuid (system.models.model_id)");
    return schema;
}

const nlohmann::json& get_model_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::ml::model_value_schema());
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
    // feature_schema.entity_key_mapping: a count unless asked for (#1202).
    if (!input.value("include_entity_keys", false)) {
        zetta::data::summarize_entity_key_mappings(output);
    }
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_monitor_schema() {
    static const nlohmann::json schema = id_input_schema(
        "solution",
        "the solution name or monitor uuid");
    return schema;
}

const nlohmann::json& get_monitor_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(
            zetta::solutions::monitor_value_schema());
    return schema;
}

ApiResult handle_get_monitor(const nlohmann::json& input,
                             ApiContext& context) {
    const std::string solution = input.at("solution").get<std::string>();
    const auto monitor = context.node.find_monitor(solution);
    if (!monitor) {
        return ApiResult::failure(
            "not_found",
            "no monitor for solution or id '" + solution + "'");
    }
    nlohmann::json output = monitor->metadata();
    output["id"] = monitor->id();
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_roi_readout_schema() {
    static const nlohmann::json schema = id_input_schema(
        "solution",
        "the solution name or ROI readout definition uuid");
    return schema;
}

ApiResult handle_get_roi_readout(
    const nlohmann::json& input,
    ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure(
            "engine_error",
            "ROI readouts require a replicated node");
    }
    const std::string solution = input.at("solution").get<std::string>();
    std::string error;
    const auto readout = zetta::solutions::generate_roi_readout(
        context.node,
        *context.node.replication(),
        solution,
        &error);
    if (!readout) {
        return ApiResult::failure("not_found", error);
    }
    return ApiResult::success(zetta::solutions::to_json(*readout));
}

// --- model_health (Monitor, #983) -------------------------------------------------------------
//
// The first Monitor capability. The agent is meant to watch a deployed model and say whether it
// can still be trusted, and until now it had no tool that answers that — so it could only
// describe monitoring, which is #976 at the methodology level.
//
// Deliberately computed from what a Model record ALREADY carries. A drift capability needing a
// stored reference distribution is a schema change and a migration; this needs neither, and the
// question it answers is the one asked first: **is the number I was given honest, and is it still
// the number?**
const nlohmann::json& model_health_schema() {
    static const nlohmann::json schema =
        id_input_schema("model_id", "the model's uuid (system.models.model_id)");
    return schema;
}

const nlohmann::json& model_health_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "trustworthy", "findings"}},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"trustworthy", {{"type", "boolean"}}},
          {"held_out_auc", {{"type", "number"}}},
          {"in_sample_auc", {{"type", "number"}}},
          {"overfit_gap", {{"type", "number"}}},
          {"calibrated", {{"type", "boolean"}}},
          {"findings", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}};
    return schema;
}

ApiResult handle_model_health(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) return ApiResult::failure("not_found", "no model with id '" + id + "'");

    const nlohmann::json metadata = model->metadata();
    const nlohmann::json metrics =
        metadata.contains("metrics") ? metadata.at("metrics") : nlohmann::json::object();
    const auto number = [&metrics](const char* key) -> std::optional<double> {
        const auto entry = metrics.find(key);
        if (entry == metrics.end() || !entry->is_number()) return std::nullopt;
        return entry->get<double>();
    };

    nlohmann::json output;
    output["model_id"] = id;
    std::vector<std::string> findings;
    bool trustworthy = true;

    const std::optional<double> held_out = number("held_out_auc");
    const std::optional<double> in_sample = number("in_sample_auc");
    if (held_out) output["held_out_auc"] = *held_out;
    if (in_sample) output["in_sample_auc"] = *in_sample;

    if (!held_out) {
        // The single most important thing this can say. Without a holdout the only figure is
        // measured on the rows the model was fit on, and it flatters — an in-sample 0.868 reached
        // a client report once where the held-out figure was 0.726.
        trustworthy = false;
        findings.push_back(
            "No held-out metric. This model was trained without a holdout, so every quality "
            "figure it carries was measured on the rows it learned from and cannot be quoted. "
            "Retrain with holdout_fraction set.");
    } else if (in_sample) {
        const double gap = *in_sample - *held_out;
        output["overfit_gap"] = gap;
        if (gap > 0.1) {
            trustworthy = false;
            findings.push_back(
                "In-sample exceeds held-out by " + std::to_string(gap) +
                ". The model has memorized rather than generalized; the held-out figure is the "
                "only one that describes new accounts.");
        }
    }

    // The run report's held-out caveats, from the same function (#1224): a figure the
    // report tells a reader not to quote cannot come from a model this calls trustworthy.
    for (const HeldOutCaveat& caveat : held_out_caveats(
             number("held_out_rows"), held_out, model->kind() == "rank")) {
        trustworthy = false;
        findings.push_back("Held-out AUC caveat (" + caveat.check + "): " + caveat.why + ".");
    }

    const auto calibrated = metadata.find("calibrated");
    const bool is_calibrated =
        calibrated != metadata.end() && calibrated->is_boolean() && calibrated->get<bool>();
    output["calibrated"] = is_calibrated;
    if (!is_calibrated) {
        // Not disqualifying — ranking is a legitimate use — but a threshold set on an
        // uncalibrated score means something different from what the person setting it expects.
        findings.push_back(
            "Scores are not calibrated: they rank accounts but are not probabilities, so a "
            "threshold like 0.8 does not mean an 80% chance.");
    }

    output["trustworthy"] = trustworthy;
    output["findings"] = findings;
    return ApiResult::success(std::move(output));
}

// --- propose_improvement (Improve, #983) ------------------------------------------------------
//
// The Improve verb, and the one property that makes it gradeable: a proposal MUST carry its
// predicted effect. Kernel D7-2 — the metric the customer pays for is the metric the agent learns
// on — so "retrain it" with no predicted effect is not a proposal, it is a suggestion, and
// nothing can score it or hold it to account afterwards.
//
// It refuses rather than guesses. A proposal whose effect cannot be predicted from what the model
// actually carries returns `predictable: false` with the reason, because an invented number here
// would be worse than silence: it would be a number the customer plans against.
const nlohmann::json& propose_improvement_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "change"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}, {"minLength", 1}}},
          {"change",
           {{"type", "string"},
            {"enum", {"add_holdout", "calibrate", "retrain"}},
            {"description", "the change being proposed"}}}}}};
    return schema;
}

const nlohmann::json& propose_improvement_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "change", "predictable", "rationale"}},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"change", {{"type", "string"}}},
          {"predictable", {{"type", "boolean"}}},
          {"predicted_effect", {{"type", "string"}}},
          {"rationale", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_propose_improvement(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const std::string change = input.at("change").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) return ApiResult::failure("not_found", "no model with id '" + id + "'");

    const nlohmann::json metadata = model->metadata();
    const nlohmann::json metrics =
        metadata.contains("metrics") ? metadata.at("metrics") : nlohmann::json::object();
    const auto number = [&metrics](const char* key) -> std::optional<double> {
        const auto entry = metrics.find(key);
        if (entry == metrics.end() || !entry->is_number()) return std::nullopt;
        return entry->get<double>();
    };

    nlohmann::json output;
    output["model_id"] = id;
    output["change"] = change;

    const std::optional<double> held_out = number("held_out_auc");
    const std::optional<double> in_sample = number("in_sample_auc");

    if (change == "add_holdout") {
        if (held_out) {
            output["predictable"] = false;
            output["rationale"] =
                "This model already has a held-out figure, so adding a holdout changes nothing.";
        } else if (in_sample) {
            // The one effect that IS predictable, because it is not a guess about the model — it
            // is a statement about what the current number means.
            output["predictable"] = true;
            output["predicted_effect"] =
                "The reported quality will FALL, and that fall is the point: the current figure "
                "of " + std::to_string(*in_sample) +
                " was measured on the rows the model was fit on. A held-out figure describes "
                "accounts it has never seen, and it is the only one that can be quoted.";
            output["rationale"] =
                "Retrain with holdout_fraction set. Expect a lower number and a truthful one.";
        } else {
            output["predictable"] = false;
            output["rationale"] =
                "This model carries no quality figure at all, so there is nothing to compare a "
                "holdout against.";
        }
    } else if (change == "calibrate") {
        output["predictable"] = false;
        output["rationale"] =
            "Calibration changes what the scores MEAN, not how well they rank, so the ranking "
            "metric will not move. Whether the resulting probabilities are honest can only be "
            "measured after fitting the calibrator on held-out rows.";
    } else {
        // "retrain" with no stated reason. Refusing here is the point: an agent that predicts an
        // improvement from a retrain it has no evidence for is the failure this capability
        // exists to prevent.
        output["predictable"] = false;
        output["rationale"] =
            "A retrain with no change to the data or the pipeline has no predictable effect. Say "
            "what would differ - more history, a new feature, a different cutoff - and the effect "
            "of THAT can be reasoned about.";
    }
    return ApiResult::success(std::move(output));
}

const nlohmann::json& get_job_schema() {
    static const nlohmann::json schema =
        id_input_schema("job_id", "the compute job's uuid (system.jobs.job_id)");
    return schema;
}

const nlohmann::json& get_job_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::compute::compute_job_value_schema());
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

const nlohmann::json& get_index_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::index::index_value_schema());
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

// A dataset's typed columns as get_dataset answers them (#1021):
// `[{name, type, nullable, null_fraction}]`, `null_fraction` null when unknown.
// A column the caller may not read under `sql:read` keeps its name and answers
// `type`, `nullable` and `null_fraction` as null — the way SQL masks it (the
// column stays in `SELECT *`, its values read as null), so nothing about its
// values reaches a caller who cannot read them.
nlohmann::json typed_columns_json(const zetta::data::DatasetSummary& dataset,
                                  const zetta::governance::ColumnAccess& access) {
    nlohmann::json columns = nlohmann::json::array();
    for (const zetta::data::DatasetColumn& column : dataset.typed_columns) {
        if (!access.allows(column.name)) {
            columns.push_back({{"name", column.name},
                               {"type", nullptr},
                               {"nullable", nullptr},
                               {"null_fraction", nullptr}});
            continue;
        }
        columns.push_back(
            {{"name", column.name},
             {"type", column.type},
             {"nullable", column.nullable},
             {"null_fraction", column.null_fraction ? nlohmann::json(*column.null_fraction)
                                                    : nlohmann::json()}});
    }
    return columns;
}

// A DatasetSummary as JSON — the full computed shape, with the per-partition
// placement kept structured (the SQL view flattens it to strings for tabular
// reading; the tool keeps the arrays an agent can walk). `columns` is the names
// in order (list_datasets); get_dataset replaces it with the typed columns.
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
                          {"source_format", dataset.source_format},
                          {"replication", dataset.replication},
                          {"rows", dataset.rows},
                          {"size_bytes", dataset.size_bytes},
                          {"columns", dataset.columns},
                          // #1021: "source" when the columns follow the file's
                          // or query's own order, "first_seen" when none was
                          // recorded (a dataset loaded before it was kept).
                          {"column_order",
                           dataset.column_order_recorded ? "source" : "first_seen"},
                          {"partitions", std::move(partitions)},
                          // #837: whether this node can see the WHOLE dataset.
                          // An agent that reads `rows` must be able to tell a
                          // total from a fraction.
                          {"degraded", dataset.degraded},
                          {"degraded_reason", dataset.degraded_reason},
                          {"expected_partitions", dataset.expected_partitions},
                          {"expected_rows", dataset.expected_rows}};
}

// The schema of dataset_summary_json — list_datasets' array items, and with
// `columns` typed (get_dataset_output_schema below) get_dataset's value.
const nlohmann::json& dataset_summary_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"name", "database", "table_id", "format", "source_format", "replication", "rows",
          "size_bytes", "columns", "column_order", "partitions", "degraded",
          "degraded_reason", "expected_partitions", "expected_rows"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"database", {{"type", "string"}}},
          {"table_id", {{"type", "string"}}},
          {"format",
           {{"type", "string"},
            {"description", "how the rows are stored (jsonl); not the upload's format"}}},
          {"source_format",
           {{"type", "string"},
            {"description",
             "what the rows were loaded from: csv, jsonl, json, parquet, a connector "
             "type (postgres, snowflake, mysql) or rows; empty for a table not loaded from a "
             "source (SQL, engine results)"}}},
          {"replication",
           {{"type", "string"},
            {"description",
             "replicate_to_all, partitioned or partitioned_with_replicas"}}},
          {"rows", {{"type", "integer"}}},
          {"size_bytes", {{"type", "integer"}}},
          {"columns",
           {{"type", "array"},
            {"description",
             "the column names, in the source's order when column_order is "
             "\"source\" (#1021)"},
            {"items", {{"type", "string"}}}}},
          {"column_order",
           {{"type", "string"},
            {"enum", {"source", "first_seen"}},
            {"description",
             "source: columns follow the file's or query's own order (the CSV "
             "header, the Parquet schema, the first JSON row, the SELECT list); "
             "first_seen: no order was recorded (a dataset loaded before #1021), "
             "so the columns come in the order the stored rows present them, "
             "which is alphabetical"}}},
          {"partitions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"partition_id", "blob_ref", "row_start", "row_end",
                "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"blob_ref", {{"type", "string"}}},
                {"row_start", {{"type", "integer"}}},
                {"row_end", {{"type", "integer"}}},
                {"owner_nodes",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}}}}}}}}}},
          {"degraded", {{"type", "boolean"}}},
          {"degraded_reason", {{"type", "string"}}},
          {"expected_partitions", {{"type", "integer"}}},
          {"expected_rows", {{"type", "integer"}}}}}};
    return schema;
}

// get_dataset's value: the summary with `columns` typed (#1021).
const nlohmann::json& get_dataset_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json out = dataset_summary_schema();
        out["properties"]["columns"] = {
            {"type", "array"},
            {"description",
             "the dataset's columns, in the source's order when column_order is "
             "\"source\" (#1021), each with the JSON type its values share and "
             "the share of rows that hold null for it or omit it"},
            {"items",
             {{"type", "object"},
              {"required", {"name", "type", "nullable", "null_fraction"}},
              {"additionalProperties", false},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"type",
                 {{"type", {"string", "null"}},
                  {"enum",
                   {"boolean", "integer", "number", "string", "object", "array", "null",
                    "mixed", "unknown", nullptr}},
                  {"description",
                   "the JSON type the column's values share: integer widens to "
                   "number when both occur, mixed when the values differ otherwise, "
                   "\"null\" when every value is null, unknown for a partition "
                   "written before types were recorded; JSON null when the caller "
                   "may not read the column (sql:read)"}}},
                {"nullable",
                 {{"type", {"boolean", "null"}},
                  {"description", "null when the caller may not read the column"}}},
                {"null_fraction",
                 {{"type", {"number", "null"}},
                  {"minimum", 0},
                  {"maximum", 1},
                  {"description",
                   "nulls (or missing cells) over rows; null when unknown (a "
                   "partition-only dataset written before null counts were "
                   "recorded) or when the caller may not read the column"}}}}}}}};
        return out;
    }();
    return schema;
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
            {"description",
             "the dataset name (= the table name), or database.table as "
             "analyze_dataset takes it"}}},
          {"database",
           {{"type", "string"},
            {"description", "the owning database — required only when the "
                            "same dataset name exists in several databases"}}}}}};
    return schema;
}

ApiResult handle_get_dataset(const nlohmann::json& input, ApiContext& context) {
    std::string name = input.at("dataset").get<std::string>();
    std::string database = input.value("database", "");
    // `database.table`, the form analyze_dataset and run's dataset_path take
    // (#1221). An exact name is tried first, so a table whose own name holds a
    // dot is still found by it.
    if (const std::size_t dot = name.find('.'); dot != std::string::npos) {
        const bool exact = std::ranges::any_of(
            context.node.database_names(), [&](const std::string& each) {
                const auto found = context.node.find_database(each);
                return found && found->find_table(name);
            });
        if (!exact) {
            const std::string named_database = name.substr(0, dot);
            if (!database.empty() && database != named_database) {
                return ApiResult::failure(
                    "invalid_input",
                    "dataset '" + name + "' names database '" + named_database +
                        "', but database is '" + database + "'",
                    "/database");
            }
            database = named_database;
            name = name.substr(dot + 1);
        }
    }
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
    nlohmann::json value = dataset_summary_json(matches.front());
    // Column-level read access (#747), decided as the SQL executor decides it.
    const zetta::governance::ColumnAccess access = context.node.column_scope(
        context.acting_key, context.acting_is_user,
        Operation{zetta::governance::CapabilityDomain::Data, "read"},
        matches.front().database, matches.front().name);
    value["columns"] = typed_columns_json(matches.front(), access);
    return ApiResult::success(std::move(value));
}

ApiResult handle_get_run_report(const nlohmann::json& input, ApiContext& context) {
    auto result = assemble_run_report(input, context);
    if (!result.ok) {
        return result;
    }
    const auto run = context.node.find_run(input.at("run_id").get<std::string>());
    for (const auto& output : run->outputs()) {
        if (output.kind != "model") {
            continue;
        }
        if (const auto model = context.node.find_model(output.ref)) {
            result.value["features"] = zetta::ml::run_feature_report(*run, *model);
            break;
        }
    }
    return result;
}

// --- analyze_dataset / plan_features (#1062) --------------------------------

const nlohmann::json& dataset_reference_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"oneOf", {{{"required", {"dataset"}}},
                   {{"required", {"dataset_path"}}}}},
        {"properties",
         {{"dataset",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a dataset name, or database.table; a bare name must be unique"}}},
          {"dataset_path",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the canonical dataset_path grammar; this capability reads "
                            "landed database.table datasets"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "database for a bare dataset name"}}}}}};
    return schema;
}

nlohmann::json analysis_options_schema() {
    return nlohmann::json{
        {"sample_rows",
         {{"type", "integer"},
          {"minimum", 1},
          {"maximum", 10000},
          {"description", "maximum rows sampled for evidence (default 256)"}}},
        {"label",
         {{"type", "string"},
          {"minLength", 1},
          {"description", "label column for single-feature leakage checks"}}},
        {"time_column",
         {{"type", "string"},
          {"minLength", 1},
          {"description", "declared observation/event time column"}}},
        {"as_of",
         {{"oneOf", {{{"type", "string"}}, {{"type", "number"}}}},
          {"description", "point-in-time cutoff for later-timestamp leakage checks"}}}};
}

const nlohmann::json& analyze_dataset_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json result = dataset_reference_schema();
        const nlohmann::json options = analysis_options_schema();
        for (const auto& [name, property] : options.items()) {
            result["properties"][name] = property;
        }
        return result;
    }();
    return schema;
}

nlohmann::json column_analysis_schema() {
    return nlohmann::json{
        {"type", "object"},
        {"required",
         {"name", "semantic_type", "evidence", "role_candidates", "null_count",
          "null_rate", "distinct_count", "distinct_ratio", "top_values", "quantiles",
          "string_lengths", "sentinel_values"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"semantic_type",
           {{"type", "string"},
            {"enum", {"boolean", "datetime", "free_text", "identifier",
                      "categorical", "numeric", "json", "unknown"}}}},
          {"evidence", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"role_candidates", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"null_count", {{"type", "integer"}}},
          {"null_rate", {{"type", "number"}}},
          {"distinct_count", {{"type", "integer"}}},
          {"distinct_ratio", {{"type", "number"}}},
          {"top_values", {{"type", "array"}}},
          {"quantiles", {{"type", "object"}}},
          {"string_lengths", {{"type", "object"}}},
          {"sentinel_values", {{"type", "array"}}}}}};
}

const nlohmann::json& analyze_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"table_id", "database", "table", "rows", "sampled_rows", "columns",
          "grain", "grain_evidence", "candidate_keys", "joinable_tables",
          "leakage_candidates"}},
        {"additionalProperties", false},
        {"properties",
         {{"table_id", {{"type", "string"}}},
          {"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"rows", {{"type", "integer"}}},
          {"sampled_rows", {{"type", "integer"}}},
          {"columns", {{"type", "array"}, {"items", column_analysis_schema()}}},
          {"grain",
           {{"type", "string"},
            {"enum", {"entity", "event", "time_series", "snapshot", "ambiguous"}}}},
          {"grain_evidence", {{"type", "string"}}},
          {"candidate_keys", {{"type", "array"}}},
          {"joinable_tables", {{"type", "array"}}},
          {"leakage_candidates", {{"type", "array"}}}}}};
    return schema;
}

std::optional<zetta::data::DatasetPath> capability_dataset_path(
    const nlohmann::json& input, ApiContext& context, std::string& error) {
    if (input.contains("dataset_path")) {
        zetta::data::DatasetPath path = zetta::data::parse_dataset_path(
            input.at("dataset_path").get<std::string>());
        if (path.kind == zetta::data::DatasetPathKind::Directory) {
            error = "dataset_path: analysis reads landed database.table datasets, not a "
                    "filesystem directory";
            return std::nullopt;
        }
        return path;
    }

    const std::string dataset = input.at("dataset").get<std::string>();
    if (dataset.find('.') != std::string::npos) {
        return zetta::data::parse_dataset_path(dataset);
    }
    const std::string requested_database = input.value("database", "");
    std::vector<zetta::data::DatasetSummary> matches;
    for (zetta::data::DatasetSummary& summary : context.node.datasets()) {
        if (summary.name != dataset) {
            continue;
        }
        if (!requested_database.empty() && summary.database != requested_database) {
            continue;
        }
        // #1396: a table the caller may not read is not a match — it neither
        // resolves the name nor makes it ambiguous, as if it did not exist.
        if (!context.acting_key.empty() &&
            !context.node
                 .column_scope(context.acting_key, context.acting_is_user,
                               Operation{zetta::governance::CapabilityDomain::Data, "read"},
                               summary.database, summary.name)
                 .table_allowed) {
            continue;
        }
        matches.push_back(std::move(summary));
    }
    if (matches.empty()) {
        error = "dataset: no dataset named '" + dataset + "'";
        return std::nullopt;
    }
    if (matches.size() > 1) {
        error = "dataset: '" + dataset +
                "' exists in several databases; pass database or database.table";
        return std::nullopt;
    }
    return zetta::data::DatasetPath{zetta::data::DatasetPathKind::Table, {},
                                    matches.front().database, matches.front().name};
}

std::optional<std::string> as_of_string(const nlohmann::json& input) {
    const auto found = input.find("as_of");
    if (found == input.end()) {
        return std::nullopt;
    }
    return found->is_string() ? std::optional<std::string>{found->get<std::string>()}
                              : std::optional<std::string>{found->dump()};
}

ApiResult handle_analyze_dataset(const nlohmann::json& input, ApiContext& context) {
    std::string error;
    const auto path = capability_dataset_path(input, context, error);
    if (!path) {
        return ApiResult::failure("invalid_input", error);
    }
    zetta::data::AnalysisOptions options;
    options.sample_rows = input.value("sample_rows", options.sample_rows);
    options.label_column = input.value("label", "");
    options.time_column = input.value("time_column", "");
    options.as_of = as_of_string(input);
    // #1300, #1396: read as the caller — its table and column grants and its row
    // security, as its own SQL would be — never as the trusted local session.
    const zetta::data::DatasetAnalysis analysis = zetta::data::analyze_dataset_as(
        context.node, *path, options, {context.acting_key, context.acting_is_user},
        context.cluster);
    if (!analysis.ok()) {
        return ApiResult::failure("engine_error", analysis.error);
    }
    return ApiResult::success(zetta::data::to_json(analysis));
}

const nlohmann::json& plan_features_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json result = dataset_reference_schema();
        result["properties"]["methodology"] = {
            {"type", "string"},
            {"minLength", 1},
            {"description", "methodology string such as M-1, segmentation, or forecast"}};
        result["properties"]["group_column"] = {
            {"type", "string"},
            {"minLength", 1},
            {"description", "ranking query/session group column"}};
        const nlohmann::json options = analysis_options_schema();
        for (const auto& [name, property] : options.items()) {
            result["properties"][name] = property;
        }
        for (nlohmann::json& choice : result["oneOf"]) {
            choice["required"].push_back("methodology");
        }
        return result;
    }();
    return schema;
}

const nlohmann::json& plan_features_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"status", "dataset", "methodology", "analysis", "entries", "excluded",
          "transform_required"}},
        {"additionalProperties", false},
        {"properties",
         {{"status", {{"type", "string"}, {"enum", {"ready", "transform_required"}}}},
          {"dataset", {{"type", "string"}}},
          {"methodology", {{"type", "string"}}},
          {"analysis", analyze_dataset_output_schema()},
          {"entries",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"source_columns", "features", "transform", "reason"}},
              {"additionalProperties", false},
              {"properties",
               {{"source_columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"features", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"transform", {{"type", "string"}}},
                {"reason", {{"type", "string"}}}}}}}}},
          {"excluded", {{"type", "array"}}},
          {"transform_required", {{"type", {"object", "null"}}}}}}};
    return schema;
}

ApiResult handle_plan_features(const nlohmann::json& input, ApiContext& context) {
    std::string error;
    const auto path = capability_dataset_path(input, context, error);
    if (!path) {
        return ApiResult::failure("invalid_input", error);
    }
    zetta::data::FeaturePlanOptions options;
    options.methodology = input.at("methodology").get<std::string>();
    options.label_column = input.value("label", "");
    options.time_column = input.value("time_column", "");
    options.group_column = input.value("group_column", "");
    options.as_of = as_of_string(input);
    options.sample_rows = input.value("sample_rows", options.sample_rows);
    // #1396: planned from what the caller may read, like analyze_dataset.
    const zetta::data::FeaturePlan plan = zetta::data::plan_features_as(
        context.node, *path, options, {context.acting_key, context.acting_is_user},
        context.cluster);
    if (!plan.ok()) {
        return ApiResult::failure("engine_error", plan.error);
    }
    return ApiResult::success(zetta::data::to_json(plan));
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

const nlohmann::json& list_runs_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"runs"}},
        {"additionalProperties", false},
        {"properties",
         {{"runs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "project", "graph", "project_name", "pipeline_name",
                "deployment", "status", "metric_summary", "created", "updated",
                "log_count", "output_count"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"project_name", {{"type", "string"}}},
                {"pipeline_name", {{"type", "string"}}},
                {"deployment", {{"type", "string"}}},
                {"status", {{"type", "string"}}},
                {"metric_summary",
                 zetta::pipeline::run_value_schema()
                     .at("properties")
                     .at("metrics")
                     .at("properties")
                     .at("summary")},
                {"metrics_suppressed", {{"type", "string"}}},
                {"created", {{"type", "string"}}},
                {"updated", {{"type", "string"}}},
                {"log_count", {{"type", "integer"}}},
                {"output_count", {{"type", "integer"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_runs(const nlohmann::json&, ApiContext& context) {
    nlohmann::json runs = nlohmann::json::array();
    for (const auto& run : context.node.runs()) {
        nlohmann::json item = {{"id", run->id()},
                               {"project", run->project()},
                               {"graph", run->graph()},
                               {"project_name", run->project_name()},
                               {"pipeline_name", run->pipeline_name()},
                               {"deployment", run->deployment()},
                               {"status", std::string(to_string(run->status()))},
                               {"metric_summary", run->value().metrics.at("summary")},
                               {"created", run->created()},
                               {"updated", run->updated()},
                               {"log_count", run->logs().size()},
                               {"output_count", run->outputs().size()}};
        if (!run->value().metrics_suppressed.empty()) {
            item["metrics_suppressed"] = run->value().metrics_suppressed;
        }
        runs.push_back(std::move(item));
    }
    nlohmann::json output;
    output["runs"] = std::move(runs);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_models_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_models_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"models"}},
        {"additionalProperties", false},
        {"properties",
         {{"models",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "name", "kind", "project", "project_name",
                "pipeline_name", "run", "run_id", "metrics", "feature_schema",
                "exposed", "created", "has_weights"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"kind", {{"type", "string"}}},
                {"project", {{"type", "string"}}},
                {"project_name", {{"type", "string"}}},
                {"pipeline_name", {{"type", "string"}}},
                {"run", {{"type", "string"}}},
                {"run_id", {{"type", "string"}}},
                {"metrics",
                 {{"type", "object"},
                  {"additionalProperties", {{"type", "number"}}}}},
                {"feature_schema", {{"type", "object"}}},
                {"exposed", {{"type", "boolean"}}},
                {"created", {{"type", "string"}}},
                {"has_weights", {{"type", "boolean"}}},
                {"standalone",
                 {{"type", "boolean"},
                  {"description",
                   "made on its own (train_model, register_model), not by a "
                   "project's pipeline; its project is internal and never listed "
                   "(lake#654)"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_models(const nlohmann::json&, ApiContext& context) {
    nlohmann::json models = nlohmann::json::array();
    for (const auto& model : context.node.models()) {
        // A list never carries a mapping's entries (#1202): get_model with
        // include_entity_keys reads them for one model.
        nlohmann::json feature_schema = model->feature_schema();
        zetta::data::summarize_entity_key_mappings(feature_schema);
        models.push_back({{"id", model->id()},
                          {"name", model->name()},
                          {"kind", model->kind()},
                          {"project", model->project()},
                          {"project_name", model->project_name()},
                          {"pipeline_name", model->pipeline_name()},
                          {"run", model->run()},
                          {"run_id", model->run_id()},
                          {"metrics", model->metrics()},
                          {"feature_schema", std::move(feature_schema)},
                          {"exposed", model->exposed()},
                          {"created", model->created()},
                          {"has_weights", model->has_weights()}});
        if (const auto project = model->project().empty()
                                     ? nullptr
                                     : context.node.project_by_id(model->project())) {
            models.back()["standalone"] = project->internal();
        } else {
            models.back()["standalone"] = false;
        }
    }
    nlohmann::json output;
    output["models"] = std::move(models);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& list_jobs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_jobs_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"jobs"}},
        {"additionalProperties", false},
        {"properties",
         {{"jobs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "kind", "status", "project", "graph", "run", "created",
                "updated", "detail"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"kind", {{"type", "string"}}},
                {"status", {{"type", "string"}}},
                {"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"run", {{"type", "string"}}},
                {"created", {{"type", "string"}}},
                {"updated", {{"type", "string"}}},
                {"detail", {{"type", "string"}}}}}}}}}}}};
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

const nlohmann::json& list_indexes_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"indexes"}},
        {"additionalProperties", false},
        {"properties",
         {{"indexes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "table", "kind", "backend", "fields",
                            "created"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"table", {{"type", "string"}}},
                {"kind", {{"type", "string"}}},
                {"backend", {{"type", "string"}}},
                {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"created", {{"type", "string"}}}}}}}}}}}};
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
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"detail",
           {{"type", "string"},
            {"enum", {"full", "summary"}},
            {"default", "full"},
            {"description",
             "full (the default): every dataset's whole summary, as before — "
             "columns, partition placement, and size_bytes measured from the "
             "files. summary (#1195): only what a dataset list shows, from "
             "counters the engine keeps, reading no row and no file; columns, "
             "types and placement stay on get_dataset"}}}}}};
    return schema;
}

// A dataset as list_datasets' `detail: "summary"` answers it (#1195): the
// display fields of the full form, unchanged in name and meaning, plus
// partition_count in place of the placement array.
nlohmann::json dataset_list_summary_json(const zetta::data::DatasetSummary& dataset) {
    return nlohmann::json{{"name", dataset.name},
                          {"database", dataset.database},
                          {"table_id", dataset.table_id},
                          {"format", dataset.format},
                          {"source_format", dataset.source_format},
                          {"replication", dataset.replication},
                          {"rows", dataset.rows},
                          {"size_bytes", dataset.size_bytes},
                          {"column_order",
                           dataset.column_order_recorded ? "source" : "first_seen"},
                          {"partition_count", dataset.partition_count},
                          {"degraded", dataset.degraded},
                          {"degraded_reason", dataset.degraded_reason},
                          {"expected_partitions", dataset.expected_partitions},
                          {"expected_rows", dataset.expected_rows}};
}

const nlohmann::json& dataset_list_summary_schema() {
    static const nlohmann::json schema = [] {
        const nlohmann::json& full = dataset_summary_schema().at("properties");
        nlohmann::json properties = nlohmann::json::object();
        for (const char* name :
             {"name", "database", "table_id", "format", "source_format", "replication",
              "rows", "size_bytes", "column_order", "degraded", "degraded_reason",
              "expected_partitions", "expected_rows"}) {
            properties[name] = full.at(name);
        }
        properties["rows"]["description"] =
            "the row count: the local store's, or for a partition-only dataset the "
            "sum of the partition catalog's row ranges (a fraction when degraded)";
        properties["size_bytes"]["description"] =
            "bytes of row data in the table's chunk files, from the engine's own "
            "counter: equal to the full form's measured value once pending writes "
            "are flushed";
        properties["partition_count"] = {
            {"type", "integer"},
            {"minimum", 0},
            {"description",
             "the partition catalog entries this node sees (0 = not partitioned); "
             "the full form's partitions array has this many entries"}};
        nlohmann::json required = nlohmann::json::array();
        for (const auto& [name, _] : properties.items()) required.push_back(name);
        return nlohmann::json{{"type", "object"},
                              {"description",
                               "a dataset in list_datasets' summary form (#1195)"},
                              {"required", std::move(required)},
                              {"additionalProperties", false},
                              {"properties", std::move(properties)}};
    }();
    return schema;
}

const nlohmann::json& list_datasets_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"datasets"}},
        {"additionalProperties", false},
        {"properties",
         {{"detail",
           {{"type", "string"},
            {"enum", {"full", "summary"}},
            {"description", "the form each entry of datasets takes"}}},
          {"datasets",
           {{"type", "array"},
            {"description",
             "one entry per dataset: the full summary, or with detail \"summary\" "
             "the summary form"},
            {"items",
             {{"anyOf", {dataset_summary_schema(), dataset_list_summary_schema()}}}}}}}}};
    return schema;
}

// The full form computes every dataset from its rows and files (Node::datasets:
// columns and types from every stored row, size from the chunk files). The
// summary form reads only counters (Node::dataset_list_summaries), so a page
// that lists datasets costs per dataset, not per row. Both forms are this one
// tool, behind its one right, and list the same datasets.
ApiResult handle_list_datasets(const nlohmann::json& input, ApiContext& context) {
    const std::string detail = input.value("detail", std::string{"full"});
    nlohmann::json datasets = nlohmann::json::array();
    if (detail == "summary") {
        for (const zetta::data::DatasetSummary& dataset :
             context.node.dataset_list_summaries()) {
            datasets.push_back(dataset_list_summary_json(dataset));
        }
    } else {
        for (const zetta::data::DatasetSummary& dataset : context.node.datasets()) {
            datasets.push_back(dataset_summary_json(dataset));
        }
    }
    nlohmann::json output;
    output["detail"] = detail;
    output["datasets"] = std::move(datasets);
    return ApiResult::success(std::move(output));
}

// A property of the stored workspace methodology's schema
// (ml/workspace_methodology.hpp), reused so the capability schemas and the
// replicated value cannot disagree (zetta#1261).
nlohmann::json workspace_value_property(const char* name) {
    return zetta::ml::workspace_methodology_value_schema().at("properties").at(name);
}

const nlohmann::json& workspace_interface_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"description",
         "what a workspace methodology's code declares: the input kinds it needs, its "
         "target, required features, parameters (for studies to search), the metric base "
         "names it reports (as in_sample_<m> and held_out_<m>) and whether it explains"},
        {"properties",
         {{"input_kinds", workspace_value_property("input_kinds")},
          {"target", workspace_value_property("target")},
          {"required_features", workspace_value_property("required_features")},
          {"parameters", workspace_value_property("parameters")},
          {"metrics", workspace_value_property("metrics")},
          {"explain", {{"type", "boolean"}}}}}};
    return schema;
}

const nlohmann::json& list_methodologies_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_methodologies_output_schema() {
    static const nlohmann::json input_schema = {
        {"type", "object"},
        {"required", {"role", "required", "description"}},
        {"additionalProperties", false},
        {"properties",
         {{"role", {{"type", "string"}}},
          {"required", {{"type", "boolean"}}},
          {"description", {{"type", "string"}}}}}};
    static const nlohmann::json string_array = {
        {"type", "array"}, {"items", {{"type", "string"}}}};
    static const nlohmann::json study_parameter_schema = {
        {"type", "object"},
        {"required",
         {"name", "kind", "minimum", "maximum", "step", "log", "default",
          "choices", "aliases", "condition_parameter", "condition_values"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"kind", {{"type", "string"},
                    {"enum", {"int", "float", "categorical"}}}},
          {"minimum", {{"type", "number"}}},
          {"maximum", {{"type", "number"}}},
          {"step", {{"type", "number"}, {"minimum", 0}}},
          {"log", {{"type", "boolean"}}},
          {"default", {{"type", {"string", "number", "integer", "boolean"}}}},
          {"choices",
           {{"type", "array"},
            {"items",
             {{"type", {"string", "number", "integer", "boolean"}}}}}},
          {"aliases", string_array},
          {"condition_parameter", {{"type", "string"}}},
          {"condition_values",
           {{"type", "array"},
            {"items",
             {{"type", {"string", "number", "integer", "boolean"}}}}}}}}};
    static const nlohmann::json study_dataset_path_schema = {
        {"type", "object"},
        {"required",
         {"kind", "format", "must_exist",
          "required_without_project_pipeline", "directory_paths"}},
        {"additionalProperties", false},
        {"properties",
         {{"kind", {{"type", "string"}, {"const", "table"}}},
          {"format", {{"type", "string"}, {"const", "database.table"}}},
          {"must_exist", {{"type", "boolean"}}},
          {"required_without_project_pipeline", {{"type", "boolean"}}},
          {"directory_paths", {{"type", "boolean"}}}}}};
    static const nlohmann::json study_search_space_schema = {
        {"type", "object"},
        {"required",
         {"parameters", "objective_metrics", "objective_metric_aliases",
          "validation_splits", "dataset_path"}},
        {"additionalProperties", false},
        {"properties",
         {{"parameters",
           {{"type", "array"}, {"items", study_parameter_schema}}},
          {"objective_metrics", string_array},
          {"objective_metric_aliases",
           {{"type", "object"},
            {"description",
             "accepted input aliases per canonical objective metric; they "
             "normalize to the canonical name"},
            {"additionalProperties", string_array}}},
          {"validation_splits", string_array},
          {"dataset_path", study_dataset_path_schema}}}};
    static const nlohmann::json methodology_schema = {
        {"type", "object"},
        {"required",
         {"id", "name", "family", "model_kinds", "solutions", "inputs",
          "outputs", "metrics", "runnable", "partial", "issue", "studies",
          "study_reason", "study_search_space", "source", "editable"}},
        {"additionalProperties", false},
        {"properties",
         {{"source",
           {{"type", "string"},
            {"enum", {"orama", "workspace"}},
            {"description",
             "orama: the catalogue, read-only; workspace: the workspace's own, its "
             "Python code run by Lake's runner (zetta#1261)"}}},
          {"editable", {{"type", "boolean"}}},
          {"description", {{"type", "string"}}},
          {"interface", workspace_interface_schema()},
          {"code", workspace_value_property("code")},
          {"image", workspace_value_property("image")},
          {"validation", workspace_value_property("validation")},
          {"network", {{"type", "boolean"}}},
          {"resources", workspace_value_property("resources")},
          {"duplicated_from", {{"type", "string"}}},
          {"created_by", {{"type", "string"}}},
          {"created_at", {{"type", "string"}}},
          {"updated_at", {{"type", "string"}}},
          {"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"family", {{"type", "string"}}},
          {"model_kinds", string_array},
          {"solutions", string_array},
          {"inputs", {{"type", "array"}, {"items", input_schema}}},
          {"outputs", string_array},
          {"metrics", string_array},
          {"runnable", {{"type", "boolean"}}},
          {"partial", {{"type", "boolean"}}},
          {"issue", {{"type", "string"}}},
          {"studies", {{"type", "boolean"}}},
          {"study_reason", {{"type", "string"}}},
          {"study_search_space",
           {{"oneOf",
             nlohmann::json::array(
                 {study_search_space_schema,
                  nlohmann::json{{"type", "null"}}})}}}}}};
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"methodologies"}},
        {"additionalProperties", false},
        {"properties",
         {{"methodologies",
           {{"type", "array"}, {"items", methodology_schema}}}}}};
    return schema;
}

// One catalogue entry as list_methodologies and get_methodology answer it:
// Orama's, read-only (zetta#1261).
nlohmann::json catalogue_methodology_json(const zetta::ml::Methodology& methodology) {
    nlohmann::json entry = zetta::ml::methodology_json(methodology);
    for (auto& [key, value] :
         study_search_space_for_methodology(methodology.id).items()) {
        entry[key] = value;
    }
    entry["source"] = "orama";
    entry["editable"] = false;
    return entry;
}

ApiResult handle_list_methodologies(const nlohmann::json&, ApiContext& context) {
    nlohmann::json entries = nlohmann::json::array();
    for (const zetta::ml::Methodology& methodology :
         zetta::ml::methodologies()) {
        entries.push_back(catalogue_methodology_json(methodology));
    }
    // The workspace's own after the catalogue (zetta#1261).
    if (context.node.replication() != nullptr) {
        const zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
        for (const nlohmann::json& value : store.list()) {
            entries.push_back(zetta::ml::workspace_methodology_listing(value));
        }
    }
    return ApiResult::success({{"methodologies", std::move(entries)}});
}

// --- solutions audit ledger (#1107) -----------------------------------------

nlohmann::json ledger_entry_output_item_schema() {
    nlohmann::json schema = zetta::solutions::audit_ledger_entry_schema();
    for (nlohmann::json& choice : schema["oneOf"]) {
        choice["required"].push_back("id");
    }
    return schema;
}

const nlohmann::json& record_action_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"subject", "score_id", "score", "decision", "action",
          "actor_kind", "model_version", "policy_version", "agent_version",
          "cell", "cost", "currency", "score_at", "decision_at",
          "action_at"}},
        {"additionalProperties", false},
        {"properties",
         {{"subject", {{"type", "string"}, {"minLength", 1}}},
          {"score_id", {{"type", "string"}, {"minLength", 1}}},
          {"score", {{"type", "number"}}},
          {"decision", {{"type", "string"}, {"minLength", 1}}},
          {"action",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the action taken; use `no_contact` for a deliberate hold"}}},
          {"actor_kind", {{"type", "string"}, {"enum", {"agent", "human"}}}},
          {"actor",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the acting agent or human; defaults to the authenticated caller"}}},
          {"model_version", {{"type", "string"}, {"minLength", 1}}},
          {"policy_version", {{"type", "string"}, {"minLength", 1}}},
          {"agent_version", {{"type", "string"}, {"minLength", 1}}},
          {"cell", {{"type", "string"}, {"minLength", 1}}},
          {"cost", {{"type", "number"}, {"minimum", 0}}},
          {"currency", {{"type", "string"}, {"minLength", 1}}},
          {"score_at", {{"type", "string"}, {"minLength", 1}}},
          {"decision_at", {{"type", "string"}, {"minLength", 1}}},
          {"action_at", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& record_outcome_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"action_id", "outcome", "value", "currency", "source",
          "occurred_at"}},
        {"additionalProperties", false},
        {"properties",
         {{"action_id", {{"type", "string"}, {"minLength", 1}}},
          {"outcome", {{"type", "string"}, {"minLength", 1}}},
          {"value", {{"type", "number"}}},
          {"currency", {{"type", "string"}, {"minLength", 1}}},
          {"source",
           {{"type", "string"}, {"enum", {"connector", "manual"}}}},
          {"occurred_at", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& record_ledger_entry_output_schema() {
    static const nlohmann::json schema = ledger_entry_output_item_schema();
    return schema;
}

const nlohmann::json& list_ledger_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_ledger_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"entries"}},
        {"additionalProperties", false},
        {"properties",
         {{"entries",
           {{"type", "array"}, {"items", ledger_entry_output_item_schema()}}}}}};
    return schema;
}

ApiResult handle_record_action(const nlohmann::json& input,
                               ApiContext& context) {
    zetta::solutions::ActionLedgerValue value;
    value.subject = input.at("subject").get<std::string>();
    value.score_id = input.at("score_id").get<std::string>();
    value.score = input.at("score").get<double>();
    value.decision = input.at("decision").get<std::string>();
    value.action = input.at("action").get<std::string>();
    value.actor_kind = zetta::solutions::action_actor_kind_from(
        input.at("actor_kind").get<std::string>()).value();
    value.actor = input.value(
        "actor", context.acting_key.empty() ? std::string{"local"}
                                             : context.acting_key);
    value.model_version = input.at("model_version").get<std::string>();
    value.policy_version = input.at("policy_version").get<std::string>();
    value.agent_version = input.at("agent_version").get<std::string>();
    value.cell = input.at("cell").get<std::string>();
    value.cost = input.at("cost").get<double>();
    value.currency = input.at("currency").get<std::string>();
    value.score_at = input.at("score_at").get<std::string>();
    value.decision_at = input.at("decision_at").get<std::string>();
    value.action_at = input.at("action_at").get<std::string>();
    const auto entry = context.node.record_action(std::move(value));
    if (!entry) {
        return ApiResult::failure(
            "engine_error", "record_action rejected the ledger record");
    }
    nlohmann::json output = entry->metadata();
    output["id"] = entry->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_record_outcome(const nlohmann::json& input,
                                ApiContext& context) {
    const std::string action_id = input.at("action_id").get<std::string>();
    const auto action = context.node.find_ledger_entry(action_id);
    if (!action || action->kind() != zetta::solutions::LedgerEntryKind::Action) {
        return ApiResult::failure(
            "not_found", "no action ledger entry with id '" + action_id + "'",
            "/action_id");
    }
    zetta::solutions::OutcomeLedgerValue value;
    value.action_id = action_id;
    value.outcome = input.at("outcome").get<std::string>();
    value.value = input.at("value").get<double>();
    value.currency = input.at("currency").get<std::string>();
    value.source = zetta::solutions::outcome_source_from(
        input.at("source").get<std::string>()).value();
    value.occurred_at = input.at("occurred_at").get<std::string>();
    const auto entry = context.node.record_outcome(std::move(value));
    if (!entry) {
        return ApiResult::failure(
            "engine_error", "record_outcome rejected the ledger record");
    }
    nlohmann::json output = entry->metadata();
    output["id"] = entry->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_list_ledger(const nlohmann::json&, ApiContext& context) {
    nlohmann::json entries = nlohmann::json::array();
    for (const auto& entry : context.node.ledger_entries()) {
        nlohmann::json document = entry->metadata();
        document["id"] = entry->id();
        entries.push_back(std::move(document));
    }
    return ApiResult::success({{"entries", std::move(entries)}});
}

// --- create_project ----------------------------------------------------------

const nlohmann::json& create_project_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {
             {"name", {{"type", "string"}, {"minLength", 1}}},
             {"owner",
              {{"type", "string"},
               {"description", "owner key id recorded on the project (optional)"}}},
             {"description",
              {{"type", "string"},
               {"description",
                "a one-line human description of the project's intent (optional)"}}},
         }}};
    return schema;
}

const nlohmann::json& create_project_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "owner"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"owner", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_create_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::string owner =
        input.value("owner", context.acting_key.empty() ? std::string("local")
                                                        : context.acting_key);
    const std::string description = input.value("description", std::string{});
    auto project = context.node.create_project(name, owner, description);
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

// --- delete_project (#1009) --------------------------------------------------
//
// Removes the project with its inputs and graphs (Node::drop_project — the
// replicated document first, so every peer drops them, then the on-disk
// directory). Runs and models are deliberately KEPT: they are records of what
// happened and what was trained, a model may be deployed or exposed, and each
// carries the project only as a label. They stay listed; this answer says how
// many were left behind so the caller is not surprised to still see them.

const nlohmann::json& delete_project_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the project's name (list_projects)"}}}}}};
    return schema;
}

const nlohmann::json& delete_project_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "id", "removed", "kept"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"id", {{"type", "string"}, {"description", "the deleted project's uuid"}}},
          {"removed",
           {{"type", "object"},
            {"required", {"inputs", "graphs"}},
            {"additionalProperties", false},
            {"description", "what was deleted with the project"},
            {"properties",
             {{"inputs", {{"type", "integer"}}}, {"graphs", {{"type", "integer"}}}}}}},
          {"kept",
           {{"type", "object"},
            {"required", {"runs", "models"}},
            {"additionalProperties", false},
            {"description",
             "records that name this project and were NOT deleted: they stay "
             "listed in list_runs / list_models, still labelled with the project's "
             "id"},
            {"properties",
             {{"runs", {{"type", "integer"}}}, {"models", {{"type", "integer"}}}}}}}}}};
    return schema;
}

ApiResult handle_delete_project(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Project> project =
        context.node.find_project(name);
    if (!project) {
        return ApiResult::failure("not_found", "no project named '" + name + "'");
    }
    const std::string id = project->id();
    // A run records its project by id (the run capability) or, from the in-process
    // runners, by name; count both so `kept` is honest either way.
    const auto names_this_project = [&](const std::string& label) {
        return label == id || label == name;
    };

    std::size_t runs = 0;
    std::size_t in_progress = 0;
    for (const auto& run : context.node.runs()) {
        if (!names_this_project(run->project())) continue;
        ++runs;
        if (run->status() == zetta::pipeline::RunStatus::Pending ||
            run->status() == zetta::pipeline::RunStatus::Running) {
            ++in_progress;
        }
    }
    // Deleting the graph out from under a run that is still executing it would
    // leave that run to fail half-way for a reason nobody asked for.
    if (in_progress > 0) {
        return ApiResult::failure(
            "engine_error",
            "project '" + name + "' has " + std::to_string(in_progress) +
                " run(s) still pending or running; wait for them to finish "
                "(get_run) and delete it then");
    }
    std::size_t models = 0;
    for (const auto& model : context.node.models()) {
        if (names_this_project(model->project())) ++models;
    }
    const std::size_t inputs = project->input_count();
    const std::size_t graphs = project->graph_count();

    if (!context.node.drop_project(name)) {
        return ApiResult::failure("engine_error",
                                  "could not delete project '" + name + "'");
    }
    nlohmann::json output;
    output["name"] = name;
    output["id"] = id;
    output["removed"] = {{"inputs", inputs}, {"graphs", graphs}};
    output["kept"] = {{"runs", runs}, {"models", models}};
    return ApiResult::success(std::move(output));
}

// --- delete_dataset / delete_model / delete_run / delete_index (lake#645) -----
//
// The entities a workspace accumulates could be created but never removed: a
// dataset could only be dropped through SQL (which leaves its partitions and its
// indexes behind), a model or a search index could only be unexposed, and a run
// stayed forever. Each delete goes through the replicated drop path
// (Node::drop_* / Database::drop_table), so every peer converges, and each takes
// `dry_run: true`, which changes nothing and answers what would go.

namespace {

nlohmann::json delete_input_schema(std::string_view field,
                                   std::string_view description) {
    nlohmann::json schema = id_input_schema(field, description);
    schema["properties"]["dry_run"] = {
        {"type", "boolean"},
        {"description",
         "true: change nothing and answer what would be deleted (default false)"}};
    return schema;
}

bool is_in_flight(zetta::pipeline::RunStatus status) {
    return status == zetta::pipeline::RunStatus::Pending ||
           status == zetta::pipeline::RunStatus::Running;
}

// The pending or running runs and the running studies that read `dataset`, as
// "run <id> (running), study '<name>' (running)"; empty when none does
// (zetta#1258). A run's recorded reads are the check; a lineage source naming
// the dataset counts too, for a runner that records it early.
std::string dataset_readers(zetta::Node& node, const zetta::data::DatasetSummary& dataset) {
    const std::string source = "dataset:" + dataset.database + "." + dataset.name;
    std::string readers;
    const auto name = [&](const std::string& reader) {
        if (!readers.empty()) readers += ", ";
        readers += reader;
    };
    for (const auto& run : node.runs()) {
        if (!run || !is_in_flight(run->status())) continue;
        const bool reads =
            zetta::pipeline::reads_table(run->reads(), dataset.database, dataset.name,
                                         dataset.table_id) ||
            std::ranges::find(run->lineage().sources, source) != run->lineage().sources.end();
        if (reads) name("run " + run->id() + " (" + std::string(to_string(run->status())) + ")");
    }
    for (const auto& study : node.studies()) {
        if (!study || study->status() != zetta::ml::StudyStatus::Running) continue;
        if (zetta::pipeline::reads_table(study->value().reads, dataset.database, dataset.name,
                                         dataset.table_id)) {
            name("study '" + study->spec().name + "' (running)");
        }
    }
    return readers;
}

}  // namespace

const nlohmann::json& delete_dataset_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json value = delete_input_schema(
            "dataset",
            "the dataset's name as list_datasets reports it, `database.table`, "
            "or its table id");
        value["properties"]["database"] = {
            {"type", "string"},
            {"minLength", 1},
            {"description",
             "the dataset's database — needed only when the same name exists in "
             "several databases"}};
        return value;
    }();
    return schema;
}

const nlohmann::json& delete_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"dataset", "name", "database", "table_id", "dry_run", "removed"}},
        {"additionalProperties", false},
        {"properties",
         {{"dataset", {{"type", "string"}, {"description", "the dataset's name"}}},
          {"name", {{"type", "string"}}},
          {"database", {{"type", "string"}}},
          {"table_id", {{"type", "string"}}},
          {"dry_run", {{"type", "boolean"}}},
          {"removed",
           {{"type", "object"},
            {"required", {"rows", "size_bytes", "partitions", "indexes"}},
            {"additionalProperties", false},
            {"description", "what was (or, on a dry run, would be) deleted"},
            {"properties",
             {{"rows", {{"type", "integer"}}},
              {"size_bytes", {{"type", "integer"}}},
              {"partitions", {{"type", "integer"}}},
              {"indexes",
               {{"type", "array"},
                {"description", "the ids of the indexes built over the dataset"},
                {"items", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_delete_dataset(const nlohmann::json& input, ApiContext& context) {
    const std::string target = input.at("dataset").get<std::string>();
    const bool dry_run = input.value("dry_run", false);
    std::string name = target;
    std::string database = input.value("database", "");
    std::vector<zetta::data::DatasetSummary> matches;
    const std::vector<zetta::data::DatasetSummary> all = context.node.datasets();
    // By table id first: it is unambiguous.
    for (const zetta::data::DatasetSummary& dataset : all) {
        if (dataset.table_id == target) matches.push_back(dataset);
    }
    if (matches.empty()) {
        // `database.table`, unless a table's own name holds the dot (get_dataset's rule).
        const bool exact = std::ranges::any_of(
            all, [&](const zetta::data::DatasetSummary& each) { return each.name == target; });
        if (const std::size_t dot = target.find('.'); dot != std::string::npos && !exact) {
            const std::string named_database = target.substr(0, dot);
            if (!database.empty() && database != named_database) {
                return ApiResult::failure(
                    "invalid_input",
                    "dataset '" + target + "' names database '" + named_database +
                        "', but database is '" + database + "'",
                    "/database");
            }
            database = named_database;
            name = target.substr(dot + 1);
        }
        for (const zetta::data::DatasetSummary& dataset : all) {
            if (dataset.name != name) continue;
            if (!database.empty() && dataset.database != database) continue;
            matches.push_back(dataset);
        }
    }
    if (matches.empty()) {
        return ApiResult::failure(
            "not_found", "no dataset named '" + target + "'" +
                             (database.empty() ? "" : " in database '" + database + "'"));
    }
    if (matches.size() > 1) {
        std::string databases;
        for (const zetta::data::DatasetSummary& match : matches) {
            if (!databases.empty()) databases += ", ";
            databases += match.database;
        }
        return ApiResult::failure(
            "engine_error", "dataset '" + target + "' exists in several databases (" +
                                databases + ") — pass \"database\" to pick one");
    }
    const zetta::data::DatasetSummary dataset = matches.front();
    if (dataset.database == zetta::connect::kConnectionDatabase) {
        return ApiResult::failure(
            "forbidden", "\"" + dataset.database +
                             "\" is the reserved connection store; remove a connection "
                             "with delete_connection");
    }
    const std::shared_ptr<zetta::data::Database> owner =
        context.node.find_database(dataset.database);
    if (!owner || !owner->find_table_by_id(dataset.table_id)) {
        return ApiResult::failure("not_found", "no dataset named '" + target + "'");
    }
    const std::string qualified = dataset.database + "." + dataset.name;
    // A pending or running run, or a running study, that reads the dataset
    // keeps it (zetta#1258): each records the tables it reads when it is
    // created. A dry run refuses the same way, so it never promises a delete
    // that would be refused.
    if (const std::string readers = dataset_readers(context.node, dataset);
        !readers.empty()) {
        return ApiResult::failure(
            "engine_error", "dataset '" + qualified + "' is being read by " + readers +
                                ": wait for its runs to finish (get_run) and stop "
                                "its studies (stop_study), then delete it");
    }
    // An index records the table by the name create_index was given: the bare
    // name, or `database.table`.
    std::vector<std::string> indexes;
    for (const auto& index : context.node.indexes()) {
        if (index->table() == dataset.name || index->table() == qualified) {
            indexes.push_back(index->id());
        }
    }
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> partitions;
    for (const auto& partition : context.node.dataset_partitions()) {
        if (partition && partition->table() == dataset.table_id) {
            partitions.push_back(partition);
        }
    }
    if (!dry_run) {
        for (const std::string& index : indexes) context.node.drop_index(index);
        // A partition's body is a large object of the dataset's database; unlink
        // it with the catalog entry, as a discarded ingest does.
        for (const auto& partition : partitions) {
            if (!partition->blob_ref().empty()) {
                owner->large_objects().unlink(partition->blob_ref());
            }
            context.node.drop_dataset_partition(partition->id());
        }
        if (!owner->drop_table(dataset.name)) {
            return ApiResult::failure("engine_error",
                                      "could not delete dataset '" + qualified + "'");
        }
    }
    nlohmann::json output;
    output["dataset"] = dataset.name;
    output["name"] = dataset.name;
    output["database"] = dataset.database;
    output["table_id"] = dataset.table_id;
    output["dry_run"] = dry_run;
    output["removed"] = {{"rows", dataset.rows},
                         {"size_bytes", dataset.size_bytes},
                         {"partitions", partitions.size()},
                         {"indexes", indexes}};
    return ApiResult::success(std::move(output));
}

const nlohmann::json& delete_model_schema() {
    static const nlohmann::json schema = delete_input_schema(
        "model_id", "the model's uuid (system.models.model_id), or its display name");
    return schema;
}

const nlohmann::json& delete_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "name", "dry_run", "removed", "kept"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"dry_run", {{"type", "boolean"}}},
          {"removed",
           {{"type", "object"},
            {"required", {"model", "was_exposed"}},
            {"additionalProperties", false},
            {"properties",
             {{"model", {{"type", "integer"}}},
              {"was_exposed",
               {{"type", "boolean"},
                {"description",
                 "the model was served; its endpoint stops answering with the "
                 "delete"}}},
              {"standalone_project",
               {{"type", "integer"},
                {"description",
                 "1 when this was a standalone model's last version: the internal "
                 "project that held it goes too (lake#654)"}}}}}}},
          {"kept",
           {{"type", "object"},
            {"required", {"run"}},
            {"additionalProperties", false},
            {"description", "the run that trained it is a record and is kept"},
            {"properties", {{"run", {{"type", "string"}}}}}}}}}};
    return schema;
}

ApiResult handle_delete_model(const nlohmann::json& input, ApiContext& context) {
    const std::string target = input.at("model_id").get<std::string>();
    const bool dry_run = input.value("dry_run", false);
    std::shared_ptr<zetta::ml::Model> model = context.node.find_model(target);
    if (!model) {
        // By display name only when it names exactly one model: a delete never
        // guesses between several.
        std::size_t named = 0;
        for (const auto& each : context.node.models()) {
            if (each && !each->name().empty() && each->name() == target) {
                model = each;
                ++named;
            }
        }
        if (named > 1) {
            return ApiResult::failure(
                "engine_error", std::to_string(named) + " models are named '" + target +
                                    "' — delete one by its model_id");
        }
    }
    if (!model) {
        return ApiResult::failure("not_found", "no model named '" + target + "'");
    }
    const std::string id = model->id();
    const std::string name = model->name();
    const bool was_exposed = model->exposed();
    const std::string run = !model->run_id().empty() ? model->run_id() : model->run();
    // A standalone model's internal project (lake#654) goes with its last
    // version, unless one of its runs is still training a next one.
    std::shared_ptr<zetta::pipeline::Project> standalone;
    if (!model->project().empty()) {
        if (auto project = context.node.project_by_id(model->project());
            project && project->internal()) {
            const bool others = std::ranges::any_of(context.node.models(), [&](const auto& each) {
                return each && each->id() != id && each->project() == project->id();
            });
            const bool training = std::ranges::any_of(context.node.runs(), [&](const auto& each) {
                return each && each->project() == project->id() && is_in_flight(each->status());
            });
            if (!others && !training) standalone = std::move(project);
        }
    }
    if (!dry_run) {
        // Unexpose first, so the replicated flag stops serving it on every peer
        // even before the drop converges.
        if (was_exposed) model->set_exposed(false);
        if (!context.node.drop_model(id)) {
            return ApiResult::failure("engine_error", "could not delete model '" + id + "'");
        }
        if (standalone) context.node.drop_project(standalone->name());
    }
    nlohmann::json output;
    output["model_id"] = id;
    output["name"] = name;
    output["dry_run"] = dry_run;
    output["removed"] = {{"model", 1}, {"was_exposed", was_exposed}};
    if (standalone) output["removed"]["standalone_project"] = 1;
    output["kept"] = {{"run", run}};
    return ApiResult::success(std::move(output));
}

const nlohmann::json& delete_run_schema() {
    static const nlohmann::json schema =
        delete_input_schema("run_id", "the run's uuid (system.runs.run_id)");
    return schema;
}

const nlohmann::json& delete_run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "name", "dry_run", "removed", "kept"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id", {{"type", "string"}}},
          {"name",
           {{"type", "string"},
            {"description", "the run's pipeline name, when it has one"}}},
          {"dry_run", {{"type", "boolean"}}},
          {"removed",
           {{"type", "object"},
            {"required", {"run", "status"}},
            {"additionalProperties", false},
            {"properties",
             {{"run", {{"type", "integer"}}}, {"status", {{"type", "string"}}}}}}},
          {"kept",
           {{"type", "object"},
            {"required", {"models"}},
            {"additionalProperties", false},
            {"description",
             "the models this run trained are kept (delete them with delete_model)"},
            {"properties",
             {{"models", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_delete_run(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("run_id").get<std::string>();
    const bool dry_run = input.value("dry_run", false);
    const auto run = context.node.find_run(id);
    if (!run) {
        return ApiResult::failure("not_found", "no run with id '" + id + "'");
    }
    if (is_in_flight(run->status())) {
        return ApiResult::failure(
            "engine_error", "run '" + id + "' is still " +
                                std::string(zetta::pipeline::to_string(run->status())) +
                                "; wait for it to finish (get_run) and delete it then");
    }
    std::vector<std::string> models;
    for (const auto& model : context.node.models()) {
        if (model && (model->run() == id || model->run_id() == id)) {
            models.push_back(model->id());
        }
    }
    for (const auto& output : run->outputs()) {
        if (output.kind == "model" && !output.ref.empty() &&
            std::ranges::find(models, output.ref) == models.end() &&
            context.node.find_model(output.ref)) {
            models.push_back(output.ref);
        }
    }
    const std::string status(zetta::pipeline::to_string(run->status()));
    const std::string name = run->pipeline_name();
    if (!dry_run && !context.node.drop_run(id)) {
        return ApiResult::failure("engine_error", "could not delete run '" + id + "'");
    }
    nlohmann::json output;
    output["run_id"] = id;
    output["name"] = name;
    output["dry_run"] = dry_run;
    output["removed"] = {{"run", 1}, {"status", status}};
    output["kept"] = {{"models", models}};
    return ApiResult::success(std::move(output));
}

const nlohmann::json& delete_index_schema() {
    static const nlohmann::json schema =
        delete_input_schema("index_id", "the index's uuid (system.indexes.index_id)");
    return schema;
}

const nlohmann::json& delete_index_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"index_id", "name", "table", "dry_run", "removed"}},
        {"additionalProperties", false},
        {"properties",
         {{"index_id", {{"type", "string"}}},
          {"name",
           {{"type", "string"},
            {"description", "indexes have no name of their own: the indexed table"}}},
          {"table", {{"type", "string"}}},
          {"dry_run", {{"type", "boolean"}}},
          {"removed",
           {{"type", "object"},
            {"required", {"index", "was_exposed"}},
            {"additionalProperties", false},
            {"properties",
             {{"index", {{"type", "integer"}}},
              {"was_exposed",
               {{"type", "boolean"},
                {"description",
                 "the index was served; its endpoint stops answering with the "
                 "delete"}}}}}}}}}};
    return schema;
}

ApiResult handle_delete_index(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("index_id").get<std::string>();
    const bool dry_run = input.value("dry_run", false);
    const auto index = context.node.find_index(id);
    if (!index) {
        return ApiResult::failure("not_found", "no index with id '" + id + "'");
    }
    const bool was_exposed = index->exposed();
    const std::string table = index->table();
    if (!dry_run) {
        if (was_exposed) index->set_exposed(false);
        if (!context.node.drop_index(id)) {
            return ApiResult::failure("engine_error", "could not delete index '" + id + "'");
        }
    }
    nlohmann::json output;
    output["index_id"] = id;
    output["name"] = table;
    output["table"] = table;
    output["dry_run"] = dry_run;
    output["removed"] = {{"index", 1}, {"was_exposed", was_exposed}};
    return ApiResult::success(std::move(output));
}

// --- expose_model / expose_index (#648, docs/api/serving.md) ------------------
//
// Exposure flips the entity's REPLICATED `exposed` flag (Model::set_exposed /
// Index::set_exposed): the CRDT value is the source of truth, so every node of
// the cluster converges on serving — or refusing — the entity's HTTP endpoint.
// Idempotent both ways; un-exposing is the same one action with `false`.

const nlohmann::json& expose_model_schema() {
    static const nlohmann::json schema = id_input_schema(
        "model",
        "the model's uuid (system.models.model_id), or its display name");
    return schema;
}

const nlohmann::json& expose_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "exposed", "endpoint"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"exposed", {{"type", "boolean"}}},
          {"endpoint",
           {{"type", "string"},
            {"description",
             "the served route (by id — the unambiguous spelling; the display "
             "name also resolves)"}}}}}};
    return schema;
}

ApiResult set_model_exposure(const nlohmann::json& input, ApiContext& context,
                             bool exposed) {
    const std::string target = input.at("model").get<std::string>();
    const auto model = resolve_served_model(context.node, target);
    if (!model) {
        return ApiResult::failure("not_found",
                                  "no model named '" + target + "'");
    }
    model->set_exposed(exposed);
    nlohmann::json output;
    output["model_id"] = model->id();
    output["name"] = model->name();
    output["exposed"] = model->exposed();
    output["endpoint"] = model_endpoint_path(model->id());
    return ApiResult::success(std::move(output));
}

ApiResult handle_expose_model(const nlohmann::json& input,
                              ApiContext& context) {
    return set_model_exposure(input, context, /*exposed=*/true);
}

ApiResult handle_unexpose_model(const nlohmann::json& input,
                                ApiContext& context) {
    return set_model_exposure(input, context, /*exposed=*/false);
}

const nlohmann::json& expose_index_schema() {
    static const nlohmann::json schema = id_input_schema(
        "index",
        "the index's uuid (system.indexes.index_id), or an indexed table's "
        "name (resolves like `search`: the newest Search-kind index over it)");
    return schema;
}

const nlohmann::json& expose_index_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"index_id", "table", "exposed", "endpoint"}},
        {"additionalProperties", false},
        {"properties",
         {{"index_id", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"exposed", {{"type", "boolean"}}},
          {"endpoint",
           {{"type", "string"},
            {"description",
             "the served route (by id — the unambiguous spelling; the table "
             "name also resolves)"}}}}}};
    return schema;
}

ApiResult set_index_exposure(const nlohmann::json& input, ApiContext& context,
                             bool exposed) {
    const std::string target = input.at("index").get<std::string>();
    const auto index =
        zetta::index::resolve_search_target(context.node, target);
    if (!index) {
        return ApiResult::failure("not_found",
                                  "no index named '" + target + "'");
    }
    index->set_exposed(exposed);
    nlohmann::json output;
    output["index_id"] = index->id();
    output["table"] = index->table();
    output["exposed"] = index->exposed();
    output["endpoint"] = index_endpoint_path(index->id());
    return ApiResult::success(std::move(output));
}

ApiResult handle_expose_index(const nlohmann::json& input,
                              ApiContext& context) {
    return set_index_exposure(input, context, /*exposed=*/true);
}

ApiResult handle_unexpose_index(const nlohmann::json& input,
                                ApiContext& context) {
    return set_index_exposure(input, context, /*exposed=*/false);
}

// --- create_index (https://github.com/oramasearch/zetta/issues/825) ----------
//
// The public create path for an index over a loaded table: it records the Index
// entity (Node::create_index) then reconciles so this node's per-partition BM25F
// sub-indexes build immediately. With add_dataset's default (partitioned)
// placement the table has placed partitions the node owns, so `search` then takes
// the distributed path and returns ranked hits. This is the tool search()'s own
// resolution names ("create one with create_index").

const nlohmann::json& create_index_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"table"}},
        {"additionalProperties", false},
        {"properties",
         {{"table",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the source table to index (load it first with add_dataset)"}}},
          {"fields",
           {{"type", "array"},
            {"minItems", 1},
            {"uniqueItems", true},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"description", "the fields to index (default: all of the table's columns)"}}},
          {"kind",
           {{"type", "string"},
            {"enum", {"search", "vector", "feature"}},
            {"description",
             "index kind (default search — the only kind with a built-in "
             "backend today)"}}},
          {"backend",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the IndexBackend that builds it (default oramacore_fulltext, "
             "BM25F full-text; memory_search is the exact-match reference)"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the table's database — needed only when the same table name "
             "exists in several databases"}}}}}};
    return schema;
}

const nlohmann::json& create_index_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"index_id", "table", "kind", "backend", "fields", "exposed"}},
        {"additionalProperties", false},
        {"properties",
         {{"index_id", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"kind", {{"type", "string"}}},
          {"backend", {{"type", "string"}}},
          {"fields", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"exposed", {{"type", "boolean"}}}}}};
    return schema;
}

// Resolve a table by name — within `database` when named, else the first database
// holding a table of that name (the index_step_executor resolution).
std::shared_ptr<zetta::data::Table> resolve_index_source_table(
    Node& node, const std::string& table, const std::string& database_hint) {
    if (!database_hint.empty()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(database_hint);
        return database ? database->find_table(table) : nullptr;
    }
    for (const std::string& name : node.database_names()) {
        const std::shared_ptr<zetta::data::Database> database =
            node.find_database(name);
        if (database) {
            if (const std::shared_ptr<zetta::data::Table> found =
                    database->find_table(table)) {
                return found;
            }
        }
    }
    return nullptr;
}

ApiResult handle_create_index(const nlohmann::json& input, ApiContext& context) {
    const std::string table = input.at("table").get<std::string>();
    std::vector<std::string> fields =
        input.value("fields", std::vector<std::string>{});
    const std::string kind_name = input.value("kind", std::string{"search"});
    const std::string backend =
        input.value("backend", std::string{"oramacore_fulltext"});
    const std::string database_hint = input.value("database", std::string{});

    // (1) The source table must exist — an index is built over its partitions.
    const std::shared_ptr<zetta::data::Table> source_table =
        resolve_index_source_table(context.node, table, database_hint);
    if (!source_table) {
        return ApiResult::failure(
            "not_found",
            "table '" + table + "' not found — load it first with add_dataset");
    }

    // Default the indexed fields to ALL of the table's columns (from its first
    // row) when none are named — indexing everything is the sane default; a
    // caller passes fields only to narrow it.
    if (fields.empty() && source_table->store().size() > 0) {
        const zetta::storage::ParquetLazyPtr first_row = source_table->store().at(0);
        if (first_row && first_row->is_object())
            for (auto it = first_row->begin(); it != first_row->end(); ++it)
                fields.push_back(it.key());
    }
    if (fields.empty()) {
        return ApiResult::failure(
            "invalid_input",
            "no fields to index — the table has no rows yet; load data first or "
            "pass fields explicitly");
    }

    // (2) The kind is schema-constrained; parse it back to the enum.
    const std::optional<zetta::index::IndexKind> kind =
        zetta::index::index_kind_from_name(kind_name);
    if (!kind) {
        return ApiResult::failure("invalid_input",
                                  "unknown index kind '" + kind_name + "'");
    }

    zetta::index::IndexValue value;
    value.table = table;
    value.kind = *kind;
    value.backend = backend;
    value.fields = fields;

    // (3) The backend must resolve for this kind, or the recorded index could
    // never build — fail with the reason rather than persist a dead entity.
    if (!context.node.index_registry().resolve(value.spec())) {
        return ApiResult::failure(
            "invalid_input", "no index backend named '" + backend +
                                 "' implements kind '" + kind_name +
                                 "' on this node");
    }

    // (4) Record the entity, then reconcile so this node's per-partition
    // sub-indexes build now (search would also reconcile before querying).
    const std::shared_ptr<zetta::index::Index> index =
        context.node.create_index(std::move(value));
    if (!index) {
        return ApiResult::failure(
            "engine_error",
            "could not record the index (needs a table, a backend, and >= 1 "
            "field)");
    }
    context.node.reconcile_search_indexes();

    nlohmann::json output;
    output["index_id"] = index->id();
    output["table"] = index->table();
    output["kind"] = std::string(index->kind_name());
    output["backend"] = index->backend();
    output["fields"] = index->fields();
    output["exposed"] = index->exposed();
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

// One recorded span as observability::to_json renders it — the same shape the
// system.spans view flattens. Kept open: spans carry free-form attributes.
const nlohmann::json& span_json_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"trace_id", "span_id", "name", "ok", "duration_ms"}},
        {"properties",
         {{"trace_id", {{"type", "string"}}},
          {"span_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"ok", {{"type", "boolean"}}},
          {"duration_ms", {{"type", "number"}}},
          {"attributes", {{"type", "object"}}}}}};
    return schema;
}

const nlohmann::json& list_traces_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"spans"}},
        {"additionalProperties", false},
        {"properties",
         {{"spans", {{"type", "array"}, {"items", span_json_schema()}}}}}};
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

const nlohmann::json& get_trace_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"trace_id", "spans"}},
        {"additionalProperties", false},
        {"properties",
         {{"trace_id", {{"type", "string"}}},
          {"spans", {{"type", "array"}, {"items", span_json_schema()}}}}}};
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

const nlohmann::json& submit_feedback_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"feedback_id", "name", "database", "size", "sha256",
          "traces_attached", "errors_attached", "crashes_attached"}},
        {"additionalProperties", false},
        {"properties",
         {{"feedback_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"database", {{"type", "string"}}},
          {"size", {{"type", "integer"}}},
          {"sha256", {{"type", "string"}}},
          {"traces_attached", {{"type", "integer"}}},
          {"errors_attached", {{"type", "integer"}}},
          {"crashes_attached", {{"type", "integer"}}},
          {"exported_to",
           {{"type", "string"},
            {"description",
             "present only when export_path was given and the export "
             "succeeded"}}}}}};
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
// authorize + audit happen in Registry::call, the gate inside the engine interfaces).
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
    return context.node.project_by_id(name);
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

nlohmann::json graph_json(const ApiContext& context, const zetta::pipeline::ProjectGraph& graph) {
    const auto [methodology, runnable] = graph_methodology_fields(context, graph.graph());
    return nlohmann::json{
        {"graph_id", graph.id()},
        {"name", graph.name()},
        {"steps", graph.graph().node_count()},
        {"edges", graph.graph().edge_count()},
        {"approval", graph.approval_status()},
        {"methodology", methodology},
        {"runnable", runnable}};
}

// --- get_graph / save_graph: the visual graph workspace's read + write path ---

const nlohmann::json& graph_ref_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the owning project's name or uuid"}}},
          {"graph",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the graph's name or uuid"}}}}}};
    return schema;
}

const nlohmann::json& graph_structure_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"graph_id", "name", "project", "nodes", "edges"}},
        {"additionalProperties", false},
        {"properties",
         {{"graph_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"project", {{"type", "string"}}},
          {"approval",
           {{"type", "string"},
            {"enum", {"proposed", "approved"}},
            {"description", "plan-approval state (#885)"}}},
          {"approved_by", {{"type", "string"}}},
          {"approved_at", {{"type", "string"}}},
          {"methodology", {{"type", {"string", "null"}}}},
          {"runnable", {{"type", "boolean"}}},
          {"nodes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description", "a step: { id, kind, name }"}}}}},
          {"edges",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"description", "a connection: { from, to, kind }"}}}}}}}};
    return schema;
}

ApiResult handle_get_graph(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const auto graph = resolve_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found",
                                  "no graph named '" + graph_name +
                                      "' on project '" + project->name() + "'");
    }
    nlohmann::json output = zetta::ir::to_json(graph->graph());  // { nodes, edges }
    output["graph_id"] = graph->id();
    output["name"] = graph->name();
    output["project"] = project->name();
    // Plan-approval state (#885) so the Graph Workspace / Proposed Solution
    // screen shows proposed vs approved and enables the approve action.
    output["approval"] = graph->approval_status();
    output["approved_by"] = graph->approved_by();
    output["approved_at"] = graph->approved_at();
    const auto [methodology, runnable] = graph_methodology_fields(context, graph->graph());
    output["methodology"] = methodology;
    output["runnable"] = runnable;
    return ApiResult::success(std::move(output));
}

const nlohmann::json& save_graph_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph", "nodes", "edges"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the owning project's name or uuid"}}},
          {"graph",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the graph's name or uuid (created if absent)"}}},
          {"nodes",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"description", "the steps: each { id, kind, name }"}}},
          {"edges",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"description", "the connections: each { from, to, kind }"}}},
          {"new_name",
           {{"type", "string"}, {"description", "rename the graph (optional)"}}}}}};
    return schema;
}

ApiResult handle_save_graph(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + project_name + "'");
    }
    nlohmann::json document;
    document["nodes"] = input.at("nodes");
    document["edges"] = input.at("edges");
    std::string parse_error;
    std::optional<zetta::ir::ExecutionGraph> parsed =
        zetta::ir::from_json(document, &parse_error);
    if (!parsed) {
        return ApiResult::failure("invalid_input", "invalid graph: " + parse_error);
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const std::string new_name = input.value("new_name", std::string{});
    std::shared_ptr<zetta::pipeline::ProjectGraph> saved;
    if (const auto existing = resolve_graph(*project, graph_name)) {
        saved = project->update_graph(existing->name(), std::move(*parsed), new_name);
    } else {
        saved = project->create_graph(graph_name, std::move(*parsed));
    }
    if (!saved) {
        return ApiResult::failure(
            "engine_error",
            "could not save the graph (empty name, or a name collision)");
    }
    nlohmann::json output = graph_json(context, *saved);
    output["project"] = project->name();
    return ApiResult::success(std::move(output));
}

// --- approve_plan (#885, review-before-build) -----------------------------------

const nlohmann::json& approve_plan_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the owning project's name or uuid"}}},
          {"graph",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the stored plan's name or uuid to approve"}}}}}};
    return schema;
}

const nlohmann::json& approve_plan_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph", "approval", "approved_by", "approved_at"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", {{"type", "string"}}},
          {"graph", {{"type", "string"}}},
          {"approval", {{"type", "string"}, {"enum", {"proposed", "approved"}}}},
          {"approved_by", {{"type", "string"}}},
          {"approved_at", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_approve_plan(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = resolve_project(context, project_name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const auto existing = resolve_graph(*project, graph_name);
    if (!existing) {
        return ApiResult::failure("not_found",
                                  "no graph named '" + graph_name +
                                      "' on project '" + project->name() + "'");
    }
    // Governed + audited inside the engine (Project::approve_graph): captures the
    // approval into the CRDT (peers reconcile it) and records the audit event.
    const auto approved = project->approve_graph(
        existing->name(), actor_of(context), zetta::util::now_iso8601());
    if (!approved) {
        return ApiResult::failure("engine_error", "could not approve the plan");
    }
    nlohmann::json output;
    output["project"] = project->name();
    output["graph"] = approved->name();
    output["approval"] = approved->approval_status();
    output["approved_by"] = approved->approved_by();
    output["approved_at"] = approved->approved_at();
    return ApiResult::success(std::move(output));
}

// The schema of input_json + the owning project — the output of the two
// input-producing actions (capture_intent, import_n8n).
const nlohmann::json& project_input_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"input_id", "kind", "filename", "content_hash", "size", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"input_id", {{"type", "string"}}},
          {"kind", {{"type", "string"}}},
          {"filename", {{"type", "string"}}},
          {"content_hash", {{"type", "string"}}},
          {"size", {{"type", "integer"}}},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

// The schema of graph_json + the owning project — the output of the three
// compilers (compile_intent, compile_n8n, compile_python).
const nlohmann::json& project_graph_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"graph_id", "name", "steps", "edges", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"graph_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"steps", {{"type", "integer"}}},
          {"edges", {{"type", "integer"}}},
          {"approval",
           {{"type", "string"},
            {"enum", {"proposed", "approved"}},
            {"description",
             "plan-approval state (#885); a fresh plan is proposed"}}},
          {"methodology", {{"type", {"string", "null"}}}},
          {"runnable", {{"type", "boolean"}}},
          {"template",
           {{"type", "string"},
            {"description",
             "compile_intent only: the template the steps started from "
             "(\"the M-1 churn template\"); their names are the template's "
             "until edited (#1238)"}}},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

// A ValidationReport as its ADL to_json renders it (the shape mirrored by
// schemas/validation_report.schema.json). Kept open at the report level: the
// gate's checks evolve with the check catalog.
const nlohmann::json& validation_report_json_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"checks", "block_deploy"}},
        {"properties",
         {{"checks",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"name", "status"}},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"status",
                 {{"type", "string"},
                  {"enum", {"not_applicable", "pass", "warn", "fail"}}}},
                {"reason", {{"type", "string"}}}}}}},
            {"description",
             "the checks that applied to the graph; the ones that did not are "
             "left out (counted in not_applicable) unless include_not_applicable "
             "was asked for"}}},
          {"verdict",
           {{"type", "string"}, {"enum", {"pass", "warn", "fail"}}}},
          {"applied",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "how many checks applied to the graph"}}},
          {"not_applicable",
           {{"type", "integer"},
            {"minimum", 0},
            {"description",
             "how many checks had nothing to check on this graph (never "
             "reported as pass)"}}},
          {"block_deploy", {{"type", "boolean"}}}}}};
    return schema;
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

// --- workspace methodologies (zetta#1261, lake#655) -------------------------------
//
// A workspace's own methodology: an entry in the replicated registry whose
// Python code runs in Lake's sandboxed runner, never in the engine (ADR-17).
// The catalogue (M-n) is Orama's and read-only; these capabilities change only
// the workspace's own (W-<8 hex>).

// Decoded artifacts above this are refused: the request that carries one,
// base64-encoded, must stay under the HTTP adapter's body limit.
constexpr std::size_t kMaxMethodologyArtifactBytes = 24 * 1024 * 1024;
// A larger artifact arrives in chunks of at most this many bytes (lake#690),
// staged on disk until record_methodology_run adopts it as a Model's weights,
// up to kMaxStagedMethodologyArtifactBytes in all. Weights are read back in
// chunks of the same size.
constexpr std::size_t kMaxMethodologyArtifactChunkBytes = 8 * 1024 * 1024;
constexpr std::uintmax_t kMaxStagedMethodologyArtifactBytes = std::uintmax_t{1} << 30;

nlohmann::json workspace_methodology_input_properties(bool include_state) {
    nlohmann::json properties = nlohmann::json::object();
    for (const char* name :
         {"name", "description", "family", "input_kinds", "target", "required_features",
          "parameters", "metrics", "explain", "code", "network", "resources",
          "duplicated_from", "created_by"}) {
        properties[name] = workspace_value_property(name);
    }
    if (include_state) {
        properties["image"] = workspace_value_property("image");
        properties["validation"] = workspace_value_property("validation");
    }
    return properties;
}

const nlohmann::json& register_methodology_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "input_kinds", "target", "metrics", "code"}},
        {"additionalProperties", false},
        {"properties", workspace_methodology_input_properties(false)}};
    return schema;
}

const nlohmann::json& update_methodology_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json properties = workspace_methodology_input_properties(true);
        properties["id"] = {{"type", "string"},
                            {"minLength", 1},
                            {"description", "the workspace methodology's id (W-<8 hex>)"}};
        return nlohmann::json{{"type", "object"},
                              {"required", {"id"}},
                              {"additionalProperties", false},
                              {"properties", std::move(properties)}};
    }();
    return schema;
}

const nlohmann::json& methodology_id_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id"}},
        {"additionalProperties", false},
        {"properties",
         {{"id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "a catalogue id (M-n) or a workspace methodology's id (W-<8 hex>)"}}}}}};
    return schema;
}

const nlohmann::json& methodology_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"methodology"}},
        {"additionalProperties", false},
        {"properties",
         {{"methodology",
           list_methodologies_output_schema()
               .at("properties")
               .at("methodologies")
               .at("items")}}}};
    return schema;
}

const nlohmann::json& delete_methodology_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"removed", "id"}},
        {"additionalProperties", false},
        {"properties", {{"removed", {{"type", "boolean"}}}, {"id", {{"type", "string"}}}}}};
    return schema;
}

// The refusal for any change to Orama's catalogue: read-only, and the way to
// a changeable copy.
ApiResult catalogue_methodology_refusal(const std::string& id, const char* verb) {
    return ApiResult::failure(
        "forbidden",
        "methodology " + id + " is Orama's and read-only: it cannot be " + verb +
            ". Duplicate it into a workspace methodology of your own "
            "(register_methodology with duplicated_from \"" + id + "\") and change that.");
}

bool is_catalogue_methodology_id(const std::string& id) {
    if (zetta::ml::is_workspace_methodology_id(id)) return false;
    for (const zetta::ml::Methodology& methodology : zetta::ml::methodologies()) {
        if (methodology.id == id) return true;
    }
    return id.starts_with("M-");
}

ApiResult handle_register_methodology(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error",
                                  "workspace methodologies require a replicated node");
    }
    zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
    nlohmann::json value = input;
    value["id"] = store.new_id();
    if (!value.contains("family")) value["family"] = "Workspace";
    if (!value.contains("parameters")) value["parameters"] = nlohmann::json::array();
    if (!value.contains("required_features")) {
        value["required_features"] = nlohmann::json::array();
    }
    if (!value.contains("explain")) value["explain"] = false;
    if (!value.contains("network")) value["network"] = false;
    if (const auto from = value.find("duplicated_from");
        from != value.end() && !from->get<std::string>().empty()) {
        const std::string source = from->get<std::string>();
        const bool known = is_catalogue_methodology_id(source) ||
                           store.get(source).has_value();
        if (!known) {
            return ApiResult::failure("invalid_input",
                                      "duplicated_from names no methodology '" + source + "'",
                                      "/duplicated_from");
        }
    }
    value["validation"] = {{"status", "not_validated"}, {"findings", nlohmann::json::array()}};
    value["created_at"] = zetta::util::now_iso8601();
    value["updated_at"] = value["created_at"];
    std::string error;
    const std::optional<nlohmann::json> stored = store.put(value, &error);
    if (!stored) return ApiResult::failure("invalid_input", error);
    return ApiResult::success(
        {{"methodology", zetta::ml::workspace_methodology_listing(*stored)}});
}

ApiResult handle_update_methodology(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("id").get<std::string>();
    if (is_catalogue_methodology_id(id)) return catalogue_methodology_refusal(id, "changed");
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no workspace methodologies on a standalone node");
    }
    zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
    const std::optional<nlohmann::json> existing = store.get(id);
    if (!existing) return ApiResult::failure("not_found", "no methodology '" + id + "'");
    nlohmann::json updated = *existing;
    for (const auto& [key, field] : input.items()) {
        if (key != "id") updated[key] = field;
    }
    // New code is not the code that was validated: unless this call reports the
    // validation of the new revision, the entry goes back to not validated.
    const std::string before = existing->at("code").value("revision", std::string());
    const std::string after = updated.at("code").value("revision", std::string());
    if (before != after && !input.contains("validation")) {
        updated["validation"] = {{"status", "not_validated"},
                                 {"findings", nlohmann::json::array()}};
    }
    updated["updated_at"] = zetta::util::now_iso8601();
    std::string error;
    const std::optional<nlohmann::json> stored = store.put(updated, &error);
    if (!stored) return ApiResult::failure("invalid_input", error);
    return ApiResult::success(
        {{"methodology", zetta::ml::workspace_methodology_listing(*stored)}});
}

ApiResult handle_delete_methodology(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("id").get<std::string>();
    if (is_catalogue_methodology_id(id)) return catalogue_methodology_refusal(id, "deleted");
    bool removed = false;
    if (context.node.replication() != nullptr) {
        zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
        removed = store.remove(id);
    }
    return ApiResult::success({{"removed", removed}, {"id", id}});
}

ApiResult handle_get_methodology(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("id").get<std::string>();
    if (zetta::ml::is_workspace_methodology_id(id)) {
        if (context.node.replication() != nullptr) {
            const zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
            if (const std::optional<nlohmann::json> value = store.get(id)) {
                return ApiResult::success(
                    {{"methodology", zetta::ml::workspace_methodology_listing(*value)}});
            }
        }
        return ApiResult::failure("not_found", "no methodology '" + id + "'");
    }
    for (const zetta::ml::Methodology& methodology : zetta::ml::methodologies()) {
        if (methodology.id == id) {
            return ApiResult::success({{"methodology", catalogue_methodology_json(methodology)}});
        }
    }
    return ApiResult::failure("not_found", "no methodology '" + id + "'");
}

// The workspace methodology a graph's model step names, its stored entry, and
// whether it is runnable; nullopt for a graph that names none.
struct GraphWorkspaceMethodology {
    std::string id;
    std::optional<nlohmann::json> value;
    bool runnable = false;
};

std::optional<GraphWorkspaceMethodology> graph_workspace_methodology(
    const ApiContext& context, const zetta::ir::ExecutionGraph& graph) {
    std::string id = zetta::ml::workspace_methodology_of_graph(graph);
    if (id.empty()) return std::nullopt;
    GraphWorkspaceMethodology found{std::move(id), std::nullopt, false};
    if (context.node.replication() != nullptr) {
        const zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
        found.value = store.get(found.id);
        found.runnable =
            found.value.has_value() && zetta::ml::workspace_methodology_runnable(*found.value);
    }
    return found;
}

// The gate as the API runs it: the default gate, then a `methodology` check
// the pure gate cannot make, from the workspace methodology's registry entry:
// it passes only once the entry passed validation for its current code.
zetta::pipeline::ValidationReport gate_with_workspace_methodologies(
    const ApiContext& context, const zetta::ir::ExecutionGraph& graph) {
    zetta::pipeline::ValidationReport report = zetta::pipeline::default_gate().run(graph);
    const std::optional<GraphWorkspaceMethodology> workspace =
        graph_workspace_methodology(context, graph);
    if (!workspace) return report;
    zetta::pipeline::CheckResult result{"methodology", zetta::pipeline::CheckStatus::Pass,
                                        ""};
    if (!workspace->value) {
        result.status = zetta::pipeline::CheckStatus::Fail;
        result.reason = "this pipeline names the workspace methodology " + workspace->id +
                        ", which is not registered in this workspace";
    } else if (!workspace->runnable) {
        const nlohmann::json validation =
            workspace->value->value("validation", nlohmann::json::object());
        result.status = zetta::pipeline::CheckStatus::Fail;
        result.reason = "the workspace methodology " + workspace->id + " (" +
                        workspace->value->at("name").get<std::string>() +
                        ") has not passed validation for its current code (status " +
                        validation.value("status", std::string("not_validated")) +
                        "): validate it before this pipeline can be verified or run";
    } else {
        result.reason = "the workspace methodology " + workspace->id +
                        " passed validation for its current code; Lake's runner runs it";
    }
    bool replaced = false;
    for (zetta::pipeline::CheckResult& check : report.checks) {
        if (check.name == "methodology") {
            check = result;
            replaced = true;
        }
    }
    if (!replaced) report.checks.push_back(result);
    report.block_deploy = !report.passed();
    return report;
}

// get_graph / get_project / graph_json report a graph's methodology and
// whether it can run; a workspace methodology's answer comes from its entry.
std::pair<nlohmann::json, bool> graph_methodology_fields(const ApiContext& context,
                                                         const zetta::ir::ExecutionGraph& graph) {
    if (const std::optional<GraphWorkspaceMethodology> workspace =
            graph_workspace_methodology(context, graph)) {
        return {nlohmann::json(workspace->id), workspace->runnable};
    }
    const zetta::ml::Methodology* methodology = zetta::ml::methodology_for_graph(graph);
    const bool has_model = !graph.nodes_of_kind(zetta::ir::StepKind::Model).empty();
    return {methodology != nullptr ? nlohmann::json(methodology->id) : nlohmann::json(nullptr),
            methodology != nullptr ? methodology->runnable : !has_model};
}

// run on a graph whose model step is a workspace methodology: the engine does
// not run Python, so it answers `runner_required` with what Lake needs to run
// the code in its sandbox and report back through record_methodology_run.
ApiResult runner_required(const ApiContext& context, const zetta::pipeline::Project& project,
                          const zetta::pipeline::ProjectGraph& graph,
                          const GraphWorkspaceMethodology& workspace,
                          const nlohmann::json& input) {
    nlohmann::json model_step = nlohmann::json::object();
    for (const auto& node : graph.graph().nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (node && node->attributes.is_object()) {
            model_step = node->attributes;
            break;
        }
    }
    std::string dataset_path = input.value("dataset_path", std::string());
    if (dataset_path.empty()) {
        for (const auto& node : graph.graph().nodes_of_kind(zetta::ir::StepKind::Connect)) {
            if (!node || !node->attributes.is_object()) continue;
            const std::string source = node->attributes.value("source", std::string());
            const std::string table = node->attributes.value("table", std::string());
            if (!source.empty() && !table.empty()) {
                dataset_path = source + "." + table;
                break;
            }
        }
    }
    (void)context;
    ApiResult result = ApiResult::failure(
        "runner_required",
        "this pipeline's model step is the workspace methodology " + workspace.id +
            ", whose Python code runs in Lake's runner, not in the engine. Run it through "
            "Lake (start_run); the runner reports the result with record_methodology_run.");
    result.error.details = {
        {"methodology", workspace.id},
        {"dataset_path", dataset_path},
        {"params", model_step.value("parameters", nlohmann::json::object())},
        {"target", model_step.value("target", std::string())},
        {"holdout_fraction", model_step.value("holdout_fraction", 0.2)},
        {"project", project.id()},
        {"project_name", project.name()},
        {"graph", graph.id()},
        {"graph_name", graph.name()},
        {"code", workspace.value ? workspace.value->at("code") : nlohmann::json(nullptr)}};
    return result;
}

bool strict_base64(std::string_view text);

// Where a chunked artifact (lake#690) waits until record_methodology_run
// adopts it: one file per methodology and Lake run, named by a digest so no
// caller-chosen text reaches the path.
std::filesystem::path methodology_artifact_staging_path(const ApiContext& context,
                                                        const std::string& methodology,
                                                        const std::string& lake_run) {
    return std::filesystem::path(context.node.config().data_dir) / "methodology-artifacts" /
           (zetta::util::sha256_hex(methodology + "\n" + lake_run) + ".part");
}

const nlohmann::json& append_methodology_artifact_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"methodology", "run_id", "offset", "chunk_base64"}},
        {"additionalProperties", false},
        {"properties",
         {{"methodology",
           {{"type", "string"},
            {"pattern", "^W-[0-9a-f]{8}$"},
            {"description", "the workspace methodology that ran"}}},
          {"run_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "Lake's id for the run; record_methodology_run names the same"}}},
          {"offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"description",
             "where this chunk starts; at most the bytes already received. A chunk below that "
             "replaces what was held from there"}}},
          {"chunk_base64",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the chunk, base64; at most 8 MiB decoded"}}}}}};
    return schema;
}

const nlohmann::json& append_methodology_artifact_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"received"}},
        {"additionalProperties", false},
        {"properties",
         {{"received",
           {{"type", "integer"}, {"description", "the bytes now staged for this run"}}}}}};
    return schema;
}

ApiResult handle_append_methodology_artifact(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("methodology").get<std::string>();
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no workspace methodologies on a standalone node");
    }
    const zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
    if (!store.get(id)) return ApiResult::failure("not_found", "no methodology '" + id + "'");
    const std::string encoded = input.at("chunk_base64").get<std::string>();
    if (!strict_base64(encoded)) {
        return ApiResult::failure("invalid_input", "chunk_base64 is not base64", "/chunk_base64");
    }
    if (encoded.size() / 4 * 3 > kMaxMethodologyArtifactChunkBytes + 3) {
        return ApiResult::failure("invalid_input",
                                  "a chunk is at most " +
                                      std::to_string(kMaxMethodologyArtifactChunkBytes) + " bytes",
                                  "/chunk_base64");
    }
    const std::string chunk = zetta::detail::base64_decode_to_string(encoded);
    const std::uintmax_t offset = input.at("offset").get<std::uintmax_t>();
    if (offset + chunk.size() > kMaxStagedMethodologyArtifactBytes) {
        return ApiResult::failure(
            "invalid_input",
            "the artifact would be larger than " +
                std::to_string(kMaxStagedMethodologyArtifactBytes) + " bytes, the most a run can report",
            "/offset");
    }
    const std::filesystem::path path =
        methodology_artifact_staging_path(context, id, input.at("run_id").get<std::string>());
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::uintmax_t held = std::filesystem::exists(path, ec) ? std::filesystem::file_size(path, ec) : 0;
    if (ec) held = 0;
    if (offset > held) {
        return ApiResult::failure("conflict",
                                  "expected a chunk at offset " + std::to_string(held) +
                                      " or below; " + std::to_string(held) + " bytes are staged",
                                  "/offset");
    }
    if (offset == 0) {
        std::ofstream create(path, std::ios::binary | std::ios::trunc);
        if (!create) return ApiResult::failure("engine_error", "the artifact could not be staged");
    } else if (offset < held) {
        std::filesystem::resize_file(path, offset, ec);
        if (ec) return ApiResult::failure("engine_error", "the artifact could not be staged");
    }
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out.write(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (!out.good()) {
            return ApiResult::failure("engine_error", "the artifact could not be staged");
        }
    }
    return ApiResult::success({{"received", offset + chunk.size()}});
}

const nlohmann::json& read_model_weights_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}, {"minLength", 1}, {"description", "the model's id"}}},
          {"offset", {{"type", "integer"}, {"minimum", 0}, {"default", 0}}},
          {"limit",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", static_cast<std::int64_t>(kMaxMethodologyArtifactChunkBytes)},
            {"default", static_cast<std::int64_t>(kMaxMethodologyArtifactChunkBytes)},
            {"description", "bytes to read, at most 8 MiB"}}}}}};
    return schema;
}

const nlohmann::json& read_model_weights_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "offset", "chunk_base64", "next_offset", "total_bytes", "sha256"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"offset", {{"type", "integer"}}},
          {"chunk_base64", {{"type", "string"}}},
          {"next_offset", {{"type", {"integer", "null"}}}},
          {"total_bytes", {{"type", "integer"}}},
          {"sha256", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_read_model_weights(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("model_id").get<std::string>();
    const auto model = context.node.find_model(id);
    if (!model) return ApiResult::failure("not_found", "no model with id '" + id + "'");
    if (!model->weights() || !model->has_weights_body()) {
        return ApiResult::failure("not_found", "model '" + id + "' has no weights on this node");
    }
    const std::uintmax_t total = model->weights()->size;
    const std::uintmax_t offset = input.value("offset", std::uintmax_t{0});
    if (offset > total) {
        return ApiResult::failure("invalid_input",
                                  "offset is past the weights' " + std::to_string(total) + " bytes",
                                  "/offset");
    }
    const std::size_t limit =
        input.value("limit", static_cast<std::size_t>(kMaxMethodologyArtifactChunkBytes));
    const std::optional<std::string> chunk = model->read_weights_range(offset, limit);
    if (!chunk) {
        return ApiResult::failure("not_found", "model '" + id + "' has no weights on this node");
    }
    const std::uintmax_t end = offset + chunk->size();
    return ApiResult::success(
        {{"model_id", id},
         {"offset", offset},
         {"chunk_base64", zetta::detail::base64_encode_bytes(*chunk)},
         {"next_offset", end < total ? nlohmann::json(end) : nlohmann::json(nullptr)},
         {"total_bytes", total},
         {"sha256", model->weights()->sha256}});
}

const nlohmann::json& record_methodology_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"methodology", "run_id", "mode"}},
        {"additionalProperties", false},
        {"properties",
         {{"methodology",
           {{"type", "string"},
            {"pattern", "^W-[0-9a-f]{8}$"},
            {"description", "the workspace methodology that ran"}}},
          {"run_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "Lake's id for the run, kept on the Model as its lineage"}}},
          {"mode",
           {{"type", "string"},
            {"enum", {"validate", "train"}},
            {"description",
             "train registers a Model and a Run; validate registers nothing (Lake records the "
             "outcome with update_methodology's validation)"}}},
          {"dataset_path", {{"type", "string"}, {"description", "database.table it read"}}},
          {"params", {{"type", "object"}, {"description", "the parameters it ran with"}}},
          {"metrics",
           {{"type", "object"},
            {"additionalProperties", {{"type", "number"}}},
            {"description", "in_sample_<m> and held_out_<m>, numbers only"}}},
          {"rows",
           {{"type", "object"},
            {"additionalProperties", false},
            {"properties",
             {{"train", {{"type", "integer"}, {"minimum", 0}}},
              {"held_out", {{"type", "integer"}, {"minimum", 0}}}}}}},
          {"artifact_base64",
           {{"type", "string"},
            {"description",
             "the fitted artifact, base64; at most 24 MiB decoded. Required for train."}}},
          {"artifact_staged",
           {{"type", "object"},
            {"required", {"bytes", "sha256"}},
            {"additionalProperties", false},
            {"description",
             "instead of artifact_base64, for an artifact sent in chunks with "
             "append_methodology_artifact (lake#690): its size and SHA-256, which must match "
             "what was staged for this methodology and run_id"},
            {"properties",
             {{"bytes", {{"type", "integer"}, {"minimum", 1}}},
              {"sha256", {{"type", "string"}, {"pattern", "^[0-9a-f]{64}$"}}}}}}},
          {"artifact_format", {{"type", "string"}, {"enum", {"bytes", "pickle"}}}},
          {"features",
           {{"type", "array"}, {"items", {{"type", "string"}}},
            {"description", "the feature columns it was given"}}},
          {"project",
           {{"type", "string"},
            {"description", "for a pipeline step: the project's name or id"}}},
          {"graph",
           {{"type", "string"},
            {"description", "for a pipeline step: the graph's name or id"}}},
          {"study",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "for an experiment's trial (lake#689): the study it belongs to; with trial"}}},
          {"trial",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "for an experiment's trial: the trial's Run id, from the study's "
                            "runner work; the result completes that Run"}}},
          {"duration_seconds",
           {{"type", "number"},
            {"minimum", 0},
            {"description", "how long the code ran; an experiment's trial counts it as its cost "
                            "in trial-seconds"}}}}}};
    return schema;
}

const nlohmann::json& record_methodology_run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "run_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", {"string", "null"}}}},
          {"run_id", {{"type", {"string", "null"}}}},
          {"study",
           {{"type", "object"},
            {"description",
             "for an experiment's trial: the trial's outcome, the study's status, and the trials "
             "now waiting for Lake's runner"},
            {"required", {"id", "trial_state", "objective", "status", "runner"}},
            {"additionalProperties", false},
            {"properties",
             {{"id", {{"type", "string"}}},
              {"trial_state", {{"type", "string"}, {"enum", {"complete", "failed"}}}},
              {"objective", {{"type", {"number", "null"}}}},
              {"status",
               {{"type", "string"}, {"enum", {"running", "stopped", "complete", "failed"}}}},
              {"runner",
               {{"type", "array"}, {"items", zetta::ml::workspace_study_work_schema()}}}}}}}}}};
    return schema;
}

bool strict_base64(std::string_view text) {
    std::size_t padding = 0;
    for (const char character : text) {
        if (character == '=') {
            ++padding;
            continue;
        }
        if (padding > 0) return false;
        const bool valid = (character >= 'A' && character <= 'Z') ||
                           (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9') || character == '+' ||
                           character == '/';
        if (!valid) return false;
    }
    return padding <= 2 && text.size() % 4 == 0;
}

ApiResult handle_record_methodology_run(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("methodology").get<std::string>();
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no workspace methodologies on a standalone node");
    }
    const zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
    const std::optional<nlohmann::json> methodology = store.get(id);
    if (!methodology) return ApiResult::failure("not_found", "no methodology '" + id + "'");
    if (input.at("mode").get<std::string>() == "validate") {
        return ApiResult::success({{"model_id", nullptr}, {"run_id", nullptr}});
    }

    const std::string lake_run_for_staging = input.at("run_id").get<std::string>();
    const bool staged = input.contains("artifact_staged");
    const std::string encoded = input.value("artifact_base64", std::string());
    std::string artifact;
    std::filesystem::path staged_path;
    std::uintmax_t staged_bytes = 0;
    std::string staged_sha256;
    if (staged && !encoded.empty()) {
        return ApiResult::failure("invalid_input",
                                  "report the artifact inline or staged, not both",
                                  "/artifact_staged");
    }
    if (staged) {
        // lake#690: the artifact was sent with append_methodology_artifact.
        staged_path = methodology_artifact_staging_path(context, id, lake_run_for_staging);
        staged_bytes = input.at("artifact_staged").at("bytes").get<std::uintmax_t>();
        staged_sha256 = input.at("artifact_staged").at("sha256").get<std::string>();
        std::error_code ec;
        const std::uintmax_t held = std::filesystem::file_size(staged_path, ec);
        if (ec) {
            return ApiResult::failure("conflict",
                                      "no artifact is staged for this run; send it with "
                                      "append_methodology_artifact first",
                                      "/artifact_staged");
        }
        if (held != staged_bytes) {
            return ApiResult::failure("conflict",
                                      "the staged artifact is " + std::to_string(held) +
                                          " bytes, not " + std::to_string(staged_bytes),
                                      "/artifact_staged/bytes");
        }
        const std::optional<std::string> digest = zetta::util::sha256_file_hex(staged_path);
        if (!digest || *digest != staged_sha256) {
            return ApiResult::failure("conflict",
                                      "the staged artifact does not match its SHA-256",
                                      "/artifact_staged/sha256");
        }
    } else {
        if (encoded.empty()) {
            return ApiResult::failure("invalid_input", "a training run reports its artifact",
                                      "/artifact_base64");
        }
        if (!strict_base64(encoded)) {
            return ApiResult::failure("invalid_input", "artifact_base64 is not base64",
                                      "/artifact_base64");
        }
        if (encoded.size() / 4 * 3 > kMaxMethodologyArtifactBytes + 3) {
            return ApiResult::failure(
                "invalid_input",
                "the artifact is larger than " + std::to_string(kMaxMethodologyArtifactBytes) +
                    " bytes, the most a run can report inline; send it in chunks with "
                    "append_methodology_artifact and report artifact_staged",
                "/artifact_base64");
        }
        artifact = zetta::detail::base64_decode_to_string(encoded);
        if (artifact.size() > kMaxMethodologyArtifactBytes) {
            return ApiResult::failure("invalid_input",
                                      "the artifact is larger than " +
                                          std::to_string(kMaxMethodologyArtifactBytes) + " bytes",
                                      "/artifact_base64");
        }
    }

    std::shared_ptr<zetta::pipeline::Project> project;
    std::shared_ptr<zetta::pipeline::ProjectGraph> graph;
    if (input.contains("project") || input.contains("graph")) {
        if (!input.contains("project") || !input.contains("graph")) {
            return ApiResult::failure("invalid_input",
                                      "a pipeline step names both project and graph");
        }
        project = resolve_project(context, input.at("project").get<std::string>());
        if (!project) {
            return ApiResult::failure("not_found", "no project '" +
                                                       input.at("project").get<std::string>() +
                                                       "'");
        }
        graph = resolve_graph(*project, input.at("graph").get<std::string>());
        if (!graph) {
            return ApiResult::failure("not_found", "no graph '" +
                                                       input.at("graph").get<std::string>() +
                                                       "' on project '" + project->name() + "'");
        }
        if (zetta::ml::workspace_methodology_of_graph(graph->graph()) != id) {
            return ApiResult::failure("invalid_input",
                                      "the graph '" + graph->name() +
                                          "' does not run the workspace methodology " + id,
                                      "/graph");
        }
    }

    const std::string lake_run = input.at("run_id").get<std::string>();
    const std::string dataset_path = input.value("dataset_path", std::string());
    const nlohmann::json params = input.value("params", nlohmann::json::object());
    const nlohmann::json metrics = input.value("metrics", nlohmann::json::object());
    const std::string name = methodology->at("name").get<std::string>();

    // An experiment's trial (lake#689): the result completes the trial's own
    // Run, which the study admitted and Lake's runner ran.
    std::shared_ptr<zetta::ml::Study> study;
    const zetta::ml::StudyTrial* trial = nullptr;
    if (input.contains("study") || input.contains("trial")) {
        if (!input.contains("study") || !input.contains("trial")) {
            return ApiResult::failure("invalid_input", "an experiment's trial names both study and trial");
        }
        study = context.node.find_study(input.at("study").get<std::string>());
        if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
        if (study->spec().methodology != id) {
            return ApiResult::failure("invalid_input",
                                      "the study " + study->spec().name + " runs " +
                                          study->spec().methodology + ", not " + id,
                                      "/study");
        }
        const std::string trial_run = input.at("trial").get<std::string>();
        const auto found = std::ranges::find(study->value().trials, trial_run,
                                             &zetta::ml::StudyTrial::run);
        if (found == study->value().trials.end()) {
            return ApiResult::failure("not_found", "the study has no trial " + trial_run,
                                      "/trial");
        }
        if (found->state != zetta::ml::TrialState::Running) {
            return ApiResult::failure("conflict",
                                      "trial " + std::to_string(found->number) +
                                          " already ended (" +
                                          std::string(zetta::ml::to_string(found->state)) + ")",
                                      "/trial");
        }
        trial = &*found;
    }

    const std::shared_ptr<zetta::pipeline::Run> run =
        trial != nullptr
            ? context.node.find_run(trial->run)
            : context.node.create_run(project ? project->id() : std::string(),
                                      graph ? graph->id() : std::string());
    if (!run) return ApiResult::failure("engine_error", "the run record could not be created");
    // The dataset Lake's runner read (zetta#1272).
    if (!dataset_path.empty()) run->add_reads(context.node.tables_read({}, {}, dataset_path));
    run->set_status(zetta::pipeline::RunStatus::Running);
    run->add_log({zetta::util::now_iso8601(), "info",
                  "ran the workspace methodology " + id + " (" + name +
                      ") in Lake's runner, Lake run " + lake_run +
                      (dataset_path.empty() ? std::string() : ", on " + dataset_path)});
    std::string error;
    // A trial's Run already holds its sampled parameters.
    if (trial == nullptr && !params.empty() &&
        !context.node.log_run_params(run->id(), params, &error)) {
        run->add_log({zetta::util::now_iso8601(), "warn", "parameters not recorded: " + error});
    }
    std::map<std::string, double> values;
    for (const auto& [key, number] : metrics.items()) values[key] = number.get<double>();
    if (!values.empty() && !context.node.log_run_metrics(run->id(), 0, values, {}, &error)) {
        run->add_log({zetta::util::now_iso8601(), "warn", "metrics not recorded: " + error});
    }

    zetta::ml::ModelValue model_value;
    model_value.kind = zetta::ml::workspace_model_kind(id);
    model_value.name =
        trial != nullptr
            ? zetta::ml::study_trial_model_name(study->spec().name, study->id(), trial->number)
            : name + " " + zetta::util::now_iso8601().substr(0, 10);
    model_value.project = project ? project->id() : std::string();
    model_value.run = run->id();
    model_value.metrics = metrics;
    model_value.feature_schema = {
        {"methodology", id},
        {"lake_run_id", lake_run},
        {"code", methodology->at("code")},
        {"params", params},
        {"features", input.value("features", nlohmann::json::array())},
        {"artifact_format", input.value("artifact_format", std::string("pickle"))}};
    if (!dataset_path.empty()) model_value.feature_schema["dataset_path"] = dataset_path;
    if (input.contains("rows")) model_value.feature_schema["rows"] = input.at("rows");
    const std::shared_ptr<zetta::ml::Model> model = context.node.create_model(model_value);
    const bool weighted =
        model && (staged ? model->adopt_weights_file(staged_path, staged_bytes, staged_sha256)
                         : model->set_weights(artifact));
    if (!weighted) {
        run->add_log({zetta::util::now_iso8601(), "error", "the model could not be registered"});
        run->set_status(zetta::pipeline::RunStatus::Failed);
        return ApiResult::failure("engine_error", "the model could not be registered");
    }
    run->set_outputs({{"model", name, model->id()}});
    zetta::pipeline::RunLineage lineage;
    if (!dataset_path.empty()) lineage.sources.push_back(dataset_path);
    lineage.steps.push_back(graph ? "python_methodology" : id);
    lineage.freshness = zetta::util::now_iso8601();
    lineage.confidence = 1.0;
    run->set_lineage(std::move(lineage));
    run->set_status(zetta::pipeline::RunStatus::Succeeded);
    nlohmann::json output = {{"model_id", model->id()}, {"run_id", run->id()}};
    if (trial != nullptr) {
        const std::string metric = study->spec().objective.metric;
        std::optional<double> objective;
        if (const auto value = values.find(metric); value != values.end() &&
                                                    std::isfinite(value->second)) {
            objective = value->second;
        }
        const std::string trial_run = trial->run;
        const double cost = input.value("duration_seconds", 0.0);
        zetta::ml::finish_workspace_trial(
            context.node, *study, trial_run, objective, cost,
            objective ? std::string()
                      : "the run did not report the objective metric " + metric,
            model->id());
        zetta::ml::advance_workspace_study(context.node, *study);
        output["study"] = {
            {"id", study->id()},
            {"trial_state", objective ? "complete" : "failed"},
            {"objective", objective ? nlohmann::json(*objective) : nlohmann::json(nullptr)},
            {"status", zetta::ml::to_string(study->status())},
            {"runner", zetta::ml::workspace_study_runner_work(context.node, *study)}};
    }
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
        {"required", {"project"}},
        {"additionalProperties", false},
        {"properties",
         {{"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the owning project's name, or its uuid"}}},
          {"input_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the captured intent's uuid (capture_intent). Optional: omit it "
             "and the "
             "project's most recently captured intent compiles, which is what "
             "you want "
             "in the common case of describing a goal and building it"}}},
          {"name",
           {{"type", "string"},
            {"description",
             "the stored graph's name (default '<shape> skeleton')"}}},
          {"methodology",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "methodology id (a catalogue M-n or a workspace W-<8 hex>) or model kind; "
             "takes precedence"}}},
          {"solution",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "solution id resolved to its primary methodology"}}},
          {"hints",
           {{"type", "object"},
            {"description",
             "known dataset, label, time column, and other shape hints"}}}}}};
    return schema;
}

ApiResult handle_compile_intent(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("project").get<std::string>();
    auto project = resolve_project(context, name);
    if (!project) {
        return ApiResult::failure("not_found",
                                  "no project named '" + name + "' (or with that id)");
    }
    // Threading a uuid from capture_intent into compile_intent made two calls out of
    // one act, and forgetting it failed with a schema error naming a property rather
    // than anything about intents. Omit it and the project's most recent intent
    // compiles — the common case by a wide margin.
    std::string input_id = input.value("input_id", std::string{});
    if (input_id.empty()) {
        for (const std::shared_ptr<zetta::pipeline::ProjectInput>& candidate :
             project->inputs()) {
            if (candidate) input_id = candidate->id();  // inputs() is insertion order
        }
        if (input_id.empty()) {
            return ApiResult::failure(
                "not_found",
                "project '" + project->name() +
                    "' has no captured intent to compile — describe the goal first "
                    "(capture_intent)");
        }
    }
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
    zetta::pipeline::IntentCompileOptions options;
    options.project = project->name();
    options.methodology = input.value("methodology", "");
    options.solution = input.value("solution", "");
    options.hints = input.value("hints", nlohmann::json::object());
    // A workspace methodology (zetta#1261) is read from the replicated registry;
    // its shape is built from the entry.
    std::optional<nlohmann::json> workspace_entry;
    if (zetta::ml::is_workspace_methodology_id(options.methodology) &&
        context.node.replication() != nullptr) {
        const zetta::ml::WorkspaceMethodologyStore store(*context.node.replication());
        workspace_entry = store.get(options.methodology);
        if (workspace_entry) options.workspace_methodology = &*workspace_entry;
    }
    std::string compile_error;
    std::string template_name;
    auto graph = project->compile_intent_to_graph(
        input_id, input.value("name", ""), options, &compile_error,
        &template_name);
    if (!graph) {
        if (!compile_error.empty()) {
            const bool unsupported =
                compile_error.find("is not supported yet:") !=
                    std::string::npos ||
                compile_error.find("does not match a supported methodology") !=
                    std::string::npos;
            return ApiResult::failure(
                unsupported ? "not_supported" : "invalid_input", compile_error);
        }
        return ApiResult::failure(
            "engine_error",
            "compile_intent could not store a graph from input '" + input_id +
                "' (payload body not present locally, empty intent, or the "
                "graph name collides with an existing graph)");
    }
    nlohmann::json output = graph_json(context, *graph);
    output["project"] = project->name();
    // Which template the steps came from (#1238): their names are the
    // template's until someone edits them.
    output["template"] = template_name;
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
    nlohmann::json output = graph_json(context, *graph);
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
    nlohmann::json output = graph_json(context, *graph);
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
            {"description", "the stored graph's name, or its uuid"}}},
          {"include_not_applicable",
           {{"type", "boolean"},
            {"description",
             "also list the checks that had nothing to check on this graph, "
             "with status not_applicable (default false: only the checks that "
             "applied are listed, and the rest are counted)"}}}}}};
    return schema;
}

const nlohmann::json& gate_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json report = validation_report_json_schema();
        report["properties"]["project"] = {{"type", "string"}};
        report["properties"]["graph"] = {{"type", "string"}};
        report["required"].push_back("project");
        report["required"].push_back("graph");
        return report;
    }();
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
    // Review-before-build (#885): a proposed plan must be approved before it can
    // be verified. Approval is the human "yes, build this" the design's Proposed
    // Solution screen collects; approve_plan flips it.
    if (!graph->approved()) {
        return ApiResult::failure(
            "not_approved",
            "the plan '" + graph->name() +
                "' is proposed — approve it (approve_plan) before verification");
    }
    const zetta::pipeline::ValidationReport report =
        gate_with_workspace_methodologies(context, graph->graph());
    // A blocked graph is a successful gate *evaluation* — the report carries
    // the verdict; deploy/run are where a blocked graph becomes an error.
    // checks that applied + verdict + block_deploy + applied/not_applicable
    // counts (#1240); the not-applicable checks only on request, for audits.
    nlohmann::json output;
    zetta::pipeline::to_json(output, report,
                             input.value("include_not_applicable", false));
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

const nlohmann::json& deploy_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"deployment_id", "graph", "target", "gate", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"deployment_id", {{"type", "string"}}},
          {"graph", {{"type", "string"}}},
          {"target", {{"type", "string"}}},
          {"gate", validation_report_json_schema()},
          {"project", {{"type", "string"}}}}}};
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
    // Review-before-build (#885): no deploy of a plan the human hasn't approved.
    // (This is orthogonal to the correctness gate below — approval is the human
    // sign-off; the gate is the machine verification. Both must pass.)
    if (!graph->approved()) {
        return ApiResult::failure(
            "not_approved",
            "the plan '" + graph->name() +
                "' is proposed — approve it (approve_plan) before deploy");
    }
    const auto kind = zetta::pipeline::deployment_kind_from_name(
        input.value("target", "local"));
    if (!kind) {
        return ApiResult::failure("invalid_input", "unknown deployment target",
                                  "/target");
    }
    // A workspace methodology's model is served by nothing yet: its code runs only
    // in Lake's runner (zetta#1261).
    if (const std::optional<GraphWorkspaceMethodology> workspace =
            graph_workspace_methodology(context, graph->graph())) {
        return ApiResult::failure(
            "not_supported",
            "this pipeline runs the workspace methodology " + workspace->id +
                " in Lake's runner; serving it from a deployment is not built yet. Run it "
                "through Lake instead.");
    }
    // The gate report first, for the failing check's reason; the engine's own
    // deploy_graph re-runs the gate and audits BOTH outcomes (RULE-1 lives in
    // the engine interface — this handler cannot bypass it by construction).
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

const nlohmann::json& label_efficiency_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"queue_size", {{"type", "integer"}, {"minimum", 1}}},
          {"initial_labels", {{"type", "integer"}, {"minimum", 1}}},
          {"gold_fraction",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"exclusiveMaximum", 1}}},
          {"seed", {{"type", "integer"}, {"minimum", 0}}},
          {"uncertainty",
           {{"type", "string"}, {"enum", {"margin", "entropy"}}}},
          {"diversity_weight",
           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}}},
          {"logistic_iterations",
           {{"type", "integer"}, {"minimum", 1}}},
          {"logistic_learning_rate",
           {{"type", "number"}, {"exclusiveMinimum", 0}}},
          {"label_model_iterations",
           {{"type", "integer"}, {"minimum", 1}}},
          {"labelling_functions",
           {{"type", "array"},
            {"minItems", 1},
            {"items",
             {{"type", "object"},
              {"required", {"name", "column", "value", "label"}},
              {"additionalProperties", false},
              {"properties",
               {{"name", {{"type", "string"}, {"minLength", 1}}},
                {"column", {{"type", "string"}, {"minLength", 1}}},
                {"operator",
                 {{"type", "string"},
                  {"enum",
                   {"less", "less_equal", "greater", "greater_equal",
                    "equal"}}}},
                {"value", {{"type", "number"}}},
                {"label", {{"type", "integer"}, {"enum", {0, 1}}}},
                {"otherwise_label",
                 {{"type", {"integer", "null"}},
                  {"enum", {0, 1, nullptr}}}}}}}}}}}}};
    return schema;
}

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
             "the data to train on, in either form: a `database.table` landed by "
             "a connection or a query (what add_dataset / create_dataset_from_query "
             "produce), or a churn dataset directory on the node's filesystem. "
             "Default: the table the pipeline's first connect step names (its "
             "`source` + `table` attributes), when this node holds it, with the model "
             "step's `target` as the label; otherwise the built-in demo dataset"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"model_name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model or solver-result name"}}},
          {"algorithm",
           {{"type", "string"},
            {"enum", {"thompson", "linucb", "linear_thompson"}},
            {"description", "M-15 policy algorithm"}}},
          {"arms",
           {{"type", "array"},
            {"minItems", 2},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"description", "M-15 arm names; inferred from the table when omitted"}}},
          {"baseline_arm", {{"type", "string"}}},
          {"decision_column", {{"type", "string"}, {"minLength", 1}}},
          {"arm_column", {{"type", "string"}, {"minLength", 1}}},
          {"reward_column", {{"type", "string"}, {"minLength", 1}}},
          {"context_columns",
           {{"type", "array"},
            {"items", {{"type", "string"}, {"minLength", 1}}}}},
          {"exploration_floor",
           {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
          {"default_arm_cap",
           {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
          {"arm_caps",
           {{"type", "object"},
            {"additionalProperties",
             {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}}}},
          {"ridge", {{"type", "number"}, {"exclusiveMinimum", 0}}},
          {"confidence", {{"type", "number"}, {"minimum", 0}}},
          {"seed",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "deterministic seed for document, bandit, Bayesian, and privacy runs"}}},
          {"streaming", zetta::ml::streaming_params_schema()},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "at-risk list / playbook cap (0 = all)"}}},
          {"holdout_fraction",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"description",
             "hold this fraction of rows OUT of training and measure the model "
             "on them separately (e.g. 0.3). Without it the run's only metrics "
             "are in-sample — scored on the rows it trained on — and flatter"}}},
          {"duration_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "survival duration column (default duration)"}}},
          {"feature_time_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "survival feature cutoff column (default feature_time)"}}},
          {"interval_width",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"description", "survival interval width (default 30)"}}},
          {"max_intervals",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "survival horizon in intervals (default 12)"}}},
          {"validation_fraction",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"exclusiveMaximum", 1},
            {"description", "newest fraction held out for survival validation"}}},
          {"embedder_directory",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "M-10 only: local directory containing the pinned MiniLM config, "
             "vocab.txt, and model.safetensors (tests never download it)"}}},
          {"qwen_directory",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "M-10 only: optional local native-Qwen checkpoint used to compose "
             "answers; without it the grounded path returns an extractive answer"}}},
          {"text_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "M-10 only: text column (default the dataset contract's first feature)"}}},
          {"query_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-10 only: labelled evaluation query column"}}},
          {"relevant_id_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "M-10 only: integer id of the planted relevant chunk"}}},
          {"max_tokens",
           {{"type", "integer"},
            {"minimum", 2},
            {"description", "M-10 only: WordPiece token limit per text"}}},
          {"answer_tokens",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "M-10 only: native-Qwen completion token limit"}}},
          {"minimum_relevance",
           {{"type", "number"},
            {"minimum", -1},
            {"maximum", 1},
            {"description",
             "M-10 only: refuse an answer below this cosine score"}}},
          {"classifier_regularization",
           {{"type", "number"},
            {"minimum", 0},
            {"description",
             "M-10 only: non-negative regularization for the linear classifier head"}}},
          {"group_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-17 hierarchical group column"}}},
          {"nominal_coverage",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"exclusiveMaximum", 1},
            {"description", "M-17 requested interval coverage"}}},
          {"burn_in",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "M-17 Gibbs draws discarded before measurement"}}},
          {"posterior_draws",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "M-17 retained Gibbs draws"}}},
          {"epsilon",
           {{"type", "number"},
            {"exclusiveMinimum", 0},
            {"description", "M-20 total differential-privacy epsilon"}}},
          {"delta",
           {{"type", "number"},
            {"minimum", 0},
            {"description", "M-20 total differential-privacy delta"}}},
          {"noise",
           {{"type", "string"},
            {"enum", {"gaussian", "laplace"}},
            {"description", "M-20 DP histogram mechanism"}}},
          {"clip_min",
           {{"type", "number"},
            {"description", "M-20 public lower feature bound"}}},
          {"clip_max",
           {{"type", "number"},
            {"description", "M-20 public upper feature bound"}}},
          {"synthetic_rows",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "M-20 synthetic row count (default: source rows)"}}},
          {"num_trees",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "M-20 DP GBDT tree count"}}},
          {"max_depth",
           {{"type", "integer"},
            {"minimum", 1},
            {"description", "M-20 DP GBDT maximum depth"}}},
          {"coordinate",
           {{"type", "boolean"},
            {"description", "fan the run out to the live membership (default "
                            "true; false is only correct on a single-member "
                            "cluster)"}}},
          {"label_efficiency", label_efficiency_run_schema()},
          {"treatment_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-6 binary treatment column"}}},
          {"outcome_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-6 binary observed outcome column"}}},
          {"targeting_fraction",
           {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 1}}},
          {"bootstrap_samples", {{"type", "integer"}, {"minimum", 0}}},
          {"fields",
           {{"type", "array"},
            {"minItems", 1},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"description", "M-11: declared field names to extract"}}},
          {"document_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-11: source column containing text, PDF, or PGM bytes"}}},
          {"document_id_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-11: stable source document identifier column"}}},
          {"labels_column",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-11: optional object of labelled field values"}}},
          {"review_threshold",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"description", "M-11: route lower-confidence fields to review"}}},
          {"trocr_directory",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "M-11: pinned native TrOCR checkpoint directory"}}},

          {"optimization",
           {{"type", "object"},
            {"additionalProperties", false},
            {"description", "M-16 native LP/MIP solver controls"},
            {"properties",
             {{"maximize", {{"type", "boolean"}}},
              {"iteration_limit", {{"type", "integer"}, {"minimum", 1}}},
              {"node_limit", {{"type", "integer"}, {"minimum", 1}}},
              {"time_limit_ms", {{"type", "integer"}, {"minimum", 1}}},
              {"feasibility_tolerance",
               {{"type", "number"}, {"exclusiveMinimum", 0}}},
              {"integrality_tolerance",
               {{"type", "number"}, {"exclusiveMinimum", 0}}},
              {"seed", {{"type", "integer"}, {"minimum", 0}}}}}}}}}};
    return schema;
}

const nlohmann::json& run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "engine", "status"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"description",
             "the durable engine handle — poll get_run / system.runs"}}},
          {"engine",
           {{"type", "string"},
            {"description", "the methodology runner, or structural"}}},
          {"status", {{"type", "string"}}},
          {"note",
           {{"type", "string"},
            {"description", "structural runs: that nothing was trained, and "
                            "whether a given dataset was "
                            "read"}}},
          {"failed_step",
           {{"type", "string"},
            {"description", "structural runs only, on failure"}}},
          {"error",
           {{"type", "string"},
            {"description", "structural runs only, on failure"}}},
          {"poll",
           {{"type", "string"},
            {"description", "churn runs: where to read progress"}}},
          {"dataset",
           {{"type", "object"},
            {"description", "table datasets only: the resolved column contract "
                            "the run trained "
                            "under"}}},
          {"dataset_notes",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "what was inferred rather than given — read these "
                            "before trusting the "
                            "model"}}},
          {"solver_status",
           {{"type", "string"},
            {"description", "M-16 solver termination status"}}},
          {"model_id", {{"type", "string"}}},
          {"plan_table",
           {{"type", "string"},
            {"description",
             "M-16 decision table; absent without a feasible plan"}}},
          {"constraint_table",
           {{"type", "string"},
            {"description",
             "M-16 independent constraint verification table"}}},
          {"output_table",
           {{"type", "string"},
            {"description", "M-17 posterior estimate table"}}}}}};
    return schema;
}

// The table a pipeline reads, when its run is given none: the first connect step whose
// `source`+`table` attributes name a dataset this node holds, with the first model step's
// `target` as the label. lake points a project's pipeline at a table this way (a project made
// from an Opportunity, "Build a Pipeline" from a dataset); before, such a run trained on the
// built-in demo dataset and said nothing. Empty when no step names an existing table.
struct PipelineDataset {
    std::string path, step, label;
};
std::optional<PipelineDataset> pipeline_dataset(zetta::Node& node,
                                                const zetta::ir::ExecutionGraph& graph) {
    std::optional<PipelineDataset> found;
    std::string label;
    for (const std::shared_ptr<zetta::ir::Node>& step : graph.nodes()) {
        if (!step || !step->attributes.is_object()) continue;
        const auto text = [&](const char* key) {
            const auto value = step->attributes.find(key);
            return value != step->attributes.end() && value->is_string() ? value->get<std::string>()
                                                                         : std::string();
        };
        if (step->kind == zetta::ir::StepKind::Model && label.empty()) label = text("target");
        if (step->kind != zetta::ir::StepKind::Connect || found) continue;
        const std::string source = text("source"), table = text("table");
        if (source.empty() || table.empty()) continue;
        const auto database = node.find_database(source);
        if (!database || !database->find_table(table)) continue;
        found = PipelineDataset{source + "." + table, step->name, ""};
    }
    if (found) found->label = label;
    return found;
}

ApiResult handle_run(const nlohmann::json& given, ApiContext& context) {
    nlohmann::json input = given;
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
        gate_with_workspace_methodologies(context, graph->graph());
    if (report.block_deploy) {
        return ApiResult::failure(
            "engine_error", "blocked by gate: " + first_gate_failure(report));
    }
    // A workspace methodology (zetta#1261) runs in Lake's runner, never here.
    if (const std::optional<GraphWorkspaceMethodology> workspace =
            graph_workspace_methodology(context, graph->graph())) {
        return runner_required(context, *project, *graph, *workspace, input);
    }

    const bool has_model =
        !graph->graph().nodes_of_kind(zetta::ir::StepKind::Model).empty();
    const zetta::ml::Methodology* methodology =
        zetta::ml::methodology_for_graph(graph->graph());
    if (has_model && methodology == nullptr) {
        return ApiResult::failure(
            "not_supported",
            "this pipeline names a model_kind no registered methodology can "
            "run. Runnable methodologies: " +
                zetta::ml::runnable_methodologies());
    }
    if (methodology != nullptr && !methodology->runnable) {
        return ApiResult::failure(
            "not_supported",
            zetta::ml::unsupported_methodology_message(*methodology));
    }
    if (methodology != nullptr && !methodology->run) {
        return ApiResult::failure(
            "not_supported",
            methodology->name + " (" + methodology->id +
                ") is marked runnable but has no run callback");
    }

    // Step authority (#843): every step runs as the caller, not as the node.
    const zetta::pipeline::RunPrincipal principal{context.acting_key,
                                                  context.acting_is_user};
    if (methodology == nullptr) {
        // The structural walk checks each step as it reaches it (GraphExecutor).
        const zetta::pipeline::ExecutionResult executed = context.node.run_graph(
            graph->graph(), project->id(), graph->id(), principal);
        if (!executed.run) {
            return ApiResult::failure("engine_error",
                                      "the run record could not be created");
        }
        std::string structural =
            "structural: this pipeline has no model step, so its steps ran "
            "without training or scoring anything";
        if (input.contains("dataset_path")) {
            structural += ", and the dataset it was given was not read";
        }
        executed.run->add_log({zetta::util::now_iso8601(), "warn", structural});
        nlohmann::json output;
        output["run_id"] = executed.run->id();
        output["engine"] = "structural";
        output["note"] = structural;
        output["status"] = std::string(to_string(executed.run->status()));
        if (!executed.ok) {
            output["failed_step"] = executed.failed_step;
            output["error"] = executed.error;
        }
        return ApiResult::success(std::move(output));
    }

    // A methodology runs the pipeline through its own callback, not step by
    // step through GraphExecutor, so every step is authorized for the caller
    // before it starts. A denial runs nothing.
    zetta::pipeline::GraphAuthorization authorized;
    context.node.run_on_owner([&] {
        authorized =
            zetta::pipeline::authorize_graph_steps(context.node, principal, graph->graph());
    });
    if (!authorized) {
        return ApiResult::failure("unauthorized", authorized.reason);
    }

    // No dataset given (#1040): M-1 trains on the table the pipeline's own
    // connect step reads, with the model step's target as the label. The other
    // methodologies resolve their pipeline's table in their own run callbacks.
    if (methodology->id == "M-1" && !input.contains("dataset_path")) {
        if (const auto own = pipeline_dataset(context.node, graph->graph())) {
            input["dataset_path"] = own->path;
            if (!own->label.empty() && !input.contains("dataset")) {
                input["dataset"] = {{"label_column", own->label}};
            }
            input["pipeline_note"] =
                "read from the pipeline's connect step '" + own->step + "': " + own->path +
                (own->label.empty() ? std::string() : ", label '" + own->label + "'");
        }
    }
    zetta::ml::RunRequest request;
    request.context = &context;
    request.project = project.get();
    request.graph = graph.get();
    request.input = input;
    ApiResult result = methodology->run(request);
    if (!result.ok || !result.value.contains("run_id") ||
        !result.value.at("run_id").is_string()) {
        return result;
    }
    const std::string run_id = result.value.at("run_id").get<std::string>();
    const std::shared_ptr<zetta::pipeline::Run> run =
        context.node.find_run(run_id);
    // The run reads the dataset it was handed as well as its pipeline's own
    // (zetta#1258). Recorded before this handler returns, so no delete_dataset
    // can come between the run starting and the record.
    if (run) {
        if (const auto path = input.find("dataset_path");
            path != input.end() && path->is_string()) {
            run->add_reads(context.node.tables_read({}, {}, path->get<std::string>()));
        }
    }
    if (!run || !run->value().metrics_suppressed.empty() ||
        !run->value().metrics.at("series").empty()) {
        return result;
    }
    const bool completed =
        run->status() == zetta::pipeline::RunStatus::Succeeded;
    std::string metric_error;
    if (!context.node.log_run_metrics(
            run_id, 0,
            {{completed ? "fit.completed" : "fit.started", 1.0}}, {},
            &metric_error)) {
        return ApiResult::failure(
            "engine_error",
            "methodology Run was created but its initial metric failed: " +
                metric_error);
    }
    return result;
}
// --- standalone models (lake#654, docs/ml/standalone-models.md) -----------------
//
// A model people make on its own, not as one step of a project's pipeline. The
// methodology runners train through a project and a pipeline (RunRequest carries
// both, and the Run and the Model record them as lineage), so a standalone model
// gets an **internal** project of its own: created here, holding the one
// pipeline its methodology composes, and never listed to people
// (Node::project_names() leaves it out). Each training is a run of that pipeline,
// so the model's records are its versions exactly as a project's are.
// move_model_to_project reveals the project when a pipeline should grow around
// the model; deleting its last version removes it.

constexpr std::string_view kStandaloneSuffix = " (standalone model)";
// The model step attribute that keeps what a standalone model trains on, so
// retrain_model trains the same thing again. It rides in the replicated graph.
constexpr const char* kStandaloneTraining = "standalone_training";

std::string standalone_project_name(const std::string& model_name) {
    return model_name + std::string(kStandaloneSuffix);
}

// The internal project that holds a standalone model, or empty.
std::shared_ptr<zetta::pipeline::Project> standalone_project_of(
    ApiContext& context, const zetta::ml::Model& model) {
    if (model.project().empty()) return nullptr;
    auto project = context.node.project_by_id(model.project());
    return project && project->internal() ? project : nullptr;
}

std::shared_ptr<zetta::ml::Model> resolve_model(ApiContext& context,
                                                const std::string& wanted,
                                                std::string* error) {
    if (auto model = context.node.find_model(wanted)) return model;
    std::shared_ptr<zetta::ml::Model> named;
    std::size_t count = 0;
    for (const auto& each : context.node.models()) {
        if (each && !each->name().empty() && each->name() == wanted) {
            named = each;
            ++count;
        }
    }
    if (count > 1) {
        *error = std::to_string(count) + " models are named '" + wanted +
                 "'; name one by its model_id";
        return nullptr;
    }
    if (!named) *error = "no model named '" + wanted + "' (or with that id)";
    return named;
}

nlohmann::json model_step_training(const zetta::ir::ExecutionGraph& graph) {
    for (const auto& step : graph.nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (!step || !step->attributes.is_object()) continue;
        const auto found = step->attributes.find(kStandaloneTraining);
        if (found != step->attributes.end() && found->is_object()) return *found;
    }
    return nlohmann::json::object();
}

// The graph with `training` recorded on its model steps and the connect step
// pointed at the dataset (so the pipeline says what it reads when it is moved
// into a project). nullopt when the copy fails to parse back.
std::optional<zetta::ir::ExecutionGraph> with_standalone_training(
    const zetta::ir::ExecutionGraph& graph, const nlohmann::json& training) {
    std::string error;
    auto copy = zetta::ir::from_json(zetta::ir::to_json(graph), &error);
    if (!copy) return std::nullopt;
    const zetta::data::DatasetPath path =
        zetta::data::parse_dataset_path(training.value("dataset_path", std::string{}));
    for (const auto& step : copy->nodes_of_kind(zetta::ir::StepKind::Model)) {
        if (!step->attributes.is_object()) step->attributes = nlohmann::json::object();
        step->attributes[kStandaloneTraining] = training;
        if (training.contains("target")) step->attributes["target"] = training.at("target");
        step->attributes["model_name"] = training.value("model_name", std::string{});
    }
    if (path.kind == zetta::data::DatasetPathKind::Table) {
        const auto connects = copy->nodes_of_kind(zetta::ir::StepKind::Connect);
        if (!connects.empty()) {
            if (!connects.front()->attributes.is_object()) {
                connects.front()->attributes = nlohmann::json::object();
            }
            connects.front()->attributes["source"] = path.database;
            connects.front()->attributes["table"] = path.table;
        }
    }
    return copy;
}

// The `run` input that trains a standalone model from its recorded training.
nlohmann::json standalone_run_input(const std::string& project_id,
                                    const std::string& graph_name,
                                    const nlohmann::json& training) {
    nlohmann::json input = {{"project", project_id},
                            {"graph", graph_name},
                            {"model_name", training.value("model_name", graph_name)}};
    if (const auto path = training.find("dataset_path");
        path != training.end() && path->is_string()) {
        input["dataset_path"] = *path;
    }
    nlohmann::json contract = nlohmann::json::object();
    if (const auto target = training.find("target"); target != training.end() &&
                                                     target->is_string()) {
        contract["label_column"] = *target;
    }
    if (const auto entity = training.find("entity_column");
        entity != training.end() && entity->is_string()) {
        contract["entity_column"] = *entity;
    }
    if (!contract.empty()) input["dataset"] = std::move(contract);
    if (const auto holdout = training.find("holdout_fraction");
        holdout != training.end() && holdout->is_number() && holdout->get<double>() > 0) {
        input["holdout_fraction"] = *holdout;
    }
    return input;
}

nlohmann::json standalone_training_properties() {
    return {
        {"dataset_path",
         {{"type", "string"},
          {"minLength", 1},
          {"description",
           "the dataset to learn from, as `database.table` (list_datasets)"}}},
        {"target",
         {{"type", "string"},
          {"minLength", 1},
          {"description",
           "the column to predict. Omit it for a methodology that learns without one "
           "(segmentation, anomaly detection); a methodology whose `label` input is "
           "required refuses without it"}}},
        {"entity_column",
         {{"type", "string"},
          {"minLength", 1},
          {"description", "the column that identifies a row (default: the dataset's own)"}}},
        {"holdout_fraction",
         {{"type", "number"},
          {"exclusiveMinimum", 0},
          {"exclusiveMaximum", 1},
          {"description",
           "hold this fraction of rows out of training and score the model on them "
           "(e.g. 0.2). Without it the only figures are in-sample, and they flatter"}}}};
}

const nlohmann::json& train_model_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json properties = standalone_training_properties();
        properties["name"] = {{"type", "string"},
                              {"minLength", 1},
                              {"description", "the model's name; its versions keep it"}};
        properties["methodology"] = {
            {"type", "string"},
            {"minLength", 1},
            {"description",
             "how it learns: a methodology id from list_methodologies (e.g. `M-1`), "
             "or one of its model kinds"}};
        return nlohmann::json{{"type", "object"},
                              {"required", {"name", "methodology", "dataset_path"}},
                              {"additionalProperties", false},
                              {"properties", std::move(properties)}};
    }();
    return schema;
}

const nlohmann::json& train_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "run_id", "project_id", "pipeline", "standalone"}},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "the model's name"}}},
          {"run_id",
           {{"type", "string"},
            {"description",
             "the training run; the model's new version appears in list_models when "
             "it finishes (get_run)"}}},
          {"project_id",
           {{"type", "string"},
            {"description", "the internal project that holds the model (never listed)"}}},
          {"pipeline", {{"type", "string"}, {"description", "its pipeline's name"}}},
          {"methodology", {{"type", "string"}}},
          {"standalone", {{"type", "boolean"}}},
          {"status", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_train_model(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::string wanted = input.at("methodology").get<std::string>();
    const zetta::ml::Methodology* methodology = zetta::ml::find_methodology(wanted);
    if (methodology == nullptr) {
        return ApiResult::failure(
            "not_found", "no methodology '" + wanted +
                             "' (list_methodologies). Runnable: " +
                             zetta::ml::runnable_methodologies());
    }
    if (!methodology->runnable || !methodology->run || !methodology->build_shape) {
        return ApiResult::failure("not_supported",
                                  zetta::ml::unsupported_methodology_message(*methodology));
    }
    const std::string dataset_path = input.at("dataset_path").get<std::string>();
    const zetta::data::DatasetPath path = zetta::data::parse_dataset_path(dataset_path);
    if (path.kind != zetta::data::DatasetPathKind::Table) {
        return ApiResult::failure("invalid_input",
                                  "/dataset_path: name a dataset as `database.table`");
    }
    const auto database = context.node.find_database(path.database);
    if (!database || !database->find_table(path.table)) {
        return ApiResult::failure("not_found", "/dataset_path: no dataset '" +
                                                   path.database + "." + path.table +
                                                   "' (list_datasets)");
    }
    const bool needs_label = std::ranges::any_of(
        methodology->inputs, [](const zetta::ml::MethodologyInput& role) {
            return role.required && role.role == "label";
        });
    if (needs_label && !input.contains("target")) {
        return ApiResult::failure(
            "invalid_input", "/target: " + methodology->name + " (" + methodology->id +
                                 ") learns to predict a column; name it");
    }
    const std::string project_name = standalone_project_name(name);
    if (context.node.find_project(project_name)) {
        return ApiResult::failure(
            "invalid_input", "/name: a standalone model named '" + name +
                                 "' exists; retrain_model trains a new version of it");
    }

    auto project = context.node.create_project(
        project_name, actor_of(context),
        "Holds the standalone model '" + name + "'.", /*internal=*/true);
    if (!project) {
        return ApiResult::failure("engine_error", "could not create the model's project");
    }
    const auto discard = [&] { context.node.drop_project(project->name()); };

    nlohmann::json training = {{"model_name", name},
                               {"methodology", methodology->id},
                               {"dataset_path", path.database + "." + path.table}};
    for (const char* key : {"target", "entity_column", "holdout_fraction"}) {
        if (input.contains(key)) training[key] = input.at(key);
    }
    std::string intent = "Train " + name + " on " + path.database + "." + path.table;
    if (input.contains("target")) {
        intent += " to predict " + input.at("target").get<std::string>();
    }
    const auto intent_input = project->capture_intent(intent, actor_of(context));
    if (!intent_input) {
        discard();
        return ApiResult::failure("engine_error", "could not record what the model is for");
    }
    zetta::pipeline::IntentCompileOptions options;
    options.project = project->name();
    options.methodology = methodology->id;
    options.hints = {{"source", path.database}, {"table", path.table}};
    if (input.contains("target")) options.hints["label_column"] = input.at("target");
    if (input.contains("entity_column")) options.hints["entity_column"] = input.at("entity_column");
    std::string compile_error;
    auto compiled = project->compile_intent_to_graph(intent_input->id(), name, options,
                                                     &compile_error);
    if (!compiled) {
        discard();
        return ApiResult::failure("invalid_input",
                                  compile_error.empty()
                                      ? "the methodology composed no pipeline for this dataset"
                                      : compile_error);
    }
    const auto fitted = with_standalone_training(compiled->graph(), training);
    if (!fitted || !project->update_graph(compiled->name(), *fitted)) {
        discard();
        return ApiResult::failure("engine_error", "could not record the model's training");
    }
    // Asking for the model is the approval: there is no proposed pipeline for
    // anyone else to review, and it is never shown as one.
    project->approve_graph(name, actor_of(context), zetta::util::now_iso8601());
    ApiResult run = handle_run(standalone_run_input(project->id(), name, training), context);
    if (!run.ok) {
        discard();
        return run;
    }
    nlohmann::json output = run.value;
    output["name"] = name;
    output["project_id"] = project->id();
    output["pipeline"] = name;
    output["methodology"] = methodology->id;
    output["standalone"] = true;
    return ApiResult::success(std::move(output));
}

const nlohmann::json& retrain_model_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json properties = standalone_training_properties();
        properties["model_id"] = {
            {"type", "string"},
            {"minLength", 1},
            {"description",
             "any version of the model (its uuid), or its name when one model has it"}};
        for (auto& [key, value] : properties.items()) {
            if (key != "model_id") {
                value["description"] = value.value("description", std::string{}) +
                                       ". Given, it replaces what the model trained on "
                                       "and the model keeps it for its next retrain";
            }
        }
        return nlohmann::json{{"type", "object"},
                              {"required", {"model_id"}},
                              {"additionalProperties", false},
                              {"properties", std::move(properties)}};
    }();
    return schema;
}

const nlohmann::json& retrain_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "run_id", "project_id", "pipeline", "standalone"}},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"run_id",
           {{"type", "string"},
            {"description", "the run that trains the new version (get_run)"}}},
          {"project_id", {{"type", "string"}}},
          {"pipeline", {{"type", "string"}}},
          {"standalone", {{"type", "boolean"}}},
          {"status", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_retrain_model(const nlohmann::json& input, ApiContext& context) {
    std::string error;
    const auto model = resolve_model(context, input.at("model_id").get<std::string>(), &error);
    if (!model) return ApiResult::failure("not_found", error);
    const std::string run_id = !model->run_id().empty() ? model->run_id() : model->run();
    const auto run = run_id.empty() ? nullptr : context.node.find_run(run_id);
    const auto project = model->project().empty()
                             ? nullptr
                             : context.node.project_by_id(model->project());
    const auto graph = run && project ? project->find_graph_by_id(run->graph()) : nullptr;
    if (!graph) {
        return ApiResult::failure(
            "not_supported",
            "model '" + model->name() +
                "' was not trained here, so there is nothing to retrain; register_model "
                "with version_of adds a version of a brought model");
    }
    nlohmann::json training = model_step_training(graph->graph());
    if (training.empty()) training = {{"model_name", model->name()}};
    bool changed = false;
    for (const char* key : {"dataset_path", "target", "entity_column", "holdout_fraction"}) {
        if (input.contains(key) && training.value(key, nlohmann::json()) != input.at(key)) {
            training[key] = input.at(key);
            changed = true;
        }
    }
    if (input.contains("dataset_path")) {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(input.at("dataset_path").get<std::string>());
        const auto database = path.kind == zetta::data::DatasetPathKind::Table
                                  ? context.node.find_database(path.database)
                                  : nullptr;
        if (!database || !database->find_table(path.table)) {
            return ApiResult::failure("not_found",
                                      "/dataset_path: no dataset '" +
                                          input.at("dataset_path").get<std::string>() +
                                          "' (list_datasets)");
        }
        training["dataset_path"] = path.database + "." + path.table;
    }
    // A project's model keeps its pipeline as it is: what it trains on is the
    // pipeline's to say. Only a standalone model records its training here.
    const bool standalone = project->internal();
    if (changed && standalone) {
        const auto fitted = with_standalone_training(graph->graph(), training);
        if (!fitted || !project->update_graph(graph->name(), *fitted)) {
            return ApiResult::failure("engine_error", "could not record the new training");
        }
    }
    nlohmann::json run_input = standalone_run_input(project->id(), graph->name(), training);
    if (!standalone) {
        // Only what was asked to change; the pipeline supplies the rest.
        run_input = {{"project", project->id()},
                     {"graph", graph->name()},
                     {"model_name", model->name()}};
        const nlohmann::json overrides = standalone_run_input(project->id(), graph->name(),
                                                              training);
        for (const char* key : {"dataset_path", "holdout_fraction"}) {
            if (input.contains(key) && overrides.contains(key)) run_input[key] = overrides.at(key);
        }
        if ((input.contains("target") || input.contains("entity_column")) &&
            overrides.contains("dataset")) {
            run_input["dataset"] = overrides.at("dataset");
        }
    }
    ApiResult result = handle_run(run_input, context);
    if (!result.ok) return result;
    nlohmann::json output = result.value;
    output["name"] = model->name();
    output["project_id"] = project->id();
    output["pipeline"] = graph->name();
    output["standalone"] = standalone;
    return ApiResult::success(std::move(output));
}

const nlohmann::json& register_model_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the model's name (required unless `version_of` is given)"}}},
          {"weights",
           {{"type", "string"},
            {"minLength", 2},
            {"description",
             "the model file's contents, for a text file: XGBoost's JSON model, LightGBM's "
             "text model, or the engine's own JSON weights (objective, base_score, "
             "feature_names, trees). Give this or `weights_base64`"}}},
          {"weights_base64",
           {{"type", "string"},
            {"minLength", 4},
            {"description",
             "the model file's contents in base64, for any file and required for a binary "
             "one: ONNX (a TreeEnsembleRegressor or TreeEnsembleClassifier, as scikit-learn, "
             "XGBoost and LightGBM export) or XGBoost's UBJSON model. Give this or `weights`"}}},
          {"format",
           {{"type", "string"},
            {"enum", {"zetta", "onnx", "xgboost_json", "xgboost_ubjson", "lightgbm"}},
            {"description",
             "the file's format (default: read from the file). Every format is converted "
             "into the engine's tree model, so it is scored, served and explained like a "
             "trained one; a model it cannot convert exactly (a neural network, a "
             "multiclass or random-forest classifier, categorical splits, a pickle) is "
             "refused and nothing is kept"}}},
          {"feature_names",
           {{"type", "array"},
            {"items", {{"type", "string"}, {"minLength", 1}}},
            {"minItems", 1},
            {"description",
             "the columns the model reads, in its own order: name an ONNX model's columns "
             "(ONNX names none, so they are f0, f1, … otherwise), or rename another file's"}}},
          {"kind",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "`classifier` or `regressor` (default: from the file's objective)"}}},
          {"metrics",
           {{"type", "object"},
            {"additionalProperties", {{"type", "number"}}},
            {"description",
             "figures measured where it was trained, by name (held_out_auc, held_out_rmse, "
             "…), so it compares with trained models. Kept as given"}}},
          {"source",
           {{"type", "string"},
            {"maxLength", 2048},
            {"description",
             "where the file came from (a drive path, a model repository and revision); "
             "kept with the model"}}},
          {"version_of",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a brought standalone model's id: add this file as its new version"}}}}}};
    return schema;
}

const nlohmann::json& register_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "name", "kind", "project_id", "features", "standalone"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"kind", {{"type", "string"}}},
          {"project_id",
           {{"type", "string"},
            {"description", "the internal project that holds the model (never listed)"}}},
          {"features",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "the columns it reads, in its own order"}}},
          {"standalone", {{"type", "boolean"}}},
          {"format",
           {{"type", "string"},
            {"description", "the format the file was read as (zetta, onnx, xgboost_json, "
                            "xgboost_ubjson or lightgbm)"}}}}}};
    return schema;
}

ApiResult handle_register_model(const nlohmann::json& input, ApiContext& context) {
    // lake#681: a model file in any supported format becomes the engine's tree model.
    const bool text = input.contains("weights");
    if (text == input.contains("weights_base64")) {
        return ApiResult::failure("invalid_input",
                                  "/weights: give the file as `weights` (text) or "
                                  "`weights_base64`, one of them");
    }
    const std::string field = text ? "/weights" : "/weights_base64";
    const std::string bytes =
        text ? input.at("weights").get<std::string>()
             : zetta::detail::base64_decode_to_string(input.at("weights_base64").get<std::string>());
    zetta::ml::ModelImportOptions options;
    if (input.contains("format")) {
        options.format =
            zetta::ml::model_file_format_from_string(input.at("format").get<std::string>());
    }
    if (input.contains("feature_names")) {
        options.feature_names = input.at("feature_names").get<std::vector<std::string>>();
    }
    std::string reason;
    const std::optional<zetta::ml::ImportedModel> imported =
        zetta::ml::import_model(bytes, options, &reason);
    if (!imported) return ApiResult::failure("not_supported", field + ": " + reason);
    const zetta::ml::GbdtModel* weights = &imported->model;
    std::shared_ptr<zetta::pipeline::Project> project;
    std::string name = input.value("name", std::string{});
    if (input.contains("version_of")) {
        std::string error;
        const auto earlier =
            resolve_model(context, input.at("version_of").get<std::string>(), &error);
        if (!earlier) return ApiResult::failure("not_found", "/version_of: " + error);
        project = standalone_project_of(context, *earlier);
        if (!project) {
            return ApiResult::failure(
                "invalid_input",
                "/version_of: '" + earlier->name() +
                    "' is not a standalone model; a project's model gets new versions "
                    "from its pipeline");
        }
        if (!name.empty() && name != earlier->name()) {
            return ApiResult::failure("invalid_input",
                                      "/name: a version keeps its model's name '" +
                                          earlier->name() + "'");
        }
        name = earlier->name();
    } else {
        if (name.empty()) {
            return ApiResult::failure("invalid_input",
                                      "/name: name the model (or give version_of)");
        }
        if (context.node.find_project(standalone_project_name(name))) {
            return ApiResult::failure(
                "invalid_input", "/name: a standalone model named '" + name +
                                     "' exists; version_of adds a version of it");
        }
        project = context.node.create_project(standalone_project_name(name),
                                              actor_of(context),
                                              "Holds the standalone model '" + name + "'.",
                                              /*internal=*/true);
        if (!project) {
            return ApiResult::failure("engine_error", "could not create the model's project");
        }
    }
    const std::string kind = input.value("kind", imported->kind);
    const std::string format = zetta::ml::to_string(imported->format);
    zetta::ml::ModelValue value;
    value.name = name;
    value.kind = kind;
    value.project = project->id();
    value.project_name = project->name();
    nlohmann::json features = weights->feature_names;
    value.feature_schema = {{"features", features},
                            {"objective", weights->objective},
                            {"trees", weights->tree_count()},
                            {"brought", true},
                            {"format", format}};
    if (input.contains("source")) value.feature_schema["source"] = input.at("source");
    value.metrics = input.value("metrics", nlohmann::json::object());
    const auto model = context.node.create_model(std::move(value));
    if (!model || !model->set_weights(weights->to_json().dump())) {
        if (model) context.node.drop_model(model->id());
        if (!input.contains("version_of")) context.node.drop_project(project->name());
        return ApiResult::failure("engine_error", "could not keep the model");
    }
    return ApiResult::success({{"model_id", model->id()},
                               {"name", model->name()},
                               {"kind", model->kind()},
                               {"project_id", project->id()},
                               {"features", std::move(features)},
                               {"standalone", true},
                               {"format", format}});
}

// --- export_model (lake#681) ---------------------------------------------------------

const nlohmann::json& export_model_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the model version's id (or a name one model has)"}}},
          {"format",
           {{"type", "string"},
            {"enum", {"zetta", "onnx"}},
            {"description",
             "`zetta` (default): the weights exactly as the engine keeps them, for any "
             "model. `onnx`: a gradient-boosted tree model as an ONNX TreeEnsembleClassifier "
             "or TreeEnsembleRegressor that onnxruntime scores as the engine does"}}}}}};
    return schema;
}

const nlohmann::json& export_model_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"model_id", "name", "format", "file_name", "media_type", "encoding", "content", "bytes"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"format", {{"type", "string"}}},
          {"file_name", {{"type", "string"}, {"description", "a name to save the file as"}}},
          {"media_type", {{"type", "string"}}},
          {"encoding",
           {{"type", "string"},
            {"enum", {"utf-8", "base64"}},
            {"description", "how `content` holds the file: as text, or in base64"}}},
          {"content", {{"type", "string"}, {"description", "the file"}}},
          {"bytes", {{"type", "integer"}, {"description", "the file's size"}}},
          {"features",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "the columns it reads, in its own order (a tree model's)"}}}}}};
    return schema;
}

ApiResult handle_export_model(const nlohmann::json& input, ApiContext& context) {
    std::string error;
    const auto model = resolve_model(context, input.at("model_id").get<std::string>(), &error);
    if (!model) return ApiResult::failure("not_found", "/model_id: " + error);
    const std::optional<std::string> weights = model->read_weights();
    if (!weights) {
        return ApiResult::failure("not_found", "/model_id: '" + model->name() +
                                                   "' has no weights kept on this node yet");
    }
    const std::string format = input.value("format", std::string("zetta"));
    std::string file_stem;
    for (const char character : model->name()) {
        const bool plain = std::isalnum(static_cast<unsigned char>(character)) != 0 ||
                           character == '-' || character == '_';
        file_stem.push_back(plain ? character : '_');
    }
    if (file_stem.empty()) file_stem = "model";
    file_stem += "-" + model->id().substr(0, 8);
    const nlohmann::json body = nlohmann::json::parse(*weights, nullptr, false);
    std::string reason;
    const std::optional<zetta::ml::GbdtModel> gbdt =
        body.is_discarded() ? std::nullopt : zetta::ml::GbdtModel::from_json(body, &reason);
    nlohmann::json output{{"model_id", model->id()}, {"name", model->name()}, {"format", format}};
    if (gbdt) output["features"] = gbdt->feature_names;
    if (format == "onnx") {
        if (!gbdt) {
            return ApiResult::failure("not_supported",
                                      "/format: '" + model->name() +
                                          "' is not a gradient-boosted tree model, so it cannot "
                                          "be written as ONNX; format zetta downloads its weights");
        }
        const std::optional<std::string> onnx = zetta::ml::export_onnx(*gbdt, &reason);
        if (!onnx) return ApiResult::failure("not_supported", "/format: " + reason);
        output["file_name"] = file_stem + ".onnx";
        output["media_type"] = "application/onnx";
        output["encoding"] = "base64";
        output["content"] = zetta::detail::base64_encode_bytes(*onnx);
        output["bytes"] = onnx->size();
    } else {
        output["file_name"] = file_stem + ".json";
        output["media_type"] = "application/json";
        output["encoding"] = "utf-8";
        output["content"] = *weights;
        output["bytes"] = weights->size();
    }
    return ApiResult::success(std::move(output));
}

const nlohmann::json& move_model_to_project_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "any version of the standalone model, or its name"}}},
          {"project_name",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the project's name (default: the model's name)"}}}}}};
    return schema;
}

const nlohmann::json& move_model_to_project_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project_id", "project_name", "name", "versions"}},
        {"additionalProperties", false},
        {"properties",
         {{"project_id", {{"type", "string"}}},
          {"project_name", {{"type", "string"}}},
          {"name", {{"type", "string"}, {"description", "the model's name"}}},
          {"versions",
           {{"type", "integer"},
            {"description", "the model's versions, now the project's"}}}}}};
    return schema;
}

ApiResult handle_move_model_to_project(const nlohmann::json& input, ApiContext& context) {
    std::string error;
    const auto model = resolve_model(context, input.at("model_id").get<std::string>(), &error);
    if (!model) return ApiResult::failure("not_found", error);
    const auto project = standalone_project_of(context, *model);
    if (!project) {
        return ApiResult::failure("invalid_input",
                                  "/model_id: '" + model->name() +
                                      "' already belongs to a project");
    }
    const std::string name = input.value("project_name", model->name());
    if (context.node.find_project(name) && context.node.find_project(name) != project) {
        return ApiResult::failure("invalid_input", "/project_name: a project named '" + name +
                                                       "' exists; choose another name");
    }
    if (!context.node.reveal_project(project->id(), name)) {
        return ApiResult::failure("engine_error", "could not move the model into a project");
    }
    const auto versions = std::ranges::count_if(
        context.node.models(), [&](const auto& each) { return each->project() == project->id(); });
    return ApiResult::success({{"project_id", project->id()},
                               {"project_name", project->name()},
                               {"name", model->name()},
                               {"versions", versions}});
}

// --- score ---------------------------------------------------------------------

const nlohmann::json& score_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model"}},
        {"additionalProperties", false},
        {"properties",
         {{"model",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model's id, or its name"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "`database.table` to score every row of; omit when supplying `rows`"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"rows",
           {{"type", "array"},
            {"items", {{"type", "object"}}},
            {"description",
             "score these rows instead of a dataset - an account that is not in the "
             "data at all. Each row is keyed by the model's inputs "
             "(feature_schema.inputs): numeric features by name, and a text category "
             "by its original column and text value, one-hot encoded with the levels "
             "fitted at training time (an unseen level encodes as all zeros)"}}},
          {"entities",
           {{"type", "array"},
            {"items",
             {{"oneOf", {{{"type", "integer"}}, {{"type", "string"}}}}}},
            {"description",
             "entity key per supplied row (default: the row's index)"}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "cap the ranked list (0 = all)"}}}}}};
    return schema;
}

const nlohmann::json& score_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"ranked", "feature_columns", "rows_scored"}},
        {"properties",
         {{"ranked",
           {{"type", "array"},
            {"description", "entities by risk, highest first"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"entity",
                 {{"oneOf",
                   {{{"type", "integer"}}, {{"type", "string"}}}}}},
                {"risk", {{"type", "number"}}}}}}}}},
          {"feature_columns",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "what the model was trained on, in the order it expects"}}},
          {"drivers",
           {{"type", "array"},
            {"description", "population drivers, strongest first"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"feature", {{"type", "string"}}},
                {"importance", {{"type", "number"}}}}}}}}},
          {"rows_scored", {{"type", "integer"}}},
          {"partitions_read", {{"type", "integer"}}}}}};
    return schema;
}

ApiResult handle_score(const nlohmann::json& input, ApiContext& context) {
    const std::string wanted = input.at("model").get<std::string>();
    std::shared_ptr<zetta::ml::Model> model = context.node.find_model(wanted);
    if (!model) {
        for (const std::shared_ptr<zetta::ml::Model>& candidate : context.node.models()) {
            if (candidate && candidate->value().name == wanted) {
                model = candidate;
                break;
            }
        }
    }
    if (!model) {
        return ApiResult::failure("not_found", "no model '" + wanted + "' (or with that id)");
    }

    const auto rows = input.find("rows");
    const auto dataset_path = input.find("dataset_path");
    if (rows == input.end() && dataset_path == input.end()) {
        return ApiResult::failure(
            "invalid_input",
            "give either `dataset_path` (score a whole dataset) or `rows` (score rows "
            "that are not in it)");
    }

    const std::size_t top_k = input.value("top_k", std::size_t{0});
    zetta::ml::TableScoreResult scored;
    if (rows != input.end()) {
        std::shared_ptr<const zetta::data::EntityKeyMapping> entity_keys;
        if (const auto mapping = model->feature_schema().find("entity_key_mapping");
            mapping != model->feature_schema().end()) {
            std::string reason;
            entity_keys = zetta::data::entity_key_mapping_from_json(*mapping, &reason);
            if (!entity_keys) {
                return ApiResult::failure(
                    "engine_error",
                    "model has an invalid entity key mapping: " + reason);
            }
        }
        std::vector<std::int64_t> entities;
        if (const auto given = input.find("entities"); given != input.end()) {
            for (const nlohmann::json& entity : *given) {
                if (entity.is_number_integer()) {
                    entities.push_back(entity.get<std::int64_t>());
                    continue;
                }
                if (!entity.is_string() || !entity_keys) {
                    return ApiResult::failure(
                        "invalid_input",
                        "entities[]: text keys require a model trained with text entity ids");
                }
                const std::optional<std::int64_t> derived =
                    entity_keys->derived(entity.get_ref<const std::string&>());
                if (!derived) {
                    return ApiResult::failure(
                        "invalid_input",
                        "entities[]: unknown text entity '" +
                            entity.get<std::string>() + "'");
                }
                entities.push_back(*derived);
            }
        }
        scored = zetta::ml::score_rows(*model, rows->get<std::vector<nlohmann::json>>(),
                                       entities, top_k);
    } else {
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind != zetta::data::DatasetPathKind::Table) {
            return ApiResult::failure(
                "invalid_input",
                "score reads a `database.table` dataset; a dataset directory has no "
                "partitions to score near");
        }
        zetta::data::DatasetContract overrides;
        if (const auto given = input.find("dataset"); given != input.end()) {
            std::string reason;
            const std::optional<zetta::data::DatasetContract> parsed =
                zetta::data::dataset_contract_from_json(*given, &reason);
            if (!parsed) return ApiResult::failure("invalid_input", reason);
            overrides = *parsed;
        }
        const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
            context.node, path, overrides,
            zetta::data::default_dataset_sampler(context.node, context.cluster));
        if (!resolved.ok()) return ApiResult::failure("engine_error", resolved.error);

        compute::CpuDevice device;
        scored = zetta::ml::score_table(context.node, *model, resolved.table_id,
                                        resolved.contract, device, top_k);
    }
    if (!scored.ok()) return ApiResult::failure("engine_error", scored.error);

    nlohmann::json ranked = nlohmann::json::array();
    for (const zetta::ml::EntityScore& score : scored.ranked) {
        ranked.push_back(
            {{"entity", scored.entity_keys
                            ? scored.entity_keys->original(score.entity)
                            : nlohmann::json(score.entity)},
             {"risk", score.risk}});
    }
    nlohmann::json drivers = nlohmann::json::array();
    for (const zetta::ml::ModelDriver& driver : scored.drivers) {
        drivers.push_back({{"feature", driver.feature}, {"importance", driver.importance}});
    }
    nlohmann::json output;
    output["ranked"] = std::move(ranked);
    output["feature_columns"] = scored.feature_columns;
    output["drivers"] = std::move(drivers);
    output["rows_scored"] = scored.rows_scored;
    output["partitions_read"] = scored.partitions_read;
    return ApiResult::success(std::move(output));
}

// --- explain -----------------------------------------------------------------------
//
// "Why is THIS one at risk" (#948 step 6, #960 item 5). `score`'s drivers are the
// POPULATION ranking — the same list for every account — so they cannot answer the
// question a person asks when they open the at-risk list. Two accounts with the same
// score can be there for opposite reasons.
// -----------------------------------------------------------------------------------

const nlohmann::json& explain_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model"}},
        {"additionalProperties", false},
        {"properties",
         {{"model",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the registered model's id, or its name"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "`database.table` holding the entity; pair with `entity`"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"entity",
           {{"oneOf", {{{"type", "integer"}}, {{"type", "string"}}}},
            {"description",
             "the entity key to explain, read from `dataset_path`"}}},
          {"row",
           {{"type", "object"},
            {"description",
             "explain this row instead of a dataset entity — an account that is not "
             "in the data at all. Keyed by the model's inputs (feature_schema.inputs): "
             "numeric features by name, a text category by its original column and "
             "text value"}}}}}};
    return schema;
}

const nlohmann::json& explain_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"contributions", "base_value", "raw", "risk"}},
        {"properties",
         {{"entity",
           {{"oneOf", {{{"type", "integer"}}, {{"type", "string"}}}},
            {"description", "present when a dataset entity was explained"}}},
          {"base_value",
           {{"type", "number"},
            {"description",
             "the model's expected raw score — what an account with nothing notable "
             "about it would get. Contributions are movements away from this"}}},
          {"raw",
           {{"type", "number"},
            {"description",
             "this row's raw score. Equals base_value + the sum of contributions"}}},
          {"risk",
           {{"type", "number"},
            {"description",
             "the same prediction as a probability, comparable with a ranked list. "
             "NOT the sum of the contributions — probabilities are not additive"}}},
          {"residual",
           {{"type", "number"},
            {"description",
             "raw - (base_value + sum of contributions); ~0 when the explanation is "
             "exact. Exposed so a caller can check the arithmetic rather than trust it"}}},
          {"contributions",
           {{"type", "array"},
            {"description",
             "every feature, strongest influence first. `contribution` is signed in "
             "raw-score space: positive pushed this row's score up. A contribution "
             "says how much this row's value moved the model relative to a typical "
             "row — it does not say the column caused the outcome"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"feature", {{"type", "string"}}},
                {"value", {{"type", "number"}}},
                {"contribution", {{"type", "number"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_explain(const nlohmann::json& input, ApiContext& context) {
    const std::string wanted = input.at("model").get<std::string>();
    std::shared_ptr<zetta::ml::Model> model = context.node.find_model(wanted);
    if (!model) {
        for (const std::shared_ptr<zetta::ml::Model>& candidate : context.node.models()) {
            if (candidate && candidate->value().name == wanted) {
                model = candidate;
                break;
            }
        }
    }
    if (!model) {
        return ApiResult::failure("not_found", "no model '" + wanted + "' (or with that id)");
    }

    const auto row = input.find("row");
    const auto entity = input.find("entity");
    if (row == input.end() && entity == input.end()) {
        return ApiResult::failure(
            "invalid_input",
            "give either `entity` with `dataset_path` (explain one account in the "
            "data) or `row` (explain an account that is not in it)");
    }

    zetta::ml::RowExplanation explained;
    std::shared_ptr<const zetta::data::EntityKeyMapping> entity_keys;
    if (row != input.end()) {
        explained = zetta::ml::explain_row(*model, *row);
    } else {
        const auto dataset_path = input.find("dataset_path");
        if (dataset_path == input.end()) {
            return ApiResult::failure("invalid_input",
                                      "`entity` needs `dataset_path` to read it from");
        }
        const zetta::data::DatasetPath path =
            zetta::data::parse_dataset_path(dataset_path->get<std::string>());
        if (path.kind != zetta::data::DatasetPathKind::Table) {
            return ApiResult::failure(
                "invalid_input",
                "explain reads a `database.table` dataset; a dataset directory has no "
                "partitions to read near");
        }
        zetta::data::DatasetContract overrides;
        if (const auto given = input.find("dataset"); given != input.end()) {
            std::string reason;
            const std::optional<zetta::data::DatasetContract> parsed =
                zetta::data::dataset_contract_from_json(*given, &reason);
            if (!parsed) return ApiResult::failure("invalid_input", reason);
            overrides = *parsed;
        }
        const zetta::data::ResolvedDataset resolved = zetta::data::resolve_dataset(
            context.node, path, overrides,
            zetta::data::default_dataset_sampler(context.node, context.cluster));
        if (!resolved.ok()) return ApiResult::failure("engine_error", resolved.error);

        std::int64_t internal_entity = 0;
        entity_keys = resolved.contract.entity_keys;
        if (entity->is_number_integer()) {
            internal_entity = entity->get<std::int64_t>();
        } else if (entity->is_string() && entity_keys) {
            const std::optional<std::int64_t> derived =
                entity_keys->derived(entity->get_ref<const std::string&>());
            if (!derived) {
                return ApiResult::failure(
                    "invalid_input",
                    "unknown text entity '" + entity->get<std::string>() + "'");
            }
            internal_entity = *derived;
        } else {
            return ApiResult::failure(
                "invalid_input",
                "a text entity requires a dataset with text entity ids");
        }
        compute::CpuDevice device;
        explained = zetta::ml::explain_entity(context.node, *model, resolved.table_id,
                                              resolved.contract, device,
                                              internal_entity);
    }
    if (!explained.ok()) return ApiResult::failure("engine_error", explained.error);

    nlohmann::json contributions = nlohmann::json::array();
    for (const zetta::ml::FeatureContribution& contribution : explained.contributions) {
        contributions.push_back({{"feature", contribution.feature},
                                 {"value", contribution.value},
                                 {"contribution", contribution.contribution}});
    }
    nlohmann::json output;
    if (explained.entity) {
        output["entity"] = entity_keys
                               ? entity_keys->original(*explained.entity)
                               : nlohmann::json(*explained.entity);
    }
    output["base_value"] = explained.base_value;
    output["raw"] = explained.raw;
    output["risk"] = explained.risk;
    output["residual"] = explained.residual;
    output["contributions"] = std::move(contributions);
    return ApiResult::success(std::move(output));
}

// --- deployment_profile (#974) ------------------------------------------------------
//
// Charles's ask, on the 2026-08-05 call: "given a Helm chart dependency, this is how
// you should configure this deployment instance… at that point I don't need anything
// from you." The goal is not that we write a customer a chart — it is that a coding
// agent pointed at a node over MCP can produce one without asking us anything.
//
// Computed from the LIVE node, never from a document. That is the whole value: a
// deployment doc drifts from the binary, and the drift is only discovered in someone
// else's cluster. Same rule as the `system.*` read views.
//
// It never emits a credential. It says how to seed one.
// -----------------------------------------------------------------------------------

const nlohmann::json& deployment_profile_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"target",
           {{"type", "string"},
            {"enum", {"generic", "helm"}},
            {"description",
             "`helm` adds a values-shaped block a chart can consume directly, instead "
             "of prose an agent has to interpret. Default `generic`"}}}}}};
    return schema;
}

const nlohmann::json& deployment_profile_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"ports", "storage", "clustering", "security", "requires"}},
        {"properties",
         {{"ports", {{"type", "array"}, {"description", "every port, and what it carries"}}},
          {"storage", {{"type", "object"}}},
          {"clustering", {{"type", "object"}}},
          {"security", {{"type", "object"}}},
          {"identity", {{"type", "object"}}},
          {"requires",
           {{"type", "object"},
            {"description",
             "what this node needs from the cluster — and, as `none_of`, the "
             "dependencies people assume it needs and it does not"}}},
          {"helm", {{"type", "object"}, {"description", "present when target=helm"}}}}}};
    return schema;
}

ApiResult handle_deployment_profile(const nlohmann::json& input, ApiContext& context) {
    const std::string target = input.value("target", std::string("generic"));

    nlohmann::json ports = nlohmann::json::array();
    ports.push_back(
        {{"port", context.cluster != nullptr ? 7777 : 7777},
         {"name", "peer"},
         {"carries", "cluster traffic between nodes"},
         {"flag", "--listen"},
         {"note",
          "`--listen` moves THIS port, not the API. Setting it and then calling the "
          "port you chose returns nothing, which reads exactly like a dead node — the "
          "most common misconfiguration we see"}});
    ports.push_back({{"port", 4800},
                     {"name", "api"},
                     {"carries", "REST /v1, MCP at /mcp, and the web UI"},
                     {"flag", "--mcp-listen"},
                     {"note", "what a client Service should target"}});

    nlohmann::json profile;
    profile["ports"] = std::move(ports);
    profile["storage"] = {
        {"data_dir", "--data-dir"},
        {"durable", true},
        {"note",
         "the node's durable state. A StatefulSet wants one PersistentVolumeClaim per "
         "member; it is not shareable between members"}};
    profile["clustering"] = {
        {"model", "equal peers — no leader, no election"},
        {"join", "--peer HOST:PORT (repeatable) plus --join-token"},
        {"members_now", context.cluster != nullptr
                            ? static_cast<long long>(context.cluster->peer_count() + 1)
                            : 1},
        {"dial_out_only",
         "--dial-out-only for a member with no routable address (NAT, private VNet). "
         "It must never advertise a private pod IP: when two clusters share a pod CIDR "
         "the dial reaches the wrong node and membership undercounts"}};
    profile["security"] = {
        {"peer_transport", "mTLS, always"},
        {"plaintext_mode", false},
        {"note",
         "there is no plaintext mode to disable or justify to a reviewer; one shared "
         "join token per cluster"},
        {"external_egress_required", false},
        {"egress_note",
         "the web UI serves no external assets — no CDN, no external fonts — so an "
         "air-gapped node works"}};
    profile["identity"] = {
        {"seed", "--admin-token, or ZETTA_ADMIN_TOKEN"},
        {"why",
         "a fresh node otherwise generates its owner token and prints it once, which an "
         "automated deploy cannot capture. Seeding makes the deploy deterministic"},
        {"never_emitted",
         "this capability reports how to seed a credential and never a credential"}};
    profile["requires"] = {
        {"runtime", nlohmann::json::array({"one statically linked binary", "a data directory"})},
        {"none_of",
         nlohmann::json::array({"external Postgres", "Redis", "Python at runtime",
                                "Node.js at runtime", "a package manager at runtime"})},
        {"note",
         "the `none_of` list is the point: enterprise build templates commonly carry all "
         "of these, and zetta uses none of them"}};

    if (target == "helm") {
        // Values-shaped, so a chart consumes it rather than an agent interpreting prose.
        profile["helm"] = {
            {"values",
             {{"image", {{"repository", "<your registry>/zetta"}, {"tag", "<version>"}}},
              {"service",
               {{"api", {{"port", 4800}}},
                {"peer", {{"port", 7777}, {"clusterIP", "None"}}}}},
              {"persistence", {{"enabled", true}, {"mountPath", "/var/lib/zetta"}}},
              {"env",
               {{"ZETTA_ADMIN_TOKEN", "<from a Secret — never inline>"},
                {"ZETTA_CLUSTER_TOKEN", "<from a Secret>"},
                {"ZETTA_SEED_PEER", "<headless-service>:7777"}}},
              {"args", nlohmann::json::array({"--headless", "--data-dir",
                                              "/var/lib/zetta"})}}},
            {"shape", "StatefulSet + a headless Service on the peer port + a client Service on the API port"},
            {"probes",
             {{"readiness", "GET /v1/server_info on the API port"},
              {"note", "the API answering is the readiness signal; the peer port is not"}}}};
    }
    return ApiResult::success(std::move(profile));
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
          {"partition_only",
           {{"type", "boolean"},
            {"description",
             "do NOT capture the rows into the replicated document (#833) — the "
             "data lives only in its partition bodies, which their owners hold. "
             "This is the right setting for connector-sized data: capturing rows "
             "replicates every one of them to every peer, and reconciling them "
             "back rewrites the whole local store per row, which saturates the "
             "node's owner loop. Reads go through the scatter-gather select. "
             "Default false, for back-compatibility with small replicated "
             "tables"}}},
          {"partition_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 4096},
            {"description",
            "partitions to write; omit to let the partition scheduler "
            "size them (a target body size, with a floor of partitions per "
            "node) instead of fixing a count"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "owners per partition (default 3, clamped to the node count)"}}}}}};
    return schema;
}

const nlohmann::json& add_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"database", "table", "format", "rows", "partitions"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"renamed_from",
           {{"type", "string"},
            {"description",
             "upload_dataset with if_exists rename: the name asked for, when it was "
             "taken and `table` is the next free one"}}},
          {"format", {{"type", "string"}}},
          {"rows", {{"type", "integer"}}},
          {"partitions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"partition_id", "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"owner_nodes",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}}}}}}}}}}}}};
    return schema;
}

ApiResult handle_add_dataset(const nlohmann::json& input, ApiContext& context) {
    zetta::data::AddDatasetOptions options;
    options.database = input.value("database", options.database);
    options.table_name = input.value("table_name", options.table_name);
    options.partitioned = input.value("partitioned", options.partitioned);
    options.partition_only = input.value("partition_only", options.partition_only);
    // Absent means AUTO — the scheduler targets a partition size instead of a
    // constant (#833). A value present is a manual override, honoured exactly.
    if (const auto it = input.find("partition_count"); it != input.end()) {
        options.partition_count = it->get<std::uint32_t>();
    }
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

// --- snapshot (#1103) -------------------------------------------------------

const nlohmann::json& snapshot_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"database", "table", "source_rows", "materialized_rows",
          "existing_rows", "future_rows_excluded", "as_of"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"source_rows", {{"type", "integer"}, {"minimum", 0}}},
          {"materialized_rows", {{"type", "integer"}, {"minimum", 0}}},
          {"existing_rows", {{"type", "integer"}, {"minimum", 0}}},
          {"future_rows_excluded", {{"type", "integer"}, {"minimum", 0}}},
          {"as_of",
           {{"oneOf",
             nlohmann::json::array({nlohmann::json{{"type", "number"}},
                                    nlohmann::json{{"type", "string"}}})}}}}}};
    return schema;
}

ApiResult handle_snapshot(const nlohmann::json& input, ApiContext& context) {
    std::string error;
    const std::optional<zetta::solutions::SnapshotSpec> spec =
        zetta::solutions::SnapshotSpec::from_json(input, &error);
    if (!spec) {
        return ApiResult::failure("invalid_input", error);
    }
    const zetta::solutions::SnapshotResult snapshot =
        zetta::solutions::materialize_snapshot(context.node, *spec);
    if (!snapshot.ok()) {
        return ApiResult::failure("engine_error", snapshot.error);
    }
    return ApiResult::success(
        {{"database", spec->database},
         {"table", snapshot.table->name()},
         {"source_rows", snapshot.source_rows},
         {"materialized_rows", snapshot.materialized_rows},
         {"existing_rows", snapshot.existing_rows},
         {"future_rows_excluded", snapshot.future_rows_excluded},
         {"as_of", spec->as_of}});
}

// --- upload_dataset (#1008) ---------------------------------------------------------
//
// add_dataset reads a file on the NODE's filesystem, which a remote caller cannot
// write. This takes the file's text in the request and lands it through the same
// decode + table-creation + partition-write + audit path (add_dataset_content).

const nlohmann::json& upload_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"table_name", "format", "content"}},
        {"additionalProperties", false},
        {"properties",
         {{"table_name",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the new table's name: a letter or underscore, then letters, digits and "
             "underscores, at most 63 characters, not a SQL keyword. A name already "
             "taken in the database is refused (conflict) unless if_exists is rename"}}},
          {"if_exists",
           {{"type", "string"},
            {"enum", {"fail", "rename"}},
            {"description",
             "what to do when table_name is taken: fail (the default) refuses with "
             "conflict; rename saves under the next free name (_2, _3, ...) and the "
             "output's renamed_from names the one asked for"}}},
          {"format",
           {{"type", "string"},
            {"enum", {"csv", "jsonl"}},
            {"description",
             "how `content` is laid out: csv (a header row, then one row per line with "
             "as many fields as the header, cells type-inferred) or jsonl (one JSON "
             "object per line)"}}},
          {"content",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the file's text, verbatim. At most 16 MiB (16777216 bytes of UTF-8); "
             "upload a larger file to a bucket (PATCH /v1/objects/<bucket>/<key>, a "
             "chunk at a time) and load it with load_dataset_object"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "target database, created if absent (default datasets); named by the "
             "same rule as table_name"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "owners per partition (default 3, clamped to the node count)"}}}}}};
    return schema;
}

ApiResult handle_upload_dataset(const nlohmann::json& input, ApiContext& context) {
    const std::string& content = input.at("content").get_ref<const std::string&>();
    // Checked here as well as in the engine, so the refusal is an invalid_input
    // (the caller's request is wrong) rather than an engine_error.
    if (content.size() > zetta::data::kMaxDatasetContentBytes) {
        return ApiResult::failure(
            "invalid_input",
            "content is " + std::to_string(content.size()) +
                " bytes; upload_dataset accepts at most " +
                std::to_string(zetta::data::kMaxDatasetContentBytes) +
                " bytes (16 MiB). Upload a larger file to a bucket (PATCH "
                "/v1/objects/<bucket>/<key>, a chunk at a time) and load it with "
                "load_dataset_object",
            "/content");
    }
    const std::string format_name = input.at("format").get<std::string>();
    const zetta::data::dataset_file::Format format =
        format_name == "csv" ? zetta::data::dataset_file::Format::Csv
                             : zetta::data::dataset_file::Format::Jsonl;

    zetta::data::AddDatasetOptions options;
    options.database = input.value("database", options.database);
    options.table_name = input.at("table_name").get<std::string>();

    // The rules lake applies in front of the workspace (#1221, lake PR #459), in
    // the same words, so a direct caller gets the same answer: names SQL can
    // query unquoted, a CSV whose rows match its header, and no silent rename.
    for (const auto& [field, value] :
         {std::pair<std::string, const std::string*>{"table_name", &options.table_name},
          std::pair<std::string, const std::string*>{"database", &options.database}}) {
        if (const std::string problem = zetta::data::dataset_name_problem(*value);
            !problem.empty()) {
            return ApiResult::failure("invalid_input", field + ": " + problem, "/" + field);
        }
    }
    if (format == zetta::data::dataset_file::Format::Csv) {
        if (const std::string problem = zetta::data::csv_problem(content); !problem.empty()) {
            return ApiResult::failure("invalid_input",
                                      "the CSV file is malformed: " + problem, "/content");
        }
    }
    const bool rename = input.value("if_exists", std::string("fail")) == "rename";
    if (!rename) {
        const auto database = context.node.find_database(options.database);
        if (database && database->find_table(options.table_name)) {
            return ApiResult::failure(
                "conflict",
                zetta::data::dataset_name_taken(options.table_name, options.database),
                "/table_name");
        }
    }
    options.replication_factor =
        input.value("replication_factor", options.replication_factor);
    options.approx_bytes = content.size();
    options.members = context.node.cluster_members();  // live placement roster

    const zetta::data::AddDatasetResult added =
        zetta::data::add_dataset_content(context.node, content, format, options);
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
    // Saved under the next free name because the caller asked (if_exists:
    // rename): say which name it asked for, as lake does.
    if (added.table && added.table->name() != options.table_name) {
        output["renamed_from"] = options.table_name;
    }
    return ApiResult::success(std::move(output));
}

// --- load_dataset_object (#1302) ----------------------------------------------------
//
// upload_dataset carries the file in the request, capped at 16 MiB. A larger file
// is first uploaded to a bucket a chunk at a time (PATCH /v1/objects/<bucket>/<key>,
// the object capability's resumable append, #1005); this loads that object in the
// background (AsyncObjectDatasetLoad) and answers with the run to poll.

const nlohmann::json& load_dataset_object_schema() {
    static const nlohmann::json schema = [] {
        // The fields it shares with upload_dataset, described the same way.
        const nlohmann::json& upload = upload_dataset_schema().at("properties");
        return nlohmann::json{
            {"type", "object"},
            {"required", {"bucket", "key", "format", "table_name"}},
            {"additionalProperties", false},
            {"properties",
             {{"bucket",
               {{"type", "string"},
                {"pattern", "^[a-z0-9][a-z0-9_.-]{0,95}$"},
                {"description",
                 "the bucket holding the object (create_bucket); the caller needs "
                 "objects:read on it, and objects:write with remove_object"}}},
              {"key",
               {{"type", "string"},
                {"minLength", 1},
                {"maxLength", 1024},
                {"description",
                 "the object's key: a plain (unsealed) csv or jsonl file, typically "
                 "uploaded a chunk at a time with PATCH /v1/objects/<bucket>/<key>?offset=<n>. "
                 "At most 1 GiB (1073741824 bytes), no line longer than 16 MiB"}}},
              {"format",
               {{"type", "string"},
                {"enum", {"csv", "jsonl"}},
                {"description",
                 "how the object is laid out: csv (a header row, then one row per line with "
                 "as many fields as the header, cells type-inferred) or jsonl (one JSON "
                 "object per line)"}}},
              {"table_name", upload.at("table_name")},
              {"if_exists",
               {{"type", "string"},
                {"enum", {"fail", "rename"}},
                {"description",
                 "what to do when table_name is taken: fail (the default) refuses with "
                 "conflict; rename saves under the next free name (_2, _3, ...) and the "
                 "run's final log entry names the one asked for"}}},
              {"database", upload.at("database")},
              {"replication_factor", upload.at("replication_factor")},
              {"remove_object",
               {{"type", "boolean"},
                {"description",
                 "remove the object once the load has finished, whether it succeeded or "
                 "failed (default false). Needs objects:write on the bucket, as "
                 "delete_object does, and is audited as a delete_object call"}}}}}};
    }();
    return schema;
}

const nlohmann::json& load_dataset_object_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "status", "poll"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"description", "the load's run: poll get_run / system.runs"}}},
          {"status", {{"type", "string"}}},
          {"poll", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_load_dataset_object(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectDatasetSource source;
    source.bucket = input.at("bucket").get<std::string>();
    source.key = input.at("key").get<std::string>();
    if (!zetta::data::valid_bucket_name(source.bucket)) {
        return ApiResult::failure(
            "invalid_input",
            "bucket: '" + source.bucket +
                "' is not a bucket name (a lowercase letter or digit, then up to 95 of "
                "a-z, 0-9, _, . and -)",
            "/bucket");
    }
    if (!zetta::data::valid_object_key(source.key)) {
        return ApiResult::failure(
            "invalid_input", "key: an object key is 1 to 1024 bytes with no control characters",
            "/key");
    }
    source.format = input.at("format").get<std::string>() == "csv"
                        ? zetta::data::dataset_file::Format::Csv
                        : zetta::data::dataset_file::Format::Jsonl;
    source.rename_if_exists = input.value("if_exists", std::string("fail")) == "rename";
    source.remove_object = input.value("remove_object", false);

    zetta::data::AddDatasetOptions options;
    options.database = input.value("database", options.database);
    options.table_name = input.at("table_name").get<std::string>();
    for (const auto& [field, value] :
         {std::pair<std::string, const std::string*>{"table_name", &options.table_name},
          std::pair<std::string, const std::string*>{"database", &options.database}}) {
        if (const std::string problem = zetta::data::dataset_name_problem(*value);
            !problem.empty()) {
            return ApiResult::failure("invalid_input", field + ": " + problem, "/" + field);
        }
    }
    options.replication_factor =
        input.value("replication_factor", options.replication_factor);

    zetta::data::ObjectStore* objects = context.node.object_store();
    if (!objects) {
        return ApiResult::failure("engine_error",
                                  "this node has no replicated catalogue to keep buckets in");
    }
    // The read right GET /v1/objects/<bucket>/<key> asks for, so a caller cannot
    // read a bucket through a load that it could not read directly.
    if (auto denied = authorize_object_route(context.node, context.acting_key,
                                             context.acting_is_user, "load_dataset_object",
                                             kObjectsRead, source.bucket)) {
        return *denied;
    }
    // Removing the object afterwards is a delete: it needs the write right
    // DELETE /v1/objects/<bucket>/<key> asks for.
    if (source.remove_object) {
        if (auto denied = authorize_object_route(context.node, context.acting_key,
                                                 context.acting_is_user, "load_dataset_object",
                                                 kObjectsWrite, source.bucket)) {
            return *denied;
        }
    }
    if (!objects->find_bucket(source.bucket)) {
        return ApiResult::failure("not_found", "no bucket '" + source.bucket + "'", "/bucket");
    }
    if (!source.rename_if_exists) {
        const auto database = context.node.find_database(options.database);
        if (database && database->find_table(options.table_name)) {
            return ApiResult::failure(
                "conflict", zetta::data::dataset_name_taken(options.table_name, options.database),
                "/table_name");
        }
    }
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "load_dataset_object is a job capability and this embedding wired no "
            "background action owner");
    }
    options.members = context.node.cluster_members();  // live placement roster
    source.acting_key = context.acting_key;  // the removal's audit row names the caller
    source.trace = context.trace;

    auto handle = zetta::data::AsyncObjectDatasetLoad::start(context.node, *objects,
                                                             std::move(source), std::move(options));
    if (!handle) {
        return ApiResult::failure("engine_error", "the run record could not be created");
    }
    context.background->adopt(handle, [worker = handle.get()] { return worker->done(); });
    nlohmann::json output;
    output["run_id"] = handle->run_id();
    output["status"] = "running";
    output["poll"] = "get_run until status is succeeded or failed; the dataset is the run's output";
    return ApiResult::success(std::move(output));
}

// --- saved connections + create_dataset_from_query ----------------------------------

// The connector config as an embeddable sub-schema: the code-owned
// connector_config_schema() minus its document-level keys, so it nests cleanly
// inside this capability's input schema (and therefore inside the OpenAPI doc).
const nlohmann::json& connector_property_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json nested = zetta::connect::connector_config_schema();
        nested.erase("$schema");
        nested.erase("$id");
        return nested;
    }();
    return schema;
}

// The shape every connection read surface returns — note the absence of a
// `token` anywhere in it. `SavedConnection::to_redacted_json()` produces it from
// `ConnectorConfig::to_json()`, which never emits the credential, so the
// redaction is structural rather than a field someone must remember to strip.
const nlohmann::json& saved_connection_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "connector"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"connector",
           {{"type", "object"},
            {"description",
             "the saved connector settings WITHOUT the credential — the token "
             "is never returned by any surface"}}},
          {"credential",
           {{"type", "string"},
            {"enum", {"set", "none", "unreadable", "plaintext"}},
            {"description",
             "whether a credential is stored — set (sealed, and this node opens "
             "it), none, unreadable (sealed under a key this node does not "
             "hold), or plaintext (a legacy entry not yet re-sealed). Never the "
             "credential itself."}}},
          {"created",
           {{"type", "string"},
            {"description", "when the connection was last saved (ISO-8601 UTC)"}}}}}};
    return schema;
}

const nlohmann::json& create_connection_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "connector"}},
        {"additionalProperties", false},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 64},
            {"description",
             "the name to save this connection under (letters, digits, '_' and "
             "'-'); saving an existing name replaces it"}}},
          {"connector", connector_property_schema()}}}};
    return schema;
}

const nlohmann::json& create_connection_output_schema() {
    return saved_connection_schema();
}

const nlohmann::json& list_connections_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_connections_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"connections"}},
        {"additionalProperties", false},
        {"properties",
         {{"connections",
           {{"type", "array"}, {"items", saved_connection_schema()}}}}}};
    return schema;
}

const nlohmann::json& delete_connection_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& delete_connection_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "removed"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}}, {"removed", {{"type", "boolean"}}}}}};
    return schema;
}

ApiResult handle_create_connection(const nlohmann::json& input,
                                   ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    const std::optional<zetta::connect::ConnectorConfig> config =
        zetta::connect::ConnectorConfig::from_json(input.at("connector"));
    if (!config) {
        return ApiResult::failure(
            "invalid_input",
            "connector: expected {type, account, token} with a known type "
            "(and an optional auth of pat|oauth, database, schema, warehouse, "
            "role)");
    }
    const std::string rejection =
        zetta::connect::save_connection(context.node, name, *config);
    if (!rejection.empty()) {
        return ApiResult::failure("invalid_input", rejection);
    }
    const std::optional<zetta::connect::SavedConnection> saved =
        zetta::connect::find_connection(context.node, name);
    if (!saved) {
        return ApiResult::failure("engine_error",
                                  "connection '" + name + "' did not persist");
    }
    // Redacted on the way out — a create call never echoes the credential back.
    return ApiResult::success(saved->to_redacted_json());
}

ApiResult handle_list_connections(const nlohmann::json& input,
                                  ApiContext& context) {
    (void)input;
    nlohmann::json connections = nlohmann::json::array();
    for (const zetta::connect::SavedConnection& saved :
         zetta::connect::list_connections(context.node)) {
        connections.push_back(saved.to_redacted_json());
    }
    nlohmann::json output;
    output["connections"] = std::move(connections);
    return ApiResult::success(std::move(output));
}

ApiResult handle_delete_connection(const nlohmann::json& input,
                                   ApiContext& context) {
    const std::string name = input.at("name").get<std::string>();
    nlohmann::json output;
    output["name"] = name;
    output["removed"] = zetta::connect::remove_connection(context.node, name);
    return ApiResult::success(std::move(output));
}

const nlohmann::json& create_dataset_from_query_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        // Exactly one of `connection` / `connector` is required; the schema
        // cannot say "exactly one" without oneOf (which the query-string REST
        // coercion does not model), so the handler enforces it and says which.
        {"required", {"query"}},
        {"additionalProperties", false},
        {"properties",
         {{"connection",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "the name of a saved connection (create_connection / "
             "list_connections) — use this instead of an inline connector so "
             "the credential never travels with the request"}}},
          {"connector", connector_property_schema()},
          {"query",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a single read-only SELECT (or WITH) to run against the source"}}},
          {"database",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "target database, created if absent (default datasets)"}}},
          {"table_name",
           {{"type", "string"},
            {"description", "name for the new table (default 'dataset')"}}},
          {"max_rows",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 10000000},
            {"description", "cap on rows pulled from the source (default "
                            "100000)"}}},
          {"partitioned",
           {{"type", "boolean"},
            {"description",
             "shard + place the rows across the cluster (default true)"}}},
          {"partition_count",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 4096},
            {"description",
            "partitions to write; omit to let the partition scheduler "
            "size them (a target body size, with a floor of partitions per "
            "node) instead of fixing a count"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "owners per partition (default 1 — a connector result set is a "
             "cache, re-pullable from its source; promote_dataset raises it)"}}}}}};
    return schema;
}

const nlohmann::json& create_dataset_from_query_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"database", "table", "rows", "columns", "partitions"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"rows", {{"type", "integer"}}},
          {"columns", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"partitions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"partition_id", "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"owner_nodes",
                 {{"type", "array"},
                  {"items", {{"type", "string"}}}}}}}}}}},
          {"coordinated",
           {{"type", "boolean"},
            {"description",
             "true when the fetch was co-launched across the cluster (every "
             "participant pulled its own tile straight from the source)"}}},
          {"participants",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "the nodes that fetched a tile"}}}}}};
    return schema;
}

// Does this connector stay on the machine the node runs on? A loopback host or
// a unix-socket path never crosses the network, so it is not "a cloud provider"
// by any reading (#994). Kept deliberately narrow — loopback and sockets only.
// A private-range address may still be a different machine across a VPN, and
// that is a network reach worth naming.
bool is_local_source(const zetta::connect::ConnectorConfig& config) {
    // warehouses are remote by construction
    if (config.type != "postgres" && config.type != "mysql") return false;
    const std::string& host = config.host;
    if (host.empty()) return true;          // libpq default: the local socket
    if (host.front() == '/') return true;   // an explicit unix-socket directory
    return host == "localhost" || host == "127.0.0.1" || host == "::1" ||
           host == "[::1]";
}

// Resolve the connector this call should use: either a saved connection by name
// (the credential stays on the cluster) or an inline connector object. Exactly
// one; `reason` explains the rejection.
std::optional<zetta::connect::ConnectorConfig> resolve_query_connector(
    const nlohmann::json& input, ApiContext& context, std::string& reason) {
    const bool has_connection = input.contains("connection");
    const bool has_connector = input.contains("connector");
    if (has_connection == has_connector) {
        reason = has_connection
                     ? "give either a saved 'connection' name or an inline "
                       "'connector' object, not both"
                     : "give either a saved 'connection' name or an inline "
                       "'connector' object";
        return std::nullopt;
    }
    if (has_connection) {
        const std::string name = input.at("connection").get<std::string>();
        const std::optional<zetta::connect::SavedConnection> saved =
            zetta::connect::find_connection(context.node, name);
        if (!saved) {
            reason = "connection: no saved connection named '" + name +
                     "' (list them with list_connections)";
            return std::nullopt;
        }
        if (saved->credential == zetta::connect::CredentialState::Unreadable) {
            reason = "connection '" + name +
                     "': its credential does not open on this node (" +
                     saved->credential_error + ") — save it again";
            return std::nullopt;
        }
        return saved->config;
    }
    std::optional<zetta::connect::ConnectorConfig> config =
        zetta::connect::ConnectorConfig::from_json(input.at("connector"));
    if (!config) {
        reason =
            "connector: expected {type, account, token} with a known type "
            "(and an optional auth of pat|oauth, database, schema, warehouse, "
            "role)";
    }
    return config;
}

// --- list_source_tables ------------------------------------------------------
//
// Browse a connected source's schema WITHOUT ingesting from it (#994). Until
// now the only way to answer "what tables are in this database?" was to run
// `create_dataset_from_query` against `information_schema.tables` — which
// materializes a partitioned, replicated dataset on the cluster in order to
// read a catalog. That is what a customer actually did, and it leaves a junk
// dataset behind every time someone looks.
//
// This is a Read: it opens one connection, runs one catalog query, returns
// rows, and lands nothing.

const nlohmann::json& list_source_tables_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"connection",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a saved connection by name (give this OR an inline connector)"}}},
          {"connector", connector_property_schema()},
          {"schema",
           {{"type", "string"},
            {"description",
             "restrict to one schema; omitted = every non-system schema"}}},
          {"match",
           {{"type", "string"},
            {"description",
             "case-insensitive substring the table name must contain, e.g. "
             "'score' to find scoring tables"}}},
          {"include_columns",
           {{"type", "boolean"},
            {"default", true},
            {"description",
             "return each table's columns and their types (default true)"}}},
          {"max_tables",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 2000},
            {"default", 200},
            {"description", "cap on tables returned (default 200)"}}}}}};
    return schema;
}

const nlohmann::json& list_source_tables_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"tables"}},
        {"additionalProperties", false},
        {"properties",
         {{"tables",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"schema", {{"type", "string"}}},
                {"table", {{"type", "string"}}},
                {"kind", {{"type", "string"},
                          {"description", "BASE TABLE | VIEW | …"}}},
                {"columns",
                 {{"type", "array"},
                  {"items",
                   {{"type", "object"},
                    {"properties",
                     {{"column", {{"type", "string"}}},
                      {"type", {{"type", "string"}}},
                      {"nullable", {{"type", "boolean"}}}}}}}}}}}}}}},
          {"table_count", {{"type", "integer"}}},
          {"truncated",
           {{"type", "boolean"},
            {"description", "true when max_tables cut the list short"}}}}}};
    return schema;
}

// A single-quoted SQL literal. The inputs here are caller-supplied filters that
// go into a catalog query, so they are escaped rather than concatenated raw —
// the connector's read-only guard stops a second statement, but it is not an
// argument for building the first one carelessly. MySQL also treats a
// backslash inside a literal as an escape (unless NO_BACKSLASH_ESCAPES), so for
// it a backslash is doubled too — otherwise a trailing `\` would escape the
// closing quote.
std::string sql_literal(const std::string& value, bool backslash_escapes = false) {
    std::string out = "'";
    for (const char character : value) {
        if (character == '\'') out += '\'';
        if (backslash_escapes && character == '\\') out += '\\';
        out += character;
    }
    out += "'";
    return out;
}

ApiResult handle_list_source_tables(const nlohmann::json& input,
                                    ApiContext& context) {
    std::string reason;
    const std::optional<zetta::connect::ConnectorConfig> config =
        resolve_query_connector(input, context, reason);
    if (!config) return ApiResult::failure("invalid_input", reason);

    const auto max_tables =
        input.value("max_tables", static_cast<std::size_t>(200));
    const bool include_columns = input.value("include_columns", true);
    const std::string only_schema = input.value("schema", std::string{});
    const std::string match = input.value("match", std::string{});

    // information_schema is ISO SQL and every shipped connector exposes it, so
    // one statement serves Postgres, Snowflake and MySQL alike. The row cap is
    // applied here rather than with LIMIT, which they spell the same way but
    // the read-only guard need not care about.
    //
    // MySQL differs in three places: 8.0 reports information_schema columns in
    // upper case unless aliased (so every column is aliased to the lower-case
    // name the reader below expects), its own catalog schemas are `mysql`,
    // `performance_schema` and `sys`, and its literals take backslash escapes.
    const bool is_mysql = config->type == "mysql";
    const std::string excluded_schemas =
        is_mysql ? "('information_schema', 'mysql', 'performance_schema', 'sys')"
                 : "('pg_catalog', 'information_schema')";
    const auto alias = [is_mysql](const char* column) {
        return is_mysql ? std::string(column) + " AS " + (std::string(column).substr(2))
                        : std::string(column);
    };
    std::string statement =
        "SELECT " + alias("t.table_schema") + ", " + alias("t.table_name") + ", " +
        alias("t.table_type") +
        " FROM information_schema.tables t"
        " WHERE t.table_schema NOT IN " + excluded_schemas;
    if (!only_schema.empty()) {
        statement += " AND t.table_schema = " + sql_literal(only_schema, is_mysql);
    }
    if (!match.empty()) {
        statement += " AND LOWER(t.table_name) LIKE " +
                     sql_literal("%" + [&] {
                       std::string lowered = match;
                       std::transform(lowered.begin(), lowered.end(),
                                      lowered.begin(), [](unsigned char c) {
                                          return static_cast<char>(std::tolower(c));
                                      });
                       return lowered;
                     }() + "%", is_mysql);
    }
    statement += " ORDER BY t.table_schema, t.table_name";

    const zetta::connect::ConnectorResult listed = zetta::connect::run_connector_query(
        *config, statement, max_tables + 1);
    if (!listed.ok()) return ApiResult::failure("engine_error", listed.error);

    const bool truncated = listed.data.rows.size() > max_tables;
    nlohmann::json tables = nlohmann::json::array();
    std::vector<std::pair<std::string, std::string>> wanted;
    for (const nlohmann::json& row : listed.data.rows) {
        if (tables.size() >= max_tables) break;
        const std::string schema_name = row.value("table_schema", std::string{});
        const std::string table_name = row.value("table_name", std::string{});
        wanted.emplace_back(schema_name, table_name);
        tables.push_back({{"schema", schema_name},
                          {"table", table_name},
                          {"kind", row.value("table_type", std::string{})}});
    }

    if (include_columns && !wanted.empty()) {
        // One catalog query for every column of every table listed, rather than
        // one round trip per table — a schema with 200 tables would otherwise be
        // 200 connections.
        std::string columns_statement =
            "SELECT " + alias("c.table_schema") + ", " + alias("c.table_name") + ", " +
            alias("c.column_name") + ", " + alias("c.data_type") + "," + " " +
            alias("c.is_nullable") + ", " + alias("c.ordinal_position") +
            " FROM information_schema.columns c"
            " WHERE c.table_schema NOT IN " + excluded_schemas;
        if (!only_schema.empty()) {
            columns_statement +=
                " AND c.table_schema = " + sql_literal(only_schema, is_mysql);
        }
        columns_statement += " ORDER BY c.table_schema, c.table_name, c.ordinal_position";

        const zetta::connect::ConnectorResult described =
            zetta::connect::run_connector_query(*config, columns_statement,
                                                /*max_rows=*/100000);
        if (described.ok()) {
            for (const nlohmann::json& row : described.data.rows) {
                const std::string schema_name =
                    row.value("table_schema", std::string{});
                const std::string table_name =
                    row.value("table_name", std::string{});
                for (nlohmann::json& entry : tables) {
                    if (entry.value("schema", std::string{}) != schema_name ||
                        entry.value("table", std::string{}) != table_name) {
                        continue;
                    }
                    entry["columns"].push_back(
                        {{"column", row.value("column_name", std::string{})},
                         {"type", row.value("data_type", std::string{})},
                         {"nullable",
                          row.value("is_nullable", std::string{"YES"}) == "YES"}});
                    break;
                }
            }
        }
        // A source that refuses the column catalog still returns its table list;
        // the tables are the answer to "what is in here", columns are detail.
    }

    nlohmann::json output;
    output["table_count"] = tables.size();
    output["truncated"] = truncated;
    output["tables"] = std::move(tables);
    return ApiResult::success(std::move(output));
}

ApiResult handle_create_dataset_from_query(const nlohmann::json& input,
                                           ApiContext& context) {
    std::string reason;
    const std::optional<zetta::connect::ConnectorConfig> config =
        resolve_query_connector(input, context, reason);
    if (!config) {
        return ApiResult::failure("invalid_input", reason);
    }

    zetta::connect::DistributedIngestOptions options;
    // The SAVED CONNECTION name, when that is how the caller identified the
    // source. It is what makes the cross-node co-launch possible: every node
    // resolves the credential from its own replicated copy, so nothing secret
    // rides the launch. An inline `connector` block leaves it empty, which
    // degrades the ingest to this node alone.
    options.connection = input.value("connection", std::string{});
    options.query = input.at("query").get<std::string>();
    options.max_rows = input.value("max_rows", static_cast<std::size_t>(100000));
    options.database = input.value("database", options.database);
    options.table_name = input.value("table_name", options.table_name);
    // Absent means AUTO — the scheduler targets a partition size instead of a
    // constant (#833). A value present is a manual override, honoured exactly.
    if (const auto it = input.find("partition_count"); it != input.end()) {
        options.partition_count = it->get<std::uint32_t>();
    }
    // A connector result set is a CACHE — re-pullable from its source and cheap
    // to lose — so it is RF=1 by default, explicitly, against the engine's
    // durable default of 3 (#833 section 8). promote_dataset upgrades one that
    // turns out to be worth keeping.
    options.replication_factor = input.value("replication_factor", std::size_t{1});

    // Reaches the external source over the network (consent-gated above), from
    // every participating node at once (#833 section 7).
    const zetta::connect::DistributedIngestResult ingested =
        zetta::connect::run_distributed_connector_ingest(context.node, *config,
                                                         options);
    if (!ingested.ok()) {
        return ApiResult::failure("engine_error", ingested.error);
    }
    if (ingested.rows == 0) {
        return ApiResult::failure(
            "engine_error",
            "the query returned no rows — nothing to load as a dataset");
    }

    nlohmann::json partitions = nlohmann::json::array();
    for (const auto& partition : ingested.partitions) {
        if (!partition) continue;
        partitions.push_back({{"partition_id", partition->id()},
                              {"owner_nodes", partition->owner_nodes()}});
    }
    nlohmann::json output;
    output["database"] =
        ingested.database ? ingested.database->name() : options.database;
    output["table"] = ingested.table ? ingested.table->name() : "";
    output["rows"] = ingested.rows;
    output["columns"] = ingested.columns;
    output["partitions"] = std::move(partitions);
    output["coordinated"] = ingested.coordinated;
    output["participants"] = ingested.participants;
    return ApiResult::success(std::move(output));
}

// What `create_dataset_from_query` would actually reach, named per call rather
// than per tool (#995). Two things the old generic challenge got wrong: it
// described every source as Snowflake whatever the connector's `type` really
// was, and it called a Postgres on `127.0.0.1` a cloud provider (#994).
//
// A loopback host is not a cloud: nothing leaves the machine, there is no
// provider account and no bill, so returning std::nullopt lets the call
// execute directly — the same input-aware exemption `add_nodes {target:
// "local"}` already gets. Anything else names its real type and target so the
// approver reads what would be queried, not a fixed sentence.
std::optional<zetta::governance::ConsentAction> describe_create_dataset_from_query(
    const nlohmann::json& input, ApiContext& context) {
    std::string reason;
    const std::optional<zetta::connect::ConnectorConfig> config =
        resolve_query_connector(input, context, reason);
    if (!config) {
        // The input does not resolve to a source. Fail closed: challenge with
        // what we can say, and let the handler return the real rejection.
        return zetta::governance::ConsentAction{
            "Run a read-only query against an external source and load its "
            "rows into a new partitioned table. The source could not be "
            "resolved from this input (" + reason + ").",
            {},
            {}};
    }

    // The source as a human reads it: "postgres 127.0.0.1:5433/motive",
    // "snowflake ab12345.us-east-1".
    std::string target = config->type;
    if (config->type == "postgres" || config->type == "mysql") {
        target += " " + (config->host.empty() ? std::string{"localhost"} : config->host);
        if (!config->port.empty()) target += ":" + config->port;
        if (!config->database.empty()) target += "/" + config->database;
    } else if (!config->account.empty()) {
        target += " " + config->account;
    }

    if (is_local_source(*config)) {
        return std::nullopt;  // stays on this machine — no cloud, no gate
    }

    const std::string query = input.value("query", std::string{});
    zetta::governance::ConsentAction action;
    action.summary =
        "Run a read-only query against " + target +
        " with the stored credential and load its rows into a new partitioned "
        "table placed across the cluster. Only a single SELECT/WITH may run. "
        "Reaches the source over the network (audited as dataset_added; "
        "visible in system.datasets).";
    if (!query.empty()) action.runs.push_back(query);
    return action;
}

// --- promote_dataset ----------------------------------------------------------------

const nlohmann::json& promote_dataset_schema() {
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
            {"minLength", 1},
            {"description",
             "the owning database (default datasets) — required when the same "
             "dataset name exists in several databases"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "target owners per partition (default 3), clamped to the node "
             "count; a partition that already has at least this many owners is "
             "left alone — promotion never removes one"}}}}}};
    return schema;
}

const nlohmann::json& promote_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"database", "table", "replication_factor", "partitions", "promoted"}},
        {"additionalProperties", false},
        {"properties",
         {{"database", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"table_id", {{"type", "string"}}},
          {"replication_factor",
           {{"type", "integer"},
            {"description", "the factor actually applied (clamped to the roster)"}}},
          {"partitions", {{"type", "integer"}}},
          {"promoted",
           {{"type", "integer"},
            {"description", "partitions whose owner set grew"}}},
          {"promotions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"partition_id", "owner_nodes"}},
              {"additionalProperties", false},
              {"properties",
               {{"partition_id", {{"type", "string"}}},
                {"partition", {{"type", "integer"}}},
                {"previous_owner_nodes",
                 {{"type", "array"}, {"items", {{"type", "string"}}}}},
                {"owner_nodes",
                 {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}}}}}};
    return schema;
}

ApiResult handle_promote_dataset(const nlohmann::json& input, ApiContext& context) {
    const std::string name = input.at("dataset").get<std::string>();
    const std::string requested_database = input.value("database", std::string{});
    // Resolve the dataset the same way get_dataset does: by name, with the
    // database required only when the name is ambiguous.
    std::vector<std::string> databases;
    for (const zetta::data::DatasetSummary& dataset : context.node.datasets()) {
        if (dataset.name != name) continue;
        if (!requested_database.empty() && dataset.database != requested_database) {
            continue;
        }
        databases.push_back(dataset.database);
    }
    if (databases.empty()) {
        return ApiResult::failure(
            "not_found",
            "no dataset named '" + name + "'" +
                (requested_database.empty()
                     ? ""
                     : " in database '" + requested_database + "'"));
    }
    if (databases.size() > 1) {
        std::string names;
        for (const std::string& database : databases) {
            if (!names.empty()) names += ", ";
            names += database;
        }
        return ApiResult::failure(
            "engine_error", "dataset '" + name + "' exists in several databases (" +
                                names + ") — pass \"database\" to pick one");
    }

    const std::size_t replication_factor = input.value(
        "replication_factor",
        static_cast<std::size_t>(zetta::data::kDefaultReplicationFactor));
    const zetta::data::PromoteDatasetResult promoted = zetta::data::promote_dataset(
        context.node, databases.front(), name, replication_factor,
        context.node.cluster_members());
    if (!promoted.ok()) {
        return ApiResult::failure("engine_error", promoted.error);
    }

    nlohmann::json promotions = nlohmann::json::array();
    for (const zetta::data::PartitionPromotion& promotion : promoted.promotions) {
        promotions.push_back({{"partition_id", promotion.partition_uuid},
                              {"partition", promotion.partition},
                              {"previous_owner_nodes", promotion.from},
                              {"owner_nodes", promotion.to}});
    }
    nlohmann::json output;
    output["database"] = promoted.database;
    output["table"] = promoted.table;
    output["table_id"] = promoted.table_id;
    output["replication_factor"] = promoted.replication_factor;
    output["partitions"] = promoted.partitions;
    output["promoted"] = promoted.promoted;
    output["promotions"] = std::move(promotions);
    return ApiResult::success(std::move(output));
}

// --- testsuite_run (kind: job) ------------------------------------------------------

const nlohmann::json& testsuite_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"case"}},
        {"additionalProperties", false},
        {"properties",
         {{"case",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "a test-suite case id (testsuite_list / "
                            "system.test_cases / the Test suite panel list "
                            "them)"}}},
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

const nlohmann::json& testsuite_run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"run_id", "case", "status", "poll"}},
        {"additionalProperties", false},
        {"properties",
         {{"run_id",
           {{"type", "string"},
            {"description",
             "the durable engine handle — poll get_run / system.runs"}}},
          {"case", {{"type", "string"}}},
          {"status", {{"type", "string"}}},
          {"poll", {{"type", "string"}}}}}};
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

ApiResult handle_testsuite_run(const nlohmann::json& input, ApiContext& context) {
    if (!context.background) {
        return ApiResult::failure(
            "engine_error",
            "testsuite_run is a job capability and this embedding wired no "
            "background action owner");
    }
    const std::string case_id = input.at("case").get<std::string>();
    const zetta::testkit::TestCase* test_case =
        zetta::testkit::TestSuite::builtin().find(case_id);
    if (!test_case) {
        return ApiResult::failure(
            "not_found", "no test-suite case with id '" + case_id + "'");
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

// --- testsuite_list (read) ----------------------------------------------------
//
// The whole in-app test-suite catalog (zetta::testkit::TestSuite::builtin()) as
// a structured listing — the same cases the headed Test suite panel renders and
// system.test_cases serves — so an agent can discover every case id + its
// tunable options before feeding one to testsuite_run.

const nlohmann::json& testsuite_list_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

std::string_view option_type_name(zetta::testkit::OptionType type) {
    switch (type) {
        case zetta::testkit::OptionType::Bool:
            return "bool";
        case zetta::testkit::OptionType::Int:
            return "int";
        case zetta::testkit::OptionType::Double:
            return "double";
        case zetta::testkit::OptionType::Choice:
            return "choice";
        case zetta::testkit::OptionType::Text:
            return "text";
    }
    return "text";
}

const nlohmann::json& testsuite_list_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"tests"}},
        {"additionalProperties", false},
        {"properties",
         {{"tests",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required",
               {"id", "category", "name", "description", "options"}},
              {"additionalProperties", false},
              {"properties",
               {{"id", {{"type", "string"}}},
                {"category", {{"type", "string"}}},
                {"name", {{"type", "string"}}},
                {"description", {{"type", "string"}}},
                {"options",
                 {{"type", "array"},
                  {"items",
                   {{"type", "object"},
                    {"required", {"key", "label", "type"}},
                    {"additionalProperties", false},
                    {"properties",
                     {{"key", {{"type", "string"}}},
                      {"label", {{"type", "string"}}},
                      {"type", {{"type", "string"}}}}}}}}}}}}}}}}}};
    return schema;
}

ApiResult handle_testsuite_list(const nlohmann::json&, ApiContext&) {
    nlohmann::json tests = nlohmann::json::array();
    for (const zetta::testkit::TestCase& test_case :
         zetta::testkit::TestSuite::builtin().cases()) {
        nlohmann::json options = nlohmann::json::array();
        for (const zetta::testkit::TestOption& option : test_case.options) {
            options.push_back(
                {{"key", option.key},
                 {"label", option.label},
                 {"type", std::string(option_type_name(option.type))}});
        }
        tests.push_back(
            {{"id", test_case.id},
             {"category",
              std::string(zetta::testkit::to_string(test_case.category))},
             {"name", test_case.name},
             {"description", test_case.description},
             {"options", std::move(options)}});
    }
    nlohmann::json output;
    output["tests"] = std::move(tests);
    return ApiResult::success(std::move(output));
}

// --- list_specs / get_spec (#646) --------------------------------------------

// The spec index: the OpenAPI document + every capability input/output schema
// + every boundary schema, rendered by specs.cpp from the registry and
// zetta::schemas::boundary_schemas() — served from the C++ accessors, never
// from the generated schemas/*.json artifacts.

const nlohmann::json& list_specs_schema() {
    static const nlohmann::json schema = empty_object_schema();
    return schema;
}

const nlohmann::json& list_specs_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"count", "specs"}},
        {"additionalProperties", false},
        {"properties",
         {{"count", {{"type", "integer"}}},
          {"specs",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"name", "kind", "href"}},
              {"additionalProperties", false},
              {"properties",
               {{"name",
                 {{"type", "string"},
                  {"description", "pass to get_spec for the document"}}},
                {"kind",
                 {{"type", "string"},
                  {"enum", {"openapi", "capability_input", "capability_output",
                            "boundary"}}}},
                {"href",
                 {{"type", "string"},
                  {"description", "GET this route for the raw document"}}},
                {"capability", {{"type", "string"}}},
                {"file",
                 {{"type", "string"},
                  {"description",
                   "the generated schemas/<file> artifact this accessor "
                   "keeps honest"}}},
                {"description", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

const nlohmann::json& get_spec_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "a spec name from list_specs: 'openapi', '<capability>.input', "
             "'<capability>.output', or a boundary schema name like 'run'"}}}}}};
    return schema;
}

const nlohmann::json& get_spec_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name", "kind", "schema"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"kind",
           {{"type", "string"},
            {"enum", {"openapi", "capability_input", "capability_output",
                      "boundary"}}}},
          {"capability", {{"type", "string"}}},
          {"file", {{"type", "string"}}},
          {"description", {{"type", "string"}}},
          {"schema",
           {{"type", "object"},
            {"description",
             "the document itself — draft-07 JSON Schema, or the OpenAPI 3.1 "
             "document for 'openapi'"}}}}}};
    return schema;
}

// --- agents: CRUD for the served, selectable agent entity (#849) --------------

const nlohmann::json& agent_fields_schema() {
    // The agent record shape — reused as the create/get/update output schema.
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"description", {{"type", "string"}}},
          {"model", {{"type", "string"}}},
          {"system_prompt", {{"type", "string"}}},
          {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          // zetta#1015: the agent's family and runtime, the Studio agent shape, the Ask Orama
          // contract, granted Pipelines, a Python agent's definition and a service's endpoint.
          {"kind", {{"type", "string"}, {"enum", {"custom", "orama"}}}},
          {"runtime", {{"type", "string"}, {"enum", {"native", "python", "service"}}}},
          {"purpose", {{"type", "string"}}},
          {"work_area",
           {{"type", "object"},
            {"properties",
             {{"kind", {{"type", "string"}, {"enum", {"workspace", "project", "dataset"}}}},
              {"detail", {{"type", "string"}}}}}}},
          {"manifesto", {{"type", "string"}}},
          {"goal", {{"type", "string"}}},
          {"skills",
           {{"type", "array"},
            {"items", zetta::agent::agent_skill_schema()}}},
          {"trigger", {{"type", "string"}, {"description", "empty = conversation only"}}},
          {"status",
           {{"type", "string"},
            {"enum", {"idle", "running", "waiting", "completed", "failed"}}}},
          {"model_name", {{"type", "string"}}},
          {"model_version", {{"type", "string"}}},
          {"chat_capable", {{"type", "boolean"}}},
          {"is_default", {{"type", "boolean"}}},
          {"pipelines",
           {{"type", "array"},
            {"description",
             "approved Pipelines the agent may call as tools; version pins the approval"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"version", {{"type", "string"}}}}}}}}},
          {"definition",
           {{"type", "object"},
            {"description",
             "runtime: python. source zetta (instructions + files inline, at most 1 MiB) or "
             "lake (repository_id + revision); entrypoint is module:function"}}},
          {"endpoint", {{"type", "string"}, {"description", "runtime: service"}}},
          {"created_at", {{"type", "string"}}},
          {"updated_at", {{"type", "string"}}}}}};
    return schema;
}

// A Python agent's `definition` as create_agent / update_agent take it (#1220). Closed at both
// levels: `apply_agent_fields` reads only these keys, so an unknown one (a file sent as
// `{path, content}`) used to be stored as an empty file without a word. Refused here instead,
// with the field's path, and with the same wording lake gives in front of the workspace.
const nlohmann::json& agent_definition_input_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"description",
         "runtime: python. `{source: \"zetta\", instructions, entrypoint, files: [{path, "
         "contents}]}` (inline, at most 1 MiB) or `{source: \"lake\", repository_id, revision, "
         "entrypoint}`; entrypoint is `module:function`"},
        {"properties",
         {{"source", {{"type", "string"}, {"description", "`zetta` (inline) or `lake`"}}},
          {"instructions", {{"type", "string"}, {"description", "with `source: \"zetta\"`: the agent's instructions"}}},
          {"entrypoint", {{"type", "string"}, {"description", "module:function"}}},
          {"repository_id", {{"type", "string"}, {"description", "with `source: \"lake\"`: the repository"}}},
          {"revision",
           {{"type", "string"}, {"description", "with `source: \"lake\"`: a pinned revision of the repository"}}},
          {"files",
           {{"type", "array"},
            {"description", "with `source: \"zetta\"`: the agent's files, one entry per file"},
            {"items",
             {{"type", "object"},
              // `path` and `contents` are not `required` here: the validator reports a
              // missing required key before an unknown one, so {path, content} would be
              // refused as "no contents" rather than at the misspelt key. Presence is
              // checked by definition_file_refusal once the shape has passed.
              {"additionalProperties", false},
              {"properties",
               {{"path",
                 {{"type", "string"},
                  {"minLength", 1},
                  {"description", "the file's path, relative to the definition"}}},
                {"contents",
                 {{"type", "string"}, {"description", "the file's text"}}}}}}}}}}}};
    return schema;
}

// The plain statement of the definition's shape, appended to a schema rejection under
// `definition` — the same words lake's agent_definition_refusal answers with (lake PR #457), so
// a caller reads one answer from either layer. Empty when the definition is not at fault.
std::string agent_definition_hint(const nlohmann::json& input) {
    if (!input.is_object()) return {};
    const auto definition = input.find("definition");
    if (definition == input.end()) return {};
    const std::string shape =
        "definition is {source: \"zetta\", instructions, entrypoint, files: [{path, contents}]} "
        "or {source: \"lake\", repository_id, revision, entrypoint}";
    if (!definition->is_object()) return shape;
    constexpr std::array<std::string_view, 6> fields = {
        "source", "instructions", "files", "entrypoint", "repository_id", "revision"};
    for (const auto& [key, value] : definition->items()) {
        if (std::ranges::find(fields, key) == fields.end()) {
            return "definition has no field '" + key + "'; " + shape;
        }
        if (key != "files" && !value.is_string()) return "definition." + key + " is a string";
    }
    const auto files = definition->find("files");
    if (files == definition->end()) return {};
    if (!files->is_array()) {
        return "definition.files is a list of {path, contents}, one entry per file";
    }
    for (const nlohmann::json& file : *files) {
        if (!file.is_object()) {
            return "each file is {path, contents}: the path and the file's text";
        }
        for (const auto& [key, value] : file.items()) {
            if (key != "path" && key != "contents") {
                return "a file has no field '" + key +
                       "'; each file is {path, contents}, and contents holds the file's text";
            }
            if (!value.is_string()) return key + " is a string";
        }
        if (file.value("path", std::string()).empty()) return "each file names its path";
        if (!file.contains("contents")) return "each file carries its text in contents";
    }
    return {};
}

// Each inline file names its path and carries its text (lake's words, lake PR #457). Runs after
// the closed schema has passed, so `files` is already a list of {path?, contents?} strings.
std::optional<ApiResult> definition_file_refusal(const nlohmann::json& input) {
    const auto definition = input.find("definition");
    if (definition == input.end() || !definition->is_object()) return std::nullopt;
    const auto files = definition->find("files");
    if (files == definition->end() || !files->is_array()) return std::nullopt;
    for (std::size_t index = 0; index < files->size(); ++index) {
        const nlohmann::json& file = (*files)[index];
        const std::string here = "/definition/files/" + std::to_string(index);
        if (!file.contains("path")) {
            return ApiResult::failure("invalid_input", "each file names its path", here + "/path");
        }
        if (!file.contains("contents")) {
            return ApiResult::failure("invalid_input", "each file carries its text in contents",
                                      here + "/contents");
        }
    }
    return std::nullopt;
}

const nlohmann::json& create_agent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1}}},
          {"description", {{"type", "string"}}},
          {"model",
           {{"type", "string"},
            {"description",
             "native agents (required there): checkpoint dir, model:<uuid>, or a HuggingFace "
             "repo id"}}},
          // zetta#1015: the agent's family and runtime, the Studio agent shape, the Ask Orama
          // contract, granted Pipelines, a Python agent's definition and a service's endpoint.
          {"kind", {{"type", "string"}, {"enum", {"custom", "orama"}}}},
          {"runtime", {{"type", "string"}, {"enum", {"native", "python", "service"}}}},
          {"purpose", {{"type", "string"}}},
          {"work_area",
           {{"type", "object"},
            {"properties",
             {{"kind", {{"type", "string"}, {"enum", {"workspace", "project", "dataset"}}}},
              {"detail", {{"type", "string"}}}}}}},
          {"manifesto", {{"type", "string"}}},
          {"goal", {{"type", "string"}}},
          {"skills",
           {{"type", "array"},
            {"items", zetta::agent::agent_skill_schema()}}},
          {"trigger", {{"type", "string"}, {"description", "empty = conversation only"}}},
          {"status",
           {{"type", "string"},
            {"enum", {"idle", "running", "waiting", "completed", "failed"}}}},
          {"model_name", {{"type", "string"}}},
          {"model_version", {{"type", "string"}}},
          {"chat_capable", {{"type", "boolean"}}},
          {"is_default", {{"type", "boolean"}}},
          {"pipelines",
           {{"type", "array"},
            {"description",
             "approved Pipelines the agent may call as tools; version pins the approval"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"version", {{"type", "string"}}}}}}}}},
          {"definition", agent_definition_input_schema()},
          {"endpoint", {{"type", "string"}, {"description", "runtime: service"}}},

          {"system_prompt", {{"type", "string"}}},
          {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          // Goals are editable after creation: what an agent is rewarded for is the thing most
          // likely to change as a deployment learns what it wants. Editing them moves the
          // environment fingerprint and marks every existing Saved Version stale — the intended
          // consequence, reported by agent_training_status.
          {"goals", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
    return schema;
}

const nlohmann::json& update_agent_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id"}},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}, {"minLength", 1}}},
          {"name", {{"type", "string"}, {"minLength", 1}}},
          {"description", {{"type", "string"}}},
          {"model", {{"type", "string"}}},
          // zetta#1015: the agent's family and runtime, the Studio agent shape, the Ask Orama
          // contract, granted Pipelines, a Python agent's definition and a service's endpoint.
          {"kind", {{"type", "string"}, {"enum", {"custom", "orama"}}}},
          {"runtime", {{"type", "string"}, {"enum", {"native", "python", "service"}}}},
          {"purpose", {{"type", "string"}}},
          {"work_area",
           {{"type", "object"},
            {"properties",
             {{"kind", {{"type", "string"}, {"enum", {"workspace", "project", "dataset"}}}},
              {"detail", {{"type", "string"}}}}}}},
          {"manifesto", {{"type", "string"}}},
          {"goal", {{"type", "string"}}},
          {"skills",
           {{"type", "array"},
            {"items", zetta::agent::agent_skill_schema()}}},
          {"trigger", {{"type", "string"}, {"description", "empty = conversation only"}}},
          {"status",
           {{"type", "string"},
            {"enum", {"idle", "running", "waiting", "completed", "failed"}}}},
          {"model_name", {{"type", "string"}}},
          {"model_version", {{"type", "string"}}},
          {"chat_capable", {{"type", "boolean"}}},
          {"is_default", {{"type", "boolean"}}},
          {"pipelines",
           {{"type", "array"},
            {"description",
             "approved Pipelines the agent may call as tools; version pins the approval"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"project", {{"type", "string"}}},
                {"graph", {{"type", "string"}}},
                {"version", {{"type", "string"}}}}}}}}},
          {"definition", agent_definition_input_schema()},
          {"endpoint", {{"type", "string"}, {"description", "runtime: service"}}},

          {"system_prompt", {{"type", "string"}}},
          {"tools", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          // Goals are editable after creation: what an agent is rewarded for is the thing most
          // likely to change as a deployment learns what it wants. Editing them moves the
          // environment fingerprint and marks every existing Saved Version stale — the intended
          // consequence, reported by agent_training_status.
          {"goals", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
    return schema;
}

const nlohmann::json& agent_id_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"id"}},
        {"additionalProperties", false},
        {"properties", {{"id", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& list_agents_schema() {
    static const nlohmann::json schema = {{"type", "object"},
                                          {"additionalProperties", false},
                                          {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_agents_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"agents"}},
        {"properties",
         {{"agents", {{"type", "array"}, {"items", agent_fields_schema()}}},
          {"default_agent_id",
           {{"type", "string"},
            {"description", "the agent marked is_default (Ask Orama); empty when none"}}}}}};
    return schema;
}

const nlohmann::json& delete_agent_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"removed"}},
        {"properties",
         {{"removed", {{"type", "boolean"}}}, {"id", {{"type", "string"}}}}}};
    return schema;
}

// Apply the provided fields of a create/update input onto `base` (a fresh record on
// create, the existing one on update). Only present fields overwrite.
zetta::agent::AgentRecord agent_from_input(const nlohmann::json& input,
                                           zetta::agent::AgentRecord base) {
    if (input.contains("name")) base.name = input.at("name").get<std::string>();
    if (input.contains("description")) {
        base.description = input.at("description").get<std::string>();
    }
    if (input.contains("model")) base.model = input.at("model").get<std::string>();
    if (input.contains("system_prompt")) {
        base.system_prompt = input.at("system_prompt").get<std::string>();
    }
    if (input.contains("tools") && input.at("tools").is_array()) {
        base.tools.clear();
        for (const nlohmann::json& tool : input.at("tools")) {
            if (tool.is_string()) base.tools.push_back(tool.get<std::string>());
        }
    }
    if (input.contains("goals") && input.at("goals").is_array()) {
        base.goals.clear();
        for (const nlohmann::json& entry : input.at("goals")) {
            if (std::optional<zetta::agent::Goal> goal = zetta::agent::goal_from_json(entry)) {
                base.goals.push_back(*std::move(goal));
            }
        }
        // Resolve to checks here, so the stored record says what it actually trains on rather
        // than leaving it to be inferred at reward time.
        zetta::agent::interpret_goals(base.goals);
    }
    return base;
}

// The fields zetta#1015 added, then every granted Pipeline checked: it must exist and be approved,
// and the grant is pinned to that approval (its approved_at). A graph that is only proposed can
// never be granted, so an agent never calls an unapproved graph.
std::optional<std::string> apply_agent_input(const nlohmann::json& input,
                                             zetta::agent::AgentRecord& agent,
                                             ApiContext& context) {
    if (std::optional<std::string> reason = zetta::agent::apply_agent_fields(input, agent)) {
        return reason;
    }
    if (!input.contains("pipelines")) return std::nullopt;
    for (zetta::agent::PipelineGrant& grant : agent.pipelines) {
        const auto project = resolve_project(context, grant.project);
        if (!project) return "agent.pipelines: no project '" + grant.project + "'";
        const auto graph = resolve_graph(*project, grant.graph);
        if (!graph) {
            return "agent.pipelines: no graph '" + grant.graph + "' on project '" +
                   project->name() + "'";
        }
        if (graph->approval_status() != "approved") {
            return "agent.pipelines: '" + graph->name() + "' on '" + project->name() +
                   "' is not approved; only an approved Pipeline can be granted to an agent";
        }
        grant.project = project->name();
        grant.graph = graph->name();
        grant.version = graph->approved_at();
    }
    return std::nullopt;
}

ApiResult handle_create_agent(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    zetta::agent::AgentRecord agent;
    agent.id = zetta::util::new_uuid();
    agent.created_at = zetta::util::now_iso8601();
    agent.updated_at = agent.created_at;
    if (std::optional<ApiResult> refused = definition_file_refusal(input)) return *refused;
    agent = agent_from_input(input, std::move(agent));
    if (std::optional<std::string> reason = apply_agent_input(input, agent, context)) {
        return ApiResult::failure("invalid_argument", *reason);
    }

    zetta::agent::AgentStore store(*context.node.replication());
    std::string error;
    const std::optional<zetta::agent::AgentRecord> stored = store.put(agent, &error);
    if (!stored) return ApiResult::failure("invalid_argument", error);
    return ApiResult::success(zetta::agent::to_json(*stored));
}

ApiResult handle_list_agents(const nlohmann::json&, ApiContext& context) {
    nlohmann::json agents = nlohmann::json::array();
    std::string default_agent_id;
    if (context.node.replication() != nullptr) {
        zetta::agent::AgentStore store(*context.node.replication());
        for (const zetta::agent::AgentRecord& agent : store.list()) {
            // The Ask Orama contract: one explicit default, and only when it can hold a
            // conversation. List position never supplies it.
            if (agent.is_default && agent.chat_capable && default_agent_id.empty()) {
                default_agent_id = agent.id;
            }
            agents.push_back(zetta::agent::to_json(agent));
        }
    }
    nlohmann::json output;
    output["agents"] = std::move(agents);
    output["default_agent_id"] = default_agent_id;
    return ApiResult::success(std::move(output));
}

ApiResult handle_get_agent(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no agents on a standalone node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + id + "'");
    return ApiResult::success(zetta::agent::to_json(*agent));
}

const nlohmann::json& training_schedule_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["required"] = nlohmann::json::array({"id", "training"});
        document["additionalProperties"] = false;
        document["properties"]["id"] = {{"type", "string"}, {"minLength", 1},
                                        {"description", "the agent's id"}};
        document["properties"]["training"] = zetta::agent::training_spec_schema();
        return document;
    }();
    return schema;
}

const nlohmann::json& training_status_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document;
        document["type"] = "object";
        document["properties"]["stale"] = {{"type", "boolean"}};
        document["properties"]["due"] = {{"type", "boolean"}};
        document["properties"]["reason"] = {{"type", "string"}};
        document["properties"]["versions"] = {{"type", "array"}};
        return document;
    }();
    return schema;
}

ApiResult handle_update_agent(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> existing = store.get(id);
    if (!existing) return ApiResult::failure("not_found", "no agent '" + id + "'");
    if (std::optional<ApiResult> refused = definition_file_refusal(input)) return *refused;
    zetta::agent::AgentRecord updated = agent_from_input(input, *existing);
    if (std::optional<std::string> reason = apply_agent_input(input, updated, context)) {
        return ApiResult::failure("invalid_argument", *reason);
    }
    updated.updated_at = zetta::util::now_iso8601();
    std::string error;
    const std::optional<zetta::agent::AgentRecord> stored = store.put(updated, &error);
    if (!stored) return ApiResult::failure("invalid_argument", error);
    return ApiResult::success(zetta::agent::to_json(*stored));
}

// Whether an agent should be retrained, and why (#941). Read-only: the answer a console shows
// next to a Retrain button, and the same answer a scheduler acts on — one source, so the two
// cannot disagree about the same agent.
ApiResult handle_agent_training_status(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + id + "'");
    if (!agent->native()) {
        // A Python or service agent runs a frontier model or its own code; Zetta trains nothing
        // for it. Saying "untrained" would invite a retrain that cannot happen.
        return ApiResult::success(nlohmann::json{
            {"agent", agent->id},
            {"trained", false},
            {"stale", false},
            {"due", false},
            {"reason", "Not trained by Zetta: this agent runs outside the engine (" +
                           agent->runtime + ")."},
            {"versions", nlohmann::json::array()}});
    }

    const zetta::agent::RetrainStatus status = zetta::agent::retrain_status(
        agent->training, agent->versions, agent->tools, agent->goals, zetta::util::now_iso8601());

    nlohmann::json versions = nlohmann::json::array();
    for (const zetta::agent::AgentVersion& version : agent->versions) {
        versions.push_back(zetta::agent::to_json(version));
    }
    // Goals nobody can verify are reported here too — a goal that resolves to no check is one
    // training silently drops, and the person who wrote it should be told rather than assume.
    return ApiResult::success(
        nlohmann::json{{"agent", agent->id},
                       {"trained", status.trained},
                       {"stale", status.stale},
                       {"due", status.due},
                       {"reason", status.reason},
                       {"fingerprint", status.fingerprint},
                       {"training", zetta::agent::to_json(agent->training)},
                       {"versions", std::move(versions)},
                       {"unverifiable_goals", zetta::agent::unmapped_goals(agent->goals)}});
}

// Set how and when an agent retrains (#941). Separate from update_agent because changing a
// SCHEDULE is not the same act as changing what the agent can do — one is operational, the other
// changes the environment and makes every existing version stale.
ApiResult handle_set_training_schedule(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("engine_error", "agents require a replicated node");
    }
    const std::string id = input.at("id").get<std::string>();
    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> existing = store.get(id);
    if (!existing) return ApiResult::failure("not_found", "no agent '" + id + "'");

    std::string reason;
    const std::optional<zetta::agent::TrainingSpec> spec =
        zetta::agent::training_spec_from_json(input.at("training"), &reason);
    if (!spec) return ApiResult::failure("invalid_argument", reason);

    zetta::agent::AgentRecord updated = *existing;
    updated.training = *spec;
    updated.updated_at = zetta::util::now_iso8601();
    std::string error;
    const std::optional<zetta::agent::AgentRecord> stored = store.put(updated, &error);
    if (!stored) return ApiResult::failure("invalid_argument", error);
    return ApiResult::success(zetta::agent::to_json(*stored));
}

ApiResult handle_delete_agent(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("id").get<std::string>();
    bool removed = false;
    if (context.node.replication() != nullptr) {
        zetta::agent::AgentStore store(*context.node.replication());
        removed = store.remove(id);
    }
    nlohmann::json output;
    output["removed"] = removed;
    output["id"] = id;
    return ApiResult::success(std::move(output));
}

// --- workspace profiles (#886, the create-a-data-profile pathway) ------------

const nlohmann::json& get_workspace_profile_schema() {
    static const nlohmann::json schema = id_input_schema(
        "workspace_profile_id",
        "the profile's uuid (system.workspace_profiles.workspace_profile_id)");
    return schema;
}

const nlohmann::json& workspace_profile_output_schema() {
    static const nlohmann::json schema = entity_value_with_id_schema(
        zetta::workspace::workspace_profile_value_schema());
    return schema;
}

const nlohmann::json& create_workspace_profile_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"role",
           {{"type", "string"}, {"description", "the user's workspace role"}}},
          {"daily_work",
           {{"type", "string"},
            {"description", "plain-language daily-work description"}}},
          {"sources",
           {{"type", "array"}, {"items", {{"type", "object"}}},
            {"description", "approved source refs, each {id,name,kind}"}}}}}};
    return schema;
}

const nlohmann::json& update_workspace_profile_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"workspace_profile_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"workspace_profile_id",
           {{"type", "string"}, {"minLength", 1},
            {"description", "the profile's uuid to update"}}},
          {"role", {{"type", "string"}}},
          {"daily_work", {{"type", "string"}}},
          {"sources", {{"type", "array"}, {"items", {{"type", "object"}}}}}}}};
    return schema;
}

ApiResult handle_get_workspace_profile(const nlohmann::json& input,
                                       ApiContext& context) {
    const std::string id = input.at("workspace_profile_id").get<std::string>();
    const auto profile = context.node.find_workspace_profile(id);
    if (!profile) {
        return ApiResult::failure("not_found",
                                  "no workspace profile with id '" + id + "'");
    }
    nlohmann::json output = profile->metadata();
    output["id"] = profile->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_create_workspace_profile(const nlohmann::json& input,
                                          ApiContext& context) {
    // The profile belongs to the calling principal (server-set, not client-set).
    zetta::workspace::WorkspaceProfileValue value;
    value.owner = actor_of(context);
    value.role = input.value("role", std::string{});
    value.daily_work = input.value("daily_work", std::string{});
    if (const auto it = input.find("sources"); it != input.end()) {
        value.sources = *it;
    }
    const auto profile = context.node.create_workspace_profile(std::move(value));
    if (!profile) {
        return ApiResult::failure("engine_error",
                                  "could not create the workspace profile");
    }
    nlohmann::json output = profile->metadata();
    output["id"] = profile->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_update_workspace_profile(const nlohmann::json& input,
                                          ApiContext& context) {
    const std::string id = input.at("workspace_profile_id").get<std::string>();
    const auto profile = context.node.find_workspace_profile(id);
    if (!profile) {
        return ApiResult::failure("not_found",
                                  "no workspace profile with id '" + id + "'");
    }
    // Apply present fields, defaulting to the profile's current values; the owner
    // and created stamp stay fixed.
    const std::string role = input.value("role", profile->role());
    const std::string daily_work = input.value("daily_work", profile->daily_work());
    nlohmann::json sources = profile->sources();
    if (const auto it = input.find("sources"); it != input.end()) sources = *it;
    profile->set_content(role, daily_work, std::move(sources),
                         zetta::util::now_iso8601());
    nlohmann::json output = profile->metadata();
    output["id"] = profile->id();
    return ApiResult::success(std::move(output));
}

// --- plans (Conflict D: metered usage + plan) --------------------------------

const nlohmann::json& get_plan_schema() {
    static const nlohmann::json schema =
        id_input_schema("plan_id", "the plan's uuid (system.plans.plan_id)");
    return schema;
}

const nlohmann::json& plan_output_schema() {
    static const nlohmann::json schema =
        entity_value_with_id_schema(zetta::billing::plan_value_schema());
    return schema;
}

const nlohmann::json& create_plan_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"name"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}, {"minLength", 1},
                    {"description", "the plan's display name"}}},
          {"tier", {{"type", "string"},
                    {"description", "display tier (free / pro / enterprise)"}}},
          {"quotas", {{"type", "object"},
                      {"additionalProperties", {{"type", "number"}}},
                      {"description", "numeric limits, e.g. {max_runs, max_storage_bytes}"}}},
          {"period_start", {{"type", "string"},
                            {"description", "ISO-8601 start of the current period"}}}}}};
    return schema;
}

const nlohmann::json& update_plan_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"plan_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"plan_id", {{"type", "string"}, {"minLength", 1},
                       {"description", "the plan's uuid to update"}}},
          {"name", {{"type", "string"}, {"minLength", 1}}},
          {"tier", {{"type", "string"}}},
          {"quotas", {{"type", "object"}, {"additionalProperties", {{"type", "number"}}}}},
          {"period_start", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_get_plan(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("plan_id").get<std::string>();
    const auto plan = context.node.find_plan(id);
    if (!plan) {
        return ApiResult::failure("not_found", "no plan with id '" + id + "'");
    }
    nlohmann::json output = plan->metadata();
    output["id"] = plan->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_create_plan(const nlohmann::json& input, ApiContext& context) {
    zetta::billing::PlanValue value;
    value.name = input.at("name").get<std::string>();
    value.tier = input.value("tier", std::string{});
    if (const auto it = input.find("quotas"); it != input.end()) value.quotas = *it;
    value.period_start = input.value("period_start", std::string{});
    const auto plan = context.node.create_plan(std::move(value));
    if (!plan) {
        return ApiResult::failure("engine_error", "could not create the plan");
    }
    nlohmann::json output = plan->metadata();
    output["id"] = plan->id();
    return ApiResult::success(std::move(output));
}

ApiResult handle_update_plan(const nlohmann::json& input, ApiContext& context) {
    const std::string id = input.at("plan_id").get<std::string>();
    const auto plan = context.node.find_plan(id);
    if (!plan) {
        return ApiResult::failure("not_found", "no plan with id '" + id + "'");
    }
    const std::string name = input.value("name", plan->name());
    const std::string tier = input.value("tier", plan->tier());
    nlohmann::json quotas = plan->quotas();
    if (const auto it = input.find("quotas"); it != input.end()) quotas = *it;
    const std::string period_start = input.value("period_start", plan->period_start());
    plan->set_content(name, tier, std::move(quotas), period_start,
                      zetta::util::now_iso8601());
    nlohmann::json output = plan->metadata();
    output["id"] = plan->id();
    return ApiResult::success(std::move(output));
}

// --- chat: talk to an agent, with tool-calling (#849) -------------------------

const nlohmann::json& chat_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"agent", "message"}},
        {"additionalProperties", false},
        {"properties",
         {{"agent", {{"type", "string"}, {"minLength", 1}, {"description", "the agent's id"}}},
          {"history",
             {{"type", "array"},
              {"description",
               "the conversation so far, oldest first: [{ask, reply, tools:[{tool,status,detail}]}]. "
               "The tool outcomes matter — a follow-up like \"why did that fail?\" refers to what "
               "ran, not to the prose."},
              {"items", {{"type", "object"}}}}},
            {"message", {{"type", "string"}, {"minLength", 1}}},
          {"project",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "scope the answer to this project (name or uuid): its trained model's "
             "measure and ranked drivers are given to the agent, so it answers from "
             "what the pipeline actually produced instead of from priors (#914)"}}},
          {"dataset_path",
           {{"type", "string"},
            {"minLength", 1},
            {"description",
             "when the turn proposes a run, the dataset it would train on (`database.table` "
             "or a dataset directory); the built-in demo dataset when omitted (#969)"}}},
          {"holdout_fraction",
           {{"type", "number"},
            {"minimum", 0},
            {"maximum", 1},
            {"description",
             "when the turn proposes a run, the rows it holds out to measure the model on "
             "data it did not train on (#953)"}}},
          {"max_new_tokens", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}}},
          {"max_rounds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}}}}};
    return schema;
}

const nlohmann::json& chat_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"reply", "tool_calls", "rounds", "grounded", "kind"}},
        {"properties",
         {{"reply", {{"type", "string"}}},
          {"rounds", {{"type", "integer"}}},
          {"grounded_project",
           {{"type", "string"},
            {"description",
             "the Project this reply was grounded in. Present whenever grounding "
             "happened — including when the engine chose it, because the caller "
             "named none and the most recent successful run's Project was used"}}},
          {"grounded",
           {{"type", "boolean"},
            {"description",
             "true only when a project was named AND it has a trained model whose real "
             "measure + ranked drivers were given to the agent. False means the reply "
             "rests on the model's priors — present it as such, never as a measured "
             "result (#914)"}}},
          {"grounding_warning",
           {{"type", "string"},
            {"description", "why the reply is ungrounded, when it is"}}},
          {"kind",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"action", "answer", "clarify", "abstain",
                                            "unparsed"})},
            {"description",
             "the mode the agent declared for this turn: answer renders as prose, clarify is a "
             "question awaiting a reply, abstain is a refusal to present as such, action carries "
             "a proposed Pipeline. `unparsed` means the generation was not a valid "
             "envelope - `reply` then carries an honest failure sentence and the raw "
             "generation moves to `raw`, so it never reaches a chat window (#979)"}}},
          {"raw",
           {{"type", "string"},
            {"description",
             "the raw generation, present only when `kind` is `unparsed`. For "
             "developers and traces - never render it to a user"}}},
          {"pipeline",
           {{"type", "object"},
            {"description", "the proposed Pipeline, when the turn built one"}}},
          {"proposal",
           {{"type", "object"},
            {"description",
             "when the turn is an `action`, the run it proposes (propose_run's output): the "
             "impact preview and the calls that approve or reject it. Nothing exists until "
             "approve_run_proposal — no Project, no Pipeline, no run (#969)"}}},
          {"proposal_warning",
           {{"type", "string"},
            {"description",
             "why an `action` turn carries no proposal (the ask did not compile, or the "
             "caller may not propose runs)"}}},
          {"tool_calls",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"tool", {{"type", "string"}}},
                {"status", {{"type", "string"}}},
                {"detail", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

// Render a trained model's real numbers as the plain lines the agent reads before
// answering (#914): the measure it scored and its drivers, ranked, strongest first.
// Drivers with zero importance are dropped — the model did not use them, and listing
// them invites the agent to talk about features that contributed nothing.
std::string model_grounding_text(const nlohmann::json& metadata, const std::string& model_name,
                                 const std::string& project_name) {
    std::string text = "Project: " + project_name + "\nTrained model: " + model_name;
    if (const auto kind = metadata.find("kind");
        kind != metadata.end() && kind->is_string()) {
        text += " (" + kind->get<std::string>() + ")";
    }
    text += "\n";

    if (const auto metrics = metadata.find("metrics");
        metrics != metadata.end() && metrics->is_object()) {
        const auto number = [&](const char* key) -> std::string {
            const auto found = metrics->find(key);
            if (found == metrics->end() || !found->is_number()) return {};
            std::ostringstream out;
            out << std::fixed << std::setprecision(3) << found->get<double>();
            return out.str();
        };
        const std::string auc = number("auc");
        const std::string accuracy = number("accuracy");
        if (!auc.empty()) text += "Measure: AUC " + auc;
        if (!accuracy.empty()) text += (auc.empty() ? "Measure: " : ", ") + std::string("accuracy ") + accuracy;
        if (!auc.empty() || !accuracy.empty()) text += "\n";
    }

    const auto schema = metadata.find("feature_schema");
    if (schema != metadata.end() && schema->is_object()) {
        const auto drivers = schema->find("drivers");
        if (drivers != schema->end() && drivers->is_array()) {
            std::vector<std::pair<std::string, double>> ranked;
            for (const nlohmann::json& driver : *drivers) {
                if (!driver.is_object()) continue;
                const auto feature = driver.find("feature");
                const auto importance = driver.find("importance");
                if (feature == driver.end() || !feature->is_string()) continue;
                const double weight =
                    (importance != driver.end() && importance->is_number())
                        ? importance->get<double>() : 0.0;
                if (weight <= 0.0) continue;  // the model did not use it
                ranked.emplace_back(feature->get<std::string>(), weight);
            }
            std::sort(ranked.begin(), ranked.end(),
                      [](const auto& left, const auto& right) { return left.second > right.second; });
            if (ranked.empty()) {
                text += "Drivers: the model reported no feature with non-zero importance.\n";
            } else {
                // Present importances as a SHARE OF TOTAL importance, which is both a
                // standard way to report feature importance and — crucially — a number
                // the agent's instinct to write "%" is correct about. Telling it "these
                // are not percentages" did not work: it wrote "failed_payments (26.4%)"
                // anyway, turning a raw weight into a false claim. Giving it real
                // percentages makes the sentence it wants to write a true one.
                double total = 0.0;
                for (const auto& [feature, weight] : ranked) total += weight;
                text +=
                    "Drivers, strongest first — each shown as its share of the model's "
                    "total feature importance (they sum to 100%):\n";
                for (const auto& [feature, weight] : ranked) {
                    std::ostringstream line;
                    line << "- " << feature << ": " << std::fixed << std::setprecision(1)
                         << (total > 0.0 ? (weight / total) * 100.0 : 0.0)
                         << "% of the model's importance\n";
                    text += line.str();
                }
            }
        }
    }
    return text;
}

// The ranked at-risk list this Project's pipeline published (#915), as lines the
// agent reads before answering "which accounts are most at risk". Matched to the
// Project's OWN pipelines by table name — never a "newest table on the node" guess,
// which would hand one Project another's accounts. Capped: the question is about the
// top of the list, and a long tail only crowds the context out.
std::string at_risk_grounding_text(Node& node,
                                   const std::vector<std::shared_ptr<zetta::pipeline::ProjectGraph>>& graphs) {
    constexpr std::size_t kMaxRows = 10;
    const std::shared_ptr<zetta::data::Database> database = node.find_database("churn_results");
    if (!database) return {};

    std::shared_ptr<zetta::data::Table> table;
    for (const auto& graph : graphs) {
        if (!graph || graph->id().size() < 8) continue;
        if (auto found = database->find_table("at_risk_" + graph->id().substr(0, 8))) {
            table = std::move(found);  // last writer wins: the newest run of this Project
        }
    }
    if (!table) return {};

    std::vector<nlohmann::json> rows;
    const zetta::storage::StorageEngine& store = table->store();
    for (std::size_t index = 0; index < store.size(); ++index) {
        const auto row = store.at(index);
        if (!row) continue;  // row.error() says why; a hole is not an answer
        rows.push_back(*row);
    }
    if (rows.empty()) return {};
    std::sort(rows.begin(), rows.end(), [](const nlohmann::json& left, const nlohmann::json& right) {
        return left.value("rank", 0.0) < right.value("rank", 0.0);
    });

    std::string text =
        "\nThe accounts this pipeline ranked most at risk. These are the ONLY accounts "
        "you may name — do not invent others — and risk is the model's calibrated "
        "probability of churn:\n";
    std::size_t shown = 0;
    for (const nlohmann::json& row : rows) {
        if (shown++ >= kMaxRows) break;
        std::ostringstream line;
        line << "- account " << row.value("account", std::int64_t{0}) << ": risk " << std::fixed
             << std::setprecision(3) << row.value("risk", 0.0);
        if (const std::string driver = row.value("dominant_driver", std::string{});
            !driver.empty()) {
            line << ", driven by " << driver;
        }
        line << "\n";
        text += line.str();
    }
    if (rows.size() > kMaxRows) {
        text += "(and " + std::to_string(rows.size() - kMaxRows) + " more below the top " +
                std::to_string(kMaxRows) + ")\n";
    }
    return text;
}

ApiResult handle_chat(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no agents on a standalone node");
    }
    const std::string agent_id = input.at("agent").get<std::string>();
    const std::string message = input.at("message").get<std::string>();
    const int max_new_tokens = input.value("max_new_tokens", 256);
    const int max_rounds = input.value("max_rounds", 3);

    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(agent_id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + agent_id + "'");
    if (!agent->native()) {
        return ApiResult::failure("invalid_argument",
                                  "agent '" + agent->name + "' runs on " +
                                      (agent->runtime == "python" ? std::string("agent-runtime")
                                                                  : std::string("its own service")) +
                                      ", outside Zetta; use it from lake");
    }

    // The tools the agent may call while answering (#904): READ-ONLY introspection,
    // an explicit allowlist — never a mutating capability, so a chat can describe
    // the node but never change it. Each call goes back through
    // `registry.call(...)` with the SAME ApiContext, so the caller's own
    // permissions and the audit trail apply exactly as if they had made the call.
    static const std::vector<zetta::agent::AgentToolSpec> kAgentTools = {
        {"list_projects", "list the Projects on this node"},
        {"get_project", "read one Project: {\"name\": \"<project>\"}"},
        {"list_datasets",
         "list the datasets (tables) available, with row and column counts"},
        {"get_dataset", "read one dataset's schema: {\"name\": \"<dataset>\"}"},
        {"analyze_dataset", "read semantic types, distributions, grain, keys, joins, and "
                            "leakage candidates for a landed dataset"},
        {"plan_features",
         "read a methodology-specific feature/exclusion plan or required dataset transform"},
        {"list_solutions", "list the configured solution catalogue"},
        {"list_methodologies",
         "list the methodologies: Orama's catalogue (read-only) and the workspace's own, "
         "each marked source and runnable"},
        {"get_methodology",
         "read one methodology, Orama's or the workspace's own: {\"id\": \"M-1 or W-…\"}"},
        {"get_solution",
         "read one solution: {\"solution\": \"<S-1 through S-12 or B-1 through B-8>\"}"},
        {"list_models", "list the trained models and their measures"},
        {"get_model", "read one model: its measures, its feature columns, and "
                      "its ranked churn "
                      "drivers: {\"model\": \"<id or name>\"}"},
        {"get_monitor",
         "read one solution's current drift, calibration, cohort performance, "
         "guardrails, active breaches, and retrain request: "
         "{\"solution\": \"<solution name or monitor uuid>\"}"},
        {"score",
         "run a trained model and get accounts ranked by risk plus what drives "
         "it — "
         "over a whole dataset: {\"model\": \"<id>\", \"dataset_path\": "
         "\"<database.table>\", \"top_k\": 10}, or over an account that is not "
         "in "
         "the data: {\"model\": \"<id>\", \"rows\": [{...}]}"},
        {"propose_improvement", "propose a change to a model and state its "
                                "predicted effect, or say why the effect "
                                "cannot be predicted: {\"model_id\": "
                                "\"<uuid>\", \"change\": \"add_holdout\"}"},
        {"model_health", "check whether a deployed model can still be trusted "
                         "- the held-out figure, the "
                         "overfitting gap, whether scores are calibrated: "
                         "{\"model_id\": \"<uuid>\"}"},
        {"get_run_report",
         "read what a finished run found — classification quality and at-risk "
         "evidence, forecast backtests and baseline comparison, or anomaly "
         "metrics and output tables: "
         "{\"run_id\": \"<uuid>\", \"limit\": 20}"},
        {"explain",
         "explain WHY one account scored as it did — every feature's signed "
         "contribution, strongest first: {\"model\": \"<id>\", "
         "\"dataset_path\": "
         "\"<database.table>\", \"entity\": 8336}, or for an account not in "
         "the "
         "data: {\"model\": \"<id>\", \"row\": {...}}"},
        {"deployment_profile",
         "ask this node how to deploy it — ports, storage, peering, security, "
         "and what "
         "it does NOT need: {\"target\": \"helm\"} returns chart values"},
        {"list_indexes", "list the search indexes and the fields they cover"},
        {"list_connections", "list the configured source connections"},
        {"server_info", "read this node's version and cluster membership"},
        // "which version are you" is asked of the model as often as of the
        // binary, and the agent is the only surface a person asks it on.
        {"versions",
         "report the software version and WHICH agent version is answering, "
         "including its checkpoint"},
    };
    // OPT-IN per agent: an agent only gets live tools it explicitly declares in its
    // record. This matters — naming tools in the prompt changes what the model
    // writes, and the churn-trained base agent (#555) answers WORSE when told about
    // tools it was never trained to use. So the default agent is untouched, and
    // enabling introspection is a data change (`update_agent` tools), not a code
    // change. Teaching a model to actually use them is #904's training half.
    std::vector<zetta::agent::AgentToolSpec> enabled_tools;
    for (const zetta::agent::AgentToolSpec& spec : kAgentTools) {
        if (std::find(agent->tools.begin(), agent->tools.end(), spec.name) !=
            agent->tools.end()) {
            enabled_tools.push_back(spec);
        }
    }
    // Pipelines granted to the agent are tools too (lake#238): gate, then run.
    for (const zetta::agent::PipelineGrant& grant : agent->pipelines) {
        enabled_tools.push_back(
            {pipeline_tool_name(grant.graph),
             "run the approved Pipeline '" + grant.graph + "' in project '" + grant.project +
                 "': checks the correctness gate, then runs it: {\"dataset\": \"<database.table>\"}"});
    }
    const zetta::agent::AgentRecord& granted = *agent;
    const zetta::agent::AgentToolRunner agent_tools =
        [&context, &granted](const std::string& tool,
                             const nlohmann::json& arguments) -> zetta::agent::ToolResult {
        if (tool.rfind("pipeline_", 0) == 0) {
            const ApiResult ran = run_granted_pipeline(context, granted, tool, arguments);
            if (!ran.ok) return zetta::agent::ToolResult::failure(ran.error.message);
            return zetta::agent::ToolResult::success(ran.value);
        }
        const bool allowed = std::any_of(
            kAgentTools.begin(), kAgentTools.end(),
            [&tool](const zetta::agent::AgentToolSpec& spec) { return spec.name == tool; });
        if (!allowed) {
            return zetta::agent::ToolResult::failure("'" + tool +
                                                     "' is not a tool this agent may call");
        }
        if (context.registry == nullptr) {
            return zetta::agent::ToolResult::failure("no capability registry for this call");
        }
        const ApiResult called = context.registry->call(
            tool, arguments.is_object() ? arguments : nlohmann::json::object(), context);
        if (!called.ok) return zetta::agent::ToolResult::failure(called.error.message);
        return zetta::agent::ToolResult::success(called.value);
    };

    // Run the agent (loads its model, runs the tool-calling loop) — the shared path the GUI
    // chat panel also uses, so both behave identically. agent.model must be a local
    // checkpoint dir; pull it first with `zetta pull` if it isn't present.
    // Ground the answer in what this Project's pipeline actually produced (#914).
    // The two questions a churn buyer asks — "what's driving churn" and "which are
    // most at risk" — are answerable from the trained model's ranked drivers and its
    // measure, which the run already wrote. Without this the agent answers from
    // training priors and cites nothing real.
    std::string grounding;
    bool grounded = false;
    std::string grounding_warning;
    std::string grounded_project;
    // The caller may name a Project. When it does not, fall back to the one whose
    // run finished most recently (#969): after `agent_run`, "which accounts are
    // most at risk" and "why that one" are about the run that just happened, and
    // making the user restate its name every turn is how a conversation stops
    // being one.
    //
    // This is grounding, not memory. It carries ONE referent — the active
    // Project — onto the retrieval path the model was actually trained on
    // (#914), rather than threading a conversation history the corpus has almost
    // no examples of.
    //
    // Whichever way it resolves, the reply reports `grounded_project`. A silent
    // guess about which Project an answer came from is the provenance failure in
    // #942 wearing a different hat.
    std::string project_hint;
    if (input.contains("project")) {
        project_hint = input.at("project").get<std::string>();
    } else {
        std::string newest_at;
        for (const std::shared_ptr<zetta::pipeline::Run>& run : context.node.runs()) {
            if (!run || run->project().empty()) continue;
            if (run->status() != zetta::pipeline::RunStatus::Succeeded) continue;
            if (newest_at.empty() || run->updated() > newest_at) {
                newest_at = run->updated();
                // find_project_by_id is private to Node; resolve by scanning the
                // public name list, which is small and already how other handlers
                // do it.
                for (const std::string& name : context.node.project_names()) {
                    const auto owner = context.node.find_project(name);
                    if (owner && owner->id() == run->project()) {
                        project_hint = owner->name();
                        break;
                    }
                }
            }
        }
    }
    if (!project_hint.empty()) {
        const std::string project_name = project_hint;
        if (auto project = resolve_project(context, project_name)) {
            std::shared_ptr<zetta::ml::Model> newest;
            for (const auto& candidate : context.node.models()) {
                if (candidate->project() != project->id()) continue;
                if (!newest || candidate->created() > newest->created()) newest = candidate;
            }
            if (newest) {
                grounding = model_grounding_text(newest->metadata(), newest->name(),
                                                 project->name());
                // …and the accounts it actually ranked, so "which are most at risk"
                // is answered with real accounts rather than invented ones (#915).
                grounding += at_risk_grounding_text(context.node, project->graphs());
                grounded = true;
                grounded_project = project->name();
            } else {
                // Deliberately NOT a prompt instruction. Measured behavior: told plainly
                // that a Project has no model and not to estimate, the base agent ignored
                // it and invented a churn rate ("dropped from 42% to 38%"). It pattern-
                // matches a churn answer rather than following an instruction it was never
                // trained on. So the refusal cannot live in the prompt — the caller is told
                // the reply is ungrounded and must present it as such (#914 / #910).
                grounding_warning =
                    "Project \"" + project->name() +
                    "\" has no trained model — its pipeline has not been run, so this reply "
                    "is not grounded in any measured result. Any figure in it is invented.";
            }
        }
    }

    // The conversation so far, supplied by the caller. Without it every turn is a FIRST turn:
    // "list all of those options" has no referent, and three different follow-ups produce the same
    // reply because the model sees three unrelated opening messages (#942).
    //
    // Caller-managed on purpose, and TEMPORARY: once a Conversation is persisted (#945) the thread
    // is read from the node and this parameter goes away. Two sources of history is how they drift.
    std::vector<zetta::agent::PriorTurn> history;
    if (const auto turns = input.find("history"); turns != input.end() && turns->is_array()) {
        for (const nlohmann::json& turn : *turns) {
            if (!turn.is_object()) continue;
            zetta::agent::PriorTurn prior;
            prior.ask = turn.value("ask", std::string());
            prior.reply = turn.value("reply", std::string());
            // Tool outcomes matter: a follow-up like "why did that fail?" refers to what RAN, not
            // to the prose of the previous answer.
            if (const auto tools = turn.find("tools"); tools != turn.end() && tools->is_array()) {
                for (const nlohmann::json& outcome : *tools) {
                    if (!outcome.is_object()) continue;
                    prior.tools.push_back({outcome.value("tool", std::string()),
                                           outcome.value("status", std::string()),
                                           outcome.value("detail", std::string())});
                }
            }
            if (!prior.ask.empty() || !prior.reply.empty()) history.push_back(std::move(prior));
        }
    }

    const zetta::agent::ChatResponse chat =
        enabled_tools.empty()
            ? zetta::agent::respond(*agent, message, max_new_tokens, max_rounds, {}, {},
                                    grounding, history)
            : zetta::agent::respond(*agent, message, max_new_tokens, max_rounds, agent_tools,
                                    enabled_tools, grounding, history);
    if (!chat.ok) return ApiResult::failure("engine_error", chat.error);

    nlohmann::json tool_calls = nlohmann::json::array();
    for (const zetta::agent::ChatToolInvocation& invocation : chat.result.invocations) {
        nlohmann::json entry;
        entry["tool"] = invocation.call.tool;
        entry["status"] = invocation.status == zetta::agent::ApplyStatus::Ok ? "ok"
                          : invocation.status == zetta::agent::ApplyStatus::UnknownTool
                              ? "unknown_tool"
                              : "failed";
        if (!invocation.detail.empty()) entry["detail"] = invocation.detail;
        tool_calls.push_back(std::move(entry));
    }
    nlohmann::json output;
    output["reply"] = chat.result.reply;
    output["tool_calls"] = std::move(tool_calls);
    output["rounds"] = chat.result.rounds;
    // Whether the reply stands on measured results, so a caller never presents a
    // generated number as a fact (#914). `grounded` is true only when a Project was
    // named AND it has a trained model whose real measure + drivers were supplied.
    output["grounded"] = grounded;
    // Always say WHICH Project the answer was grounded in. When the caller did not
    // name one, this is the only way to see that the engine chose the most
    // recently-run Project rather than the one you had in mind (#969).
    if (!grounded_project.empty()) output["grounded_project"] = grounded_project;
    if (!grounding_warning.empty()) output["grounding_warning"] = grounding_warning;
    // The mode the agent DECLARED for this turn (#918), so a caller switches on an explicit
    // value instead of inferring intent from the payload's shape. `answer` renders as prose,
    // `clarify` is a question awaiting a reply, `abstain` is a refusal the surface must present
    // as such rather than dressing up, and `action` carries a proposed Pipeline to review.
    if (chat.action) {
        output["kind"] = std::string(zetta::agent::to_string(chat.action->kind));
        // A proposed Pipeline is returned only when one was actually built — never an empty
        // graph, which a caller could mistake for "the agent built nothing on purpose".
        if (!chat.action->execution_graph.nodes().empty()) {
            output["pipeline"] = zetta::ir::to_json(chat.action->execution_graph);
            // How many steps the harness actually built by running the model's tool calls, so a
            // caller can tell a real Pipeline from an empty gesture without walking the graph.
            output["pipeline_steps"] =
                static_cast<int>(chat.action->execution_graph.nodes().size());
        }
        // An `action` turn asks to build something, and a chat may not change the node
        // (the read-only tool allowlist above). So the turn PROPOSES the run instead
        // (#969): propose_run records an impact preview and creates nothing else, and a
        // person approves it with approve_run_proposal. It goes through the registry with
        // the caller's own context, so a caller who may not propose runs gets a reply
        // without one — and the audit trail records the proposal under the caller.
        if (chat.action->kind == zetta::agent::InteractionKind::Action) {
            nlohmann::json propose = {{"message", message}, {"agent", agent_id}};
            for (const char* key : {"dataset_path", "holdout_fraction"}) {
                if (const auto found = input.find(key); found != input.end()) propose[key] = *found;
            }
            const ApiResult proposed =
                context.registry == nullptr
                    ? ApiResult::failure("engine_error", "no capability registry for this call")
                    : context.registry->call("propose_run", propose, context);
            if (proposed.ok) {
                output["proposal"] = proposed.value;
            } else {
                output["proposal_warning"] =
                    "this turn proposes a run, but none was recorded: " + proposed.error.message;
            }
        }
    } else {
        // The generation did not parse as a response envelope. Say so explicitly: an unparsed
        // turn is an error state, not an `action` that happened to carry no pipeline.
        output["kind"] = "unparsed";

        // And do NOT hand the raw generation back as the reply (#979). It was reaching
        // the Studio chat window verbatim: asked "hello", a user saw 636 characters of
        // `{"kind":"answer","message":…,"tool":"serve"}`.
        //
        // A model that emits garbage is a training problem. A harness that SHOWS the
        // garbage to a customer is a serving problem, and it is the one that has to be
        // right, because it bounds the blast radius of every future parse failure —
        // including ones nobody has seen yet.
        //
        // The payload is not discarded: it moves to `raw`, which a developer reads and a
        // chat window does not render.
        output["raw"] = output.value("reply", std::string{});
        output["reply"] =
            "I could not produce a well-formed answer to that. Nothing was built and "
            "nothing was changed. Try rephrasing, or ask something more specific.";
    }
    return ApiResult::success(std::move(output));
}

// --- agent_run: the base agent builds a plan, and runs it once it is approved ----------
//
// The point of #872: the base (#555) agent is usable RIGHT AWAY — it does not need the
// RLVR tool-calling fine-tune (#859) to drive real work. From plain language:
//   1. the agent reads the message and recognizes the use case (chat/respond),
//   2. the engine compiles that message into a real ExecutionGraph (pipeline::compile_intent
//      — the same churn-shape heuristic behind compile_intent), stored on a project
//      **proposed**, and nothing runs (#913),
//   3. once a principal holding orchestration:approve_plan approves it (approve_plan),
//      agent_run with that project + graph_name runs it through the correctness gate +
//      the real engine (delegated to handle_run).
// agent_run never approves a plan itself. It used to record the caller's approval of
// the plan it had just built, so a caller allowed only orchestration:run trained a model
// and fired an `act` step on a Pipeline no human had reviewed (#913).
const nlohmann::json& agent_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"agent", "message"}},
        {"additionalProperties", false},
        {"properties",
         {{"agent", {{"type", "string"}, {"minLength", 1}, {"description", "the agent's id"}}},
          {"message",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the plain-language ask, e.g. 'predict which customers will churn'"}}},
          {"project",
           {{"type", "string"},
            {"description",
             "land the graph + run on this project (name or uuid); a new project is "
             "created when omitted"}}},
          {"graph_name",
           {{"type", "string"},
            {"description",
             "name for the stored graph (default derived). With `project`, naming a graph "
             "already stored there runs that plan if it is approved, and is refused if it "
             "is only proposed"}}},
          {"max_new_tokens", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2048}}},
          {"max_rounds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8}}},
          {"top_k",
           {{"type", "integer"}, {"minimum", 0}, {"description", "at-risk list cap (0 = all)"}}},
          {"dataset_path",
           {{"type", "string"},
            {"description",
             "`database.table` (a connection/query landed it) or a churn dataset dir "
             "on the node (default: the built-in demo)"}}},
          {"dataset", zetta::data::dataset_contract_schema()},
          {"model_name", {{"type", "string"}, {"description", "registered model name"}}}}}};
    return schema;
}

const nlohmann::json& agent_run_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"reply", "shape", "project", "graph", "steps", "run"}},
        {"properties",
         {{"reply", {{"type", "string"}, {"description", "the agent's recognition of the ask"}}},
          {"shape",
           {{"type", "string"},
            {"description",
             "the compiled template: 'churn' | 'generic', or 'approved' when an approved "
             "plan already stored on the project ran"}}},
          {"project", {{"type", "string"}}},
          {"graph", {{"type", "string"}}},
          {"graph_id", {{"type", "string"}}},
          {"steps", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"run",
           {{"type", "object"},
            {"description", "the run result (run_id + engine + status), poll get_run"}}}}}};
    return schema;
}

ApiResult handle_agent_run(const nlohmann::json& input, ApiContext& context) {
    if (context.node.replication() == nullptr) {
        return ApiResult::failure("not_found", "no agents on a standalone node");
    }
    const std::string agent_id = input.at("agent").get<std::string>();
    const std::string message = input.at("message").get<std::string>();

    zetta::agent::AgentStore store(*context.node.replication());
    const std::optional<zetta::agent::AgentRecord> agent = store.get(agent_id);
    if (!agent) return ApiResult::failure("not_found", "no agent '" + agent_id + "'");
    if (!agent->native()) {
        return ApiResult::failure("invalid_argument",
                                  "agent '" + agent->name + "' runs on " +
                                      (agent->runtime == "python" ? std::string("agent-runtime")
                                                                  : std::string("its own service")) +
                                      ", outside Zetta; use it from lake");
    }

    // Run the stored plan the caller named, only if a principal approved it (#913).
    // This is checked before the model loads: whether a plan may run does not depend
    // on what the agent says about the ask.
    const auto named_project = input.find("project");
    const auto named_graph = input.find("graph_name");
    if (named_project != input.end() && named_graph != input.end()) {
        const std::shared_ptr<zetta::pipeline::Project> project =
            resolve_project(context, named_project->get<std::string>());
        if (!project) {
            return ApiResult::failure("not_found", "no project '" +
                                                       named_project->get<std::string>() +
                                                       "' (or with that id)");
        }
        if (const std::shared_ptr<zetta::pipeline::ProjectGraph> existing =
                resolve_graph(*project, named_graph->get<std::string>())) {
            if (!existing->approved()) {
                return ApiResult::failure(
                    "not_approved",
                    "the plan '" + existing->name() + "' on project '" + project->name() +
                        "' is proposed and nobody has approved it, so agent_run will not "
                        "run it. Approve it (approve_plan), then call agent_run again");
            }
            nlohmann::json run_input;
            run_input["project"] = project->name();
            run_input["graph"] = existing->name();
            for (const char* passthrough :
                 {"top_k", "dataset_path", "dataset", "model_name", "coordinate"}) {
                if (const auto found = input.find(passthrough); found != input.end()) {
                    run_input[passthrough] = *found;
                }
            }
            const ApiResult run_result = handle_run(run_input, context);
            if (!run_result.ok) return run_result;
            nlohmann::json output;
            output["reply"] = "Running the plan '" + existing->name() + "', approved by " +
                              existing->approved_by() + " at " + existing->approved_at() + ".";
            output["shape"] = "approved";
            output["project"] = project->name();
            output["graph"] = existing->name();
            output["graph_id"] = existing->id();
            nlohmann::json steps = nlohmann::json::array();
            for (const std::shared_ptr<zetta::ir::Node>& node : existing->graph().nodes()) {
                steps.push_back(node->name);
            }
            output["steps"] = std::move(steps);
            output["run"] = run_result.value;
            return ApiResult::success(std::move(output));
        }
    }

    // 1. The agent reads the ask and recognizes the use case (the shared respond() path).
    const zetta::agent::ChatResponse chat = zetta::agent::respond(
        *agent, message, input.value("max_new_tokens", 256), input.value("max_rounds", 3));
    if (!chat.ok) return ApiResult::failure("engine_error", chat.error);

    // 2. The engine compiles the same ask into a real, editable ExecutionGraph.
    std::string reason;
    const std::optional<zetta::pipeline::CompiledIntent> compiled =
        zetta::pipeline::compile_intent(message, &reason);
    if (!compiled) {
        return ApiResult::failure("engine_error",
                                  "the agent could not compile a graph from the ask: " + reason);
    }
    const std::string shape = std::string(zetta::pipeline::to_string(compiled->shape));
    std::vector<std::string> steps;
    for (const std::shared_ptr<zetta::ir::Node>& node : compiled->graph.nodes()) {
        steps.push_back(node->name);
    }

    // Land the graph on a project (the caller's, or a fresh one so a chat needs no setup).
    std::shared_ptr<zetta::pipeline::Project> project;
    if (const auto named = input.find("project"); named != input.end()) {
        project = resolve_project(context, named->get<std::string>());
        if (!project) {
            return ApiResult::failure(
                "not_found", "no project '" + named->get<std::string>() + "' (or with that id)");
        }
    } else {
        const std::string owner =
            context.acting_key.empty() ? std::string("local") : context.acting_key;
        project = context.node.create_project(
            "agent " + shape + " · " + zetta::util::new_uuid().substr(0, 8), owner);
        if (!project) {
            return ApiResult::failure("engine_error", "could not create a project for the run");
        }
    }

    const std::string graph_name =
        input.value("graph_name", agent->name.empty() ? shape + " graph"
                                                       : agent->name + " · " + shape + " graph");
    const std::shared_ptr<zetta::pipeline::ProjectGraph> stored =
        project->create_graph(graph_name, std::move(compiled->graph));
    if (!stored) {
        return ApiResult::failure(
            "engine_error", "could not store the graph '" + graph_name + "' on project '" +
                                project->name() + "' (name clash or empty)");
    }

    // The plan is stored proposed and nothing runs (#913). Approval is a separate
    // decision by a principal who holds orchestration:approve_plan, recorded by
    // approve_plan; agent_run holds only orchestration:run and never approves.
    return ApiResult::failure(
        "not_approved",
        "agent_run built the plan '" + stored->name() + "' on project '" + project->name() +
            "' (" + std::to_string(steps.size()) + " steps, shape " + shape +
            ") and stored it proposed; nothing ran. Approve it (approve_plan), then call "
            "agent_run with project '" + project->name() + "' and graph_name '" +
            stored->name() + "' to run it");
}

// --- dataset generation (#873): native, in-Zetta training-corpus synthesis ------------------

const nlohmann::json& list_dataset_generators_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_dataset_generators_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"generators",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"properties",
               {{"kind", {{"type", "string"}}},
                {"title", {{"type", "string"}}},
                {"description", {{"type", "string"}}}}}}}}}}}};
    return schema;
}

ApiResult handle_list_dataset_generators(const nlohmann::json&, ApiContext&) {
    nlohmann::json generators = nlohmann::json::array();
    for (const std::shared_ptr<const zetta::dataset::DatasetGenerator>& generator :
         zetta::dataset::DatasetGeneratorFactory::builtin().generators()) {
        generators.push_back({{"kind", generator->kind()},
                              {"title", generator->title()},
                              {"description", generator->description()}});
    }
    return ApiResult::success(nlohmann::json{{"generators", std::move(generators)}});
}

const nlohmann::json& generate_dataset_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"kind"}},
        {"additionalProperties", false},
        {"properties",
         {{"kind",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "a generator kind from list_dataset_generators (e.g. 'base-agent')"}}},
          {"variants_per_subject",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", zetta::dataset::kMaxVariantsPerSubject},
            {"description", "task variants per plan subject"}}},
          {"held_out_fraction",
           {{"type", "number"}, {"minimum", 0}, {"maximum", 1}, {"description", "canary fraction"}}},
          {"path",
           {{"type", "string"},
            {"description", "write the gated JSONL corpus to this file on the node (optional)"}}}}}};
    return schema;
}

const nlohmann::json& generate_dataset_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"properties",
         {{"kind", {{"type", "string"}}},
          {"cases", {{"type", "integer"}}},
          {"subjects", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"held_out", {{"type", "integer"}}},
          {"admitted", {{"type", "boolean"}}},
          {"path", {{"type", "string"}}}}}};
    return schema;
}

ApiResult handle_generate_dataset(const nlohmann::json& input, ApiContext&) {
    const std::string kind = input.at("kind").get<std::string>();
    const std::shared_ptr<const zetta::dataset::DatasetGenerator> generator =
        zetta::dataset::DatasetGeneratorFactory::builtin().create(kind);
    if (!generator) {
        return ApiResult::failure("not_found", "no dataset generator '" + kind + "'");
    }
    zetta::dataset::DatasetGenSpec spec;
    // Read wide and clamp before narrowing, so a huge count cannot wrap into range on the way to
    // int; anything over the ceiling stays over it and the generator refuses it (#1398).
    const std::int64_t variants = input.value("variants_per_subject", std::int64_t{1});
    spec.variants_per_subject = static_cast<int>(std::clamp<std::int64_t>(
        variants, 0, std::int64_t{zetta::dataset::kMaxVariantsPerSubject} + 1));
    spec.held_out_fraction = input.value("held_out_fraction", 0.0);
    const zetta::dataset::GeneratedDataset dataset = generator->generate(spec);
    // The generator refuses a spec over its work ceilings before expanding it (#1398).
    if (!dataset.error.empty()) {
        return ApiResult::failure("invalid_input", "/variants_per_subject: " + dataset.error,
                                  "/variants_per_subject");
    }

    // Gate it (blender-lr discipline) — the generator builds gated cases, but the corpus is only
    // trusted once admit_corpus confirms it (reference passes, controls fail).
    const zetta::agent::AdmissionResult gate = zetta::agent::admit_corpus(dataset.cases);

    nlohmann::json output;
    output["kind"] = kind;
    output["cases"] = dataset.cases.size();
    output["subjects"] = dataset.subjects;
    output["held_out"] = dataset.held_out;
    output["admitted"] = gate.admitted;
    if (!gate.admitted && !gate.failures.empty()) {
        output["gate_failure"] = std::string(zetta::agent::to_string(gate.failures.front().kind)) +
                                 " (task '" + gate.failures.front().task_id + "')";
    }
    if (const auto path = input.find("path"); path != input.end()) {
        std::ofstream file(path->get<std::string>());
        if (!file) {
            return ApiResult::failure(
                "engine_error", "could not open '" + path->get<std::string>() + "' for writing");
        }
        file << zetta::agent::dump_corpus_jsonl(dataset.cases);
        output["path"] = *path;
    }
    return ApiResult::success(std::move(output));
}

// --- experiments ------------------------------------------------------------

const nlohmann::json& create_experiment_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json value = zetta::solutions::experiment_value_schema();
        value["title"] = "Create experiment input";
        value["required"] = {"name", "seed", "allocations"};
        value["properties"].erase("id");
        value["properties"].erase("created");
        return value;
    }();
    return schema;
}

const nlohmann::json& experiment_record_schema() {
    static const nlohmann::json schema = entity_value_with_id_schema(
        zetta::solutions::experiment_value_schema());
    return schema;
}

const nlohmann::json& assign_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"experiment", "entity"}},
        {"additionalProperties", false},
        {"properties",
         {{"experiment",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "experiment uuid or unique name"}}},
          {"entity", {{"type", "string"}, {"minLength", 1}}},
          {"proposed_action",
           {{"type", "string"},
            {"description", "action to return only when the assignment permits it"}}},
          {"model_driven", {{"type", "boolean"}, {"default", true}}},
          {"exempt", {{"type", "boolean"}, {"default", false}}},
          {"exemption_reason", {{"type", "string"}, {"minLength", 1}}}}}};
    return schema;
}

const nlohmann::json& assignment_record_schema() {
    static const nlohmann::json schema = entity_value_with_id_schema(
        zetta::solutions::experiment_assignment_value_schema());
    return schema;
}

const nlohmann::json& assign_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"assignment", "proposed_action", "action", "model_driven", "allowed",
          "reason"}},
        {"additionalProperties", false},
        {"properties",
         {{"assignment", assignment_record_schema()},
          {"proposed_action", {{"type", "string"}}},
          {"action", {{"type", "string"}}},
          {"model_driven", {{"type", "boolean"}}},
          {"allowed", {{"type", "boolean"}}},
          {"reason", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& get_experiment_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"experiment"}},
        {"additionalProperties", false},
        {"properties",
         {{"experiment",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "experiment uuid or unique name"}}}}}};
    return schema;
}

const nlohmann::json& get_experiment_output_schema() {
    static const nlohmann::json balance_row = {
        {"type", "object"},
        {"required",
         {"name", "tier", "configured_fraction", "assignments", "expected",
          "deviation", "tolerance", "balanced"}},
        {"additionalProperties", false},
        {"properties",
         {{"name", {{"type", "string"}}},
          {"tier", {{"type", "string"}}},
          {"configured_fraction", {{"type", "number"}}},
          {"assignments", {{"type", "integer"}}},
          {"expected", {{"type", "number"}}},
          {"deviation", {{"type", "number"}}},
          {"tolerance", {{"type", "number"}}},
          {"balanced", {{"type", "boolean"}}}}}};
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"experiment", "sizes", "balance", "erosion", "training_rows"}},
        {"additionalProperties", false},
        {"properties",
         {{"experiment", experiment_record_schema()},
          {"sizes",
           {{"type", "object"},
            {"required",
             {"assignments", "exemptions", "holdout_assignments",
              "holdout_exemptions"}},
            {"additionalProperties", false},
            {"properties",
             {{"assignments", {{"type", "integer"}}},
              {"exemptions", {{"type", "integer"}}},
              {"holdout_assignments", {{"type", "integer"}}},
              {"holdout_exemptions", {{"type", "integer"}}}}}}},
          {"balance",
           {{"type", "object"},
            {"required", {"balanced", "allocations"}},
            {"additionalProperties", false},
            {"properties",
             {{"balanced", {{"type", "boolean"}}},
              {"allocations", {{"type", "array"}, {"items", balance_row}}}}}}},
          {"erosion",
           {{"type", "object"},
            {"required", {"exemption_rate", "holdout_erosion_rate"}},
            {"additionalProperties", false},
            {"properties",
             {{"exemption_rate", {{"type", "number"}}},
              {"holdout_erosion_rate", {{"type", "number"}}}}}}},
          {"training_rows",
           {{"type", "array"}, {"items", assignment_record_schema()}}}}}};
    return schema;
}

ApiResult handle_create_experiment(const nlohmann::json& input,
                                   ApiContext& context) {
    zetta::solutions::ExperimentValue value;
    value.name = input.at("name").get<std::string>();
    value.seed = input.at("seed").get<std::uint64_t>();
    for (const nlohmann::json& item : input.at("allocations")) {
        value.allocations.push_back(
            {item.at("name").get<std::string>(),
             *zetta::solutions::experiment_tier_from(
                 item.at("tier").get<std::string>()),
             item.at("fraction").get<double>()});
    }
    std::string reason;
    const auto record = context.node.experiment_manager().create(
        std::move(value), &reason);
    if (!record) {
        return ApiResult::failure("invalid_input", reason);
    }
    return ApiResult::success(zetta::solutions::to_json(*record));
}

ApiResult handle_assign(const nlohmann::json& input, ApiContext& context) {
    const bool exempt = input.value("exempt", false);
    const std::string actor =
        exempt ? (context.acting_key.empty() ? std::string("local")
                                            : context.acting_key)
               : std::string{};
    std::string reason;
    const auto decision = context.node.experiment_manager().decide(
        input.at("experiment").get<std::string>(),
        input.at("entity").get<std::string>(),
        input.value("proposed_action", std::string{}),
        input.value("model_driven", true), exempt,
        input.value("exemption_reason", std::string{}), actor, &reason);
    if (!decision) {
        return ApiResult::failure("invalid_input", reason);
    }
    return ApiResult::success(zetta::solutions::to_json(*decision));
}

ApiResult handle_get_experiment(const nlohmann::json& input,
                                ApiContext& context) {
    std::string reason;
    const auto report = context.node.experiment_manager().report(
        input.at("experiment").get<std::string>(), &reason);
    if (!report) {
        return ApiResult::failure("not_found", reason);
    }
    return ApiResult::success(zetta::solutions::to_json(*report));
}

nlohmann::json purge_audit_schema() {
    return nlohmann::json{
        {"type", "object"},
        {"required", {"before"}},
        {"additionalProperties", false},
        {"properties",
         {{"before",
           {{"type", "string"},
            {"description", "UTC cutoff; records strictly before it are removed."}}}}}};
}

nlohmann::json purge_audit_output_schema() {
    return nlohmann::json{
        {"type", "object"},
        {"required", {"before", "removed"}},
        {"additionalProperties", false},
        {"properties",
         {{"before", {{"type", "string"}}},
          {"removed", {{"type", "integer"}, {"minimum", 0}}}}}};
}

ApiResult handle_purge_audit(const nlohmann::json& input,
                             ApiContext& context) {
    const std::string before = input.at("before").get<std::string>();
    std::string error;
    const auto removed = context.node.purge_audit_before(
        before, context.acting_key, &error);
    if (!removed) {
        return ApiResult::failure("invalid_input", error, "/before");
    }
    return ApiResult::success(
        {{"before", before}, {"removed", *removed}});
}

}  // namespace

void register_core_capabilities(Registry& registry) {
    Capability purge_audit{
        "purge_audit",
        "Purge audit records",
        "Remove audit records strictly before a UTC cutoff, advance replicated "
        "low-water marks, and append an audit_purged record describing the action.",
        purge_audit_schema(),
        purge_audit_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "purge_audit"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_purge_audit};
    purge_audit.audit_policy = AuditPolicy::HandlerRecordsSuccess;
    purge_audit.blocks = true;
    registry.add(std::move(purge_audit));

    registry.add(Capability{
        "versions",
        "Show version",
        "Read the software version and the version of every agent this node "
        "serves, including which one answers in Studio. No side effects.",
        versions_schema(),
        versions_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_versions});
    registry.add(Capability{
        "server_info",
        "Server info",
        "Read this node's identity, version, entity counts, and cluster "
        "membership. No side effects.",
        server_info_schema(),
        server_info_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_server_info});
    registry.add(Capability{
        "node_memory",
        "Node memory",
        "Read process RSS, allocator truth, and per-subsystem memory ownership. "
        "No side effects.",
        node_memory_schema(),
        node_memory_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_node_memory});

    registry.add(Capability{
        "create_experiment",
        "Create an experiment",
        "Create and replicate a seeded holdout experiment definition. Mutates "
        "experiment state and returns the durable record.",
        create_experiment_schema(),
        experiment_record_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_experiment"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_experiment});
    registry.add(Capability{
        "assign",
        "Assign an experiment entity",
        "Assign an entity deterministically before any proposed action, suppress "
        "model-driven actions in protected tiers, and durably log explicit exemptions. "
        "Mutates assignment state.",
        assign_schema(),
        assign_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "assign"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_assign});
    registry.add(Capability{
        "get_experiment",
        "Get an experiment",
        "Read an experiment's allocation sizes, balance checks, exemption erosion, "
        "and uplift-training assignment rows. No side effects.",
        get_experiment_schema(),
        get_experiment_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_experiment});

    Capability sql{
        "sql",
        "Run SQL",
        "Run one governed SQL statement (Postgres subset). Every system.* "
        "observability view is readable here. Each statement is authorized on "
        "its own: SELECT needs sql:read; INSERT, UPDATE, DELETE and TRUNCATE "
        "need sql:write; CREATE/DROP DATABASE and TABLE need sql:ddl (each at "
        "table or database scope); CREATE USER needs users:manage, CREATE TOKEN "
        "tokens:manage, role and policy statements roles:manage. Every "
        "data-changing statement is audited as a data_changed action.",
        sql_schema(),
        sql_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_sql};
    // A statement of any length travels as a JSON body (#1231): the GET form
    // carries it in the request line, which the parser caps near 4 KiB.
    sql.post_alias = true;
    registry.add(std::move(sql));

    registry.add(Capability{
        "search",
        "Search",
        "Search a registered index by free text — distributed scatter-gather "
        "across the cluster when the table is partitioned — returning ranked "
        "hits with per-node provenance and a trace id. No side effects.",
        search_schema(),
        search_response_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_search});

    registry.add(Capability{
        "list_projects",
        "List projects",
        "Read the names of every AI project on this node, with a summary of "
        "each (id, name, and each graph's id, name and approval) for a caller "
        "who may also call get_project. No side effects.",
        list_projects_schema(),
        list_projects_output_schema(),
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
        list_traces_output_schema(),
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
        get_trace_output_schema(),
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
        get_project_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_project});

    registry.add(Capability{
        "get_graph",
        "Get one graph",
        "Read one execution graph's full structure — its typed step nodes "
        "{id, kind, name} and directed edges {from, to, kind} — by project + "
        "graph name or uuid. No side effects.",
        graph_ref_schema(),
        graph_structure_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_graph});

    registry.add(Capability{
        "save_graph",
        "Save a graph",
        "Persist an edited execution graph — its nodes + edges — back to a "
        "project graph, creating it when absent or replacing the existing one "
        "(replicated + audited, the visual editor's save path).",
        save_graph_schema(),
        project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "save_graph"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_save_graph});

    registry.add(Capability{
        "approve_plan",
        "Approve a proposed plan",
        "Approve a stored, proposed Pipeline for build (#885, the design's "
        "review-before-build flow): records the approving principal + time, "
        "replicates, and audits it (graph_approved). gate and deploy require an "
        "approved plan; editing a plan re-proposes it.",
        approve_plan_schema(),
        approve_plan_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "approve_plan"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_approve_plan});

    registry.add(Capability{
        "get_run",
        "Get one run",
        "Read one run's full validated record — status, logs, outputs, "
        "lineage — by its uuid. No side effects.",
        get_run_schema(),
        get_run_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_run});

    registry.add(Capability{
        "get_run_report", "Get a run's report facts",
        "Read the facts a finished run produced — how good the model is "
        "(held-out "
        "first, in-sample beside it), what drives the outcome, and the ranked "
        "at-risk list with its evidence. M-4 returns forecast backtests, seasonal-naive comparison, "
        "read/excluded data and ordered forecasts. M-7 returns anomaly metrics, ranked "
        "anomalies, output tables, and what was read or excluded. Assembled at query time from the "
        "run's "
        "model and its result tables, so it cannot drift from them. Anything a "
        "report would want and this run cannot supply is listed in `missing` "
        "with "
        "the reason, rather than left blank. For M-3 segmentation, returns K "
        "selection, "
        "separation and stability metrics, input/exclusion facts and "
        "assignment/profile "
        "tables instead of supervised risk metrics. No side effects. (#958, "
        "#1042, #1043, #1046)",
        get_run_report_schema(), get_run_report_output_schema(),
        CapabilityKind::Read, Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false, handle_get_run_report});

    registry.add(Capability{
        "propose_improvement",
        "Propose an improvement",
        "Propose a change to a deployed model and state its PREDICTED EFFECT, or refuse and say "
        "why the effect cannot be predicted. No side effects - it proposes, it does not act.",
        propose_improvement_schema(),
        propose_improvement_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_propose_improvement});
    registry.add(Capability{
        "model_health",
        "Model health",
        "Read whether a deployed model can still be trusted: the held-out figure, the "
        "overfitting gap against in-sample, whether scores are calibrated, and what each "
        "means. No side effects.",
        model_health_schema(),
        model_health_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_model_health});
    registry.add(Capability{
        "get_model",
        "Get one model",
        "Read one model's full validated record — kind, lineage, feature "
        "schema, metrics, weights fingerprint — by its uuid. No side effects.",
        get_model_schema(),
        get_model_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_model});

    registry.add(Capability{
        "get_monitor",
        "Get one solution monitor",
        "Read one solution's current input and score drift, calibration, "
        "closed-window cohort performance, guardrails, active breaches, and "
        "retrain request by solution name or monitor uuid. No side effects.",
        get_monitor_schema(),
        get_monitor_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_monitor});

    registry.add(Capability{
        "get_roi_readout",
        "Get a solution ROI readout",
        "Read one solution's versioned correctness certificate, aggregate "
        "incrementality with confidence intervals, customer-supplied ROI, "
        "guardrails, agent operations, and audit extract. No side effects.",
        get_roi_readout_schema(),
        zetta::solutions::roi_readout_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_roi_readout});

    registry.add(Capability{
        "get_workspace_profile",
        "Get one workspace profile",
        "Read one Workspace Profile's full validated record (owner, role, daily "
        "work, approved source refs) by its uuid. No side effects. (#886)",
        get_workspace_profile_schema(),
        workspace_profile_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_workspace_profile});

    registry.add(Capability{
        "create_workspace_profile",
        "Create a workspace profile",
        "Create the calling user's Workspace Profile — the input to the Discover "
        "Opportunities pathway (#886). Owner is the caller; replicates to every "
        "node. Role, daily work, and approved source refs are the editable body.",
        create_workspace_profile_schema(),
        workspace_profile_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_workspace_profile"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_workspace_profile});

    registry.add(Capability{
        "update_workspace_profile",
        "Update a workspace profile",
        "Update a Workspace Profile's role, daily work, or approved source refs "
        "by its uuid (#886); the owner and created stamp stay fixed. Replicates.",
        update_workspace_profile_schema(),
        workspace_profile_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_workspace_profile"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_update_workspace_profile});

    registry.add(Capability{
        "get_plan",
        "Get one plan",
        "Read one Plan's full validated record (name, tier, quota limits) by its "
        "uuid. Metered usage is read live from system.usage. (Conflict D)",
        get_plan_schema(),
        plan_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_plan});

    registry.add(Capability{
        "create_plan",
        "Create a plan",
        "Create a Plan — the quota/tier config the Billing/Usage surface measures "
        "live consumption against (Conflict D). Replicates to every node.",
        create_plan_schema(),
        plan_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_plan"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_plan});

    registry.add(Capability{
        "update_plan",
        "Update a plan",
        "Update a Plan's name, tier, quotas, or period by its uuid (Conflict D); "
        "the created stamp stays fixed. Replicates.",
        update_plan_schema(),
        plan_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_plan"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_update_plan});

    registry.add(Capability{
        "get_job",
        "Get one job",
        "Read one compute job's full validated record — kind, status, "
        "lineage refs, detail — by its uuid. No side effects.",
        get_job_schema(),
        get_job_output_schema(),
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
        get_index_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_index});

    registry.add(Capability{
        "get_dataset",
        "Get one dataset",
        "Read one dataset's computed summary — its columns in the source "
        "file's order with each one's type and null share, row count, size, "
        "format, replication, partition placement — by name (plus database "
        "when ambiguous). No side effects.",
        get_dataset_schema(),
        get_dataset_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_dataset});

    registry.add(Capability{
        "list_runs",
        "List runs",
        "Read every run with Project/Pipeline display names, status, stored "
        "metric summary, stamps and log/output counts. No side effects.",
        list_runs_schema(),
        list_runs_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_runs});

    registry.add(Capability{
        "list_models",
        "List models",
        "Read every model version with Project/Pipeline display names, producing "
        "Run, metrics, feature schema, serving state and weights presence. No side effects.",
        list_models_schema(),
        list_models_output_schema(),
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
        list_jobs_output_schema(),
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
        list_indexes_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_indexes});

    registry.add(Capability{
        "list_datasets",
        "List datasets",
        "Read the computed summary of every dataset — schema, row count, "
        "size, format, replication, partition placement. With detail "
        "\"summary\", only the fields a dataset list shows, from counters, "
        "reading no row and no file. No side effects.",
        list_datasets_schema(),
        list_datasets_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_datasets});

    registry.add(Capability{
        "list_methodologies", "List methodologies",
        "Read the complete methodology portfolio, including model kinds, "
        "primary solutions, data roles, outputs, metrics, implementation "
        "status, and tracking issues. No side effects.",
        list_methodologies_schema(), list_methodologies_output_schema(),
        CapabilityKind::Read, Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false, handle_list_methodologies});

    // --- workspace methodologies (zetta#1261, lake#655) ------------------------
    registry.add(Capability{
        "get_methodology", "Get a methodology",
        "Read one methodology: Orama's catalogue entry (M-n, read-only) or a workspace "
        "methodology (W-<8 hex>) with its interface, where its Python code lives, its image "
        "and its validation. No side effects.",
        methodology_id_schema(), methodology_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false, handle_get_methodology});
    registry.add(Capability{
        "register_methodology", "Register a workspace methodology",
        "Register a workspace methodology: Python code in one of the workspace's repositories "
        "that Lake's sandboxed runner runs (the engine never runs it). Declares its input "
        "kinds, target, required features, parameters, metrics and code location; returns the "
        "entry with its generated W- id, not validated. Replicates to every node.",
        register_methodology_schema(), methodology_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "register_methodology"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_register_methodology});
    registry.add(Capability{
        "update_methodology", "Update a workspace methodology",
        "Change a workspace methodology's fields, its image or its validation. A new code "
        "revision resets validation unless the same call reports it. Orama's catalogue "
        "methodologies are read-only: changing one answers forbidden.",
        update_methodology_schema(), methodology_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_methodology"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_update_methodology});
    registry.add(Capability{
        "delete_methodology", "Delete a workspace methodology",
        "Remove a workspace methodology by id (a CRDT tombstone; replicates). Idempotent. "
        "Orama's catalogue methodologies are read-only: deleting one answers forbidden.",
        methodology_id_schema(), delete_methodology_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_methodology"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_delete_methodology});
    registry.add(Capability{
        "record_methodology_run", "Record a methodology run",
        "Record what Lake's runner produced for a workspace methodology. A training run "
        "registers a Model (kind python:<id>) with its in-sample and held-out metrics and the "
        "artifact as its weights, and a Run with its parameters, metrics and lineage (on the "
        "pipeline's project and graph for a pipeline step). A validation run registers "
        "nothing.",
        record_methodology_run_schema(), record_methodology_run_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "record_methodology_run"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_record_methodology_run});
    registry.add(Capability{
        "append_methodology_artifact", "Stage part of a methodology run's artifact",
        "Send a workspace methodology run's artifact in chunks of at most 8 MiB, in order, "
        "when it is larger than record_methodology_run takes inline (24 MiB), up to 1 GiB. "
        "Answers the bytes staged; a chunk at an offset below that replaces what was held from "
        "there. record_methodology_run with artifact_staged then adopts it as the Model's "
        "weights.",
        append_methodology_artifact_schema(), append_methodology_artifact_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "append_methodology_artifact"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_append_methodology_artifact});
    registry.add(Capability{
        "read_model_weights", "Read a model's weights",
        "Read a model's serialized weights, at most 8 MiB at a time from offset, with their "
        "total size and SHA-256. Lake serves a workspace methodology's model from them.",
        read_model_weights_schema(), read_model_weights_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false, handle_read_model_weights});

    registry.add(Capability{
        "list_ledger",
        "List the solutions audit ledger",
        "Read the immutable score, decision, action and outcome reward stream in "
        "chronological order; no side effects.",
        list_ledger_schema(),
        list_ledger_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_ledger});

    registry.add(Capability{
        "submit_feedback",
        "Submit feedback",
        "Snapshot a feedback bundle — your description plus the relevant "
        "traces, recent errors, crash reports, and versions — stored as a "
        "large object you explicitly export; nothing leaves the cluster on "
        "its own.",
        submit_feedback_schema(),
        submit_feedback_output_schema(),
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
        create_project_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_project"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_project});

    registry.add(Capability{
        "record_action",
        "Record a decision and action",
        "Append one immutable score, decision and action record, including "
        "deliberate no-contact, to the replicated solutions reward ledger.",
        record_action_schema(),
        record_ledger_entry_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "record_action"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_record_action});

    registry.add(Capability{
        "record_outcome",
        "Record an action outcome",
        "Append one immutable connector or manual outcome referencing an existing "
        "action in the replicated solutions reward ledger.",
        record_outcome_schema(),
        record_ledger_entry_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "record_outcome"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_record_outcome});

    registry.add(Capability{
        "delete_project",
        "Delete project",
        "Delete a project with its captured inputs and its graphs, on every node. "
        "Its runs and models are KEPT - they stay listed in list_runs / list_models, "
        "still labelled with the deleted project's id - and the answer counts them. "
        "Refused while one of its runs is pending or running. Not undoable. (#1009)",
        delete_project_schema(),
        delete_project_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_project"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_project});
    // lake#645: what a workspace accumulates can be removed, not only created.
    registry.add(Capability{
        "delete_dataset",
        "Delete dataset",
        "Delete a dataset (its table) on every node, with its partitions and their "
        "bodies and every index built over it, so nothing of it is left. "
        "dry_run: true answers what would go and changes nothing. Refused "
        "(engine_error, naming them) while a pending or running run or a running "
        "study reads it, its dry run too. Not undoable.",
        delete_dataset_schema(),
        delete_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_dataset});
    registry.add(Capability{
        "delete_model",
        "Delete model",
        "Delete a model on every node; a served model stops being served. The run "
        "that trained it is kept. dry_run: true answers what would go and changes "
        "nothing. Not undoable.",
        delete_model_schema(),
        delete_model_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_model});
    // lake#654: a model made on its own, not only as a project pipeline's step.
    registry.add(Capability{
        "train_model",
        "Train a standalone model",
        "Train a model on its own from a dataset, a target column (none for a methodology "
        "that learns without one) and a methodology, with an optional held-out share. "
        "Starts a run and answers its run_id; the model appears in list_models, standalone, "
        "when it finishes. No project is shown for it.",
        train_model_schema(),
        train_model_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "train_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_train_model});
    registry.add(Capability{
        "retrain_model",
        "Retrain a model",
        "Train a new version of a model the engine trained, on what it trained on before "
        "or on a new dataset, target or held-out share (kept for the next retrain of a "
        "standalone model). Starts a run and answers its run_id.",
        retrain_model_schema(),
        retrain_model_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "retrain_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_retrain_model});
    registry.add(Capability{
        "register_model",
        "Register a brought model",
        "Keep a model trained elsewhere, from its file, as a standalone model that score, "
        "explain, expose_model and predict use like a trained one; or, with version_of, "
        "as a new version of one. Refuses a format the engine cannot read.",
        register_model_schema(),
        register_model_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "register_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_register_model});
    registry.add(Capability{
        "export_model",
        "Download a model's weights",
        "Read one model version's weights as a file to keep or use elsewhere: as the engine "
        "keeps them (format zetta, any model), or a gradient-boosted tree model as ONNX "
        "(format onnx), which onnxruntime and other ONNX runtimes score. Changes nothing.",
        export_model_schema(),
        export_model_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_export_model});
    registry.add(Capability{
        "move_model_to_project",
        "Move a standalone model into a project",
        "Turn a standalone model into a project people see, with its pipeline and every "
        "version, so a pipeline can grow around it. Replicates.",
        move_model_to_project_schema(),
        move_model_to_project_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "move_model_to_project"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_move_model_to_project});
    registry.add(Capability{
        "delete_run",
        "Delete run",
        "Delete a finished run with its logs and metrics on every node. Refused while "
        "it is pending or running. The models it trained are kept and listed. "
        "dry_run: true answers what would go and changes nothing. Not undoable.",
        delete_run_schema(),
        delete_run_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_run});
    registry.add(Capability{
        "delete_index",
        "Delete index",
        "Delete a search index on every node; a served index stops being served. The "
        "indexed dataset is kept. dry_run: true answers what would go and changes "
        "nothing. Not undoable.",
        delete_index_schema(),
        delete_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_index});

    // --- agents (#849): CRUD for the served, selectable agent entity ----------
    Capability create_agent_capability{
        "create_agent",
        "Create an agent",
        "Create a served, selectable agent — a model (checkpoint dir, model:<uuid>, or a "
        "HuggingFace repo id) plus an optional system prompt and the tools it may call. "
        "Returns the stored record with its generated id; replicates to every node.",
        create_agent_schema(),
        agent_fields_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_agent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_agent};
    create_agent_capability.invalid_input_hint = agent_definition_hint;
    registry.add(std::move(create_agent_capability));
    registry.add(Capability{
        "list_agents",
        "List agents",
        "Read every agent on this node (id, name, model, tools). No side effects.",
        list_agents_schema(),
        list_agents_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_agents});
    registry.add(Capability{
        "get_agent",
        "Get one agent",
        "Read one agent by id. No side effects.",
        agent_id_schema(),
        agent_fields_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_agent});
    Capability update_agent_capability{
        "update_agent",
        "Update an agent",
        "Update the provided fields of an agent (name, description, model, system prompt, "
        "tools); replicates. Returns the updated record.",
        update_agent_schema(),
        agent_fields_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "update_agent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_update_agent};
    update_agent_capability.invalid_input_hint = agent_definition_hint;
    registry.add(std::move(update_agent_capability));
    registry.add(Capability{
        "agent_training_status",
        "Should this agent be retrained?",
        "Whether an agent is stale (its tools or goals changed since its newest Saved Version was "
        "trained), whether its schedule says it is due, every version it has produced, and any "
        "goals that resolve to no verifiable check. Read-only.",
        agent_id_schema(),
        training_status_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Orchestration, "agent_training_status"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_agent_training_status});
    registry.add(Capability{
        "set_training_schedule",
        "Set when an agent retrains",
        "Set an agent's training spec: the corpus, the base checkpoint, the step count, and the "
        "cadence (manual, on_change, daily, weekly, monthly). Manual is the default - an agent "
        "never retrains behind your back.",
        training_schedule_schema(),
        agent_fields_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "set_training_schedule"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_set_training_schedule});
    registry.add(Capability{
        "score",
        "Score a dataset or rows through a trained model",
        "Run a registered model over a `database.table` dataset (compute-near-data, this "
        "node's own partitions) or over rows you supply, and get the entities ranked by "
        "risk plus what drives it. This is how the at-risk list is answerable after the "
        "run that produced it has ended, and how an account that is not in the data at "
        "all gets a score. Read-only.",
        score_schema(),
        score_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "predict"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_score});
    registry.add(Capability{
        "deployment_profile",
        "How to deploy this node",
        "Ask a node how to deploy itself: every port and what it carries, what it needs "
        "from the cluster and — as important — the dependencies people assume it needs "
        "and it does not, the storage and peering model, and the security posture. "
        "`target=helm` returns values a chart can consume directly. Computed from the "
        "live node, so it cannot drift from what the binary does. Never emits a "
        "credential; it says how to seed one. Read-only. (#974)",
        deployment_profile_schema(),
        deployment_profile_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_deployment_profile});
    registry.add(Capability{
        "explain",
        "Explain one row's score",
        "Answer WHY one account scored as it did: the exact signed contribution of "
        "every feature, strongest first, for a single entity of a dataset or for a "
        "row you supply. This is the per-account answer — `score`'s drivers are the "
        "population ranking, identical for every row, so two accounts with the same "
        "score look the same there even when they are at risk for opposite reasons. "
        "Read-only.",
        explain_schema(),
        explain_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "predict"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_explain});
    registry.add(Capability{
        "delete_agent",
        "Delete an agent",
        "Remove an agent by id (a CRDT tombstone; replicates). Idempotent.",
        agent_id_schema(),
        delete_agent_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_agent"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_agent});
    Capability chat_capability{
        "chat",
        "Chat with an agent",
        "Send a message to an agent and get its reply, with tool-calling: the agent's model "
        "generates, any tool calls it emits run in its sandbox and feed back, until it "
        "answers. Loads the agent's model (a local checkpoint dir). It changes nothing on "
        "the node: a turn that asks to build something returns a run PROPOSAL (propose_run, "
        "under the caller's own rights) with an impact preview, and nothing is created until "
        "approve_run_proposal. Also accepts a JSON-body POST.",
        chat_schema(),
        chat_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_chat};
    // NOT offloadable. The flag is only safe for a handler touching no main-thread-only Node
    // state, and grounding a reply on the active Project (#969) reads `node.runs()` and
    // `node.project_names()`, and an `action` turn records a proposal — exactly that state.
    // Decode therefore occupies the owner loop for the length of a turn (#731).
    //
    // A read, so REST serves it as GET; it also takes a JSON-body POST (#975). A turn carries a
    // message and a conversation history, which belong in a body rather than a query string, and
    // POST /v1/chat is what a client writes first. Before the router answered unmatched requests
    // (#1011), that POST matched no route and the connection sat with no reply at all, which read
    // exactly like a hung chat while MCP and /assist answered.
    chat_capability.post_alias = true;
    chat_capability.offloadable = false;
    registry.add(std::move(chat_capability));
    registry.add(Capability{
        "agent_run",
        "Agent builds a plan; runs it once approved",
        "Give an agent a plain-language ask; it recognizes the use case and compiles a real "
        "execution graph from it, stored on the project as a proposed plan. Nothing runs: "
        "the call is refused (not_approved) and names the plan to approve. Once it is "
        "approved (approve_plan), agent_run with that project + graph_name runs it through "
        "the correctness gate + the real engine (churn today) and returns the reply, the "
        "graph's steps, and the run id (poll get_run / system.runs). agent_run never "
        "approves a plan itself (#913).",
        agent_run_schema(),
        agent_run_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_agent_run});

    // --- dataset generation (#873) -------------------------------------------
    registry.add(Capability{
        "list_dataset_generators",
        "List dataset generators",
        "Read the native training-dataset generators available on this node (the base agent's "
        "corpus, Auto-Suggest, …) — each grows + gates a corpus grounded in the solutions plan. "
        "No side effects.",
        list_dataset_generators_schema(),
        list_dataset_generators_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_dataset_generators});
    registry.add(Capability{
        "generate_dataset",
        "Generate a dataset",
        "Synthesize an admission-gated training corpus natively (ADR-17), grounded in the "
        "solutions plan: pick a generator kind, get the gated cases (reference passes, negative "
        "controls fail) with reference actions, and optionally write the JSONL to a path. This is "
        "the native replacement for the offline Python generators + what a per-customer fork "
        "(#853) uses to grow its own corpus.",
        generate_dataset_schema(),
        generate_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_generate_dataset});

    // --- governed serving (#648, docs/api/serving.md) -------------------------

    registry.add(Capability{
        "expose_model",
        "Expose model",
        "Publish a model's HTTP predict endpoint (/v1/models/{name}/predict) "
        "on every node of the cluster; endpoint access then requires the "
        "data:predict capability. Idempotent; undo with unexpose_model.",
        expose_model_schema(),
        expose_model_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "expose_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_expose_model});

    registry.add(Capability{
        "unexpose_model",
        "Un-expose model",
        "Withdraw a model's HTTP predict endpoint everywhere in one action — "
        "the replicated exposure flag flips and every node refuses the route. "
        "Idempotent.",
        expose_model_schema(),
        expose_model_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "unexpose_model"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_unexpose_model});

    registry.add(Capability{
        "expose_index",
        "Expose index",
        "Publish an index's HTTP search endpoint (/v1/indexes/{name}/search — "
        "the distributed scatter-gather path underneath) on every node; "
        "endpoint access then requires the data:search capability. "
        "Idempotent; undo with unexpose_index.",
        expose_index_schema(),
        expose_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "expose_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_expose_index});

    registry.add(Capability{
        "unexpose_index",
        "Un-expose index",
        "Withdraw an index's HTTP search endpoint everywhere in one action — "
        "the replicated exposure flag flips and every node refuses the route. "
        "Idempotent.",
        expose_index_schema(),
        expose_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "unexpose_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_unexpose_index});

    registry.add(Capability{
        "create_index",
        "Create index",
        "Build a full-text index over a loaded table: records the Search index "
        "entity and builds the per-partition BM25F sub-indexes so `search` "
        "returns ranked hits (load the table first with add_dataset).",
        create_index_schema(),
        create_index_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_index"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_index});

    // --- governed action parity (#617): the rest of the product loop ---------

    registry.add(Capability{
        "capture_intent",
        "Capture intent",
        "Capture a natural-language intent as a replicated project input with "
        "provenance; compile it into a graph with compile_intent.",
        capture_intent_schema(),
        project_input_output_schema(),
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
        project_input_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "import_n8n"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_import_n8n});

    registry.add(Capability{
        "compile_intent", "Compile intent to graph",
        "Resolve a methodology from an explicit methodology, primary solution, "
        "or intent keywords, then compile its editable graph. Unavailable "
        "methodologies are refused with their implementation issue. Without "
        "a `name`, the graph is named from the intent's own words; `template` "
        "says which template its steps started from.",
        compile_intent_schema(), project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_intent"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_compile_intent});

    registry.add(Capability{
        "compile_n8n",
        "Compile n8n workflow to graph",
        "Compile a stored n8n workflow input into a named, editable execution "
        "graph stored on the project (replicated; audited as a graph change).",
        compile_n8n_schema(),
        project_graph_output_schema(),
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
        project_graph_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "compile_python"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_compile_python});

    registry.add(Capability{
        "gate",
        "Run the correctness gate",
        "Run the correctness gate over a stored project graph and return the "
        "validation report: the checks that applied (status + reason), the "
        "verdict, block_deploy, and the counts applied / not_applicable. "
        "Checks with nothing to check on this graph are counted, never "
        "reported as pass; include_not_applicable lists them too. Evaluates "
        "only — no state change.",
        gate_schema(),
        gate_output_schema(),
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
        deploy_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "deploy"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_deploy});

    registry.add(Capability{
        "run", "Run a graph",
        "Execute a stored, gate-passing project graph through its registered "
        "methodology runner. Unknown and unavailable model kinds are refused; "
        "a graph with no model step may run structurally.",
        run_schema(), run_output_schema(), CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "run"},
        /*mutates=*/true,
        /*requires_consent=*/false, handle_run});

    registry.add(Capability{
        "add_dataset",
        "Add a dataset",
        "Load a local parquet/jsonl/json/csv file into a new partitioned "
        "table placed across the cluster (audited as dataset_added; visible "
        "in system.datasets).",
        add_dataset_schema(),
        add_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "add_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_add_dataset});

    registry.add(Capability{
        "snapshot",
        "Materialize a point-in-time snapshot",
        "Materialize the latest source row per entity at or before the requested "
        "cutoff into a replicated table keyed by entity_id and as_of. Existing "
        "identical keys make retries idempotent; conflicting keys are refused.",
        zetta::solutions::snapshot_spec_schema(),
        snapshot_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "snapshot"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_snapshot});

    registry.add(Capability{
        "upload_dataset",
        "Upload a dataset",
        "Load a csv or jsonl file's TEXT, sent in the request, into a new "
        "partitioned table placed across the cluster - for a caller that cannot "
        "put a file on the node (add_dataset reads the node's filesystem). Same "
        "parsing, table creation and placement as add_dataset; at most 16 MiB of "
        "content (a larger file goes to a bucket and loads with load_dataset_object). "
        "Audited as dataset_added; the table then appears in "
        "list_datasets / system.datasets and can be passed to run as "
        "dataset_path <database>.<table>. (#1008)",
        upload_dataset_schema(),
        add_dataset_output_schema(),
        CapabilityKind::Action,
        // Authorized exactly like add_dataset: whoever may add a dataset from a
        // file may add the same rows from a request body.
        Operation{CapabilityDomain::Orchestration, "add_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_upload_dataset});

    registry.add(Capability{
        "load_dataset_object",
        "Load a dataset from a stored object",
        "Load a csv or jsonl object already in a bucket (uploaded a chunk at a time "
        "with PATCH /v1/objects/<bucket>/<key>) into a new partitioned table placed "
        "across the cluster - the path for a file larger than upload_dataset's 16 MiB, "
        "up to 1 GiB. Returns a run id at once and loads in the background: poll "
        "get_run until succeeded (the dataset is the run's output) or failed (the log "
        "says why, and no table is left). Needs objects:read on the bucket; "
        "remove_object removes the object afterwards and also needs objects:write. "
        "Audited as dataset_added. (#1302)",
        load_dataset_object_schema(),
        load_dataset_object_output_schema(),
        CapabilityKind::Job,
        // Authorized like upload_dataset: whoever may add a dataset may load one
        // from an object (the handler also checks objects:read on the bucket).
        Operation{CapabilityDomain::Orchestration, "add_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_load_dataset_object});

    registry.add(Capability{
        "create_connection",
        "Save a source connection",
        "Save a named connector configuration (credential included) so a "
        "connector query can reference it by name instead of carrying the "
        "token in every request. The connection is stored replicate-to-all — "
        "every node holds it, which is what lets each node fetch its own "
        "stripe of a distributed query straight from the source. Saving an "
        "existing name replaces it, re-sealing the new credential. The token "
        "is stored sealed (AES-256-GCM under a key derived from the cluster "
        "join token, never in the replicated catalogue) and is never returned "
        "by any surface (list_connections and system.connections report only "
        "whether one is set).",
        create_connection_schema(),
        create_connection_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_connection"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_connection});

    registry.add(Capability{
        "list_connections",
        "List saved connections",
        "The saved source connections, credentials REDACTED — name, connector "
        "settings, and when each was saved. Same source as "
        "system.connections.",
        list_connections_schema(),
        list_connections_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Orchestration, "list_connections"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_connections});

    registry.add(Capability{
        "list_source_tables",
        "List the tables in a connected source",
        "Browse a connected source's catalog WITHOUT ingesting from it: the "
        "tables (optionally with their columns and types) behind a saved "
        "connection, filtered by schema and by a name substring. Opens one "
        "connection, runs one read-only catalog query, lands nothing \u2014 use "
        "this to FIND a table, then create_dataset_from_query to pull it. "
        "Filter with match='score' to locate scoring tables.",
        list_source_tables_schema(),
        list_source_tables_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Orchestration, "list_source_tables"},
        /*mutates=*/false,
        /*requires_consent=*/true,
        handle_list_source_tables,
        describe_create_dataset_from_query});

    registry.add(Capability{
        "delete_connection",
        "Delete a saved connection",
        "Remove a saved source connection (and its stored credential) from "
        "every node. Returns removed=false when no connection had that name.",
        delete_connection_schema(),
        delete_connection_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "delete_connection"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_delete_connection});

    registry.add(Capability{
        "create_dataset_from_query",
        "Create a dataset from a connector query",
        "Run a read-only query against an external source (Snowflake, "
        "Postgres or MySQL) and load its rows into a new partitioned table placed "
        "across the cluster \u2014 the in-product path from a warehouse to a "
        "featurizable dataset, with no manual export. Only a single "
        "SELECT/WITH may run (MySQL also admits SHOW). A remote source is reached "
        "over the network with your credentials, so it is consent-gated when the "
        "node enforces governance; a loopback Postgres or MySQL touches no cloud "
        "and runs directly "
        "(audited as dataset_added; visible in system.datasets).",
        create_dataset_from_query_schema(),
        create_dataset_from_query_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_dataset_from_query"},
        /*mutates=*/true,
        /*requires_consent=*/true,
        handle_create_dataset_from_query,
        describe_create_dataset_from_query});

    registry.add(Capability{
        "promote_dataset",
        "Promote a cached dataset to durable",
        "Raise a dataset's replication factor (default 3, clamped to the node "
        "count) so a connector result set that landed as a cache at RF=1 stops "
        "being one copy with no backup. Adds owners to each partition in the "
        "catalog; the bodies then move to them over the existing large-object "
        "transport. Never removes an owner, so it can only make a dataset "
        "safer. Visible in system.dataset_partitions.",
        promote_dataset_schema(),
        promote_dataset_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "promote_dataset"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_promote_dataset});

    registry.add(Capability{
        "testsuite_run",
        "Run a test-suite case",
        "Start one in-app test-suite case in the background and return its "
        "run id immediately (poll get_run — the case's log lines land with "
        "the terminal status). List the cases with testsuite_list / "
        "system.test_cases.",
        testsuite_run_schema(),
        testsuite_run_output_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Orchestration, "testsuite_run"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_testsuite_run});

    registry.add(Capability{
        "testsuite_list",
        "List test-suite cases",
        "Read the whole in-app test-suite catalog — one entry per runnable "
        "case (id, category, name, description, tunable options). Feed a case "
        "id to testsuite_run. Also queryable as system.test_cases. No side "
        "effects.",
        testsuite_list_schema(),
        testsuite_list_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_testsuite_list});

    // The spec index (#646, docs/api/specs-index.md): discovery of every
    // schema the cluster speaks, served from the C++ accessors. The handlers
    // capture the registry they live in — safe: the registry is populated at
    // startup, then only read, and outlives its own entries.
    registry.add(Capability{
        "list_specs",
        "List specs",
        "Read the spec index: the OpenAPI document plus every JSON Schema "
        "this node speaks — capability input/output schemas and all "
        "schema-as-code boundary schemas — as names, kinds, and links. No "
        "side effects.",
        list_specs_schema(),
        list_specs_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        [&registry](const nlohmann::json&, ApiContext&) {
            return ApiResult::success(spec_index(registry));
        }});

    registry.add(Capability{
        "get_spec",
        "Get one spec",
        "Read one spec document by its list_specs name — 'openapi', "
        "'<capability>.input' / '<capability>.output', or a boundary schema "
        "name like 'run'. No side effects.",
        get_spec_schema(),
        get_spec_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        [&registry](const nlohmann::json& input, ApiContext&) {
            const std::string name = input.at("name").get<std::string>();
            auto spec = find_spec(registry, name);
            if (!spec) {
                return ApiResult::failure(
                    "not_found",
                    "unknown spec '" + name +
                        "' — call list_specs for the index",
                    "/name");
            }
            return ApiResult::success(std::move(*spec));
        }});

    // The cloud-touching tools + the consent protocol tools (#621) — the
    // consent-gated slice, in its own TU (cloud_capabilities.cpp).
    register_cloud_capabilities(registry);

    // Pub/sub (#1033), in its own TU (pubsub_capabilities.cpp).
    register_pubsub_capabilities(registry);
    register_run_metrics_capabilities(registry);
    register_record_capabilities(registry);
    register_network_capabilities(registry);
    register_object_capabilities(registry);
    register_storage_capabilities(registry);
    register_study_capabilities(registry);
    register_solution_capabilities(registry);

    // Versioned score-to-action policy (#1104).
    register_decision_policy_capabilities(registry);

    // Report, task, and bounded closed-loop action patterns (#1109).
    register_action_capabilities(registry);

    // Propose a run, then approve or reject it (#969).
    register_run_proposal_capabilities(registry);

    registry.add(Capability{
        "analyze_dataset",
        "Analyze a dataset",
        "Read a landed dataset's semantic types, evidence, distributions, grain, candidate "
        "keys, same-database joins, and leakage candidates. Reads as the caller: only "
        "tables, columns and rows its own SQL could read; a column it may not read is "
        "omitted (#1396). No side effects.",
        analyze_dataset_schema(),
        analyze_dataset_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_analyze_dataset});

    registry.add(Capability{
        "plan_features",
        "Plan dataset features",
        "Read a methodology-specific feature and exclusion plan from dataset analysis, or an "
        "explicit transform_required proposal, built only from what the caller may read "
        "(#1396). No side effects and no transform is applied.",
        plan_features_schema(),
        plan_features_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_plan_features});
}

std::string pipeline_tool_name(const std::string& graph) {
    std::string name = "pipeline_";
    for (const char character : graph) {
        name += std::isalnum(static_cast<unsigned char>(character))
                    ? static_cast<char>(std::tolower(static_cast<unsigned char>(character)))
                    : '_';
    }
    return name.substr(0, 57);
}

ApiResult run_granted_pipeline(ApiContext& context,
                               const zetta::agent::AgentRecord& agent,
                               const std::string& tool,
                               const nlohmann::json& arguments) {
    const auto grant = std::find_if(
        agent.pipelines.begin(), agent.pipelines.end(),
        [&tool](const zetta::agent::PipelineGrant& g) { return pipeline_tool_name(g.graph) == tool; });
    if (grant == agent.pipelines.end()) {
        return ApiResult::failure("unauthorized", "'" + tool + "' is not a Pipeline granted to this agent");
    }
    if (context.registry == nullptr) {
        return ApiResult::failure("engine_error", "no capability registry for this call");
    }
    const auto project = context.node.find_project(grant->project);
    const auto graph = project ? project->find_graph(grant->graph) : nullptr;
    if (!graph) {
        return ApiResult::failure("not_found", "the granted Pipeline '" + grant->graph + "' is gone");
    }
    if (graph->approval_status() != "approved" || graph->approved_at() != grant->version) {
        return ApiResult::failure(
            "unauthorized",
            "the Pipeline '" + grant->graph +
                "' changed since it was granted; it has to be approved and granted again");
    }
    const nlohmann::json ref = {{"project", grant->project}, {"graph", grant->graph}};
    const ApiResult gated = context.registry->call("gate", ref, context);
    if (!gated.ok) return gated;
    if (gated.value.value("block_deploy", false)) {
        return ApiResult::failure("engine_error",
                                  "the correctness gate blocks '" + grant->graph + "': " +
                                      gated.value.dump());
    }
    nlohmann::json run = ref;
    if (arguments.is_object()) {
        for (const char* key : {"dataset", "holdout", "model_name"}) {
            if (arguments.contains(key)) run[key] = arguments.at(key);
        }
    }
    return context.registry->call("run", run, context);
}

}  // namespace zetta::api
