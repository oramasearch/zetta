// The REST rendering + OpenAPI generation (#618, docs/api/rest-openapi.md):
// the route table, query coercion, status mapping, and the OpenAPI document
// are all derived mechanically from the capability registry — and
// schemas/openapi.json is the generator's drift-checked artifact, exactly the
// test_schemas.cpp pattern (ZETTA_WRITE_SCHEMAS=1 regenerates).
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <string>

#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/rest.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/version.hpp"

namespace {

using zetta::api::Capability;
using zetta::api::CapabilityKind;
using zetta::api::Registry;

Registry core_registry() {
    Registry registry;
    zetta::api::register_core_capabilities(registry);
    return registry;
}

TEST(RestRendering, RouteTableDerivesFromKind) {
    const Registry registry = core_registry();
    for (const auto& capability : registry.list()) {
        EXPECT_EQ(zetta::api::rest_path(*capability), "/v1/" + capability->name);
        if (capability->kind == CapabilityKind::Read) {
            EXPECT_EQ(zetta::api::rest_method(*capability), "GET")
                << capability->name;
        } else {
            EXPECT_EQ(zetta::api::rest_method(*capability), "POST")
                << capability->name;
        }
    }
}

TEST(RestRendering, QueryValuesCoerceByTheInputSchema) {
    const Registry registry = core_registry();
    const auto list_traces = registry.find("list_traces");
    ASSERT_TRUE(list_traces);

    // Declared integer: the literal parses; a non-integer stays a string so
    // the schema boundary rejects it with its own path-qualified reason.
    auto input = zetta::api::rest_input_from_query(
        *list_traces, {{"limit", "25"}});
    EXPECT_TRUE(input.at("limit").is_number_integer());
    EXPECT_EQ(input.at("limit"), 25);
    input = zetta::api::rest_input_from_query(*list_traces,
                                              {{"limit", "not-a-number"}});
    EXPECT_TRUE(input.at("limit").is_string());

    // Declared boolean (run.coordinate) and undeclared keys.
    const auto run = registry.find("run");
    ASSERT_TRUE(run);
    input = zetta::api::rest_input_from_query(
        *run, {{"coordinate", "false"}, {"project", "p"}, {"intent", "why"}});
    EXPECT_TRUE(input.at("coordinate").is_boolean());
    EXPECT_FALSE(input.at("coordinate").get<bool>());
    EXPECT_EQ(input.at("project"), "p");
    EXPECT_EQ(input.at("intent"), "why");

    // Declared array (submit_feedback.trace_ids): JSON-encoded in the query.
    const auto submit_feedback = registry.find("submit_feedback");
    ASSERT_TRUE(submit_feedback);
    input = zetta::api::rest_input_from_query(
        *submit_feedback, {{"trace_ids", "[\"abc\",\"def\"]"}});
    EXPECT_TRUE(input.at("trace_ids").is_array());
    EXPECT_EQ(input.at("trace_ids").size(), 2u);
}

TEST(RestRendering, ErrorCodesMapToHttpStatuses) {
    using zetta::api::ApiResult;
    EXPECT_EQ(zetta::api::rest_status(ApiResult::success({})), 200);
    EXPECT_EQ(zetta::api::rest_status(ApiResult::failure("invalid_input", "")),
              400);
    EXPECT_EQ(zetta::api::rest_status(ApiResult::failure("unauthorized", "")),
              403);
    EXPECT_EQ(
        zetta::api::rest_status(ApiResult::failure("consent_required", "")),
        403);
    EXPECT_EQ(zetta::api::rest_status(ApiResult::failure("not_found", "")), 404);
    EXPECT_EQ(
        zetta::api::rest_status(ApiResult::failure("unknown_capability", "")),
        404);
    EXPECT_EQ(zetta::api::rest_status(ApiResult::failure("engine_error", "")),
              422);
    EXPECT_EQ(zetta::api::rest_status(ApiResult::failure("busy", "")), 503);
    EXPECT_EQ(zetta::api::rest_status(ApiResult::failure("novel_code", "")),
              500);
}

// One registry, two renderings, zero drift — including on the FAILURE path.
// A structured failure payload (the consent challenge behind a
// "consent_required" refusal, #621) rode the MCP envelope and was dropped from
// the REST one, so the same call answered differently depending on which
// surface asked. A caller over REST got a challenge id in prose and no
// challenge (#995).
TEST(RestRendering, AStructuredFailurePayloadRidesTheRestEnvelope) {
    using zetta::api::ApiResult;

    ApiResult refused = ApiResult::failure("consent_required", "did not run");
    refused.error.details = nlohmann::json{{"id", "challenge-1"},
                                           {"capability", "cloud_inspect"}};
    const nlohmann::json envelope = zetta::api::rest_envelope(refused);
    ASSERT_TRUE(envelope.at("error").contains("details")) << envelope.dump();
    EXPECT_EQ(envelope["error"]["details"]["id"], "challenge-1");

    // A plain failure carries no `details` key at all — absent, not null.
    const nlohmann::json plain =
        zetta::api::rest_envelope(ApiResult::failure("engine_error", "boom"));
    EXPECT_FALSE(plain.at("error").contains("details")) << plain.dump();

    // And the documented envelope schema admits it, so the OpenAPI document
    // describes what the wire actually carries.
    EXPECT_TRUE(zetta::api::openapi_document(core_registry())
                    .at("components")
                    .at("schemas")
                    .at("error_envelope")
                    .at("properties")
                    .at("error")
                    .at("properties")
                    .contains("details"));
}

// Every core capability documents its output (#618): a non-empty draft-07
// object schema, and both schemas load in the same validator the boundary
// uses — i.e. the OpenAPI document embeds only valid schemas.
TEST(RestRendering, EveryCapabilityHasValidInputAndOutputSchemas) {
    const Registry registry = core_registry();
    for (const auto& capability : registry.list()) {
        ASSERT_TRUE(capability->output_schema.is_object() &&
                    !capability->output_schema.empty())
            << capability->name << " has no output schema";
        nlohmann::json_schema::json_validator input_validator;
        EXPECT_NO_THROW(input_validator.set_root_schema(
            capability->input_schema))
            << capability->name;
        nlohmann::json_schema::json_validator output_validator;
        EXPECT_NO_THROW(output_validator.set_root_schema(
            capability->output_schema))
            << capability->name;
    }
}

TEST(OpenApi, RunReportSchemaPreservesMethodologyOutputShapes) {
    const Registry registry = core_registry();
    const auto report = registry.find("get_run_report");
    ASSERT_TRUE(report);
    const nlohmann::json& properties = report->output_schema.at("properties");
    for (const char* field : {"uncertainty", "meta_model_weights", "privacy", "topics"}) {
        EXPECT_TRUE(properties.contains(field)) << field;
    }
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(report->output_schema);
    nlohmann::json facts = {{"run", nlohmann::json::object()},
                            {"model", nlohmann::json::object()},
                            {"features", {{"used", nlohmann::json::array()},
                                          {"excluded", nlohmann::json::array()}}},
                            {"outputs", nlohmann::json::array()}};
    EXPECT_NO_THROW(validator.validate(facts));
    facts["outputs"] = {{"survival", "survival_results.curves"}};
    EXPECT_NO_THROW(validator.validate(facts));
    facts.erase("features");
    EXPECT_THROW(validator.validate(facts), std::invalid_argument);
}

TEST(OpenApi, DocumentMatchesTheRegistry) {
    const Registry registry = core_registry();
    const nlohmann::json document = zetta::api::openapi_document(registry);

    EXPECT_EQ(document.at("openapi"), "3.1.0");
    EXPECT_EQ(document.at("info").at("version"), zetta::kVersion);
    EXPECT_TRUE(document.at("components")
                    .at("schemas")
                    .contains("error_envelope"));
    EXPECT_TRUE(
        document.at("components").at("securitySchemes").contains("bearer_token"));

    const nlohmann::json& paths = document.at("paths");
    // One path per registry entry, plus the two path-templated serving routes
    // (#648) the registry rendering cannot express.
    EXPECT_EQ(paths.size(), registry.size() + 2);
    for (const auto& capability : registry.list()) {
        const std::string path = zetta::api::rest_path(*capability);
        ASSERT_TRUE(paths.contains(path)) << path;
        const bool is_read = capability->kind == CapabilityKind::Read;
        const std::string method = is_read ? "get" : "post";
        ASSERT_TRUE(paths.at(path).contains(method)) << path;
        const nlohmann::json& operation = paths.at(path).at(method);
        EXPECT_EQ(operation.at("operationId"), capability->name);
        EXPECT_EQ(operation.at("summary"), capability->title);
        // The description carries the governance line, mechanically.
        const std::string description =
            operation.at("description").get<std::string>();
        EXPECT_NE(description.find(capability->required_operation.action),
                  std::string::npos)
            << capability->name;

        if (is_read) {
            // One query parameter per input property, plus the reserved
            // intent field.
            const nlohmann::json properties = capability->input_schema.value(
                "properties", nlohmann::json::object());
            ASSERT_TRUE(operation.contains("parameters")) << path;
            EXPECT_EQ(operation.at("parameters").size(), properties.size() + 1)
                << path;
            bool saw_intent = false;
            for (const auto& parameter : operation.at("parameters")) {
                EXPECT_EQ(parameter.at("in"), "query");
                if (parameter.at("name") == "intent") saw_intent = true;
            }
            EXPECT_TRUE(saw_intent) << path;
        } else {
            ASSERT_TRUE(operation.contains("requestBody")) << path;
            const nlohmann::json& body_schema =
                operation.at("requestBody").at("content")
                    .at("application/json").at("schema");
            EXPECT_TRUE(body_schema.at("properties").contains("intent"))
                << path;
        }

        // The 200 response wraps the entry's output schema in the envelope;
        // errors reference the shared error envelope.
        const nlohmann::json& success =
            operation.at("responses").at("200").at("content")
                .at("application/json").at("schema");
        EXPECT_EQ(success.at("properties").at("value"),
                  capability->output_schema)
            << capability->name;
        EXPECT_EQ(operation.at("responses").at("4XX").at("content")
                      .at("application/json").at("schema").at("$ref"),
                  "#/components/schemas/error_envelope");
    }

    // The serving routes (#648): declared from the same serving schemas the
    // boundary validates — a path-templated POST for predict (JSON body from
    // predict_request_schema) and GET for search (query parameters from
    // serve_search_request_schema), each with the `name` path parameter and
    // the envelope-wrapped serving response schema.
    ASSERT_TRUE(paths.contains("/v1/models/{name}/predict"));
    const nlohmann::json& predict =
        paths.at("/v1/models/{name}/predict").at("post");
    EXPECT_EQ(predict.at("operationId"), "predict");
    EXPECT_EQ(predict.at("parameters").at(0).at("in"), "path");
    EXPECT_EQ(predict.at("parameters").at(0).at("name"), "name");
    EXPECT_EQ(predict.at("requestBody").at("content").at("application/json")
                  .at("schema").at("properties").at("rows"),
              zetta::api::predict_request_schema().at("properties").at("rows"));
    EXPECT_EQ(predict.at("responses").at("200").at("content")
                  .at("application/json").at("schema").at("properties")
                  .at("value"),
              zetta::api::predict_response_schema());

    ASSERT_TRUE(paths.contains("/v1/indexes/{name}/search"));
    const nlohmann::json& serve_search =
        paths.at("/v1/indexes/{name}/search").at("get");
    EXPECT_EQ(serve_search.at("operationId"), "serve_search");
    EXPECT_EQ(serve_search.at("parameters").at(0).at("in"), "path");
    bool saw_query_parameter = false;
    for (const auto& parameter : serve_search.at("parameters")) {
        if (parameter.at("name") == "query") {
            saw_query_parameter = true;
            EXPECT_EQ(parameter.at("in"), "query");
            EXPECT_TRUE(parameter.at("required").get<bool>());
        }
    }
    EXPECT_TRUE(saw_query_parameter);
    EXPECT_EQ(serve_search.at("responses").at("200").at("content")
                  .at("application/json").at("schema").at("properties")
                  .at("value"),
              zetta::api::search_response_schema());
}

TEST(OpenApi, RunMetricCapabilitiesHaveRestSchemaParity) {
    const Registry registry = core_registry();
    const nlohmann::json document = zetta::api::openapi_document(registry);
    for (const char* name : {"log_run_metrics", "log_run_params",
                             "log_run_artifact", "get_run_metrics",
                             "compare_runs"}) {
        const auto capability = registry.find(name);
        ASSERT_TRUE(capability) << name;
        const std::string method =
            capability->kind == CapabilityKind::Read ? "get" : "post";
        const nlohmann::json& operation =
            document["paths"]["/v1/" + std::string(name)][method];
        EXPECT_EQ(operation["responses"]["200"]["content"]
                           ["application/json"]["schema"]["properties"]
                           ["value"],
                  capability->output_schema)
            << name;
        if (method == "post") {
            nlohmann::json actual = operation["requestBody"]["content"]
                                             ["application/json"]["schema"]
                                             ["properties"];
            actual.erase("intent");
            EXPECT_EQ(actual, capability->input_schema["properties"])
                << name;
        }
    }
}

TEST(OpenApi, StudyCapabilitiesHaveRestSchemaParity) {
    const Registry registry = core_registry();
    const nlohmann::json document = zetta::api::openapi_document(registry);
    for (const char* name : {"validate_study", "create_study", "get_study",
                             "list_studies",
                             "list_trials", "stop_study", "stop_trial",
                             "promote_trial", "delete_study",
                             "get_study_analysis"}) {
        const auto capability = registry.find(name);
        ASSERT_TRUE(capability) << name;
        const std::string method =
            capability->kind == CapabilityKind::Read ? "get" : "post";
        const nlohmann::json& operation =
            document["paths"]["/v1/" + std::string(name)][method];
        EXPECT_EQ(operation["responses"]["200"]["content"]
                           ["application/json"]["schema"]["properties"]
                           ["value"],
                  capability->output_schema)
            << name;
        if (method == "post") {
            nlohmann::json actual = operation["requestBody"]["content"]
                                             ["application/json"]["schema"]
                                             ["properties"];
            actual.erase("intent");
            EXPECT_EQ(actual, capability->input_schema["properties"])
                << name;
        }
    }
}

// The acceptance rule (#618): a registry entry added once appears in the REST
// route table and the OpenAPI document with no further wiring. (The MCP
// tools/list side of the same rule is covered by the protocol tests.)
TEST(OpenApi, ANewEntryAppearsInRoutesAndDocument) {
    Registry registry = core_registry();
    ASSERT_TRUE(registry.add(Capability{
        "frobnicate_widgets",
        "Frobnicate widgets",
        "Frobnicate every widget on this node.",
        nlohmann::json{{"type", "object"}, {"additionalProperties", false}},
        nlohmann::json{{"type", "object"}},
        CapabilityKind::Action,
        zetta::governance::Operation{
            zetta::governance::CapabilityDomain::Orchestration,
            "frobnicate_widgets"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        [](const nlohmann::json&, zetta::api::ApiContext&) {
            return zetta::api::ApiResult::success({});
        }}));
    const nlohmann::json document = zetta::api::openapi_document(registry);
    ASSERT_TRUE(document.at("paths").contains("/v1/frobnicate_widgets"));
    EXPECT_TRUE(
        document.at("paths").at("/v1/frobnicate_widgets").contains("post"));
}

// Schema-as-code: schemas/openapi.json is a generated artifact of
// openapi_document over the core registry — the same drift discipline as
// every schemas/*.json (test_schemas.cpp). Regenerate after any capability
// or schema change: ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R OpenApi
TEST(OpenApi, GeneratedFileMatchesTheCodeSourceOfTruth) {
    const Registry registry = core_registry();
    const nlohmann::json source = zetta::api::openapi_document(registry);
    const std::string path = std::string(ZETTA_SCHEMAS_DIR) + "/openapi.json";

    if (std::getenv("ZETTA_WRITE_SCHEMAS") != nullptr) {
        std::ofstream out(path);
        ASSERT_TRUE(out.good()) << "cannot write " << path;
        out << source.dump(2) << '\n';
        return;
    }
    std::ifstream in(path);
    ASSERT_TRUE(in.good())
        << "missing generated schemas/openapi.json — run: "
           "ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R OpenApi";
    const nlohmann::json on_disk =
        nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
    ASSERT_FALSE(on_disk.is_discarded()) << path << " is not valid JSON";
    EXPECT_EQ(on_disk, source)
        << "schemas/openapi.json is stale — regenerate with: "
           "ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R OpenApi";
}

}  // namespace
