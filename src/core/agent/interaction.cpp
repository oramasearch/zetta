#include "zetta/agent/interaction.hpp"

#include <string>

#include "zetta/agent/base_prompt.hpp"

namespace zetta::agent {

std::string_view to_string(InteractionKind kind) {
    switch (kind) {
        case InteractionKind::Action:
            return "action";
        case InteractionKind::Answer:
            return "answer";
        case InteractionKind::Clarify:
            return "clarify";
        case InteractionKind::Abstain:
            return "abstain";
    }
    return "action";
}

std::optional<InteractionKind> interaction_kind_from_string(std::string_view text) {
    if (text == "action") return InteractionKind::Action;
    if (text == "answer") return InteractionKind::Answer;
    if (text == "clarify") return InteractionKind::Clarify;
    if (text == "abstain") return InteractionKind::Abstain;
    return std::nullopt;
}

std::string_view grounding_preamble() {
    // Worded once, used by both the corpus renderer and the serving harness. The instruction is
    // explicit that these numbers are the source of truth because the failure mode to design
    // against is a fluent, plausible answer citing accounts and scores that do not exist.
    return
        "Here is what this Project's pipeline actually produced on this node. Answer using ONLY "
        "these facts, and name them. Do not invent accounts, scores, or measures. If these facts "
        "do not answer the question, say so instead of estimating.";
}

std::string render_interaction_prompt(std::string_view system_prompt, std::string_view grounding,
                                      std::string_view ask) {
    // The retrieved context rides INSIDE the ask, within the trained frame — never as a foreign
    // block wrapped around it. `agent_prompt_format` stays the single frame both paths share.
    std::string framed_ask;
    if (!grounding.empty()) {
        framed_ask.append(grounding_preamble());
        framed_ask.append("\n\n");
        framed_ask.append(grounding);
        framed_ask.append("\n\n");
    }
    framed_ask.append(ask);
    return agent_prompt_format(system_prompt, framed_ask);
}

}  // namespace zetta::agent
