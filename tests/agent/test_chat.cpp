#include "zetta/agent/chat.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/agent/sandbox.hpp"

namespace {

using zetta::agent::ApplyStatus;
using zetta::agent::parse_tool_calls;
using zetta::agent::run_chat_turn;
using zetta::agent::Sandbox;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;

TEST(ParseToolCalls, ReadsAToolCallBlock) {
    const auto calls = parse_tool_calls(
        "Let me connect it.\n"
        "<tool_call>{\"name\": \"connect\", \"arguments\": {\"source\": \"warehouse\"}}</tool_call>");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].tool, "connect");
    EXPECT_EQ(calls[0].arguments.at("source"), "warehouse");
}

TEST(ParseToolCalls, ReadsMultipleBlocksAndSkipsGarbage) {
    const auto calls = parse_tool_calls(
        "<tool_call>{\"tool\": \"connect\"}</tool_call>"
        "<tool_call>not json</tool_call>"
        "<tool_call>{\"tool\": \"serve\", \"args\": {\"port\": 8080}}</tool_call>");
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].tool, "connect");
    EXPECT_EQ(calls[1].tool, "serve");
    EXPECT_EQ(calls[1].arguments.at("port"), 8080);
}

TEST(ParseToolCalls, PlainTextHasNoCalls) {
    EXPECT_TRUE(parse_tool_calls("Here is your churn pipeline. Done.").empty());
}

TEST(ParseToolCalls, FallsBackToABareJsonObject) {
    const auto calls = parse_tool_calls("{\"tool\": \"index\", \"arguments\": {}}");
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].tool, "index");
}

TEST(RunChatTurn, RunsAToolThenAnswers) {
    Sandbox sandbox;
    bool connected = false;
    sandbox.register_tool("connect", [&connected](zetta::agent::EndState&,
                                                  const nlohmann::json&) {
        connected = true;
        return ToolResult::success();
    });

    int calls = 0;
    const auto generate = [&calls](const std::string&) -> std::string {
        // First generation: a tool call. Second: the final answer.
        return ++calls == 1
                   ? "<tool_call>{\"tool\": \"connect\", \"arguments\": {}}</tool_call>"
                   : "Connected — your data is ready.";
    };

    const auto result = run_chat_turn(generate, sandbox, "connect my warehouse");
    EXPECT_TRUE(connected);
    EXPECT_EQ(result.reply, "Connected — your data is ready.");
    EXPECT_EQ(result.rounds, 2);
    ASSERT_EQ(result.invocations.size(), 1u);
    EXPECT_EQ(result.invocations[0].call.tool, "connect");
    EXPECT_EQ(result.invocations[0].status, ApplyStatus::Ok);
}

TEST(RunChatTurn, AnswersDirectlyWithNoTools) {
    Sandbox sandbox;
    const auto generate = [](const std::string&) -> std::string {
        return "The capital of France is Paris.";
    };
    const auto result = run_chat_turn(generate, sandbox, "capital of France?");
    EXPECT_EQ(result.reply, "The capital of France is Paris.");
    EXPECT_EQ(result.rounds, 1);
    EXPECT_TRUE(result.invocations.empty());
}

TEST(RunChatTurn, ReportsAnUnknownTool) {
    Sandbox sandbox;  // no tools registered
    int calls = 0;
    const auto generate = [&calls](const std::string&) -> std::string {
        return ++calls == 1 ? "<tool_call>{\"tool\": \"nope\"}</tool_call>"
                            : "Sorry, I could not do that.";
    };
    const auto result = run_chat_turn(generate, sandbox, "do a thing");
    ASSERT_EQ(result.invocations.size(), 1u);
    EXPECT_EQ(result.invocations[0].status, ApplyStatus::UnknownTool);
    EXPECT_EQ(result.reply, "Sorry, I could not do that.");
}

}  // namespace
