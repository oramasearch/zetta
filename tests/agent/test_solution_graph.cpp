// The generic facts ↔ pipeline bridge (#873): any solution facts build a real gradeable
// pipeline whose extracted facts satisfy the same grader — the bridge that lets the whole
// corpus (not just churn/search) carry a reference action the gate scores.
#include <gtest/gtest.h>

#include "zetta/agent/curriculum.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/agent/grader.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/ir/step_kind.hpp"

namespace {

using zetta::agent::build_solution_graph;
using zetta::agent::EndState;
using zetta::agent::EndStateGrader;
using zetta::agent::graph_solution_facts;

// A graph built from a use case's reference facts passes that use case's grader — for BOTH
// churn and search, with their different shapes (churn has no index; search has no model).
TEST(SolutionGraph, ChurnReferenceFactsRoundTripThroughAGraph) {
    const EndState reference = zetta::agent::churn_solution_case().reference;
    const EndStateGrader grader = zetta::agent::churn_solution_case().task.grader();
    EXPECT_TRUE(grader.grade(graph_solution_facts(build_solution_graph(reference))).passed);
}

TEST(SolutionGraph, SearchReferenceFactsRoundTripThroughAGraph) {
    const EndState reference = zetta::agent::search_solution_case().reference;
    const EndStateGrader grader = zetta::agent::search_solution_case().task.grader();
    EXPECT_TRUE(grader.grade(graph_solution_facts(build_solution_graph(reference))).passed);
}

// The point: it's GENERIC. An arbitrary fact set (the kind an LLM-generated corpus task carries)
// builds a graph whose extracted facts satisfy those same facts — string, bool, and a step fact.
TEST(SolutionGraph, ArbitraryFactsRoundTrip) {
    EndState required;
    required.set("index", true);                    // a step-presence fact
    required.set("model_kind", "gbdt");             // a string attribute
    required.set("temporal_validation", true);      // an LLM-vocabulary bool
    required.set("serve_ranked_list", true);
    required.set("act", false);                     // a FALSE step fact — must stay false
    const EndStateGrader grader{required};
    const EndState recovered = graph_solution_facts(build_solution_graph(required));
    EXPECT_TRUE(grader.grade(recovered).passed) << "every required fact must be satisfied";
    // The true step fact became a real step node; the false one did not.
    EXPECT_FALSE(build_solution_graph(required).nodes_of_kind(zetta::ir::StepKind::Index).empty());
    EXPECT_TRUE(build_solution_graph(required).nodes_of_kind(zetta::ir::StepKind::Act).empty());
}

}  // namespace
