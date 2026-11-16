// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"
#include "zetta/api/rest.hpp"

// The spec index (#646, docs/api/specs-index.md; the AX contract is
// docs/product/experience/ax.md): one navigable object index over EVERY
// specification the cluster speaks — the OpenAPI document (#618), each
// capability's input/output schema from the registry, and every schema-as-code
// boundary schema (zetta::schemas::boundary_schemas()) — so agents and codegen
// discover every shape without reading source. Everything is served from the
// C++ accessors (schema-as-code): nothing here reads schemas/*.json from disk,
// so a node binary that ships without the generated artifacts serves the same
// index.
//
// Rendered three ways from these two functions, zero drift: the list_specs /
// get_spec capabilities (MCP tools + their derived /v1/<name> routes) and the
// plain GET /v1/specs + /v1/specs/<name> routes in the Beast2 adapter TU.
// HTTP-free by design (zetta-core).
namespace zetta::api {

// The index document: { count, specs: [ { name, kind, href, … } ] }.
// Kinds: "openapi" (the one #618 document) · "capability_input" /
// "capability_output" (named "<capability>.input" / "<capability>.output",
// carrying `capability`) · "boundary" (named after the entity, carrying the
// generated `file`). `href` is the raw-document route "/v1/specs/<name>".
nlohmann::json spec_index(const Registry& registry);
// The capabilities `acting_key` may call (#766): for a token-resolved user, the
// per-tool mcp:call scope (Node::may_call_tool), the rule tools/list and
// Registry::call apply; the trusted local session and a legacy AccessKey see
// every capability, as they do in tools/list. Reads Node state: call the
// predicate on the owning thread.
CapabilityVisible callable_by(const Node& node, std::string acting_key,
                              bool acting_is_user);

// The index with only the capabilities `visible` admits (#766): their
// input/output entries, and an "openapi" entry whose document holds only them.
// Boundary schemas are entity shapes, not tools, and are always listed.
nlohmann::json spec_index(const Registry& registry, const CapabilityVisible& visible);

// One spec: { name, kind, schema, … } with the same per-kind extras as the
// index entry; `schema` is the JSON Schema document itself (the OpenAPI 3.1
// document for "openapi"). std::nullopt on an unknown name — the caller
// answers not_found and points at the index.
std::optional<nlohmann::json> find_spec(const Registry& registry,
                                        const std::string& name);
// The same lookup under `visible` (#766): a capability the caller may not call
// is not_found, exactly like a name that does not exist, and "openapi" renders
// only the visible capabilities.
std::optional<nlohmann::json> find_spec(const Registry& registry,
                                        const std::string& name,
                                        const CapabilityVisible& visible);

}  // namespace zetta::api
