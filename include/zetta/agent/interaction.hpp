#pragma once

#include <optional>
#include <string>
#include <string_view>

// The interaction contract (#916, #918) — the ONE definition of how a turn is framed and what
// a response may be, shared by the training corpus and the serving harness.
//
// The agent is harness code with a model attached: the harness assembles the prompt, the model
// completes it, the harness parses and acts. Everything works only while the string the harness
// builds at SERVING time is the string the corpus rendered at TRAINING time. Not "the same
// shape" — the same function. `render_interaction_prompt` below is that function; both callers
// use it, and `tests/agent/test_interaction_contract.cpp` asserts they agree byte for byte.
// (Skipping that is what produced the earlier skew: serving prepended a per-agent prompt and
// appended "User:"/"Assistant:" turn labels that appear in ZERO training examples.)
//
// A response declares its own MODE, so the harness's parse is a switch and never a guess, and
// the grader can score "wrong mode" as a hard failure — emitting a pipeline for an ambiguous
// ask, or answering where it should decline, is precisely the discrimination the corpus teaches.
namespace zetta::agent {

// The mode a response declares. One envelope, four modes — chosen over four output formats so
// that broad interaction coverage does not confound the model's output contract.
enum class InteractionKind {
    Action,   // build or change a Pipeline: message + tool_calls + execution_graph
    Answer,   // answer from the supplied context: message (markdown), no pipeline
    Clarify,  // the ask is ambiguous: ask ONE clarifying question, build nothing
    Abstain,  // out of scope, or in scope but unsupported: decline, build nothing
};

std::string_view to_string(InteractionKind kind);

// Parse a mode name. Returns nullopt on an unknown value (non-throwing boundary).
std::optional<InteractionKind> interaction_kind_from_string(std::string_view text);

// The instruction that introduces retrieved context. Lives here, NOT at a call site, because
// training and serving must word it identically — a difference here is a train/serve skew.
std::string_view grounding_preamble();

// Frame one turn exactly the way the agent is trained: the system prompt, the retrieved context
// (when the harness supplied any), then the ask. The model completes with the response envelope.
//
// `grounding` is deterministic retrieval output — facts the ENGINE read off the node before
// generating (a model's measure and ranked drivers, an at-risk list, a schema, cluster state).
// The engine fetches and the model only answers from what it was handed, which is what makes
// the reward well defined (we know the context, so we know ground truth) and what makes the
// retrieval auditable. Model-driven retrieval is a later capability (#859), not this contract.
std::string render_interaction_prompt(std::string_view system_prompt, std::string_view grounding,
                                      std::string_view ask);

}  // namespace zetta::agent
