// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/curriculum.hpp"
#include "zetta/agent/grader.hpp"

// Sample #1 of the base-agent training corpus: the churn game (UC-1), the
// improvement over an LLM-pattern-matching n8n workflow. Following blender-rl's method,
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

// The AGENT'S ACTIONS build the graph (#859 substrate): applying the reference tool-call
// trajectory to the graph-building sandbox yields a real ir::ExecutionGraph whose facts pass
// the churn grader — the "run" target is a genuine execution graph, constructed by tool calls
// the agent emits, not a hand-written blob. (Wiring the LIVE agent to emit this trajectory
// instead of the solution facts is the RLVR step; running the graph is the pipeline.)
TEST(Curriculum, AgentTrajectoryBuildsAGradedChurnGraph) {
    zetta::agent::Sandbox sandbox = zetta::agent::churn_sandbox();
    for (const zetta::agent::ToolCall& call : zetta::agent::churn_reference_trajectory()) {
        const zetta::agent::ApplyOutcome outcome = sandbox.apply(call);
        EXPECT_TRUE(outcome.ok()) << call.tool << ": " << outcome.reason;
    }
    const EndStateGrader grader = churn_solution_case().task.grader();
    const zetta::agent::Grade grade = grader.grade(sandbox.state());
    EXPECT_TRUE(grade.passed)
        << "the agent's tool-call trajectory must build a graph that passes the churn grader "
           "(score "
        << grade.score << ")";
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
    // The LLM-only approach: same steps, but the model tool is an LLM, not a gbdt.
    std::vector<zetta::agent::ToolCall> trajectory = zetta::agent::churn_reference_trajectory();
    for (zetta::agent::ToolCall& call : trajectory) {
        if (call.tool == "model") call.arguments["kind"] = "llm";
    }
    for (const zetta::agent::ToolCall& call : trajectory) sandbox.apply(call);
    const EndStateGrader grader = churn_solution_case().task.grader();
    EXPECT_FALSE(grader.grade(sandbox.state()).passed);
}

// The churn reference in the agent's ACTION shape (#873): {message, tool_calls,
// execution_graph}. The RLVR reward is the action's *graph* run through the churn gate — the
// reference passes, a wrong pipeline fails (blender-rl's admission-gate discrimination, now in
// the action shape). This is the training target for churn once the corpus emits actions.
TEST(Curriculum, ChurnReferenceActionScoresThroughTheGate) {
    const zetta::agent::StudioAction action = zetta::agent::churn_reference_action();
    EXPECT_FALSE(action.message.empty());
    EXPECT_FALSE(action.tool_calls.empty());
    EXPECT_FALSE(action.execution_graph.empty());

    const EndStateGrader grader = churn_solution_case().task.grader();
    const zetta::agent::Grade reward =
        grader.grade(zetta::agent::churn_graph_facts(action.execution_graph));
    EXPECT_TRUE(reward.passed) << "reference action's pipeline must pass (score " << reward.score
                              << ")";

    // Discrimination: swap in a wrong pipeline (LLM, no explanation) — the reward must drop.
    zetta::agent::StudioAction control = action;
    control.execution_graph = zetta::agent::build_churn_graph("llm", "none", true, true, true);
    EXPECT_FALSE(grader.grade(zetta::agent::churn_graph_facts(control.execution_graph)).passed);

    // Round-trips through its wire JSON — the training-target form.
    std::string error;
    const std::optional<zetta::agent::StudioAction> parsed =
        zetta::agent::action_from_json(zetta::agent::to_json(action), &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(parsed->tool_calls.size(), action.tool_calls.size());
    EXPECT_EQ(parsed->execution_graph.node_count(), action.execution_graph.node_count());
}

// Search — the second Component (#873). It has NO model/calibration step (connect → index →
// serve): the search gate scores only search checks. This is "not every pipeline is six stages"
// made concrete, and the blender-rl discrimination on a different pipeline shape.
TEST(Curriculum, SearchGraphFactsDiscriminate) {
    using zetta::agent::build_search_graph;
    using zetta::agent::search_graph_facts;
    const EndStateGrader grader = zetta::agent::search_solution_case().task.grader();
    // Correct full-text + semantic references pass.
    EXPECT_TRUE(grader.grade(search_graph_facts(build_search_graph("bm25f", true, true))).passed);
    EXPECT_TRUE(
        grader.grade(search_graph_facts(build_search_graph("semantic", true, true))).passed);
    // Degenerate pipelines fail: an LLM guess (no index), unranked, not grounded.
    EXPECT_FALSE(grader.grade(search_graph_facts(build_search_graph("llm", true, true))).passed);
    EXPECT_FALSE(grader.grade(search_graph_facts(build_search_graph("bm25f", false, true))).passed);
    EXPECT_FALSE(grader.grade(search_graph_facts(build_search_graph("bm25f", true, false))).passed);
}

TEST(Curriculum, SearchCaseIsAdmittedByTheGate) {
    // The admission gate: reference passes + every negative control fails (discrimination).
    EXPECT_TRUE(zetta::agent::admit_corpus({zetta::agent::search_solution_case()}).admitted);
}

TEST(Curriculum, SearchReferenceActionScoresThroughTheGate) {
    const zetta::agent::StudioAction action = zetta::agent::search_reference_action();
    EXPECT_FALSE(action.message.empty());
    EXPECT_FALSE(action.tool_calls.empty());
    // The search pipeline has no Model step — connect → index → serve.
    EXPECT_TRUE(action.execution_graph.nodes_of_kind(zetta::ir::StepKind::Index).size() == 1);
    EXPECT_TRUE(action.execution_graph.nodes_of_kind(zetta::ir::StepKind::Model).empty());
    const EndStateGrader grader = zetta::agent::search_solution_case().task.grader();
    EXPECT_TRUE(
        grader.grade(zetta::agent::search_graph_facts(action.execution_graph)).passed);
}

}  // namespace
