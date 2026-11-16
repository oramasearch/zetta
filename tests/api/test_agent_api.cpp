// SPDX-License-Identifier: MIT

// The agent CRUD capabilities (#849) end to end through the registry, against a
// REPLICATED node (agents live in the CRDT). Mirrors the MCP/REST surface: the same
// registry.call() the servers dispatch to.
#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "temp_dir.hpp"
#include "agent/checkpoint_registration.hpp"
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

TEST_F(AgentApiFixture, PersistedModelSelectorsCannotProbeUnregisteredDirectories) {
    zetta::test::CheckpointRegistration checkpoint;
    checkpoint.configure("{}");
    const auto outside = std::filesystem::canonical(dir.path()).string();
    const auto created = call("create_agent", {{"name", "Unchecked model"}, {"model", outside}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const auto id = created.value.at("id").get<std::string>();
    for (const auto* capability : {"chat", "agent_run"}) {
        SCOPED_TRACE(capability);
        const auto response = call(capability, {{"agent", id}, {"message", "hello"}});
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error.message, "agent.model: checkpoint is not registered");
    }
    const auto updated = call("update_agent", {{"id", id}, {"model", outside + "/../outside"}});
    ASSERT_TRUE(updated.ok) << updated.error.message;
    for (const auto* capability : {"chat", "agent_run"}) {
        const auto response = call(capability, {{"agent", id}, {"message", "hello"}});
        EXPECT_FALSE(response.ok);
        EXPECT_EQ(response.error.message, "agent.model: checkpoint is not registered");
    }
}

// The full chat path with a REAL model: create an agent pointing at a checkpoint, then
// chat. Guarded on ZETTA_QWEN_DIR (a local checkpoint dir), skipped otherwise so CI needs
// no weights. Small max_new_tokens keeps the CPU generation quick.
TEST_F(AgentApiFixture, ChatWithARealModel) {
    const char* dir = std::getenv("ZETTA_QWEN_DIR");
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to a checkpoint dir to run";
    zetta::test::CheckpointRegistration checkpoint;
    checkpoint.add(dir, std::filesystem::canonical(dir).string());

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

// agent_run (#872): the base agent builds an execution graph from plain language, and runs it
// once a person approves it (#913),
// with no tool-calling fine-tune (#859). This asserts the capability is wired as a mutating
// Job — the run path it delegates to is gated + observable like every other run.
TEST_F(AgentApiFixture, AgentRunIsRegisteredAsAJob) {
    const std::shared_ptr<const zetta::api::Capability> capability = registry.find("agent_run");
    ASSERT_NE(capability, nullptr) << "agent_run must be registered";
    EXPECT_EQ(capability->kind, zetta::api::CapabilityKind::Job);
    EXPECT_TRUE(capability->mutates);
}

// The full agent_run composition with a REAL model: the agent reads the ask, the engine
// compiles a graph from it and stores it proposed, and nothing runs until the plan is approved
// (#913). Guarded on ZETTA_QWEN_DIR; a non-churn ask so the approved run takes the structural
// path (no background owner needed in this fixture — the churn engine is exercised live in the
// inference runbook). Proves agent -> compile -> save proposed -> refused -> approve -> run.
TEST_F(AgentApiFixture, AgentRunBuildsAPlanAndRunsItOnlyOnceApproved) {
    const char* dir = std::getenv("ZETTA_QWEN_DIR");
    if (dir == nullptr) GTEST_SKIP() << "set ZETTA_QWEN_DIR to a checkpoint dir to run";
    zetta::test::CheckpointRegistration checkpoint;
    checkpoint.add(dir, std::filesystem::canonical(dir).string());

    ApiResult created = call("create_agent",
                             {{"name", "Base agent"},
                              {"model", std::string(dir)},
                              {"tools", {"connect", "serve"}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id").get<std::string>();
    ASSERT_TRUE(call("create_project", {{"name", "agent built"}}).ok);

    // agent_run builds the plan and stores it proposed. It does not approve it and does not
    // run it: approval belongs to a principal holding orchestration:approve_plan.
    ApiResult built = call("agent_run",
                           {{"agent", id},
                            {"message", "Summarize the sales table and export a weekly report."},
                            {"project", "agent built"},
                            {"graph_name", "weekly report"},
                            {"max_new_tokens", 32}});
    ASSERT_FALSE(built.ok) << "agent_run ran a plan nobody approved";
    EXPECT_EQ(built.error.code, "not_approved");
    EXPECT_NE(built.error.message.find("weekly report"), std::string::npos) << built.error.message;
    const auto project = node.find_project("agent built");
    ASSERT_NE(project, nullptr);
    const auto stored = project->find_graph("weekly report");
    ASSERT_NE(stored, nullptr) << "the plan it built must be stored for review";
    EXPECT_FALSE(stored->approved()) << "agent_run must never approve the plan it built";
    EXPECT_TRUE(node.runs().empty()) << "nothing may run before the plan is approved";

    // Approved, the same call runs it.
    ASSERT_TRUE(call("approve_plan", {{"project", "agent built"}, {"graph", "weekly report"}}).ok);
    ApiResult run = call("agent_run",
                         {{"agent", id},
                          {"message", "Summarize the sales table and export a weekly report."},
                          {"project", "agent built"},
                          {"graph_name", "weekly report"}});
    ASSERT_TRUE(run.ok) << run.error.message;
    EXPECT_GE(run.value.at("steps").size(), 1u);
    EXPECT_FALSE(run.value.at("run").at("run_id").get<std::string>().empty());
}

// #913: agent_run on a stored plan nobody approved is refused, with a reason that names the plan
// and how to approve it, and nothing runs. The approval check comes before the model loads, so
// this needs no checkpoint (the agent's model path does not exist here).
TEST_F(AgentApiFixture, AgentRunRefusesAPlanNobodyApproved) {
    ASSERT_TRUE(call("create_project", {{"name", "unreviewed"}}).ok);
    ASSERT_TRUE(call("compile_python",
                     {{"project", "unreviewed"},
                      {"source",
                       "ds = load_dataset(\"accounts.parquet\")\n"
                       "clean = ds.dropna()\n"},
                      {"name", "Clean accounts"}})
                    .ok);
    ApiResult created =
        call("create_agent", {{"name", "Runner"}, {"model", "models/does-not-exist"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id").get<std::string>();

    const ApiResult refused = call("agent_run",
                                   {{"agent", id},
                                    {"message", "clean the accounts"},
                                    {"project", "unreviewed"},
                                    {"graph_name", "Clean accounts"}});
    ASSERT_FALSE(refused.ok) << "agent_run ran a plan nobody approved";
    EXPECT_EQ(refused.error.code, "not_approved");
    EXPECT_NE(refused.error.message.find("Clean accounts"), std::string::npos)
        << refused.error.message;
    EXPECT_NE(refused.error.message.find("approve_plan"), std::string::npos)
        << refused.error.message;
    EXPECT_TRUE(node.runs().empty()) << "a refused agent_run must not start a run";
    const auto graph = node.find_project("unreviewed")->find_graph("Clean accounts");
    ASSERT_NE(graph, nullptr);
    EXPECT_FALSE(graph->approved()) << "agent_run must never approve a plan";
}

// #913: once a principal approves the plan (approve_plan), agent_run runs it, and the reply
// names who approved it.
TEST_F(AgentApiFixture, AgentRunRunsAnApprovedPlan) {
    ASSERT_TRUE(call("create_project", {{"name", "reviewed"}}).ok);
    ASSERT_TRUE(call("compile_python",
                     {{"project", "reviewed"},
                      {"source",
                       "ds = load_dataset(\"accounts.parquet\")\n"
                       "clean = ds.dropna()\n"},
                      {"name", "Clean accounts"}})
                    .ok);
    ASSERT_TRUE(call("approve_plan", {{"project", "reviewed"}, {"graph", "Clean accounts"}}).ok);
    ApiResult created =
        call("create_agent", {{"name", "Runner"}, {"model", "models/does-not-exist"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    const std::string id = created.value.at("id").get<std::string>();

    const ApiResult ran = call("agent_run",
                               {{"agent", id},
                                {"message", "clean the accounts"},
                                {"project", "reviewed"},
                                {"graph_name", "Clean accounts"}});
    ASSERT_TRUE(ran.ok) << ran.error.message;
    EXPECT_EQ(ran.value.at("shape"), "approved");
    EXPECT_EQ(ran.value.at("graph"), "Clean accounts");
    EXPECT_GE(ran.value.at("steps").size(), 2u);
    EXPECT_FALSE(ran.value.at("run").at("run_id").get<std::string>().empty());
    EXPECT_NE(ran.value.at("reply").get<std::string>().find("approved by"), std::string::npos);
    EXPECT_EQ(node.runs().size(), 1u);
}

}  // namespace

// zetta#1015: every agent lake shows is an AgentRecord — the specialized Orama agents (the Agent
// Library) and the ones people build — so kind, runtime, the Studio shape, the Ask Orama contract,
// granted Pipelines and a Python agent's definition all round-trip through the capabilities.
TEST_F(AgentApiFixture, KindsRuntimesStudioShapeAndPipelineGrants) {
    // A record written before these fields reads back as a custom native agent.
    ApiResult native = call("create_agent", {{"name", "Base"}, {"model", "models/Qwen3-0.6B"}});
    ASSERT_TRUE(native.ok) << native.error.message;
    EXPECT_EQ(native.value.at("kind"), "custom");
    EXPECT_EQ(native.value.at("runtime"), "native");
    EXPECT_EQ(native.value.at("status"), "idle");
    EXPECT_TRUE(native.value.at("chat_capable").get<bool>());

    // Ask Orama: a specialized service agent, the default.
    ApiResult ask = call("create_agent",
                         {{"name", "Ask Orama"},
                          {"kind", "orama"},
                          {"runtime", "service"},
                          {"endpoint", "http://agent-ask-orama.lake:8080"},
                          {"is_default", true}});
    ASSERT_TRUE(ask.ok) << ask.error.message;
    EXPECT_FALSE(call("create_agent", {{"name", "No endpoint"}, {"runtime", "service"}}).ok);

    // A custom Python agent in Studio's shape, its code stored inline.
    const nlohmann::json python_agent = {
        {"name", "Renewal watcher"},
        {"runtime", "python"},
        {"purpose", "Flag renewals at risk"},
        {"work_area", {{"kind", "project"}, {"detail", "churn"}}},
        {"manifesto", "Answer from company data only."},
        {"goal", "Every at-risk renewal is flagged a month ahead."},
        {"skills", {"search", "summarize"}},
        {"trigger", "every Monday at 09:00"},
        {"model_name", "gpt-5"},
        {"definition",
         {{"source", "zetta"},
          {"instructions", "# Renewal watcher"},
          {"entrypoint", "agent.main:run"},
          {"files", {{{"path", "agent/main.py"}, {"contents", "def run(ctx):\n    return 1\n"}}}}}}};
    ApiResult python = call("create_agent", python_agent);
    ASSERT_TRUE(python.ok) << python.error.message;
    EXPECT_EQ(python.value.at("work_area").at("kind"), "project");
    EXPECT_EQ(python.value.at("skills").size(), 2u);
    EXPECT_EQ(python.value.at("definition").at("files").at(0).at("path"), "agent/main.py");
    const std::string python_id = python.value.at("id");

    // The definition's rules: a lake source needs a pinned revision, inline code is capped, paths
    // stay inside the definition, and skills come from the fixed set.
    nlohmann::json lake_source = python_agent;
    lake_source["definition"] = {{"source", "lake"}, {"repository_id", "r-1"}};
    ApiResult no_revision = call("create_agent", lake_source);
    ASSERT_FALSE(no_revision.ok);
    EXPECT_NE(no_revision.error.message.find("revision"), std::string::npos);
    lake_source["definition"]["revision"] = "c0ffee";
    EXPECT_TRUE(call("create_agent", lake_source).ok);
    nlohmann::json too_big = python_agent;
    too_big["definition"]["files"][0]["contents"] = std::string(1024 * 1024 + 1, 'x');
    ApiResult big = call("create_agent", too_big);
    ASSERT_FALSE(big.ok);
    EXPECT_NE(big.error.message.find("lake repository"), std::string::npos);
    nlohmann::json escape = python_agent;
    escape["definition"]["files"][0]["path"] = "../etc/passwd";
    EXPECT_FALSE(call("create_agent", escape).ok);
    // #1220: the definition schema is closed at both levels. A file sent as {path, content} (for
    // `contents`) used to be stored as an empty file without a word; it is refused with its path,
    // in the words lake uses, and nothing is stored.
    const std::size_t agents_before = call("list_agents", nlohmann::json::object())
                                          .value.at("agents")
                                          .size();
    nlohmann::json misspelt = python_agent;
    misspelt["definition"]["files"] = {{{"path", "agent/main.py"}, {"content", "def run(ctx): ..."}}};
    ApiResult content = call("create_agent", misspelt);
    ASSERT_FALSE(content.ok);
    EXPECT_EQ(content.error.code, "invalid_input");
    EXPECT_EQ(content.error.path, "/definition/files/0/content");
    EXPECT_NE(content.error.message.find("a file has no field 'content'; each file is {path, "
                                         "contents}, and contents holds the file's text"),
              std::string::npos)
        << content.error.message;
    nlohmann::json stray = python_agent;
    stray["definition"]["language"] = "python";
    ApiResult unknown = call("create_agent", stray);
    ASSERT_FALSE(unknown.ok);
    EXPECT_EQ(unknown.error.path, "/definition/language");
    EXPECT_NE(unknown.error.message.find("definition has no field 'language'"), std::string::npos)
        << unknown.error.message;
    nlohmann::json pathless = python_agent;
    pathless["definition"]["files"] = {{{"contents", "x = 1\n"}}};
    ApiResult no_path = call("create_agent", pathless);
    ASSERT_FALSE(no_path.ok);
    EXPECT_EQ(no_path.error.path, "/definition/files/0/path");
    EXPECT_EQ(call("list_agents", nlohmann::json::object()).value.at("agents").size(),
              agents_before);
    // update_agent takes the same closed shape.
    ApiResult update_content = call(
        "update_agent", {{"id", python_id}, {"definition", misspelt["definition"]}});
    ASSERT_FALSE(update_content.ok);
    EXPECT_EQ(update_content.error.path, "/definition/files/0/content");
    ApiResult kept = call("get_agent", {{"id", python_id}});
    ASSERT_TRUE(kept.ok) << kept.error.message;
    EXPECT_EQ(kept.value.at("definition").at("files").at(0).at("contents"),
              "def run(ctx):\n    return 1\n");

    nlohmann::json bad_skill = python_agent;
    bad_skill["skills"] = {"launch-missiles"};
    ApiResult refused_skill = call("create_agent", bad_skill);
    EXPECT_FALSE(refused_skill.ok);
    EXPECT_EQ(refused_skill.error.path, "/skills/0") << refused_skill.error.message;

    // Lake's own areas are skills too (agent-runtime#7): agent-runtime gives them their tools.
    nlohmann::json lake_skills = python_agent;
    lake_skills["name"] = "Drive reader";
    lake_skills["skills"] = {"drives", "transcripts", "repositories", "datasets", "streams",
                             "pipelines", "usage"};
    ApiResult lake_skilled = call("create_agent", lake_skills);
    ASSERT_TRUE(lake_skilled.ok) << lake_skilled.error.message;
    EXPECT_EQ(lake_skilled.value.at("skills").size(), 7u);
    ASSERT_TRUE(call("delete_agent", {{"id", lake_skilled.value.at("id")}}).ok);

    // An MCP connection in lake is a skill, mcp:<prefix> (lake#652): accepted through the API's
    // schema, at create and at update; a malformed one and an unknown plain skill are refused there.
    nlohmann::json mcp_skills = python_agent;
    mcp_skills["name"] = "Ticket Desk Agent";
    mcp_skills["skills"] = {"mcp:ticket_desk", "search"};
    ApiResult mcp_skilled = call("create_agent", mcp_skills);
    ASSERT_TRUE(mcp_skilled.ok) << mcp_skilled.error.message;
    EXPECT_EQ(mcp_skilled.value.at("skills").at(0), "mcp:ticket_desk");
    ApiResult mcp_updated = call("update_agent", {{"id", mcp_skilled.value.at("id")},
                                                  {"skills", {"mcp:wiki_2"}}});
    ASSERT_TRUE(mcp_updated.ok) << mcp_updated.error.message;
    EXPECT_EQ(mcp_updated.value.at("skills"), nlohmann::json::array({"mcp:wiki_2"}));
    for (const char* refused : {"mcp:", "mcp:Ticket", "mcp:9desk", "mcp:a-b",
                                "mcp:aaaaaaaaaaaaaaaaaaaaaaaaa", "mcp_ticket", "deploy"}) {
        nlohmann::json bad = python_agent;
        bad["name"] = std::string("Refused ") + refused;
        bad["skills"] = {refused};
        ApiResult answered = call("create_agent", bad);
        EXPECT_FALSE(answered.ok) << refused;
        EXPECT_EQ(answered.error.path, "/skills/0") << refused << ": " << answered.error.message;
    }
    ASSERT_TRUE(call("delete_agent", {{"id", mcp_skilled.value.at("id")}}).ok);

    // The Ask Orama contract: one explicit default, reported by list_agents.
    ApiResult listed = call("list_agents", nlohmann::json::object());
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value.at("default_agent_id"), ask.value.at("id"));

    // Zetta runs only native agents; the others are refused with where they do run.
    ApiResult chat = call("chat", {{"agent", python_id}, {"message", "hello"}});
    ASSERT_FALSE(chat.ok);
    EXPECT_NE(chat.error.message.find("agent-runtime"), std::string::npos);
    ApiResult status = call("agent_training_status", {{"id", python_id}});
    ASSERT_TRUE(status.ok) << status.error.message;
    EXPECT_NE(status.value.at("reason").get<std::string>().find("Not trained by Zetta"),
              std::string::npos);

    // Pipelines: only an approved graph can be granted, and the grant pins its approval.
    ASSERT_TRUE(call("create_project", {{"name", "py"}}).ok);
    ASSERT_TRUE(call("compile_python",
                     {{"project", "py"},
                      {"source",
                       "ds = load_dataset(\"accounts.parquet\")\n"
                       "clean = ds.dropna()\n"
                       "model = train(clean, target=\"churned\")\n"},
                      {"name", "churn"}})
                    .ok);
    const nlohmann::json grant = {{"id", python_id},
                                  {"pipelines", {{{"project", "py"}, {"graph", "churn"}}}}};
    ApiResult proposed = call("update_agent", grant);
    ASSERT_FALSE(proposed.ok);
    EXPECT_NE(proposed.error.message.find("not approved"), std::string::npos);
    ApiResult approved = call("approve_plan", {{"project", "py"}, {"graph", "churn"}});
    ASSERT_TRUE(approved.ok) << approved.error.message;
    ApiResult granted = call("update_agent", grant);
    ASSERT_TRUE(granted.ok) << granted.error.message;
    const nlohmann::json pinned = granted.value.at("pipelines").at(0);
    EXPECT_EQ(pinned.at("graph"), "churn");
    EXPECT_FALSE(pinned.at("version").get<std::string>().empty());
    EXPECT_EQ(call("get_agent", {{"id", python_id}}).value.at("pipelines").at(0).at("version"),
              pinned.at("version"));
}

// lake#238: a Pipeline granted to an agent is its tool — gated, then run — and only while it is
// still approved at the version the grant pinned.
TEST_F(AgentApiFixture, AGrantedPipelineRunsAsTheAgentsToolOnlyAtItsApprovedVersion) {
    ASSERT_TRUE(call("create_project", {{"name", "py"}}).ok);
    ASSERT_TRUE(call("compile_python",
                     {{"project", "py"},
                      {"source",
                       "ds = load_dataset(\"accounts.parquet\")\n"
                       "clean = ds.dropna()\n"
                       "model = train(clean, target=\"churned\")\n"},
                      {"name", "Weekly Churn"}})
                    .ok);
    ASSERT_TRUE(call("approve_plan", {{"project", "py"}, {"graph", "Weekly Churn"}}).ok);
    ApiResult created = call("create_agent",
                             {{"name", "Runner"},
                              {"model", "models/Qwen3-0.6B"},
                              {"pipelines", {{{"project", "py"}, {"graph", "Weekly Churn"}}}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    std::string reason;
    const auto agent = zetta::agent::agent_from_json(created.value, &reason);
    ASSERT_TRUE(agent) << reason;
    EXPECT_EQ(zetta::api::pipeline_tool_name("Weekly Churn"), "pipeline_weekly_churn");

    ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
    context.registry = &registry;
    const ApiResult ran =
        zetta::api::run_granted_pipeline(context, *agent, "pipeline_weekly_churn", nlohmann::json::object());
    // It passed the approval check and the gate and reached `run`. This embedding wires no
    // background owner for job capabilities, so `run` itself answers that; the live workspace runs it.
    EXPECT_TRUE(ran.ok || ran.error.message.find("job capability") != std::string::npos)
        << ran.error.message;

    EXPECT_FALSE(zetta::api::run_granted_pipeline(context, *agent, "pipeline_other", {}).ok)
        << "only a granted Pipeline is a tool";
    zetta::agent::AgentRecord stale = *agent;
    stale.pipelines[0].version = "2000-01-01T00:00:00Z";
    const ApiResult refused = zetta::api::run_granted_pipeline(context, stale, "pipeline_weekly_churn", {});
    ASSERT_FALSE(refused.ok);
    EXPECT_NE(refused.error.message.find("changed since it was granted"), std::string::npos);
}
