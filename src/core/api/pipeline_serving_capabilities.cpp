// Published pipelines on the control surface (zetta#1289, docs/api/serving.md):
// expose_pipeline pins a pipeline and publishes its served route on every node,
// unexpose_pipeline withdraws it, and run_published_pipeline /
// get_published_pipeline_run are the same served run and status as
// POST /v1/pipelines/{name}/run and GET /v1/pipelines/{name}/runs/{run_id}.

#include <exception>
#include <string>

#include <nlohmann/json-schema.hpp>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

// The largest declared input a published pipeline may accept: the HTTP body
// limit of the served route.
constexpr std::uint64_t kMaxInputBytes = 1024 * 1024;

nlohmann::json pipeline_reference(const char* description) {
    return {{"type", "string"}, {"minLength", 1}, {"description", description}};
}

const nlohmann::json& expose_pipeline_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", pipeline_reference("the owning project's name, or its uuid")},
          {"graph", pipeline_reference("the pipeline's name, or its uuid")},
          {"input_schema",
           {{"type", "object"},
            {"description",
             "the draft-07 JSON Schema callers' input must match: an object schema over "
             "`run`'s inputs (dataset_path, top_k, model_name, ...). Default: an object "
             "with no properties, so callers can pass nothing"}}},
          {"rate_limit_per_minute",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 100000},
            {"description", "calls one caller may make a minute, per node (default 60)"}}},
          {"max_input_bytes",
           {{"type", "integer"},
            {"minimum", 2},
            {"maximum", kMaxInputBytes},
            {"description", "the largest input a call may send, in bytes (default 65536)"}}},
          {"sync_timeout_ms",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", 600000},
            {"description",
             "how long the served route waits for a run to finish before it answers with "
             "the run id instead (default 30000; 0 = never wait)"}}}}}};
    return schema;
}

const nlohmann::json& unexpose_pipeline_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"project", "graph"}},
        {"additionalProperties", false},
        {"properties",
         {{"project", pipeline_reference("the owning project's name, or its uuid")},
          {"graph", pipeline_reference("the pipeline's name, or its uuid")}}}};
    return schema;
}

const nlohmann::json& exposure_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"pipeline_id", "project", "name", "exposed", "version", "endpoint"}},
        {"additionalProperties", false},
        {"properties",
         {{"pipeline_id", {{"type", "string"}}},
          {"project", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"exposed", {{"type", "boolean"}}},
          {"version",
           {{"type", "integer"},
            {"description", "the pinned version callers get (counts the exposures)"}}},
          {"endpoint",
           {{"type", "string"}, {"description", "the served run route, by id"}}},
          {"status_endpoint",
           {{"type", "string"},
            {"description", "the served status route, with {run_id} to fill in"}}},
          {"input_schema", {{"type", "object"}}},
          {"rate_limit_per_minute", {{"type", "integer"}}},
          {"max_input_bytes", {{"type", "integer"}}},
          {"sync_timeout_ms", {{"type", "integer"}}},
          {"exposed_by", {{"type", "string"}}},
          {"exposed_at", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& run_published_pipeline_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"pipeline"}},
        {"additionalProperties", false},
        {"properties",
         {{"pipeline",
           pipeline_reference(
               "the published pipeline's id, or its name when one exposed pipeline has it")},
          {"input",
           {{"type", "object"},
            {"description", "the input the pipeline declares (expose_pipeline input_schema)"}}}}}};
    return schema;
}

const nlohmann::json& get_published_pipeline_run_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"pipeline", "run_id"}},
        {"additionalProperties", false},
        {"properties",
         {{"pipeline", pipeline_reference("the published pipeline's id, or its name")},
          {"run_id", pipeline_reference("the run id run_published_pipeline answered")}}}};
    return schema;
}

std::shared_ptr<zetta::pipeline::Project> find_project(Node& node, const std::string& name) {
    if (auto project = node.find_project(name)) return project;
    for (const auto& project : node.projects()) {
        if (project->id() == name) return project;
    }
    return nullptr;
}

std::shared_ptr<zetta::pipeline::ProjectGraph> find_graph(const zetta::pipeline::Project& project,
                                                          const std::string& name) {
    if (auto graph = project.find_graph(name)) return graph;
    return project.find_graph_by_id(name);
}

nlohmann::json exposure_json(const zetta::pipeline::Project& project,
                             const zetta::pipeline::ProjectGraph& graph) {
    const zetta::pipeline::PipelineExposure& exposure = graph.exposure();
    nlohmann::json output{{"pipeline_id", graph.id()},
                          {"project", project.name()},
                          {"name", graph.name()},
                          {"exposed", exposure.exposed},
                          {"version", exposure.version},
                          {"endpoint", pipeline_endpoint_path(graph.id())}};
    if (exposure.version > 0) {
        output["status_endpoint"] = pipeline_run_status_path(graph.id(), "{run_id}");
        output["input_schema"] = exposure.input_schema;
        output["rate_limit_per_minute"] = exposure.rate_limit_per_minute;
        output["max_input_bytes"] = exposure.max_input_bytes;
        output["sync_timeout_ms"] = exposure.sync_timeout_ms;
        output["exposed_by"] = exposure.exposed_by;
        output["exposed_at"] = exposure.exposed_at;
    }
    return output;
}

// Whether `schema` compiles as a draft-07 schema; the reason when it does not.
std::string schema_problem(const nlohmann::json& schema) {
    if (schema.value("type", nlohmann::json()) != "object") {
        return "input_schema must describe an object (\"type\": \"object\")";
    }
    try {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
    } catch (const std::exception& error) {
        return std::string("input_schema is not a usable JSON Schema: ") + error.what();
    }
    return {};
}

// A published pipeline reads only tables in this workspace: a connect step that
// names a source must name a table this node holds. So a served call never
// reaches a remote source or a cloud, which only the consent gate may allow.
std::string non_local_source(Node& node, const zetta::ir::ExecutionGraph& graph) {
    for (const auto& step : graph.nodes_of_kind(zetta::ir::StepKind::Connect)) {
        if (!step || !step->attributes.is_object()) continue;
        for (const char* outside : {"uri", "path"}) {
            if (step->attributes.contains(outside)) {
                return "the connect step '" + step->id + "' reads a " + outside +
                       ", outside this workspace; a published pipeline reads only landed "
                       "tables (land it with add_dataset or create_dataset_from_query first)";
            }
        }
        const std::string source = step->attributes.value("source", std::string{});
        const std::string table = step->attributes.value("table", std::string{});
        if (source.empty() && table.empty()) continue;
        const auto database = node.find_database(source);
        if (!database || table.empty() || !database->find_table(table)) {
            return "the connect step '" + step->id + "' reads '" + source + "." + table +
                   "', which is not a table in this workspace; a published pipeline reads only "
                   "landed tables (land it with add_dataset or create_dataset_from_query first)";
        }
    }
    return {};
}

ApiResult handle_expose_pipeline(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = find_project(context.node, project_name);
    if (!project) {
        return ApiResult::failure("not_found", "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const auto graph = find_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no pipeline named '" + graph_name +
                                                   "' on project '" + project->name() + "'");
    }
    // Nothing is published that is not approved and passing its gate (#885, RULE-1).
    if (!graph->approved()) {
        return ApiResult::failure("engine_error",
                                  "pipeline '" + graph->name() +
                                      "' is proposed, not approved: approve_plan it, then "
                                      "expose it");
    }
    const zetta::pipeline::ValidationReport report =
        zetta::pipeline::default_gate().run(graph->graph());
    if (report.block_deploy) {
        std::string reason = "the correctness gate failed";
        for (const auto& check : report.checks) {
            if (check.status == zetta::pipeline::CheckStatus::Fail) {
                reason = "gate check '" + check.name + "' failed: " + check.reason;
                break;
            }
        }
        return ApiResult::failure("engine_error", "pipeline '" + graph->name() +
                                                      "' cannot be published: " + reason);
    }
    if (const std::string remote = non_local_source(context.node, graph->graph());
        !remote.empty()) {
        return ApiResult::failure("invalid_input", remote, "/graph");
    }
    zetta::pipeline::PipelineExposure exposure;
    exposure.input_schema = input.value("input_schema", default_pipeline_input_schema());
    if (const std::string problem = schema_problem(exposure.input_schema); !problem.empty()) {
        return ApiResult::failure("invalid_input", problem, "/input_schema");
    }
    exposure.exposed = true;
    // Republishing pins the pipeline as it is now, as the next version.
    exposure.version = graph->exposure().version + 1;
    exposure.pinned = zetta::ir::to_json(graph->graph());
    exposure.rate_limit_per_minute = input.value("rate_limit_per_minute", 60u);
    exposure.max_input_bytes = input.value("max_input_bytes", std::uint64_t{65536});
    exposure.sync_timeout_ms = input.value("sync_timeout_ms", 30000u);
    exposure.exposed_by = context.acting_key.empty() ? std::string("local") : context.acting_key;
    exposure.exposed_at = zetta::util::now_iso8601();
    const auto stored = project->set_graph_exposure(graph->id(), std::move(exposure));
    if (!stored) return ApiResult::failure("engine_error", "the exposure was not recorded");
    return ApiResult::success(exposure_json(*project, *stored));
}

ApiResult handle_unexpose_pipeline(const nlohmann::json& input, ApiContext& context) {
    const std::string project_name = input.at("project").get<std::string>();
    const auto project = find_project(context.node, project_name);
    if (!project) {
        return ApiResult::failure("not_found", "no project named '" + project_name + "'");
    }
    const std::string graph_name = input.at("graph").get<std::string>();
    const auto graph = find_graph(*project, graph_name);
    if (!graph) {
        return ApiResult::failure("not_found", "no pipeline named '" + graph_name +
                                                   "' on project '" + project->name() + "'");
    }
    if (graph->exposure().exposed) {
        zetta::pipeline::PipelineExposure exposure = graph->exposure();
        exposure.exposed = false;  // keeps the version count for the next exposure
        if (!project->set_graph_exposure(graph->id(), std::move(exposure))) {
            return ApiResult::failure("engine_error", "the exposure was not recorded");
        }
    }
    return ApiResult::success(exposure_json(*project, *graph));
}

ServeRequest serve_request_of(const nlohmann::json& input, const ApiContext& context,
                              nlohmann::json body) {
    ServeRequest request{input.at("pipeline").get<std::string>(), std::move(body),
                         context.acting_key, context.trace, context.recorder};
    request.acting_is_user = context.acting_is_user;
    request.registry = context.registry;
    request.background = context.background;
    request.cluster = context.cluster;
    return request;
}

ApiResult handle_run_published_pipeline(const nlohmann::json& input, ApiContext& context) {
    return serve_pipeline_run(
        context.node,
        serve_request_of(input, context, input.value("input", nlohmann::json::object())));
}

ApiResult handle_get_published_pipeline_run(const nlohmann::json& input, ApiContext& context) {
    return serve_pipeline_run_status(
        context.node, serve_request_of(input, context, {{"run_id", input.at("run_id")}}));
}

// A user whose data:run_pipeline is scoped ON PROJECT passes the registry's
// gate here; serve_pipeline_run checks the grant against the pipeline's project.
bool run_pipeline_scoped(const nlohmann::json& input, const ApiContext& context) {
    PublishedPipeline published;
    if (!resolve_published_pipeline(context.node, input.at("pipeline").get<std::string>(),
                                    published)
             .ok) {
        return false;
    }
    zetta::governance::Resource scope;
    scope.kind = zetta::governance::ResourceKind::Project;
    scope.name = published.project->name();
    return context.node.authorize_object(context.acting_key, context.acting_is_user,
                                         Operation{CapabilityDomain::Data, "run_pipeline"},
                                         scope);
}

}  // namespace

void register_pipeline_serving_capabilities(Registry& registry) {
    registry.add(Capability{
        "expose_pipeline",
        "Expose pipeline",
        "Publish an approved, gate-passing pipeline as a governed HTTP route "
        "(POST /v1/pipelines/{name}/run) on every node, pinned to the version it is now: "
        "later edits change nothing callers get until it is exposed again. Callers need "
        "data:run_pipeline and read access to the tables it reads; their input must match "
        "input_schema. Idempotent; undo with unexpose_pipeline.",
        expose_pipeline_schema(),
        exposure_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "expose_pipeline"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_expose_pipeline});

    registry.add(Capability{
        "unexpose_pipeline",
        "Un-expose pipeline",
        "Withdraw a published pipeline's HTTP route everywhere in one action: the replicated "
        "exposure flips and every node refuses the route. Runs it already served keep going. "
        "Idempotent.",
        unexpose_pipeline_schema(),
        exposure_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "unexpose_pipeline"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_unexpose_pipeline});

    Capability run{
        "run_published_pipeline",
        "Run a published pipeline",
        "Run a published pipeline's pinned version with your input, as POST "
        "/v1/pipelines/{name}/run does: your own data:run_pipeline grant and read access to "
        "the tables it reads decide, the input is checked against the schema it declares, "
        "and calls are rate limited per caller. Starts a run and answers its id and status.",
        run_published_pipeline_schema(),
        pipeline_run_response_schema(),
        CapabilityKind::Job,
        Operation{CapabilityDomain::Data, "run_pipeline"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_run_published_pipeline};
    run.authorize_scoped = run_pipeline_scoped;
    registry.add(std::move(run));

    Capability status{
        "get_published_pipeline_run",
        "Get a published pipeline's run",
        "Read the status and outputs of a run a published pipeline served to you, as GET "
        "/v1/pipelines/{name}/runs/{run_id} does. Another caller's run is not found. No side "
        "effects.",
        get_published_pipeline_run_schema(),
        pipeline_run_response_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "run_pipeline"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_get_published_pipeline_run};
    status.authorize_scoped = run_pipeline_scoped;
    registry.add(std::move(status));
}

}  // namespace zetta::api
