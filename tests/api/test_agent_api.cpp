// The agent CRUD capabilities (#849) end to end through the registry, against a
// REPLICATED node (agents live in the CRDT). Mirrors the MCP/REST surface: the same
// registry.call() the servers dispatch to.
#include <gtest/gtest.h>

#include <cstdlib>
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

// The full chat path with a REAL model: create an agent pointing at a checkpoint, then
// chat. Guarded on ZETTA_QWEN_DIR (a local checkpoint dir), skipped otherwise so CI needs
// no weights. Small max_new_tokens keeps the CPU generation quick.
TEST_F(AgentApiFixture, ChatWithARealModel) {
    const char* dir = std::getenv("ZETTA_QWEN_DIR");
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to a checkpoint dir to run";

    ApiResult created = call("create_agent",
                             {{"name", "Base agent"},
                              {"model", std::string(dir)},
                              {"tools", {"connect", "serve"}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id").get<std::string>();

    ApiResult chat = call("chat", {{"agent", id},
                                   {"message", "Say hello in one word."},
                                   {"max_new_tokens", 16},
                                   {"max_rounds", 2}});
    ASSERT_TRUE(chat.ok) << chat.error.message;
    EXPECT_FALSE(chat.value.at("reply").get<std::string>().empty());
    EXPECT_TRUE(chat.value.at("tool_calls").is_array());
    EXPECT_GE(chat.value.at("rounds").get<int>(), 1);
}

// agent_run (#872): the base agent builds AND runs an execution graph from plain language,
// with no tool-calling fine-tune (#859). This asserts the capability is wired as a mutating
// Job — the run path it delegates to is gated + observable like every other run.
TEST_F(AgentApiFixture, AgentRunIsRegisteredAsAJob) {
    const std::shared_ptr<const zetta::api::Capability> capability = registry.find("agent_run");
    ASSERT_NE(capability, nullptr) << "agent_run must be registered";
    EXPECT_EQ(capability->kind, zetta::api::CapabilityKind::Job);
    EXPECT_TRUE(capability->mutates);
}

// The full agent_run composition with a REAL model: the agent reads the ask, the engine
// compiles a graph from it, and the graph runs. Guarded on ZETTA_QWEN_DIR; a non-churn ask so
// the run takes the structural path (no background owner needed in this fixture — the churn
// engine is exercised live in the inference runbook). Proves agent -> compile -> save -> run.
TEST_F(AgentApiFixture, AgentRunBuildsAndRunsFromRealModel) {
    const char* dir = std::getenv("ZETTA_QWEN_DIR");
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to a checkpoint dir to run";

    ApiResult created = call("create_agent",
                             {{"name", "Base agent"},
                              {"model", std::string(dir)},
                              {"tools", {"connect", "serve"}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id").get<std::string>();

    ApiResult run = call("agent_run",
                         {{"agent", id},
                          {"message", "Summarize the sales table and export a weekly report."},
                          {"max_new_tokens", 32}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_FALSE(run.value.at("reply").get<std::string>().empty());
    EXPECT_FALSE(run.value.at("shape").get<std::string>().empty());
    EXPECT_GE(run.value.at("steps").size(), 1u);
    EXPECT_FALSE(run.value.at("run").at("run_id").get<std::string>().empty());
}

}  // namespace
