// SPDX-License-Identifier: MIT

// Published pipelines (zetta#1289, docs/api/serving.md): expose_pipeline pins
// an approved pipeline and publishes its served route; a served run is governed
// by the caller's own grants (data:run_pipeline, and read access to what the
// pipeline reads), its input is checked against the declared schema and the
// size limit, calls are rate limited per caller, and unexposing withdraws the
// route. The protocol-layer (loopback HTTP) acceptance is in
// tests/api/test_rest_server.cpp (PublishedPipelineEndpointsOverHttp).
#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/api/serving.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::api::ServeRequest;
using zetta::governance::CapabilityDomain;

struct PipelineServingFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    std::shared_ptr<zetta::pipeline::Project> project;
    std::shared_ptr<zetta::pipeline::ProjectGraph> pipeline;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        auto database = node.create_database("data");
        ASSERT_TRUE(database);
        for (const char* name : {"orders", "customers"}) {
            auto table = database->create_table(name);
            ASSERT_TRUE(table);
            table->add_row({{"id", 1}, {"amount", 10.0}});
        }
        project = node.create_project("shop");
        ASSERT_TRUE(project);
        pipeline = project->create_graph("orders pipeline", graph_reading("orders"));
        ASSERT_TRUE(pipeline);
        ASSERT_TRUE(project->approve_graph("orders pipeline", "owner", "2026-10-21T00:00:00Z"));
    }
    void TearDown() override { node.stop(); }

    // connect (data.<table>) -> clean: no model step, so `run` runs it
    // structurally, inline, and its lineage names the table it read.
    static zetta::ir::ExecutionGraph graph_reading(const std::string& table) {
        zetta::ir::ExecutionGraph graph;
        auto connect = graph.add_node(zetta::ir::StepKind::Connect, "load");
        auto clean = graph.add_node(zetta::ir::StepKind::Clean, "tidy");
        connect->attributes = {{"source", "data"}, {"table", table}};
        graph.add_edge(connect->id, clean->id);
        return graph;
    }

    ApiResult call(const std::string& name, nlohmann::json input,
                   const std::string& acting_key = {}, bool is_user = false) {
        ApiContext context{node, nullptr, acting_key, trace, &node.span_recorder()};
        context.acting_is_user = is_user;
        return registry.call(name, input, context);
    }

    ServeRequest request(const std::string& target, nlohmann::json input,
                         const std::string& acting_key, bool is_user = false) {
        ServeRequest served{target, std::move(input), acting_key, trace, &node.span_recorder()};
        served.acting_is_user = is_user;
        served.registry = &registry;
        return served;
    }

    ApiResult run(const std::string& target, nlohmann::json input,
                  const std::string& acting_key, bool is_user = false) {
        return zetta::api::serve_pipeline_run(node,
                                              request(target, std::move(input), acting_key, is_user));
    }

    ApiResult status(const std::string& target, const std::string& run_id,
                     const std::string& acting_key, bool is_user = false) {
        return zetta::api::serve_pipeline_run_status(
            node, request(target, {{"run_id", run_id}}, acting_key, is_user));
    }

    // A named access key holding exactly these rights.
    std::string make_key(const std::string& principal_name,
                         const std::vector<std::pair<CapabilityDomain, std::string>>& rights) {
        auto principal =
            node.create_principal(principal_name, zetta::governance::PrincipalKind::Service);
        EXPECT_TRUE(principal);
        zetta::governance::CapabilitySet capabilities;
        for (const auto& [domain, action] : rights) capabilities.grant(domain, action);
        auto key = principal->create_key(zetta::governance::unrestricted_scope(), capabilities);
        EXPECT_TRUE(key);
        return key->id();
    }

    // A served run executes as the caller, so the caller also holds the right
    // each step of the pinned pipeline exercises (#843): this pipeline's clean
    // step derives a dataset (orchestration:create_dataset_from_query).
    std::string runner_key(const std::string& name) {
        return make_key(name, {{CapabilityDomain::Data, "run_pipeline"},
                               {CapabilityDomain::Data, "read"},
                               {CapabilityDomain::Orchestration, "create_dataset_from_query"}});
    }

    ApiResult expose(nlohmann::json extra = nlohmann::json::object()) {
        nlohmann::json input{{"project", "shop"}, {"graph", "orders pipeline"}};
        input.update(extra);
        return call("expose_pipeline", input);
    }

    std::size_t audit_rows(const std::string& target, bool allowed) {
        std::size_t rows = 0;
        for (const auto& event : node.audit_events()) {
            if (event->value().target == target && event->value().allowed == allowed) ++rows;
        }
        return rows;
    }
};

// Expose -> a key with the rights runs it -> its status reads back to that
// caller only -> unexpose withdraws the route.
TEST_F(PipelineServingFixture, ExposeServeReadBackAndUnexpose) {
    const std::string key = runner_key("caller");
    const std::string id = pipeline->id();

    // Cluster-internal by default.
    auto before = run(id, {}, key);
    EXPECT_EQ(before.error.code, "not_found") << before.error.message;
    EXPECT_NE(before.error.message.find("not exposed"), std::string::npos);

    auto exposed = expose({{"input_schema",
                            {{"type", "object"},
                             {"properties", {{"top_k", {{"type", "integer"}, {"minimum", 1}}}}},
                             {"additionalProperties", false}}}});
    ASSERT_TRUE(exposed.ok) << exposed.error.message;
    EXPECT_EQ(exposed.value.at("pipeline_id"), id);
    EXPECT_EQ(exposed.value.at("version"), 1);
    EXPECT_TRUE(exposed.value.at("exposed").get<bool>());
    EXPECT_EQ(exposed.value.at("endpoint"), "/v1/pipelines/" + id + "/run");
    EXPECT_EQ(exposed.value.at("exposed_by"), "local");

    auto served = run(id, {{"top_k", 3}}, key);
    ASSERT_TRUE(served.ok) << served.error.code << ": " << served.error.message;
    EXPECT_EQ(served.value.at("version"), 1);
    EXPECT_EQ(served.value.at("project"), "shop");
    EXPECT_EQ(served.value.at("status"), "succeeded");
    EXPECT_EQ(served.trace_id.size(), 32u);
    const std::string run_id = served.value.at("run_id");
    EXPECT_EQ(served.value.at("status_endpoint"), "/v1/pipelines/" + id + "/runs/" + run_id);
    const auto stored = node.find_run(run_id);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->graph(), id);
    ASSERT_TRUE(stored->published());
    EXPECT_EQ(stored->published()->version, 1u);
    EXPECT_EQ(stored->published()->caller, key);
    // Replicated and on disk with the run.
    const auto parsed = zetta::pipeline::parse_run_value(stored->metadata());
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->published, stored->published());
    EXPECT_EQ(audit_rows("serve:run_pipeline:" + id, true), 1u);
    // The name resolves too, while one exposed pipeline has it.
    EXPECT_TRUE(run("orders pipeline", {}, key).ok);

    // Read back by the caller it was served to; another caller does not see it.
    auto read = status(id, run_id, key);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value.at("status"), "succeeded");
    EXPECT_EQ(read.value.at("version"), 1);
    const std::string other = runner_key("other");
    EXPECT_EQ(status(id, run_id, other).error.code, "not_found");
    // ...and a run started some other way is not this route's to show.
    auto direct = call("run", {{"project", "shop"}, {"graph", "orders pipeline"}});
    ASSERT_TRUE(direct.ok) << direct.error.message;
    EXPECT_EQ(status(id, direct.value.at("run_id"), key).error.code, "not_found");

    // The same through the registry capabilities.
    auto via_registry = call("run_published_pipeline", {{"pipeline", id}, {"input", {{"top_k", 1}}}},
                             key);
    ASSERT_TRUE(via_registry.ok) << via_registry.error.message;
    auto registry_read = call("get_published_pipeline_run",
                              {{"pipeline", id}, {"run_id", via_registry.value.at("run_id")}}, key);
    ASSERT_TRUE(registry_read.ok) << registry_read.error.message;
    EXPECT_EQ(registry_read.value.at("status"), "succeeded");

    // Withdrawn everywhere by the one action; idempotent.
    auto unexposed = call("unexpose_pipeline", {{"project", "shop"}, {"graph", "orders pipeline"}});
    ASSERT_TRUE(unexposed.ok) << unexposed.error.message;
    EXPECT_FALSE(unexposed.value.at("exposed").get<bool>());
    EXPECT_EQ(unexposed.value.at("version"), 1);
    EXPECT_TRUE(call("unexpose_pipeline", {{"project", "shop"}, {"graph", "orders pipeline"}}).ok);
    auto after = run(id, {}, key);
    EXPECT_EQ(after.error.code, "not_found");
    EXPECT_EQ(run("orders pipeline", {}, key).error.code, "not_found");
}

// The caller's own grants decide: without data:run_pipeline, or without read
// access to a table the pipeline reads, the call is refused and audited.
TEST_F(PipelineServingFixture, TheCallersGrantsDecideNeverTheExposers) {
    ASSERT_TRUE(expose().ok);
    const std::string id = pipeline->id();

    const std::string reader = make_key("reader", {{CapabilityDomain::Data, "read"}});
    auto no_right = run(id, {}, reader);
    EXPECT_EQ(no_right.error.code, "unauthorized");
    EXPECT_EQ(audit_rows("serve:run_pipeline:" + id, false), 1u);

    const std::string no_read = make_key("runner", {{CapabilityDomain::Data, "run_pipeline"}});
    auto refused = run(id, {}, no_read);
    EXPECT_EQ(refused.error.code, "unauthorized");
    EXPECT_NE(refused.error.message.find("data.orders"), std::string::npos)
        << refused.error.message;
    EXPECT_EQ(audit_rows("serve:run_pipeline:" + id, false), 2u);

    // A user: data:run_pipeline scoped ON PROJECT, sql:read on the table only.
    auto user = node.create_user("partner");
    ASSERT_TRUE(user);
    auto role = node.create_role("partner-role");
    ASSERT_TRUE(role);
    ASSERT_TRUE(node.assign_role(user->id(), role->id()));
    zetta::governance::Resource other_project{zetta::governance::ResourceKind::Project, "elsewhere"};
    ASSERT_TRUE(node.grant_scoped_permissions("partner-role", {"serving:run"}, other_project));
    ASSERT_TRUE(node.grant_scoped_permissions("partner-role", {"sql:read"},
                                              zetta::governance::Resource::table("data", "orders")));
    EXPECT_EQ(run(id, {}, user->id(), true).error.code, "unauthorized");

    zetta::governance::Resource shop{zetta::governance::ResourceKind::Project, "shop"};
    ASSERT_TRUE(node.grant_scoped_permissions("partner-role", {"serving:run"}, shop));
    auto allowed = run(id, {}, user->id(), true);
    ASSERT_TRUE(allowed.ok) << allowed.error.code << ": " << allowed.error.message;
    EXPECT_EQ(node.find_run(allowed.value.at("run_id"))->published()->caller, user->id());

    // A dataset_path the caller may not read is refused, though the pipeline
    // declares it.
    ASSERT_TRUE(expose({{"input_schema",
                         {{"type", "object"},
                          {"properties", {{"dataset_path", {{"type", "string"}}}}},
                          {"additionalProperties", false}}}})
                    .ok);
    auto other_table = run(id, {{"dataset_path", "data.customers"}}, user->id(), true);
    EXPECT_EQ(other_table.error.code, "unauthorized");
    EXPECT_NE(other_table.error.message.find("data.customers"), std::string::npos)
        << other_table.error.message;
}

// The declared input schema, the size limit and the per-caller rate limit.
TEST_F(PipelineServingFixture, InputSchemaSizeAndRateLimits) {
    ASSERT_TRUE(expose({{"rate_limit_per_minute", 2}, {"max_input_bytes", 32}}).ok);
    const std::string id = pipeline->id();
    const std::string key = runner_key("caller");

    // The default declared input admits nothing.
    auto undeclared = run(id, {{"top_k", 3}}, key);
    EXPECT_EQ(undeclared.error.code, "invalid_input");
    EXPECT_NE(undeclared.error.message.find("declares"), std::string::npos)
        << undeclared.error.message;
    auto too_big = run(id, {{"padding", std::string(64, 'x')}}, key);
    EXPECT_EQ(too_big.error.code, "payload_too_large");

    EXPECT_TRUE(run(id, {}, key).ok);
    EXPECT_TRUE(run(id, {}, key).ok);
    auto limited = run(id, {}, key);
    EXPECT_EQ(limited.error.code, "rate_limited");
    EXPECT_GE(limited.error.details.at("retry_after_seconds").get<int>(), 1);
    // Per caller: another caller is admitted.
    EXPECT_TRUE(run(id, {}, runner_key("second")).ok);

    // A declared field `run` does not take is refused, not passed on.
    ASSERT_TRUE(expose({{"input_schema", {{"type", "object"}}}}).ok);
    auto unknown = run(id, {{"surprise", 1}}, runner_key("third"));
    EXPECT_EQ(unknown.error.code, "invalid_input");
    EXPECT_NE(unknown.error.message.find("`run` does not take"), std::string::npos)
        << unknown.error.message;
}

zetta::governance::RateLimiter::Clock::time_point at(int seconds) {
    return zetta::governance::RateLimiter::Clock::time_point{} + std::chrono::seconds(seconds);
}

TEST(RateLimiter, ASlidingMinutePerKey) {
    zetta::governance::RateLimiter limiter;
    EXPECT_TRUE(limiter.admit("a", 2, at(0)).admitted);
    EXPECT_TRUE(limiter.admit("a", 2, at(10)).admitted);
    const auto refused = limiter.admit("a", 2, at(20));
    EXPECT_FALSE(refused.admitted);
    EXPECT_EQ(refused.retry_after_seconds, 40u);
    EXPECT_TRUE(limiter.admit("b", 2, at(20)).admitted);
    EXPECT_TRUE(limiter.admit("a", 2, at(60)).admitted);   // the first left the window
    EXPECT_FALSE(limiter.admit("a", 2, at(65)).admitted);
}

// Exposing pins the pipeline: an edit changes nothing callers get until it is
// exposed again, as the next version.
TEST_F(PipelineServingFixture, PinsTheVersionUntilItIsExposedAgain) {
    ASSERT_TRUE(expose().ok);
    const std::string id = pipeline->id();
    const std::string key = runner_key("caller");
    const auto lineage_of = [&](const ApiResult& served) {
        return node.find_run(served.value.at("run_id"))->lineage().sources;
    };
    auto first = run(id, {}, key);
    ASSERT_TRUE(first.ok) << first.error.message;
    EXPECT_EQ(lineage_of(first), std::vector<std::string>{"data:orders"});

    ASSERT_TRUE(project->update_graph("orders pipeline", graph_reading("customers")));
    ASSERT_TRUE(project->approve_graph("orders pipeline", "owner", "2026-10-21T01:00:00Z"));
    auto still_pinned = run(id, {}, key);
    ASSERT_TRUE(still_pinned.ok) << still_pinned.error.message;
    EXPECT_EQ(still_pinned.value.at("version"), 1);
    EXPECT_EQ(lineage_of(still_pinned), std::vector<std::string>{"data:orders"});

    auto republished = expose();
    ASSERT_TRUE(republished.ok) << republished.error.message;
    EXPECT_EQ(republished.value.at("version"), 2);
    auto second = run(id, {}, key);
    ASSERT_TRUE(second.ok) << second.error.message;
    EXPECT_EQ(second.value.at("version"), 2);
    EXPECT_EQ(lineage_of(second), std::vector<std::string>{"data:customers"});
}

// Nothing unapproved, nothing reading outside the workspace, and no unusable
// input schema is published.
TEST_F(PipelineServingFixture, ExposeRefusesWhatCannotBeServed) {
    auto proposed_graph = project->create_graph("proposed", graph_reading("orders"));
    ASSERT_TRUE(proposed_graph);
    auto proposed = call("expose_pipeline", {{"project", "shop"}, {"graph", "proposed"}});
    EXPECT_EQ(proposed.error.code, "engine_error");
    EXPECT_NE(proposed.error.message.find("approve_plan"), std::string::npos);

    ASSERT_TRUE(project->create_graph("remote", graph_reading("missing")));
    ASSERT_TRUE(project->approve_graph("remote", "owner", "2026-10-21T00:00:00Z"));
    auto remote = call("expose_pipeline", {{"project", "shop"}, {"graph", "remote"}});
    EXPECT_EQ(remote.error.code, "invalid_input");
    EXPECT_NE(remote.error.message.find("not a table in this workspace"), std::string::npos)
        << remote.error.message;

    auto array_schema = expose({{"input_schema", {{"type", "array"}}}});
    EXPECT_EQ(array_schema.error.code, "invalid_input");
    EXPECT_EQ(array_schema.error.path, "/input_schema");
    EXPECT_FALSE(pipeline->exposure().exposed);

    EXPECT_EQ(call("expose_pipeline", {{"project", "shop"}, {"graph", "nope"}}).error.code,
              "not_found");
}

// The exposure is replicated state: on disk and in the CRDT value with the
// graph, read back after a restart, and malformed values are not served.
TEST(PipelineExposure, PersistsAndRejectsMalformedValues) {
    zetta::test::TempDir dir;
    std::string id;
    {
        zetta::Node node{zetta::NodeConfig{dir.path().string(), false}};
        ASSERT_TRUE(node.start());
        auto project = node.create_project("shop");
        zetta::ir::ExecutionGraph graph;
        graph.add_node(zetta::ir::StepKind::Clean, "tidy");
        auto stored = project->create_graph("p", std::move(graph));
        ASSERT_TRUE(stored);
        id = stored->id();
        zetta::pipeline::PipelineExposure exposure;
        exposure.exposed = true;
        exposure.version = 3;
        exposure.pinned = zetta::ir::to_json(stored->graph());
        exposure.input_schema = zetta::api::default_pipeline_input_schema();
        exposure.rate_limit_per_minute = 7;
        ASSERT_TRUE(project->set_graph_exposure(id, exposure));
        EXPECT_EQ(stored->record().at("exposure").at("version"), 3);
        node.stop();
    }
    zetta::Node node{zetta::NodeConfig{dir.path().string(), false}};
    ASSERT_TRUE(node.start());
    auto graph = node.find_project("shop")->find_graph_by_id(id);
    ASSERT_TRUE(graph);
    EXPECT_TRUE(graph->exposure().exposed);
    EXPECT_EQ(graph->exposure().version, 3u);
    EXPECT_EQ(graph->exposure().rate_limit_per_minute, 7u);
    node.stop();

    // A never-exposed graph keeps its old shape; malformed exposures are refused.
    EXPECT_FALSE(zetta::pipeline::ProjectGraph("g", "p", "n", {}, {}).record().contains("exposure"));
    EXPECT_FALSE(zetta::pipeline::pipeline_exposure_from_json({{"exposed", "yes"}}));
    EXPECT_FALSE(zetta::pipeline::pipeline_exposure_from_json(
        {{"exposed", true}, {"version", 1}, {"pinned", {{"nodes", 7}}}, {"input_schema", {}}}));
}

}  // namespace
