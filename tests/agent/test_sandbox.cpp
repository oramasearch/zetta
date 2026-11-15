// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/tool_call.hpp"

namespace {

using zetta::agent::ApplyStatus;
using zetta::agent::EndState;
using zetta::agent::EndStateGrader;
using zetta::agent::Sandbox;
using zetta::agent::ToolCall;
using zetta::agent::ToolResult;

// A ticket sandbox with two tools: assign_ticket(assignee) and close_ticket().
// Their effects mutate the world's facts; assign_ticket rejects a missing arg.
Sandbox make_ticket_sandbox() {
    EndState initial;
    initial.set("ticket.status", "open");
    Sandbox sandbox{std::move(initial)};

    sandbox.register_tool(
        "assign_ticket",
        [](EndState& world, const nlohmann::json& args) -> ToolResult {
            if (!args.contains("assignee") || !args.at("assignee").is_string()) {
                return ToolResult::failure("assign_ticket needs a string assignee");
            }
            world.set("ticket.assignee", args.at("assignee"));
            return ToolResult::success();
        });
    sandbox.register_tool(
        "close_ticket",
        [](EndState& world, const nlohmann::json&) -> ToolResult {
            world.set("ticket.status", "closed");
            return ToolResult::success();
        });
    return sandbox;
}

TEST(Sandbox, CannedCallsDriveTheWorldToTheGoal) {
    Sandbox sandbox = make_ticket_sandbox();
    EXPECT_TRUE(
        sandbox.apply(ToolCall{"assign_ticket", nlohmann::json{{"assignee", "aria"}}})
            .ok());
    EXPECT_TRUE(
        sandbox.apply(ToolCall{"close_ticket", nlohmann::json::object()}).ok());

    EndState required;
    required.set("ticket.status", "closed");
    required.set("ticket.assignee", "aria");
    EXPECT_TRUE(EndStateGrader{required}.grade(sandbox.state()).passed);
}

TEST(Sandbox, StartsFromInitialState) {
    Sandbox sandbox = make_ticket_sandbox();
    EXPECT_TRUE(sandbox.state().matches("ticket.status", "open"));
    EXPECT_FALSE(sandbox.state().has("ticket.assignee"));
}

TEST(Sandbox, UnknownToolIsRejectedNotIgnored) {
    Sandbox sandbox = make_ticket_sandbox();
    const auto outcome =
        sandbox.apply(ToolCall{"delete_database", nlohmann::json::object()});
    EXPECT_EQ(outcome.status, ApplyStatus::UnknownTool);
    EXPECT_FALSE(outcome.ok());
    EXPECT_FALSE(outcome.reason.empty());
    // The world is untouched by an unknown call.
    EXPECT_TRUE(sandbox.state().matches("ticket.status", "open"));
}

TEST(Sandbox, ToolFailureIsReported) {
    Sandbox sandbox = make_ticket_sandbox();
    // assign_ticket with no assignee arg -> the effect reports failure.
    const auto outcome =
        sandbox.apply(ToolCall{"assign_ticket", nlohmann::json::object()});
    EXPECT_EQ(outcome.status, ApplyStatus::ToolFailed);
    EXPECT_FALSE(outcome.ok());
    EXPECT_FALSE(outcome.reason.empty());
    EXPECT_FALSE(sandbox.state().has("ticket.assignee"));
}

}  // namespace
