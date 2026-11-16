// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

// A ToolCall is one agent action: the name of a tool and the JSON arguments to
// call it with — the same shape as an MCP tool call (#555; docs/agent/README.md).
// It is plain data: a policy emits ToolCalls, the sandbox applies them to the
// world, and a rollout is the sequence of them.
namespace zetta::agent {

struct ToolCall {
    std::string tool;          // the tool's name
    nlohmann::json arguments;  // its arguments (a JSON object)

    ToolCall() = default;
    ToolCall(std::string tool_name, nlohmann::json args)
        : tool(std::move(tool_name)), arguments(std::move(args)) {}

    bool operator==(const ToolCall&) const = default;
};

}  // namespace zetta::agent
