#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/tool_call.hpp"
#include "zetta/ir/execution_graph.hpp"

// The Orama Studio auto-AI agent's **action** for one turn (#873). This is what the agent
// emits — mirroring blender-rl's Blender-Python action: the engine RUNS it and the correctness
// gate scores the running pipeline, active-weight renormalized over the checks the use case
// exercises. It is NOT the reward's checks (those live on the task as `required` / info.checks).
//
//   { "message": "...",            // shown to the user (plain language, no jargon)
//     "tool_calls": [ {tool, arguments} ],  // set up the pipeline (may call other models)
//     "execution_graph": { nodes, edges } } // the programmatic pipeline (the IR)
//
// See docs/agent/training-coverage-checklist.md §6 and the #873 epic for the reshape from the
// facts output the base agent emits today to this action. Non-throwing at the JSON boundary
// (from_json returns nullopt + a path-qualified reason), per the zetta-core convention.
namespace zetta::agent {

struct StudioAction {
    std::string message;                 // what "Ask Orama" shows the user
    std::vector<ToolCall> tool_calls;    // pipeline setup actions (name + JSON arguments)
    zetta::ir::ExecutionGraph execution_graph;  // the pipeline itself
};

// Serialize an action to its wire JSON ({message, tool_calls, execution_graph}).
nlohmann::json to_json(const StudioAction& action);

// Parse + validate an action at the boundary. Returns nullopt (and, when `error` is non-null,
// a human-readable reason) on a malformed document, a bad tool call, or an invalid graph.
std::optional<StudioAction> action_from_json(const nlohmann::json& document,
                                             std::string* error = nullptr);

// The StudioAction JSON Schema (draft-07) — the single declarative source of truth for the
// action shape, for callers/tests and the capability/tool boundary.
const nlohmann::json& studio_action_schema();

}  // namespace zetta::agent
