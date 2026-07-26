#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "zetta/agent/action.hpp"
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
    // The action the model emitted, when its generation parsed as one (#916). The base
    // agent is TRAINED to emit exactly one `{message, tool_calls, execution_graph}` action
    // (agent/action.hpp; render_action_example is the SFT target), so this — not `reply` —
    // is the structured truth of the turn: `action->message` is the user-facing prose and
    // `action->execution_graph` is the proposed Pipeline. Empty when the generation did not
    // parse as an action (an error path worth surfacing, not text worth showing raw).
    std::optional<StudioAction> action;
};

// Runs one named tool on behalf of the agent and returns what it produced (#904).
// The embedding supplies this — the API layer binds it to the capability registry
// under the CALLER's identity, so the agent can never reach past what that caller
// may do — keeping zetta-core free of any dependency on the registry. Empty
// function = the agent has no live tools and only its recorded ones.
using AgentToolRunner =
    std::function<ToolResult(const std::string& tool, const nlohmann::json& arguments)>;

// The tools an agent may call, and what each is for — rendered into the prompt so
// the model knows what exists. Name + one line, in the caller's vocabulary.
struct AgentToolSpec {
    std::string name;
    std::string description;
};

// Build the EXACT prompt `respond` sends to the model (#918). Exposed so the contract test can
// assert it is byte-identical to what the corpus renders for the same input — the check that
// makes train/serve skew structurally impossible rather than a thing we keep rediscovering.
// `agent.system_prompt` overrides the trained prompt when set; empty means "the prompt this
// model was trained under", never "no prompt".
std::string build_chat_prompt(const AgentRecord& agent, const std::string& message,
                              const std::string& grounding = {},
                              const std::vector<AgentToolSpec>& tool_specs = {});

// Talk to `agent`: load its model + tokenizer (device-agnostic; `agent.model` is a local
// checkpoint dir), wire its tools into a sandbox, and run one tool-calling chat turn for
// `message`. Non-throwing — a load failure is ok=false + a reason. BLOCKS for seconds while
// the model loads + generates, so call it OFF the UI thread. Shared by the `chat` capability
// (MCP/REST) and the GUI chat panel, so both behave identically.
//
// `tools` + `tool_specs` are the live tools (#904): when supplied, the agent is told
// what it can call and each <tool_call> it emits runs through the runner, with the
// result fed back as <tool_result> for the next round. Omit them and behavior is
// exactly as before — the agent's recorded tools, which only mark that they ran.
// `grounding` is facts the embedding already read from the node — a trained model's
// measure and ranked drivers, say — placed in the conversation ahead of the user's
// message so the agent answers from what the pipeline actually produced instead of
// from its training priors (#914). Empty = answer from the model alone.
ChatResponse respond(const AgentRecord& agent, const std::string& message,
                     int max_new_tokens = 256, int max_rounds = 3,
                     const AgentToolRunner& tools = {},
                     const std::vector<AgentToolSpec>& tool_specs = {},
                     const std::string& grounding = {});

}  // namespace zetta::agent
