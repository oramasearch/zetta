// SPDX-License-Identifier: MIT

#include "zetta/agent/conversation_walk.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/agent/interaction.hpp"
#include "zetta/agent/response_facts.hpp"

namespace zetta::agent {
namespace {

// Letters and digits only, lowercased. "Yes." and "  YES  " are the same approval, and trailing
// punctuation deciding otherwise would be an accident of typing rather than of meaning.
std::string normalized(std::string_view text) {
    std::string out;
    for (const char character : text) {
        if (std::isalnum(static_cast<unsigned char>(character)) != 0) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(character))));
        }
    }
    return out;
}

}  // namespace

bool is_affirmation(std::string_view ask) {
    // Matches the corpus's `bare_turn` family. A grader recognizing a different set than the
    // corpus teaches would score the agent on words it was never shown.
    //
    // Whole-ask only. "yes, but use the other table instead" carries its own instruction, so the
    // correct response is not necessarily to build the thing already proposed — treating it as a
    // bare approval would demand the wrong behaviour.
    static const std::vector<std::string> kAffirmations = {
        normalized("yes"),       normalized("yes please"), normalized("ok"),
        normalized("okay"),      normalized("sure"),       normalized("go ahead"),
        normalized("do it"),     normalized("yep"),        normalized("please do"),
        normalized("okay... go ahead")};

    const std::string word = normalized(ask);
    return std::ranges::find(kAffirmations, word) != kAffirmations.end();
}

EndState walk_facts(const std::vector<WalkTurn>& turns) {
    EndState facts;
    if (turns.empty()) return facts;

    // Each property is tracked as "was it exercised" alongside "did it hold". A walk that never
    // says "yes" has not demonstrated the propose-and-approve rung, and reporting it as passing
    // would be the same overclaim that made distinctness look like a PASS.
    bool repeat_exercised = false;
    bool no_consecutive_repeat = true;
    bool affirmation_exercised = false;
    bool affirmation_acted_on = true;
    bool identifiers_grounded = true;
    bool numbers_grounded = true;

    std::vector<PriorTurn> history;
    for (const WalkTurn& turn : turns) {
        const EndState turn_facts =
            response_solution_facts(turn.response, turn.grounding, history);

        if (!history.empty()) {
            repeat_exercised = true;
            if (turn_facts.matches("repeated_prior_turn", true)) no_consecutive_repeat = false;
        }

        // An affirmation approves the thing just proposed, so the response must BUILD it.
        // `built_nothing` is what `clarify` and `abstain` require and what an action must not be;
        // describing what Studio *can* do leaves it true, which is exactly how v16 failed.
        if (is_affirmation(turn.ask)) {
            affirmation_exercised = true;
            if (!turn_facts.matches("built_nothing", false)) affirmation_acted_on = false;
        }

        // These two are per-turn facts the walk simply never asserted. `numbers_from_context` is
        // emitted only when the turn had grounding, and `identifiers_from_context` only when it
        // had grounding or history — a turn with neither is not silently counted as passing.
        if (turn_facts.has("identifiers_from_context")
            && turn_facts.matches("identifiers_from_context", false)) {
            identifiers_grounded = false;
        }
        if (turn_facts.has("numbers_from_context")
            && turn_facts.matches("numbers_from_context", false)) {
            numbers_grounded = false;
        }

        PriorTurn prior;
        prior.ask = turn.ask;
        prior.reply = turn.response.message;
        for (const ToolCall& call : turn.response.tool_calls) {
            ToolOutcome outcome;
            outcome.tool = call.tool;
            outcome.status = "ok";
            prior.tools.push_back(outcome);
        }
        history.push_back(std::move(prior));
    }

    if (repeat_exercised) facts.set("no_consecutive_repeat", no_consecutive_repeat);
    if (affirmation_exercised) facts.set("affirmation_acted_on", affirmation_acted_on);
    facts.set("identifiers_grounded", identifiers_grounded);
    facts.set("numbers_grounded", numbers_grounded);

    facts.set("walk_passes", (!repeat_exercised || no_consecutive_repeat)
                                 && (!affirmation_exercised || affirmation_acted_on)
                                 && identifiers_grounded && numbers_grounded);
    return facts;
}

}  // namespace zetta::agent
