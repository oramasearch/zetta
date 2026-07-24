#pragma once

#include <functional>
#include <string>
#include <vector>

#include "zetta/agent/agent_record.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/tool_call.hpp"

// Tool-calling chat: turn a model's text output into tool calls, run them in the agent's
// sandbox, feed the results back, and loop until the model answers. The model itself is an
// INJECTED generate function, so the whole loop is unit-tested with a fake — the real MLX /
// CPU model is wired in at the serving edge. #849 (talk to an agent), #555 (the base agent).
namespace zetta::agent {

// Parse tool calls out of a model's text output. Recognizes Qwen-style
// <tool_call>{"name","arguments"}</tool_call> blocks (one or many), falling back to a single
// embedded JSON object carrying a "tool"/"name" key. Tolerates surrounding prose; a block
// whose contents are not valid JSON (or lack a tool name) is skipped. Non-throwing.
std::vector<ToolCall> parse_tool_calls(const std::string& text);

// One tool call executed during a chat turn: the call and the sandbox's outcome.
struct ChatToolInvocation {
    ToolCall call;
    ApplyStatus status = ApplyStatus::UnknownTool;
    std::string detail;  // the outcome reason (empty on Ok)
};

struct ChatResult {
    std::string reply;                            // the model's final text (no tool calls left)
    std::vector<ChatToolInvocation> invocations;  // executed calls, in order
    int rounds = 0;                               // model generations used
};

// Generate a completion for a prompt — the model, injected so the loop is testable.
using GenerateFn = std::function<std::string(const std::string& prompt)>;

// Run one tool-calling chat turn: generate from `conversation`; if the model emitted tool
// calls, apply each to `sandbox`, append their results to the conversation, and generate
// again; stop when a generation has no tool calls (that text is the reply) or after
// `max_rounds` (a hard bound, so a model that never stops still terminates). Returns the
// reply + the executed calls.
ChatResult run_chat_turn(const GenerateFn& generate, Sandbox& sandbox,
                         std::string conversation, int max_rounds = 4);

// The outcome of talking to an agent: the chat result, or a load failure.
struct ChatResponse {
    bool ok = false;
    ChatResult result;   // reply + tool invocations + rounds (valid when ok)
    std::string error;   // the reason (model/tokenizer load failure) when !ok
};

// Talk to `agent`: load its model + tokenizer (device-agnostic; `agent.model` is a local
// checkpoint dir), wire its tools into a sandbox, and run one tool-calling chat turn for
// `message`. Non-throwing — a load failure is ok=false + a reason. BLOCKS for seconds while
// the model loads + generates, so call it OFF the UI thread. Shared by the `chat` capability
// (MCP/REST) and the GUI chat panel, so both behave identically.
ChatResponse respond(const AgentRecord& agent, const std::string& message,
                     int max_new_tokens = 256, int max_rounds = 3);

}  // namespace zetta::agent
