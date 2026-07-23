// The agent CRUD capabilities (#849) end to end through the registry, against a
// REPLICATED node (agents live in the CRDT). Mirrors the MCP/REST surface: the same
// registry.call() the servers dispatch to.
#include <gtest/gtest.h>

#include <string>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct AgentApiFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{"agent-api-node"};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                     &replication};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }
};

TEST_F(AgentApiFixture, CreateListGetUpdateDelete) {
    // Empty to start.
    ApiResult listed = call("list_agents", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_TRUE(listed.value.at("agents").empty());

    // Create.
    ApiResult created = call("create_agent",
                             {{"name", "Base agent"},
                              {"model", "models/Qwen3-0.6B"},
                              {"description", "the base"},
                              {"tools", {"connect", "serve"}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id").get<std::string>();
    EXPECT_FALSE(id.empty());
    EXPECT_EQ(created.value.at("name"), "Base agent");
    EXPECT_FALSE(created.value.at("created_at").get<std::string>().empty());

    // List shows it.
    listed = call("list_agents", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value.at("agents").size(), 1u);

    // Get by id.
    ApiResult got = call("get_agent", {{"id", id}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value.at("model"), "models/Qwen3-0.6B");

    // A missing model is rejected at create.
    ApiResult bad = call("create_agent", {{"name", "No model"}});
    EXPECT_FALSE(bad.ok);

    // Update (only provided fields change; created_at preserved).
    ApiResult updated =
        call("update_agent", {{"id", id}, {"name", "Base agent v2"}});
    ASSERT_TRUE(updated.ok) << updated.error.message;
    EXPECT_EQ(updated.value.at("name"), "Base agent v2");
    EXPECT_EQ(updated.value.at("model"), "models/Qwen3-0.6B");  // unchanged
    EXPECT_EQ(updated.value.at("created_at"), created.value.at("created_at"));

    // Get a nonexistent agent.
    EXPECT_FALSE(call("get_agent", {{"id", "nope"}}).ok);

    // Delete (idempotent).
    ApiResult removed = call("delete_agent", {{"id", id}});
    ASSERT_TRUE(removed.ok) << removed.error.message;
    EXPECT_TRUE(removed.value.at("removed").get<bool>());
    EXPECT_FALSE(call("delete_agent", {{"id", id}}).value.at("removed").get<bool>());

    listed = call("list_agents", nlohmann::json::object());
    EXPECT_TRUE(listed.value.at("agents").empty());
}

}  // namespace
