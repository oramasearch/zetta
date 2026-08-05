// The Studio auto-AI agent's action {message, tool_calls, execution_graph} (#873) round-trips
// through JSON and rejects malformed documents at the boundary.
#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/agent/action.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"

namespace {

zetta::agent::StudioAction sample_action() {
    zetta::agent::StudioAction action;
    action.message = "I'll build you a churn pipeline and serve a ranked at-risk list.";
    action.tool_calls.emplace_back("add_dataset", nlohmann::json{{"path", "accounts.parquet"}});
    action.tool_calls.emplace_back("run", nlohmann::json{{"top_k", 10}});
    const auto connect =
        action.execution_graph.add_node(zetta::ir::StepKind::Connect, "Load accounts");
    const auto model =
        action.execution_graph.add_node(zetta::ir::StepKind::Model, "Train churn model");
    action.execution_graph.add_edge(connect->id, model->id);
    return action;
}

TEST(StudioAction, RoundTripsThroughJson) {
    const zetta::agent::StudioAction original = sample_action();
    const nlohmann::json wire = zetta::agent::to_json(original);

    // The wire shape is exactly {message, tool_calls, execution_graph}.
    EXPECT_TRUE(wire.contains("message"));
    ASSERT_TRUE(wire.at("tool_calls").is_array());
    ASSERT_EQ(wire.at("tool_calls").size(), 2u);
    EXPECT_EQ(wire.at("tool_calls")[0].at("tool"), "add_dataset");
    EXPECT_TRUE(wire.at("execution_graph").contains("nodes"));

    std::string error;
    const std::optional<zetta::agent::StudioAction> parsed =
        zetta::agent::action_from_json(wire, &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->message, original.message);
    ASSERT_EQ(parsed->tool_calls.size(), 2u);
    EXPECT_EQ(parsed->tool_calls[1].tool, "run");
    EXPECT_EQ(parsed->tool_calls[1].arguments.at("top_k"), 10);
    EXPECT_EQ(parsed->execution_graph.node_count(), 2u);
    EXPECT_EQ(parsed->execution_graph.edge_count(), 1u);
}

TEST(StudioAction, PartsAreOptional) {
    // A message-only turn (a clarifying question) carries no graph and no tool calls.
    std::string error;
    const std::optional<zetta::agent::StudioAction> parsed = zetta::agent::action_from_json(
        nlohmann::json{{"message", "Which table holds cancellations?"}}, &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_TRUE(parsed->tool_calls.empty());
    EXPECT_TRUE(parsed->execution_graph.empty());
}

TEST(StudioAction, ParsesFromFreeFormGeneration) {
    // The model's generation is the action JSON, possibly with surrounding text.
    const std::string generation =
        "Sure! Here's the pipeline:\n"
        "{\"message\": \"Building your churn pipeline.\", "
        "\"tool_calls\": [{\"tool\": \"run\", \"arguments\": {\"top_k\": 10}}], "
        "\"execution_graph\": {\"nodes\": [], \"edges\": []}}\n"
        "Let me know if you'd like changes.";
    const std::optional<zetta::agent::StudioAction> action =
        zetta::agent::parse_studio_action(generation);
    ASSERT_TRUE(action);
    EXPECT_EQ(action->message, "Building your churn pipeline.");
    ASSERT_EQ(action->tool_calls.size(), 1u);
    EXPECT_EQ(action->tool_calls[0].tool, "run");
}

TEST(StudioAction, ParseReturnsNulloptWithoutAnObject) {
    EXPECT_FALSE(zetta::agent::parse_studio_action("no json here"));
    EXPECT_FALSE(zetta::agent::parse_studio_action("{ not valid json"));
}

TEST(StudioAction, RejectsMalformed) {
    std::string error;
    EXPECT_FALSE(zetta::agent::action_from_json(nlohmann::json::array(), &error));  // not object
    EXPECT_FALSE(error.empty());
    // A tool call without a `tool` name is rejected with a path-qualified reason.
    EXPECT_FALSE(zetta::agent::action_from_json(
        nlohmann::json{{"tool_calls", nlohmann::json::array({nlohmann::json::object()})}}, &error));
    EXPECT_NE(error.find("tool_calls[0]"), std::string::npos);
}

// A model that never learned to stop writes a SECOND action after the first. Spanning first-`{`
// to last-`}` covered both, produced invalid JSON, and threw away a perfectly good first action —
// which is exactly how a checkpoint that WAS emitting correct tool calls measured 0% parse rate.
TEST(StudioAction, ParsesTheFirstActionWhenTheModelKeepsGenerating) {
    const std::string runaway =
        R"({"kind":"action","message":"Here's the pipeline.","tool_calls":[{"tool":"connect",)"
        R"("arguments":{"name":"source"}}]})"
        R"({"kind":"action","message":"Here's the pipeline.","tool_calls":[]})";
    const std::optional<zetta::agent::StudioAction> action =
        zetta::agent::parse_studio_action(runaway);
    ASSERT_TRUE(action.has_value()) << "a trailing second action must not destroy the first";
    EXPECT_EQ(action->kind, zetta::agent::InteractionKind::Action);
    ASSERT_EQ(action->tool_calls.size(), 1U);
    EXPECT_EQ(action->tool_calls[0].tool, "connect");
}

// A brace or quote INSIDE the message must not end the scan early.
TEST(StudioAction, BracesInsideTheMessageDoNotTruncateTheScan) {
    const std::string tricky =
        R"({"kind":"answer","message":"Use {\"a\": 1} as the config }","tool_calls":[]} trailing)";
    const std::optional<zetta::agent::StudioAction> action =
        zetta::agent::parse_studio_action(tricky);
    ASSERT_TRUE(action.has_value());
    EXPECT_NE(action->message.find("config"), std::string::npos);
}

// A truncated generation has no complete object, and must fail rather than half-parse.
TEST(StudioAction, ATruncatedGenerationDoesNotParse) {
    const std::string cut =
        R"({"kind":"action","message":"Here's the pipeline.","tool_calls":[{"tool":"conn)";
    EXPECT_FALSE(zetta::agent::parse_studio_action(cut).has_value());
}

}  // namespace
