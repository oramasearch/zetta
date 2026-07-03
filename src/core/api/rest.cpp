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
    const Capability& capability,
    const std::vector<std::pair<std::string, std::string>>& query_parameters) {
    nlohmann::json input = nlohmann::json::object();
    const nlohmann::json properties =
        capability.input_schema.value("properties", nlohmann::json::object());
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

int rest_status(const ApiResult& result) {
    if (result.ok) return 200;
    const std::string& code = result.error.code;
    if (code == "invalid_input") return 400;
    if (code == "unauthorized") return 403;
    if (code == "consent_required") return 403;
    if (code == "not_found") return 404;
    if (code == "unknown_capability") return 404;
    if (code == "engine_error") return 422;
    if (code == "busy") return 503;
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

// The entry's input schema with `intent` added — the request-body schema of a
// POST rendering.
nlohmann::json request_body_schema(const Capability& capability) {
    nlohmann::json schema = capability.input_schema;
    if (schema.is_object()) {
        schema["properties"]["intent"] = intent_property_schema();
    }
    return schema;
}

// The GET rendering's parameter list: one query parameter per input-schema
// property (plus `intent`). Scalar parameters carry their property schema;
// array/object parameters are JSON-encoded in the query string, which OpenAPI
// expresses as a `content` parameter.
nlohmann::json query_parameters(const Capability& capability) {
    nlohmann::json parameters = nlohmann::json::array();
    const nlohmann::json properties =
        capability.input_schema.value("properties", nlohmann::json::object());
    const nlohmann::json required =
        capability.input_schema.value("required", nlohmann::json::array());
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
// entry's output schema (an untyped object when none is declared).
nlohmann::json success_envelope_schema(const Capability& capability) {
    nlohmann::json value_schema = capability.output_schema;
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
                 "engine_error (422) · busy (503)"}}},
              {"message",
               {{"type", "string"},
                {"description", "the engine's reason, verbatim"}}},
              {"path",
               {{"type", "string"},
                {"description",
                 "JSON pointer into the input when it is at fault"}}}}}}},
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
             "the AccessKey id (docs/api/registry-design.md: the bearer "
             "token IS the key). Optional on the default loopback bind — a "
             "tokenless local call runs as the audited unauthenticated-local "
             "session."}}}}},
        {"schemas", {{"error_envelope", error_envelope_schema()}}}};
    // A token when configured, or the audited unauthenticated-local session.
    document["security"] = nlohmann::json::array(
        {{{"bearer_token", nlohmann::json::array()}}, nlohmann::json::object()});

    nlohmann::json paths = nlohmann::json::object();
    for (const auto& capability : registry.list()) {
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
            description +=
                " Consent-gated: never executes on a plain call (#621).";
        }
        operation["description"] = std::move(description);
        operation["tags"] =
            nlohmann::json::array({std::string(to_string(capability->kind))});

        const std::string method = rest_method(*capability);
        if (method == "GET") {
            operation["parameters"] = query_parameters(*capability);
        } else {
            const nlohmann::json required = capability->input_schema.value(
                "required", nlohmann::json::array());
            operation["requestBody"] = {
                {"required", !required.empty()},
                {"content",
                 {{"application/json",
                   {{"schema", request_body_schema(*capability)}}}}}};
        }

        nlohmann::json responses;
        responses["200"] = {
            {"description", "the call succeeded"},
            {"content",
             {{"application/json",
               {{"schema", success_envelope_schema(*capability)}}}}}};
        responses["4XX"] = error_response(
            "the input, authorization, or addressed entity is at fault; the "
            "error carries the engine's reason verbatim");
        responses["5XX"] = error_response(
            "the engine refused or the owning loop is saturated (busy)");
        operation["responses"] = std::move(responses);

        paths[rest_path(*capability)]
             [method == "GET" ? "get" : "post"] = std::move(operation);
    }
    document["paths"] = std::move(paths);
    return document;
}

}  // namespace zetta::api
