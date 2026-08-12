#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/agent/curriculum.hpp"
#include "zetta/dataset/base_agent_generator.hpp"

// Nibble 4 of #866: the training corpus's on-disk JSONL format — what the frontier-model
// generators emit and the native loader consumes before `admit_corpus` gates it. The
// churn seed round-trips exactly, a JSONL corpus loads + admits, and a malformed line is
// rejected at the boundary (non-throwing, line-qualified reason).
namespace {

using zetta::agent::admission_case_from_json;
using zetta::agent::AdmissionCase;
using zetta::agent::admit_corpus;
using zetta::agent::churn_solution_case;
using zetta::agent::corpus_task_from_json;
using zetta::agent::CorpusTask;
using zetta::agent::dump_corpus_jsonl;
using zetta::agent::held_out_cases;
using zetta::agent::load_corpus_jsonl;
using zetta::agent::to_json;
using zetta::agent::training_cases;

TEST(CorpusIo, ChurnCaseRoundTripsThroughJson) {
    const AdmissionCase original = churn_solution_case();
    std::string error;
    const auto restored = admission_case_from_json(to_json(original), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    EXPECT_EQ(restored->task.required(), original.task.required());
    EXPECT_EQ(restored->reference, original.reference);
    ASSERT_EQ(restored->negative_controls.size(), original.negative_controls.size());
    EXPECT_TRUE(admit_corpus({*restored}).admitted)
        << "the round-tripped churn case must still be admitted with discrimination";
}

TEST(CorpusIo, JsonlCorpusRoundTripsAndAdmits) {
    const std::string jsonl = dump_corpus_jsonl({churn_solution_case()});
    std::string error;
    const auto corpus = load_corpus_jsonl(jsonl, &error);
    ASSERT_TRUE(corpus.has_value()) << error;
    ASSERT_EQ(corpus->size(), 1U);
    EXPECT_TRUE(admit_corpus(*corpus).admitted);
}

// The corpus carries the reference solution in the agent's ACTION shape when present (#873):
// {message, tool_calls, execution_graph} round-trips through the JSONL alongside the facts.
TEST(CorpusIo, ReferenceActionRoundTrips) {
    AdmissionCase with_action = churn_solution_case();
    with_action.reference_action = zetta::agent::churn_reference_action();

    std::string error;
    const auto restored = admission_case_from_json(to_json(with_action), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    ASSERT_TRUE(restored->reference_action.has_value()) << "the action must survive the trip";
    EXPECT_FALSE(restored->reference_action->message.empty());
    EXPECT_EQ(restored->reference_action->tool_calls.size(),
              with_action.reference_action->tool_calls.size());
    EXPECT_EQ(restored->reference_action->execution_graph.node_count(),
              with_action.reference_action->execution_graph.node_count());

    // Backward-compatible: a case with no action still parses, with an empty reference_action.
    const auto no_action = admission_case_from_json(to_json(churn_solution_case()), &error);
    ASSERT_TRUE(no_action.has_value()) << error;
    EXPECT_FALSE(no_action->reference_action.has_value());
}

TEST(CorpusIo, BlankLinesAreSkipped) {
    std::string error;
    const std::string jsonl = "\n" + dump_corpus_jsonl({churn_solution_case()}) + "  \n";
    const auto corpus = load_corpus_jsonl(jsonl, &error);
    ASSERT_TRUE(corpus.has_value()) << error;
    EXPECT_EQ(corpus->size(), 1U);
}

TEST(CorpusIo, MalformedLineIsRejectedWithReason) {
    std::string error;
    const auto corpus = load_corpus_jsonl("{not valid json\n", &error);
    EXPECT_FALSE(corpus.has_value());
    EXPECT_NE(error.find("line 1"), std::string::npos) << error;
}

TEST(CorpusIo, TaskWithoutCriterionIsRejected) {
    std::string error;
    // A task with no `required` facts is untrainable — the loader must reject it.
    const auto corpus = load_corpus_jsonl(
        R"({"task":{"id":"x","description":"y"},"reference":{}})"
        "\n",
        &error);
    EXPECT_FALSE(corpus.has_value());
    EXPECT_FALSE(error.empty());
}

// Nibble 5: difficulty is a data field, and a held-out slice measures generalization.
TEST(CorpusIo, CorpusTaskRoundTripsWithDifficulty) {
    const CorpusTask task{churn_solution_case(), /*tier=*/1, /*difficulty=*/0.4,
                          /*held_out=*/false, {"churn", "tabular"}};
    std::string error;
    const auto restored = corpus_task_from_json(to_json(task), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    EXPECT_EQ(restored->tier, 1);
    EXPECT_DOUBLE_EQ(restored->difficulty, 0.4);
    EXPECT_FALSE(restored->held_out);
    EXPECT_EQ(restored->tags, (std::vector<std::string>{"churn", "tabular"}));
    EXPECT_TRUE(admit_corpus({restored->admission}).admitted);
}

TEST(CorpusIo, TrainingAndHeldOutSplit) {
    const std::vector<CorpusTask> corpus = {
        {churn_solution_case(), 1, 0.4, /*held_out=*/false, {}},
        {churn_solution_case(), 3, 0.9, /*held_out=*/true, {}},
    };
    EXPECT_EQ(training_cases(corpus).size(), 1U);
    EXPECT_EQ(held_out_cases(corpus).size(), 1U);
}

// History must survive the round trip. Dropping `grounding` on the trip once trained "answer from
// these facts" with no facts; a dropped history is the same failure — a follow-up that trains as a
// standalone question, silently, looking exactly like a well-formed case.
TEST(CorpusIo, HistorySurvivesTheRoundTrip) {
    zetta::agent::AdmissionCase with_history =
        zetta::agent::churn_solution_case();
    with_history.history = {{"What can I do here?", "I can build churn or lead scoring."}};

    std::string error;
    const auto restored = zetta::agent::load_corpus_jsonl(
        zetta::agent::dump_corpus_jsonl({with_history}), &error);
    ASSERT_TRUE(restored.has_value()) << error;
    ASSERT_EQ(restored->size(), 1U);
    ASSERT_EQ((*restored)[0].history.size(), 1U);
    EXPECT_EQ((*restored)[0].history[0].ask, "What can I do here?");
    EXPECT_EQ((*restored)[0].history[0].reply, "I can build churn or lead scoring.");
}

}  // namespace

// C1 (#983) — the round-trip that has to hold before an authored corpus slice can be DATA rather
// than C++. Today a slice means editing `interaction_families.cpp`, compiling, and reviewing a
// large diff; a loader (C2) makes it an artifact that can be published as the model's provenance.
// None of that is safe unless every field survives the trip.
//
// The fields most likely to be dropped are the newest ones, and they are exactly the ones the
// bare-turn work depends on: `history`, the tool OUTCOMES inside it (so "why did that fail?" is
// answerable from the call that ran rather than from prose), and `reference_action`.
TEST(CorpusIo, AMultiTurnCaseWithToolOutcomesSurvivesTheRoundTrip) {
    zetta::agent::EndState required;
    required.set("kind", std::string("answer"));
    required.set("grounded", true);
    required.set("built_nothing", true);

    zetta::agent::StudioAction action;
    action.kind = zetta::agent::InteractionKind::Answer;
    action.message = "The connect step didn't finish - the credential was rejected.";

    zetta::agent::AdmissionCase original{
        zetta::agent::Task{"follow/failure/0", "why did that fail?", zetta::agent::EndState{},
                           {}, required},
        required,
        {zetta::agent::EndState{}},
        action,
        "Project: Churn\nLast run: failed\n",
        {{"Run it.", "Started the run.", {{"run", "failed", "credential rejected"}}}}};

    std::string error;
    const auto decoded = zetta::agent::admission_case_from_json(
        zetta::agent::to_json(original), &error);
    ASSERT_TRUE(decoded.has_value()) << error;

    EXPECT_EQ(decoded->task.description(), "why did that fail?");
    EXPECT_EQ(decoded->grounding, original.grounding);

    ASSERT_EQ(decoded->history.size(), 1u) << "history is what makes a follow-up a follow-up";
    EXPECT_EQ(decoded->history[0].ask, "Run it.");
    ASSERT_EQ(decoded->history[0].tools.size(), 1u)
        << "the tool OUTCOME must survive - 'why did that fail?' is answerable from the call "
           "that ran, not from the prose of the previous reply";
    EXPECT_EQ(decoded->history[0].tools[0].tool, "run");
    EXPECT_EQ(decoded->history[0].tools[0].status, "failed");
    EXPECT_EQ(decoded->history[0].tools[0].detail, "credential rejected");

    ASSERT_TRUE(decoded->reference_action.has_value())
        << "the action is the SFT training target; losing it silently trains on nothing";
    EXPECT_EQ(decoded->reference_action->message, action.message);
    EXPECT_EQ(decoded->negative_controls.size(), original.negative_controls.size())
        << "discrimination is the product; a dropped control is a case that measures nothing";
}

// A whole corpus, not one case — the shape a loader will actually consume.
TEST(CorpusIo, AMultiTurnCorpusSurvivesJsonlAndStillAdmits) {
    const zetta::dataset::BaseAgentDatasetGenerator generator;
    zetta::dataset::DatasetGenSpec spec;
    const zetta::dataset::GeneratedDataset dataset = generator.generate(spec);
    ASSERT_FALSE(dataset.cases.empty());

    std::size_t with_history = 0;
    for (const auto& entry : dataset.cases) {
        if (!entry.history.empty()) ++with_history;
    }
    ASSERT_GT(with_history, 0u) << "the corpus must contain multi-turn cases to test this at all";

    std::string error;
    const auto reloaded =
        zetta::agent::load_corpus_jsonl(zetta::agent::dump_corpus_jsonl(dataset.cases), &error);
    ASSERT_TRUE(reloaded.has_value()) << error;
    ASSERT_EQ(reloaded->size(), dataset.cases.size());

    std::size_t reloaded_with_history = 0;
    for (const auto& entry : *reloaded) {
        if (!entry.history.empty()) ++reloaded_with_history;
    }
    EXPECT_EQ(reloaded_with_history, with_history)
        << "a corpus that loses its history on reload trains single-turn no matter what was "
           "authored";

    EXPECT_TRUE(zetta::agent::admit_corpus(*reloaded).admitted)
        << "a corpus that no longer admits after a round trip cannot be published as the "
           "model's provenance";
}
