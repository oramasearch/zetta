// The spec index (#646, docs/api/specs-index.md): the OpenAPI document, every
// capability input/output schema, and every boundary schema rendered as one
// navigable object index. Derived mechanically from the capability registry +
// zetta::schemas::boundary_schemas() — the same table tests/test_schemas.cpp
// drift-checks — so what an agent discovers is exactly what the build keeps
// honest. HTTP-free: the Beast2 adapter TU and the list_specs/get_spec
// handlers are the only consumers.
#include "zetta/api/specs.hpp"

#include <utility>

#include "zetta/api/rest.hpp"
#include "zetta/node.hpp"
#include "zetta/schemas/registry.hpp"

namespace zetta::api {

namespace {

constexpr std::string_view kOpenApiSpecName = "openapi";

std::string spec_href(std::string_view name) {
    return "/v1/specs/" + std::string(name);
}

nlohmann::json openapi_index_entry() {
    return nlohmann::json{
        {"name", kOpenApiSpecName},
        {"kind", "openapi"},
        {"href", spec_href(kOpenApiSpecName)},
        {"description",
         "the OpenAPI 3.1 document covering every /v1/<name> route (also "
         "served at /v1/openapi.json)"}};
}

bool every_capability(const Capability&) { return true; }

}  // namespace

CapabilityVisible callable_by(const Node& node, std::string acting_key,
                              bool acting_is_user) {
    if (!acting_is_user) return every_capability;
    return [&node, acting_key = std::move(acting_key)](const Capability& capability) {
        return node.may_call_tool(acting_key, /*acting_is_user=*/true, capability.name);
    };
}

nlohmann::json spec_index(const Registry& registry) {
    return spec_index(registry, every_capability);
}

nlohmann::json spec_index(const Registry& registry, const CapabilityVisible& visible) {
    nlohmann::json specs = nlohmann::json::array();
    specs.push_back(openapi_index_entry());

    for (const auto& capability : registry.list()) {
        if (!visible(*capability)) continue;
        specs.push_back({{"name", capability->name + ".input"},
                         {"kind", "capability_input"},
                         {"capability", capability->name},
                         {"href", spec_href(capability->name + ".input")}});
        if (capability->output_schema.is_object() &&
            !capability->output_schema.empty()) {
            specs.push_back(
                {{"name", capability->name + ".output"},
                 {"kind", "capability_output"},
                 {"capability", capability->name},
                 {"href", spec_href(capability->name + ".output")}});
        }
    }

    for (const auto& boundary : zetta::schemas::boundary_schemas()) {
        specs.push_back({{"name", boundary.name},
                         {"kind", "boundary"},
                         {"file", boundary.file},
                         {"href", spec_href(boundary.name)}});
    }

    return nlohmann::json{{"count", specs.size()}, {"specs", std::move(specs)}};
}

std::optional<nlohmann::json> find_spec(const Registry& registry,
                                        const std::string& name) {
    return find_spec(registry, name, every_capability);
}

std::optional<nlohmann::json> find_spec(const Registry& registry,
                                        const std::string& name,
                                        const CapabilityVisible& visible) {
    if (name == kOpenApiSpecName) {
        nlohmann::json spec = openapi_index_entry();
        spec.erase("href");
        spec["schema"] = openapi_document(registry, visible);
        return spec;
    }

    // "<capability>.input" / "<capability>.output" — the registry's schemas.
    for (const std::string_view suffix : {".input", ".output"}) {
        if (!name.ends_with(suffix)) continue;
        const std::string capability_name =
            name.substr(0, name.size() - suffix.size());
        const auto capability = registry.find(capability_name);
        if (!capability || !visible(*capability)) break;
        const bool wants_input = suffix == ".input";
        const nlohmann::json& schema =
            wants_input ? capability->input_schema : capability->output_schema;
        if (!wants_input && (!schema.is_object() || schema.empty())) break;
        return nlohmann::json{
            {"name", name},
            {"kind", wants_input ? "capability_input" : "capability_output"},
            {"capability", capability_name},
            {"schema", schema}};
    }

    if (const auto* boundary = zetta::schemas::find_boundary_schema(name)) {
        return nlohmann::json{{"name", boundary->name},
                              {"kind", "boundary"},
                              {"file", boundary->file},
                              {"schema", boundary->schema()}};
    }
    return std::nullopt;
}

}  // namespace zetta::api
