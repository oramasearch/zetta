// SPDX-License-Identifier: MIT

// walk_facts — grading a THREAD rather than a reply.
//
// The controls that matter are the real transcripts. v16 passed the old walk and was plainly bad;
// if the strengthened walk does not fail it, the strengthening did nothing.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "zetta/agent/conversation_walk.hpp"
#include "zetta/agent/solution_graph.hpp"

namespace {

zetta::agent::WalkTurn answer(std::string ask, std::string reply, std::string grounding = "") {
    zetta::agent::WalkTurn turn;
    turn.ask = std::move(ask);
    turn.response.kind = zetta::agent::InteractionKind::Answer;
    turn.response.message = std::move(reply);
    turn.grounding = std::move(grounding);
    return turn;
}

zetta::agent::WalkTurn clarify(std::string ask, std::string reply) {
    zetta::agent::WalkTurn turn = answer(std::move(ask), std::move(reply));
    turn.response.kind = zetta::agent::InteractionKind::Clarify;
    return turn;
}

// A turn that actually builds — an action carrying a pipeline.
zetta::agent::WalkTurn builds(std::string ask, std::string reply) {
    zetta::agent::WalkTurn turn;
    turn.ask = std::move(ask);
    turn.response.kind = zetta::agent::InteractionKind::Action;
    turn.response.message = std::move(reply);
    zetta::agent::EndState solution;
    solution.set("connect", true);
    solution.set("model", true);
    solution.set("serve", true);
    turn.response.execution_graph = zetta::agent::build_solution_graph(solution);
    return turn;
}

// --- the affirmation vocabulary ------------------------------------------------------------

TEST(ConversationWalk, TheAffirmationVocabularyMatchesTheCorpus) {
    for (const char* word : {"yes", "yes please", "ok", "sure", "go ahead", "do it", "yep",
                             "please do", "Yes.", "  YES  "}) {
        EXPECT_TRUE(zetta::agent::is_affirmation(word)) << word;
    }
    // "what?" is a bare turn but not an affirmation — it approves nothing, so demanding a build
    // would be demanding the wrong thing.
    for (const char* word : {"what?", "no", "not that", "new one",
                             "yes, but use the other table instead"}) {
        EXPECT_FALSE(zetta::agent::is_affirmation(word)) << word;
    }
}

// --- the v16 transcript, verbatim ----------------------------------------------------------

std::vector<zetta::agent::WalkTurn> v16_walk() {
    return {answer("create me a churn project",
                   "Yes. Studio can create a new Project from your data and your needs."),
            answer("yes",
                   "Yes. Studio can `list_models` to show which models are trained and what "
                   "each costs."),
            answer("what?",
                   "Sorry - plainly: `list_models` has over 100 entries, so anything that can "
                   "make an HTTP request can drive it.")};
}

TEST(ConversationWalk, TheOldCheckPassesTheV16Walk) {
    // Not a bug — the point. Three distinct replies. This is the property the walk asserted, and
    // asserting it alone is what let v16 be recorded as the first PASS in four checkpoints.
    EXPECT_TRUE(zetta::agent::walk_facts(v16_walk()).matches("no_consecutive_repeat", true));
}

TEST(ConversationWalk, TheStrengthenedWalkFailsTheV16Walk) {
    const zetta::agent::EndState facts = zetta::agent::walk_facts(v16_walk());

    // "yes" was answered with a description of what Studio *can* do. Nothing was built.
    EXPECT_TRUE(facts.matches("affirmation_acted_on", false));
    // `list_models` appears nowhere in the thread before turn 2 asserts it.
    EXPECT_TRUE(facts.matches("identifiers_grounded", false));
    // "over 100 entries" is grounded in nothing.
    EXPECT_TRUE(facts.matches("numbers_grounded", false));

    EXPECT_TRUE(facts.matches("walk_passes", false));
}

// --- the v13 transcript ----------------------------------------------------------------------

TEST(ConversationWalk, TheV13WalkFailsOnTheRepeatItActuallyHad) {
    // v13's turn 1 was the best of the four checkpoints — a sensible clarifying question — and it
    // then answered "yes" and "what?" with the same sentence twice.
    const zetta::agent::EndState facts = zetta::agent::walk_facts(
        {clarify("create me a churn project",
                 "Which pipeline would you like me to base this on? I'd apply it to `churn-uc1`."),
         answer("yes",
                "Yes. Studio will list the trained models and their measures on `list_models`."),
         answer("what?",
                "Yes. Studio will list the trained models and their measures on `list_models`.")});

    EXPECT_TRUE(facts.matches("no_consecutive_repeat", false));
    EXPECT_TRUE(facts.matches("walk_passes", false));
}

// --- what passing looks like ------------------------------------------------------------------

TEST(ConversationWalk, AThreadThatAnswersItselfPasses) {
    const zetta::agent::EndState facts = zetta::agent::walk_facts(
        {clarify("create me a churn project",
                 "I'd base it on `churn-uc1`. Shall I build that?"),
         builds("yes", "Building `churn-uc1` now."),
         answer("what?",
                "I built the `churn-uc1` pipeline - it connects your data, trains a model and "
                "serves the scores.")});

    EXPECT_TRUE(facts.matches("no_consecutive_repeat", true));
    EXPECT_TRUE(facts.matches("affirmation_acted_on", true));
    EXPECT_TRUE(facts.matches("identifiers_grounded", true));
    EXPECT_TRUE(facts.matches("numbers_grounded", true));
    EXPECT_TRUE(facts.matches("walk_passes", true));
}

// --- properties are emitted only when exercised ------------------------------------------------

TEST(ConversationWalk, AThreadWithNoAffirmationDoesNotClaimThatProperty) {
    // Otherwise a walk that never says "yes" would report the propose-and-approve rung as passing.
    const zetta::agent::EndState facts = zetta::agent::walk_facts(
        {answer("what tables do I have?", "You have `accounts`.", "Table: accounts\n"),
         answer("and how many rows?", "`accounts` has 1200 rows.",
                "Table: accounts\nRows: 1200\n")});

    EXPECT_FALSE(facts.has("affirmation_acted_on"));
    EXPECT_TRUE(facts.matches("walk_passes", true));
}

TEST(ConversationWalk, AnEmptyWalkPassesNothingAndClaimsNothing) {
    const zetta::agent::EndState facts = zetta::agent::walk_facts({});
    EXPECT_FALSE(facts.has("no_consecutive_repeat"));
    EXPECT_FALSE(facts.has("walk_passes"));
}

// A single turn cannot repeat anything, so the property is not exercised — but its own claims are
// still checked.
TEST(ConversationWalk, ASingleTurnIsStillCheckedForFabrication) {
    const zetta::agent::EndState facts = zetta::agent::walk_facts(
        {answer("how many rows?", "It has 4096 rows.", "Table: accounts\nRows: 1200\n")});

    EXPECT_FALSE(facts.has("no_consecutive_repeat"));
    EXPECT_TRUE(facts.matches("numbers_grounded", false));
    EXPECT_TRUE(facts.matches("walk_passes", false));
}

}  // namespace
