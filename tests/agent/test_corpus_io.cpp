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
using zetta::agent::dump_corpus_jsonl;
using zetta::agent::load_corpus_jsonl;
using zetta::agent::to_json;

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

}  // namespace
