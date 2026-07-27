// response_solution_facts — the projection that turns a response into gradeable facts, and with
// it the anti-fabrication rule. This is the function that decides whether an answer is trustworthy,
// so its edges are worth pinning directly rather than only through the corpus.
#include <gtest/gtest.h>

#include <string>

#include "zetta/agent/action.hpp"
#include "zetta/agent/interaction.hpp"
#include "zetta/agent/response_facts.hpp"
#include "zetta/agent/solution_graph.hpp"

namespace {

const std::string kGrounding =
    "Project: Churn risk - EMEA mid-market\n"
    "Table: accounts\n"
    "Rows: 1200, columns: 9\n"
    "AUC: 0.880\n";

zetta::agent::StudioAction answer_saying(std::string message) {
    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Answer;
    action.message = std::move(message);
    return action;
}

// A figure the context actually contains is fine, compared AS WRITTEN.
TEST(ResponseFacts, ANumberPresentInTheContextIsGrounded) {
    EXPECT_TRUE(zetta::agent::response_solution_facts(answer_saying("The model reached 0.880 AUC."),
                                                      kGrounding)
                    .matches("numbers_from_context", true));
}

// Rounding a retrieved figure produces a number that was never retrieved. For a product whose
// pitch is verified correctness, that is a defect, not a nitpick.
TEST(ResponseFacts, ARoundedNumberIsNotGrounded) {
    EXPECT_FALSE(zetta::agent::response_solution_facts(answer_saying("AUC came in at 0.88."),
                                                       kGrounding)
                     .matches("numbers_from_context", true));
}

// A plausible figure that simply is not in the context is the core fabrication failure mode.
TEST(ResponseFacts, AnInventedNumberIsNotGrounded) {
    EXPECT_FALSE(zetta::agent::response_solution_facts(
                     answer_saying("There are 4300 accounts at risk."), kGrounding)
                     .matches("numbers_from_context", true));
}

// Digits inside a backticked identifier are part of a NAME, not a claim. A reply naming the real
// column `precision_at_10` was graded as fabricating the figure 10 — so a correct answer failed
// the anti-fabrication check, inverting the rule's purpose.
TEST(ResponseFacts, DigitsInsideACodeSpanAreNotAClaimedNumber) {
    EXPECT_TRUE(zetta::agent::response_solution_facts(
                    answer_saying("I added a step producing `precision_at_10` on `top_20_accounts`."),
                    kGrounding)
                    .matches("numbers_from_context", true));
}

// ...and the correction must not weaken the rule: a figure stated in prose alongside a code span
// is still checked.
TEST(ResponseFacts, ProseFiguresAreStillCheckedBesideACodeSpan) {
    EXPECT_FALSE(zetta::agent::response_solution_facts(
                     answer_saying("`precision_at_10` reached 0.93 on `accounts`."), kGrounding)
                     .matches("numbers_from_context", true));
}

// An unterminated backtick must not silently swallow the rest of the message — a model could
// otherwise open one code span and state any figure it liked after it.
TEST(ResponseFacts, AnUnterminatedCodeSpanDoesNotWhitelistTheRest) {
    const zetta::agent::EndState facts = zetta::agent::response_solution_facts(
        answer_saying("The measure is `auc and we saw 0.93 across the board."), kGrounding);
    EXPECT_FALSE(facts.matches("numbers_from_context", true));
}

// With no context there is nothing to ground against, so the projection emits no grounding facts
// at all. The corpus relies on this: an ungrounded reference must stay qualitative, because
// nothing here would catch a figure it invented.
TEST(ResponseFacts, WithoutGroundingNoGroundingFactIsProjected) {
    const zetta::agent::EndState facts =
        zetta::agent::response_solution_facts(answer_saying("Here's what I'll build."), "");
    EXPECT_FALSE(facts.matches("numbers_from_context", true));
    EXPECT_FALSE(facts.matches("grounded", true));
}

// A model response carries TOOL CALLS, not a graph — the harness builds the Pipeline by running
// them. So the projection must read the pipeline out of the calls when no graph is present, or a
// perfectly good response grades as having built nothing.
//
// This was not hypothetical: it made a checkpoint that emitted correct tool calls on 100% of action
// cases score 47% on the grader, with `connect` and `serve` reported missing from responses that
// plainly called them.
TEST(ResponseFacts, PipelineFactsComeFromToolCallsWhenThereIsNoGraph) {
    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Action;
    action.message = "I'll connect your source, then serve the results.";
    action.tool_calls.emplace_back("connect", nlohmann::json{{"name", "source"}});
    action.tool_calls.emplace_back("clean", nlohmann::json{{"name", "clean"}});
    action.tool_calls.emplace_back(
        "model", nlohmann::json{{"name", "model"}, {"model_kind", "gbdt"}});
    action.tool_calls.emplace_back(
        "serve", nlohmann::json{{"name", "solution"}, {"leakage_free", true}});
    ASSERT_TRUE(action.execution_graph.nodes().empty()) << "the model emits no graph";

    const zetta::agent::EndState facts = zetta::agent::response_solution_facts(action, "");
    EXPECT_TRUE(facts.matches("connect", true));
    EXPECT_TRUE(facts.matches("clean", true));
    EXPECT_TRUE(facts.matches("model", true));
    EXPECT_TRUE(facts.matches("serve", true));
    // Node attributes are configuration and must survive too — a grader requiring leakage_free
    // is asking about the step's config, not its presence.
    EXPECT_TRUE(facts.matches("leakage_free", true));
    EXPECT_TRUE(facts.matches("model_kind", std::string("gbdt")));
}

// An explicit graph still wins, so nothing about reference cases changes.
TEST(ResponseFacts, AnExplicitGraphIsStillTheSourceOfTruth) {
    zetta::agent::EndState reference;
    reference.set("connect", true);
    reference.set("index", true);
    reference.set("serve", true);
    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Action;
    action.message = "Building it.";
    action.execution_graph = zetta::agent::build_solution_graph(reference);

    const zetta::agent::EndState facts = zetta::agent::response_solution_facts(action, "");
    EXPECT_TRUE(facts.matches("index", true));
}

}  // namespace
