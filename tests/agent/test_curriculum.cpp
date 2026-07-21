#include <gtest/gtest.h>

#include <cstddef>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/curriculum.hpp"
#include "zetta/agent/grader.hpp"

// Sample #1 of the base-agent training corpus: Charles's churn game (Motive UC-1),
// the improvement over his LLM-pattern-matching n8n. Following blender-rl's method,
// the task must ship a reference solution that PASSES and negative controls that FAIL
// — discrimination is the product, not the pass (see the rlvr-training-data skill).
namespace {

using zetta::agent::admit_corpus;
using zetta::agent::AdmissionCase;
using zetta::agent::AdmissionResult;
using zetta::agent::churn_solution_case;
using zetta::agent::EndStateGrader;
using zetta::agent::Grade;

TEST(Curriculum, ChurnCaseIsAdmittedByTheGate) {
    const AdmissionResult result = admit_corpus({churn_solution_case()});
    EXPECT_TRUE(result.admitted)
        << (result.failures.empty() ? "" : result.failures.front().detail);
}

TEST(Curriculum, ChurnReferencePassesItsGrader) {
    const AdmissionCase churn = churn_solution_case();
    const Grade grade = churn.task.grader().grade(churn.reference);
    EXPECT_TRUE(grade.passed) << "the reference should satisfy every required fact";
    EXPECT_DOUBLE_EQ(grade.score, 1.0);
}

TEST(Curriculum, ChurnNegativeControlsEachFail) {
    const AdmissionCase churn = churn_solution_case();
    ASSERT_FALSE(churn.negative_controls.empty()) << "discrimination needs negatives";
    const EndStateGrader grader = churn.task.grader();
    for (std::size_t i = 0; i < churn.negative_controls.size(); ++i) {
        EXPECT_FALSE(grader.grade(churn.negative_controls[i]).passed)
            << "negative control " << i << " should fail the churn grader";
    }
}

}  // namespace
