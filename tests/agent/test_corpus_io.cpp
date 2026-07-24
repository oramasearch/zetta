#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"
#include "zetta/agent/curriculum.hpp"

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

}  // namespace
