// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>

// The Orama Studio auto-AI agent's canonical **system prompt** (#873), grounded in the product
// it drives: it turns a plain-language ask into a production AI **pipeline** (an execution graph
// the UI only ever calls a "pipeline"), scored by running it against the user's real data and
// passing the correctness gate — the blender-rl RLVR game, per use case. The agent's action is
// `{message, tool_calls, execution_graph}` (see agent/action.hpp), NOT the solution facts the
// base agent emits today (#873 reshapes that). It is a SINGLE source of truth used in three
// places so training and inference agree:
//   1. the SFT render (tests/tokenizer/test_qwen_real_checkpoint.cpp) prepends it to every
//      training example, so the model is trained WITH this context,
//   2. agent::respond builds the inference prompt in the same shape, and
//   3. the base agent's system_prompt field defaults to it (capabilities.cpp create_agent).
// The framed turn is `<system>\n\n<ask>\n\n` and the model completes with the JSON action —
// the system prompt instructs it to, so no inline cue is needed (blender-rl frames the same way).
namespace zetta::agent {

// The canonical Studio auto-AI system prompt (see the header comment). Stable text — changing it
// changes what the SFT bakes in, so a change should be paired with a retrain.
std::string_view base_agent_system_prompt();

// Frame one turn the way the agent is trained: the system prompt, then the ask. The model
// completes with the JSON action ({message, tool_calls, execution_graph}). Used by both the SFT
// render and inference so they never drift. `system_prompt` lets a per-agent override flow
// through (defaults to base_agent_system_prompt() for the base agent).
std::string agent_prompt_format(std::string_view system_prompt, std::string_view ask);

}  // namespace zetta::agent
