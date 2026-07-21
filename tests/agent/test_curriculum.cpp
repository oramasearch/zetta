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

// Nibble 2: the reference/negatives are the facts of an ACTUALLY BUILT execution graph
// (Connect->Clean->Model->Serve->Act), not hand-written strings.
TEST(Curriculum, ChurnReferenceGraphFactsPassGrader) {
    const EndStateGrader grader = churn_solution_case().task.grader();
    const auto graph =
        zetta::agent::build_churn_graph("gbdt", "shap", true, true, true);
    EXPECT_TRUE(grader.grade(zetta::agent::churn_graph_facts(graph)).passed)
        << "a correctly built churn graph should pass the grader";
}

TEST(Curriculum, ChurnDegenerateGraphsFailGrader) {
    const EndStateGrader grader = churn_solution_case().task.grader();
    using zetta::agent::build_churn_graph;
    using zetta::agent::churn_graph_facts;
    // LLM-only (his current approach), leaky features, and no calibration loop.
    EXPECT_FALSE(
        grader.grade(churn_graph_facts(build_churn_graph("llm", "none", true, true, true)))
            .passed);
    EXPECT_FALSE(
        grader.grade(churn_graph_facts(build_churn_graph("gbdt", "shap", false, true, true)))
            .passed);
    EXPECT_FALSE(
        grader.grade(churn_graph_facts(build_churn_graph("gbdt", "shap", true, true, false)))
            .passed);
}

// Nibble 3: the reference solution as the AGENT'S OWN ACTIONS — a trajectory of tool
// calls that, applied in a churn sandbox, builds a graph whose facts pass the grader.
TEST(Curriculum, ChurnReferenceTrajectoryBuildsAPassingGraph) {
    zetta::agent::Sandbox sandbox = zetta::agent::churn_sandbox();
    for (const zetta::agent::ToolCall& call : zetta::agent::churn_reference_trajectory()) {
        ASSERT_TRUE(sandbox.apply(call).ok()) << "tool '" << call.tool << "' should apply";
    }
    const EndStateGrader grader = churn_solution_case().task.grader();
    EXPECT_TRUE(grader.grade(sandbox.state()).passed)
        << "the reference trajectory should build a graph that passes the grader";
}

TEST(Curriculum, ChurnDegenerateTrajectoryFails) {
    zetta::agent::Sandbox sandbox = zetta::agent::churn_sandbox();
    // Charles's current approach: same steps, but the model tool is an LLM, not a gbdt.
    std::vector<zetta::agent::ToolCall> trajectory = zetta::agent::churn_reference_trajectory();
    for (zetta::agent::ToolCall& call : trajectory) {
        if (call.tool == "model") call.arguments["kind"] = "llm";
    }
    for (const zetta::agent::ToolCall& call : trajectory) sandbox.apply(call);
    const EndStateGrader grader = churn_solution_case().task.grader();
    EXPECT_FALSE(grader.grade(sandbox.state()).passed);
}

}  // namespace
