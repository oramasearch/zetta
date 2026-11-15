// The REST rendering of the capability registry (#618,
// docs/api/rest-openapi.md): everything here is derived mechanically from the
// registered entries — reads render as GET /v1/<name>, actions/jobs as
// POST /v1/<name>, and the OpenAPI document is generated from the same
// input/output schemas the MCP rendering serves. HTTP-free by design: the
// Beast2 adapter TU (http_server.cpp) is the only wire-facing consumer.
#include "zetta/api/rest.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/version.hpp"

namespace zetta::api {

std::string rest_path(const Capability& capability) {
    return "/v1/" + capability.name;
}

std::string rest_method(const Capability& capability) {
    return capability.kind == CapabilityKind::Read ? "GET" : "POST";
}

namespace {

// Coerce one query-string value to the type its input-schema property
// declares. Anything that does not parse as the declared type stays a string,
// so Registry::call's schema validation rejects it with the path-qualified
// reason instead of this layer inventing its own error surface.
nlohmann::json coerce_query_value(const nlohmann::json& property_schema,
                                  const std::string& value) {
    const std::string type = property_schema.is_object()
                                 ? property_schema.value("type", "string")
                                 : std::string("string");
    if (type == "integer") {
        long long parsed = 0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error == std::errc{} && end == value.data() + value.size()) {
            return parsed;
        }
        return value;
    }
    if (type == "number") {
        double parsed = 0.0;
        const auto [end, error] =
            std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (error == std::errc{} && end == value.data() + value.size()) {
            return parsed;
        }
        return value;
    }
    if (type == "boolean") {
        if (value == "true") return true;
        if (value == "false") return false;
        return value;
    }
    if (type == "array" || type == "object") {
        nlohmann::json parsed =
            nlohmann::json::parse(value, nullptr, /*allow_exceptions=*/false);
        if (!parsed.is_discarded()) return parsed;
        return value;
    }
    return value;
}

}  // namespace

nlohmann::json rest_input_from_query(
    const nlohmann::json& input_schema,
    const std::vector<std::pair<std::string, std::string>>& query_parameters) {
    nlohmann::json input = nlohmann::json::object();
    const nlohmann::json properties =
        input_schema.value("properties", nlohmann::json::object());
    for (const auto& [key, value] : query_parameters) {
        if (properties.contains(key)) {
            input[key] = coerce_query_value(properties.at(key), value);
        } else {
            // Undeclared keys (including the reserved `intent`, which is a
            // string by contract) pass through verbatim; the schema boundary
            // decides their fate.
            input[key] = value;
        }
    }
    return input;
}

nlohmann::json rest_input_from_query(
    const Capability& capability,
    const std::vector<std::pair<std::string, std::string>>& query_parameters) {
    return rest_input_from_query(capability.input_schema, query_parameters);
}

int rest_status(const ApiResult& result) {
    if (result.ok) return 200;
    const std::string& code = result.error.code;
    if (code == "invalid_input") return 400;
    if (code == "unauthorized") return 403;
    if (code == "forbidden") return 403;          // read-only by design (zetta#1261)
    if (code == "runner_required") return 409;    // Lake's runner runs it (zetta#1261)
    if (code == "consent_required") return 403;
    // Safe mode (#1373): the call needs the caller's confirmation of the named
    // statement — a precondition the repeated request supplies.
    if (code == "safe_mode_warning") return 428;
    if (code == "not_found") return 404;
    if (code == "conflict") return 409;
    if (code == "fenced" || code == "held") return 409;  // the object lease (#1005)
    if (code == "incarnation_changed") return 409;       // the object routes (lake#543)
    if (code == "range_not_satisfiable") return 416;
    if (code == "quota_exceeded") return 507;
    if (code == "unknown_capability") return 404;
    if (code == "engine_error") return 422;
    if (code == "busy") return 503;
    if (code == "rate_limited") return 429;       // a published pipeline (zetta#1289)
    if (code == "payload_too_large") return 413;  // a published pipeline (zetta#1289)
    return 500;
}

nlohmann::json rest_envelope(const ApiResult& result) {
    nlohmann::json envelope;
    envelope["ok"] = result.ok;
    if (result.ok) {
        envelope["value"] = result.value;
    } else {
        envelope["error"] = {{"code", result.error.code},
                             {"message", result.error.message},
                             {"path", result.error.path}};
        // The structured failure payload rides along, exactly as it does on
        // MCP — a "consent_required" refusal's challenge is the whole point of
        // returning it (#621), and REST silently dropping it made the same
        // call answer differently on the two renderings, which is the drift
        // one registry exists to prevent (#995).
        if (!result.error.details.is_null()) {
            envelope["error"]["details"] = result.error.details;
        }
    }
    envelope["trace_id"] = result.trace_id;
    envelope["span_id"] = result.span_id;
    envelope["duration_ms"] = result.duration_ms;
    return envelope;
}

namespace {

// The reserved cross-capability `intent` field (stripped before schema
// validation, recorded on the call's span) — injected into every rendered
// input so the document tells the truth about what the wire accepts.
nlohmann::json intent_property_schema() {
    return nlohmann::json{
        {"type", "string"},
        {"description",
         "reserved: say what you are trying to do; recorded on the call's "
         "trace, stripped before schema validation"}};
}

// The input schema with `intent` added — the request-body schema of a POST
// rendering.
nlohmann::json request_body_schema(const nlohmann::json& input_schema) {
    nlohmann::json schema = input_schema;
    if (schema.is_object()) {
        schema["properties"]["intent"] = intent_property_schema();
    }
    return schema;
}

// The GET rendering's parameter list: one query parameter per input-schema
// property (plus `intent`). Scalar parameters carry their property schema;
// array/object parameters are JSON-encoded in the query string, which OpenAPI
// expresses as a `content` parameter.
nlohmann::json query_parameters(const nlohmann::json& input_schema) {
    nlohmann::json parameters = nlohmann::json::array();
    const nlohmann::json properties =
        input_schema.value("properties", nlohmann::json::object());
    const nlohmann::json required =
        input_schema.value("required", nlohmann::json::array());
    for (const auto& [name, property_schema] : properties.items()) {
        nlohmann::json parameter;
        parameter["name"] = name;
        parameter["in"] = "query";
        parameter["required"] =
            std::find(required.begin(), required.end(), name) != required.end();
        if (property_schema.is_object() &&
            property_schema.contains("description")) {
            parameter["description"] = property_schema.at("description");
        }
        const std::string type = property_schema.is_object()
                                     ? property_schema.value("type", "string")
                                     : std::string("string");
        if (type == "array" || type == "object") {
            parameter["content"] = {
                {"application/json", {{"schema", property_schema}}}};
        } else {
            parameter["schema"] = property_schema;
        }
        parameters.push_back(std::move(parameter));
    }
    nlohmann::json intent_parameter;
    intent_parameter["name"] = "intent";
    intent_parameter["in"] = "query";
    intent_parameter["required"] = false;
    intent_parameter["description"] =
        intent_property_schema().at("description");
    intent_parameter["schema"] = {{"type", "string"}};
    parameters.push_back(std::move(intent_parameter));
    return parameters;
}

// The 200 response body: the success envelope with `value` typed by the
// output schema (an untyped object when none is declared).
nlohmann::json success_envelope_schema(const nlohmann::json& output_schema) {
    nlohmann::json value_schema = output_schema;
    if (value_schema.is_null() ||
        (value_schema.is_object() && value_schema.empty())) {
        value_schema = {{"type", "object"}};
    }
    return nlohmann::json{
        {"type", "object"},
        {"required", {"ok", "value", "trace_id", "span_id", "duration_ms"}},
        {"properties",
         {{"ok", {{"type", "boolean"}, {"const", true}}},
          {"value", value_schema},
          {"trace_id", {{"type", "string"}}},
          {"span_id", {{"type", "string"}}},
          {"duration_ms", {{"type", "number"}}}}}};
}

nlohmann::json error_envelope_schema() {
    return nlohmann::json{
        {"type", "object"},
        {"required", {"ok", "error"}},
        {"properties",
         {{"ok", {{"type", "boolean"}, {"const", false}}},
          {"error",
           {{"type", "object"},
            {"required", {"code", "message"}},
            {"properties",
             {{"code",
               {{"type", "string"},
                {"description",
                 "invalid_input (400) · unauthorized / consent_required "
                 "(403) · not_found / unknown_capability (404) · "
                 "conflict / fenced / held (409) · payload_too_large (413) · "
                 "range_not_satisfiable (416) · engine_error (422) · rate_limited (429) · "
                 "busy (503) · quota_exceeded (507)"}}},
              {"message",
               {{"type", "string"},
                {"description", "the engine's reason, verbatim"}}},
              {"path",
               {{"type", "string"},
                {"description",
                 "JSON pointer into the input when it is at fault"}}},
              {"details",
               {{"type", "object"},
                {"description",
                 "structured payload for failures that carry one \u2014 the "
                 "consent challenge behind a consent_required refusal "
                 "(#621). Absent otherwise."}}}}}}},
          {"trace_id", {{"type", "string"}}},
          {"span_id", {{"type", "string"}}},
          {"duration_ms", {{"type", "number"}}}}}};
}

nlohmann::json error_response(std::string_view description) {
    return nlohmann::json{
        {"description", std::string(description)},
        {"content",
         {{"application/json",
           {{"schema",
             {{"$ref", "#/components/schemas/error_envelope"}}}}}}}};
}

}  // namespace

nlohmann::json openapi_document(const Registry& registry) {
    return openapi_document(registry, [](const Capability&) { return true; });
}

CapabilityVisible publicly_callable() {
    return [](const Capability&) { return false; };
}

nlohmann::json openapi_document(const Registry& registry,
                                const CapabilityVisible& visible) {
    nlohmann::json document;
    document["openapi"] = "3.1.0";
    document["info"] = {
        {"title", "Orama Zetta control surface"},
        {"version", std::string(zetta::kVersion)},
        {"description",
         "The capability registry rendered as plain HTTP routes: reads are "
         "GET /v1/<name> (input in the query string), actions and jobs are "
         "POST /v1/<name> (input as the JSON body). The same registry is "
         "served as MCP tools on POST /mcp — one registry, two renderings, "
         "zero drift. Every response carries a trace_id; read the full trace "
         "back with get_trace."}};
    document["servers"] = nlohmann::json::array({{{"url", "/"}}});
    document["components"] = {
        {"securitySchemes",
         {{"bearer_token",
           {{"type", "http"},
            {"scheme", "bearer"},
            {"description",
             "an access token that resolves to a user via the token store "
             "(#744). Required on EVERY request — auth is always on (#799); "
             "there is no anonymous path. Seed the first credential with "
             "--admin-token (the bootstrap owner, #733)."}}}}},
        {"schemas", {{"error_envelope", error_envelope_schema()}}}};
    // Auth is always required (#799) — no anonymous alternative.
    document["security"] = nlohmann::json::array(
        {{{"bearer_token", nlohmann::json::array()}}});

    nlohmann::json paths = nlohmann::json::object();
    for (const auto& capability : registry.list()) {
        if (!visible(*capability)) continue;
        nlohmann::json operation;
        operation["operationId"] = capability->name;
        operation["summary"] = capability->title;
        std::string description = capability->description;
        description += " Requires " +
                       std::string(zetta::governance::to_string(
                           capability->required_operation.domain)) +
                       ":" + capability->required_operation.action + ".";
        if (capability->kind == CapabilityKind::Job) {
            description +=
                " Long-running: returns the engine's durable handle "
                "immediately; poll progress through the read surface.";
        }
        if (capability->requires_consent) {
            // Conditional, and the spec is generated without a node, so it
            // cannot say which way this one is set (#995). Name the condition
            // instead of asserting the outcome.
            description +=
                " Consent-gated: on a node started with --governance this never "
                "executes on a plain call — it returns a one-time consent "
                "challenge that a DIFFERENT principal must approve (#621). "
                "Consent enforcement is off by default.";
        }
        operation["description"] = std::move(description);
        operation["tags"] =
            nlohmann::json::array({std::string(to_string(capability->kind))});

        const std::string method = rest_method(*capability);
        if (method == "GET") {
            operation["parameters"] = query_parameters(capability->input_schema);
        } else {
            const nlohmann::json required = capability->input_schema.value(
                "required", nlohmann::json::array());
            operation["requestBody"] = {
                {"required", !required.empty()},
                {"content",
                 {{"application/json",
                   {{"schema",
                     request_body_schema(capability->input_schema)}}}}}};
        }

        nlohmann::json responses;
        responses["200"] = {
            {"description", "the call succeeded"},
            {"content",
             {{"application/json",
               {{"schema",
                 success_envelope_schema(capability->output_schema)}}}}}};
        responses["4XX"] = error_response(
            "the input, authorization, or addressed entity is at fault; the "
            "error carries the engine's reason verbatim");
        responses["5XX"] = error_response(
            "the engine refused or the owning loop is saturated (busy)");
        operation["responses"] = std::move(responses);

        if (method == "GET" && capability->post_alias) {
            auto post = operation;
            post["operationId"] = capability->name + "_post";
            post.erase("parameters");
            post["requestBody"] = {{"required", true}, {"content", {{"application/json",
                {{"schema", request_body_schema(capability->input_schema)}}}}}};
            paths[rest_path(*capability)]["post"] = std::move(post);
        }

        paths[rest_path(*capability)]
             [method == "GET" ? "get" : "post"] = std::move(operation);
    }

    // The governed serving routes (#648, docs/api/serving.md): per-entity
    // endpoints published by expose_model / expose_index. Path-templated, so
    // the registry loop above cannot derive them — they are declared here from
    // the SAME serving schemas the boundary validates (zero drift), and served
    // by the adapter's two hand-registered routes.
    const auto name_parameter = [](std::string_view description) {
        return nlohmann::json{{"name", "name"},
                              {"in", "path"},
                              {"required", true},
                              {"description", std::string(description)},
                              {"schema", {{"type", "string"}}}};
    };
    const auto serving_responses = [](const nlohmann::json& output_schema) {
        nlohmann::json responses;
        responses["200"] = {
            {"description", "the request was served"},
            {"content",
             {{"application/json",
               {{"schema", success_envelope_schema(output_schema)}}}}}};
        responses["4XX"] = error_response(
            "the key lacks the serving capability, the entity does not exist "
            "or is not exposed, or the input is at fault");
        responses["5XX"] = error_response(
            "the engine refused or the owning loop is saturated (busy)");
        return responses;
    };

    nlohmann::json predict_operation;
    predict_operation["operationId"] = "predict";
    predict_operation["summary"] = "Predict (served model endpoint)";
    predict_operation["description"] =
        "Batch-score rows through an EXPOSED model and return the ranked "
        "at-risk list. The route exists only while the model is exposed "
        "(expose_model / unexpose_model); access requires data:predict. "
        "Every request is a serve:predict trace span and is audited.";
    predict_operation["tags"] = nlohmann::json::array({"serving"});
    predict_operation["parameters"] = nlohmann::json::array(
        {name_parameter("the model's uuid, or its display name")});
    predict_operation["requestBody"] = {
        {"required", true},
        {"content",
         {{"application/json",
           {{"schema", request_body_schema(predict_request_schema())}}}}}};
    predict_operation["responses"] = serving_responses(predict_response_schema());
    paths["/v1/models/{name}/predict"]["post"] = std::move(predict_operation);

    nlohmann::json serve_search_operation;
    serve_search_operation["operationId"] = "serve_search";
    serve_search_operation["summary"] = "Search (served index endpoint)";
    serve_search_operation["description"] =
        "Free-text search over an EXPOSED index — the distributed "
        "scatter-gather path underneath, with per-node provenance. The route "
        "exists only while the index is exposed (expose_index / "
        "unexpose_index); access requires data:search. Every request is a "
        "serve:search trace span and is audited.";
    serve_search_operation["tags"] = nlohmann::json::array({"serving"});
    nlohmann::json search_parameters =
        query_parameters(serve_search_request_schema());
    search_parameters.insert(
        search_parameters.begin(),
        name_parameter("the index's uuid, or an indexed table's name"));
    serve_search_operation["parameters"] = std::move(search_parameters);
    serve_search_operation["responses"] =
        serving_responses(search_response_schema());
    paths["/v1/indexes/{name}/search"]["get"] = std::move(serve_search_operation);

    // Published pipelines (zetta#1289): the served run and its status read.
    nlohmann::json run_pipeline_operation;
    run_pipeline_operation["operationId"] = "serve_run_pipeline";
    run_pipeline_operation["summary"] = "Run a published pipeline (served pipeline endpoint)";
    run_pipeline_operation["description"] =
        "Run a PUBLISHED pipeline's pinned version with the body as its declared input "
        "(expose_pipeline input_schema). The route exists only while the pipeline is exposed; "
        "access requires data:run_pipeline and read access to the tables it reads. Rate and "
        "size limited per caller (429, 413). Waits up to the pipeline's sync_timeout_ms for "
        "the run to finish (200 with its outputs) unless ?wait=false, else answers 202 with "
        "the run id and status_endpoint. Every request is a serve:run_pipeline trace span "
        "and is audited.";
    run_pipeline_operation["tags"] = nlohmann::json::array({"serving"});
    run_pipeline_operation["parameters"] = nlohmann::json::array(
        {name_parameter("the pipeline's uuid, or its name when one exposed pipeline has it"),
         {{"name", "wait"},
          {"in", "query"},
          {"required", false},
          {"description", "false: answer at once with the run id (202)"},
          {"schema", {{"type", "boolean"}}}}});
    run_pipeline_operation["requestBody"] = {
        {"required", false},
        {"content",
         {{"application/json", {{"schema", {{"type", "object"}}}}}}}};
    run_pipeline_operation["responses"] = serving_responses(pipeline_run_response_schema());
    run_pipeline_operation["responses"]["202"] = {
        {"description", "the run started and is still running"},
        {"content",
         {{"application/json",
           {{"schema", success_envelope_schema(pipeline_run_response_schema())}}}}}};
    paths["/v1/pipelines/{name}/run"]["post"] = std::move(run_pipeline_operation);

    nlohmann::json pipeline_run_operation;
    pipeline_run_operation["operationId"] = "serve_pipeline_run";
    pipeline_run_operation["summary"] = "A published pipeline's run (served pipeline endpoint)";
    pipeline_run_operation["description"] =
        "The status and outputs of a run a published pipeline served to the same caller; "
        "another caller's run is not found. Access requires data:run_pipeline.";
    pipeline_run_operation["tags"] = nlohmann::json::array({"serving"});
    pipeline_run_operation["parameters"] = nlohmann::json::array(
        {name_parameter("the pipeline's uuid, or its name"),
         {{"name", "run_id"},
          {"in", "path"},
          {"required", true},
          {"description", "the run id the served run answered"},
          {"schema", {{"type", "string"}}}}});
    pipeline_run_operation["responses"] = serving_responses(pipeline_run_response_schema());
    paths["/v1/pipelines/{name}/runs/{run_id}"]["get"] = std::move(pipeline_run_operation);

    // The object capability's byte routes and bucket-route aliases (#1005):
    // path-templated with raw bodies, declared beside the capabilities they
    // render (object_capabilities.cpp).
    for (auto& [path, item] : object_route_paths().items()) paths[path] = std::move(item);

    document["paths"] = std::move(paths);
    return document;
}

}  // namespace zetta::api
