// SPDX-License-Identifier: MIT

// The spec index (#646, docs/api/specs-index.md): /v1/specs +
// list_specs/get_spec serve the OpenAPI document plus EVERY JSON Schema in
// the system — capability input/output schemas and all schema-as-code
// boundary schemas — from their C++ accessors, drift-checked through the same
// zetta::schemas::boundary_schemas() table test_schemas.cpp regenerates the
// files from.
#include <gtest/gtest.h>

#include <set>
#include <string>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/specs.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/schemas/registry.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

Registry core_registry() {
    Registry registry;
    zetta::api::register_core_capabilities(registry);
    return registry;
}

// The acceptance rule (#646): the index enumerates the OpenAPI document, an
// input (and output) schema for every registered capability, and 55+ boundary
// schemas — every entry carrying a name, a kind, and a link.
TEST(SpecIndex, EnumeratesEverySchemaInTheSystem) {
    const Registry registry = core_registry();
    const nlohmann::json index = zetta::api::spec_index(registry);
    const nlohmann::json& specs = index.at("specs");
    EXPECT_EQ(index.at("count").get<std::size_t>(), specs.size());

    std::set<std::string> names;
    std::size_t openapi_entries = 0;
    std::size_t input_entries = 0;
    std::size_t output_entries = 0;
    std::size_t boundary_entries = 0;
    for (const auto& entry : specs) {
        EXPECT_TRUE(names.insert(entry.at("name").get<std::string>()).second)
            << "duplicate spec name " << entry.at("name");
        const std::string kind = entry.at("kind").get<std::string>();
        EXPECT_EQ(entry.at("href"),
                  "/v1/specs/" + entry.at("name").get<std::string>());
        if (kind == "openapi") ++openapi_entries;
        if (kind == "capability_input") ++input_entries;
        if (kind == "capability_output") ++output_entries;
        if (kind == "boundary") ++boundary_entries;
    }
    EXPECT_EQ(openapi_entries, 1u);
    EXPECT_EQ(input_entries, registry.size());
    EXPECT_EQ(output_entries, registry.size());  // every entry documents one
    // The boundary set is exactly the drift-checked table — and the issue's
    // acceptance floor.
    EXPECT_EQ(boundary_entries, zetta::schemas::boundary_schemas().size());
    EXPECT_GE(boundary_entries, 55u);
}

// Every index entry is fetchable, and what get_spec serves for a boundary
// name is the accessor's schema itself — the same object the drift check
// regenerates schemas/*.json from (served from code, never from disk).
TEST(SpecIndex, EveryEntryIsFetchableFromItsAccessor) {
    const Registry registry = core_registry();
    const nlohmann::json index = zetta::api::spec_index(registry);
    for (const auto& entry : index.at("specs")) {
        const std::string name = entry.at("name").get<std::string>();
        const auto spec = zetta::api::find_spec(registry, name);
        ASSERT_TRUE(spec.has_value()) << name;
        EXPECT_EQ(spec->at("name"), name);
        EXPECT_EQ(spec->at("kind"), entry.at("kind"));
        ASSERT_TRUE(spec->contains("schema")) << name;
    }
    for (const auto& boundary : zetta::schemas::boundary_schemas()) {
        const auto spec =
            zetta::api::find_spec(registry, std::string(boundary.name));
        ASSERT_TRUE(spec.has_value()) << boundary.name;
        EXPECT_EQ(spec->at("schema"), boundary.schema()) << boundary.name;
        EXPECT_EQ(spec->at("file"), boundary.file) << boundary.name;
    }
    EXPECT_FALSE(
        zetta::api::find_spec(registry, "no_such_spec").has_value());
    // ".output" of a real capability resolves; of a nonsense name does not.
    EXPECT_TRUE(zetta::api::find_spec(registry, "sql.output").has_value());
    EXPECT_FALSE(
        zetta::api::find_spec(registry, "no_such_capability.input").has_value());
}

// Every boundary schema loads in the same validator the boundaries use — the
// index can never serve a document the engine itself could not validate with.
TEST(SpecIndex, EveryBoundarySchemaLoadsInTheValidator) {
    for (const auto& boundary : zetta::schemas::boundary_schemas()) {
        nlohmann::json_schema::json_validator validator;
        EXPECT_NO_THROW(validator.set_root_schema(boundary.schema()))
            << boundary.name;
        EXPECT_TRUE(std::string(boundary.file).ends_with(".schema.json"))
            << boundary.name;
    }
}

// The registry names are unique and resolvable through find_boundary_schema.
TEST(SpecIndex, BoundaryRegistryNamesAreUniqueAndResolvable) {
    std::set<std::string_view> names;
    for (const auto& boundary : zetta::schemas::boundary_schemas()) {
        EXPECT_TRUE(names.insert(boundary.name).second)
            << "duplicate boundary schema name " << boundary.name;
        const auto* found = zetta::schemas::find_boundary_schema(boundary.name);
        ASSERT_NE(found, nullptr) << boundary.name;
        EXPECT_EQ(found->file, boundary.file);
    }
    EXPECT_EQ(zetta::schemas::find_boundary_schema("no_such_schema"), nullptr);
}

// The capability rendering (a layer-1 journey): list_specs returns the index,
// get_spec returns one document, an unknown name is a domain not_found that
// points back at the index.
TEST(SpecIndex, ListSpecsAndGetSpecCapabilities) {
    zetta::test::TempDir dir;
    zetta::Node node{
        zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());
    Registry registry;
    zetta::api::register_core_capabilities(registry);
    auto trace = zetta::observability::make_root_trace();

    const auto call = [&](const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    };

    const ApiResult listed = call("list_specs", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value, zetta::api::spec_index(registry));

    const ApiResult fetched = call("get_spec", {{"name", "run"}});
    ASSERT_TRUE(fetched.ok) << fetched.error.message;
    EXPECT_EQ(fetched.value.at("kind"), "boundary");
    EXPECT_EQ(fetched.value.at("schema"),
              zetta::schemas::find_boundary_schema("run")->schema());

    const ApiResult openapi = call("get_spec", {{"name", "openapi"}});
    ASSERT_TRUE(openapi.ok) << openapi.error.message;
    EXPECT_EQ(openapi.value.at("schema").at("openapi"), "3.1.0");

    const ApiResult missing = call("get_spec", {{"name", "nope"}});
    ASSERT_FALSE(missing.ok);
    EXPECT_EQ(missing.error.code, "not_found");
    EXPECT_NE(missing.error.message.find("list_specs"), std::string::npos);

    node.stop();
}

}  // namespace
