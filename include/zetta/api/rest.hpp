// SPDX-License-Identifier: MIT

#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"

// The REST rendering of the capability registry (#618,
// docs/api/rest-openapi.md): the route table, the query-string -> input
// coercion, the HTTP status mapping, and the OpenAPI document are all derived
// mechanically from the registered entries — one registry, zero drift. This
// header is HTTP-free (zetta-core): the Beast2 adapter TU
// (src/core/api/http_server.cpp) is its only wire-facing consumer, and tests
// call it directly.
namespace zetta::api {

// Every capability serves at "/v1/<name>". Reads render as GET (input in the
// query string); actions and jobs render as POST (input as the JSON body).
std::string rest_path(const Capability& capability);
std::string rest_method(const Capability& capability);  // "GET" | "POST"

// Build a capability input document from decoded query parameters, coercing
// each value by its declared type in the entry's input schema: integer/number
// and boolean values parse from their literal spelling; array/object values
// parse as JSON. Undeclared keys stay strings so Registry::call rejects them
// with the schema's own path-qualified reason. A value that does not parse as
// its declared type also stays a string for the same reason.
nlohmann::json rest_input_from_query(
    const Capability& capability,
    const std::vector<std::pair<std::string, std::string>>& query_parameters);
// The same coercion against a bare input schema — for routes that are not
// registry entries (the served index-search endpoint, #648).
nlohmann::json rest_input_from_query(
    const nlohmann::json& input_schema,
    const std::vector<std::pair<std::string, std::string>>& query_parameters);

// The HTTP status for a finished call: 200 on ok; error codes map to
// invalid_input -> 400, unauthorized/consent_required -> 403,
// not_found/unknown_capability -> 404, engine_error -> 422, busy -> 503,
// anything else -> 500.
int rest_status(const ApiResult& result);

// The full response body — the same envelope structuredContent carries over
// MCP: { ok, value | error{code,message,path}, trace_id, span_id,
// duration_ms }.
nlohmann::json rest_envelope(const ApiResult& result);

// The OpenAPI 3.1 document generated from the registry: one path per
// capability (GET for reads, POST for actions/jobs), parameters/requestBody
// from the input schema (plus the reserved `intent` field), responses from
// the output schema wrapped in the envelope. Schema-as-code: this generator
// is the source of truth and schemas/openapi.json is its drift-checked
// artifact (ZETTA_WRITE_SCHEMAS=1 regenerates — the test_schemas.cpp
// pattern). Served at GET /v1/openapi.json.
nlohmann::json openapi_document(const Registry& registry);

// Which capabilities a rendering shows. The served-per-caller documents (the
// /v1/specs index and its "openapi" entry, list_specs, get_spec) pass the
// caller's per-tool scope (#766), the same rule tools/list applies, so a user
// granted one tool discovers that tool only.
using CapabilityVisible = std::function<bool(const Capability&)>;

// The OpenAPI document with only the capabilities `visible` admits.
nlohmann::json openapi_document(const Registry& registry,
                                const CapabilityVisible& visible);

}  // namespace zetta::api
