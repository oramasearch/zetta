#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// MCP **prompts** for this node (#872). A prompt is a named, parameterized message
// template a client (Claude Desktop/Code, any MCP host) can surface as a slash command
// and expand into a conversation turn. The node's tools are the capabilities
// (`tools/list`); the prompts are guided *workflows* over those tools — the first is
// "build_and_run", which drives an agent to build + run an execution graph from a
// plain-language ask (the `agent_run` capability).
//
// This is the server side of the MCP `prompts` capability: `prompts/list` returns
// `mcp_prompt_definitions()`, `prompts/get` returns `render_mcp_prompt(name, arguments)`.
// No new wire format — these are plain MCP JSON-RPC result shapes. Boundary:
// render_mcp_prompt is non-throwing (a bad name / missing required argument returns
// std::nullopt with a reason), matching the zetta-core convention.
namespace zetta::api {

// The `prompts` array for a `prompts/list` result: one object per prompt with
// { name, title, description, arguments: [{ name, description, required }] }.
nlohmann::json mcp_prompt_definitions();

// The `prompts/get` result for `name` with the client-supplied `arguments`
// ({ description, messages: [{ role, content: { type:"text", text } }] }), or
// std::nullopt when the prompt is unknown or a required argument is missing (with
// `error`, when non-null, set to a human-readable reason).
std::optional<nlohmann::json> render_mcp_prompt(const std::string& name,
                                                const nlohmann::json& arguments,
                                                std::string* error = nullptr);

}  // namespace zetta::api
