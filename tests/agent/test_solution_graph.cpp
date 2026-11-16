// SPDX-License-Identifier: MIT

// The generic facts ↔ pipeline bridge (#873): any solution facts build a real gradeable
// pipeline whose extracted facts satisfy the same grader — the bridge that lets the whole
// corpus (not just churn/search) carry a reference action the gate scores.
#include <gtest/gtest.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

#include "zetta/agent/admission_gate.hpp"
#include "zetta/agent/corpus_io.hpp"
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
// Two methodologies must produce structurally DIFFERENT graphs. Until `model_kind` rode the
// model step, they did not: it was swept into an attribute bag on a generic serve node, so a
// forecasting pipeline and a churn pipeline were connect → clean → model → index → serve either
// way, differing only by a string nothing read.
//
// That is why the corpus could not teach one methodology from another — the distinction it was
// training had no representation in the IR, so the strongest negative control we have (the
// NEIGHBOURING solution's pipeline on this ask) had nothing structural to discriminate.
TEST(SolutionGraph, TwoMethodologiesProduceStructurallyDifferentGraphs) {
    const auto graph_for = [](const std::string& model_kind) {
        zetta::agent::EndState facts;
        facts.set("connect", true);
        facts.set("clean", true);
        facts.set("model", true);
        facts.set("serve", true);
        facts.set("model_kind", model_kind);
        return zetta::agent::build_solution_graph(facts);
    };

    const zetta::ir::ExecutionGraph churn = graph_for("gbdt");
    const zetta::ir::ExecutionGraph forecast = graph_for("forecast");

    const auto model_node = [](const zetta::ir::ExecutionGraph& graph) {
        for (const auto& node : graph.nodes()) {
            if (node->kind == zetta::ir::StepKind::Model) return node;
        }
        return std::shared_ptr<zetta::ir::Node>{};
    };

    const auto churn_model = model_node(churn);
    const auto forecast_model = model_node(forecast);
    ASSERT_TRUE(churn_model) << "a graph with model:true must carry a model step";
    ASSERT_TRUE(forecast_model);

    EXPECT_EQ(churn_model->attributes.value("model_kind", ""), "gbdt");
    EXPECT_EQ(forecast_model->attributes.value("model_kind", ""), "forecast")
        << "the methodology must ride the step that implements it";
    EXPECT_NE(churn_model->attributes, forecast_model->attributes)
        << "two methodologies whose model steps are identical cannot be told apart by any "
           "check, any gate, or any negative control";
}

// And it must not ALSO sit in the serve bag — two sources of truth for the methodology are two
// things free to disagree.
TEST(SolutionGraph, TheMethodologyIsNotDuplicatedOntoTheServeNode) {
    zetta::agent::EndState facts;
    facts.set("connect", true);
    facts.set("model", true);
    facts.set("serve", true);
    facts.set("model_kind", std::string("forecast"));
    facts.set("calibrated", false);  // a non-step fact SHOULD still reach the bag

    const zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(facts);
    for (const auto& node : graph.nodes()) {
        if (node->name == "solution") {
            EXPECT_FALSE(node->attributes.contains("model_kind"))
                << "model_kind rides the model step; duplicating it here lets the two drift";
            EXPECT_TRUE(node->attributes.contains("calibrated"))
                << "ordinary attribute facts must still reach the solution node";
        }
    }
}

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

// reference_action_from_facts composes the action (message + tool_calls + graph); its graph
// passes the grader, and there's one tool call per pipeline step.
TEST(SolutionGraph, ReferenceActionFromFactsIsGradeable) {
    const zetta::agent::AdmissionCase churn = zetta::agent::churn_solution_case();
    const zetta::agent::StudioAction action =
        zetta::agent::reference_action_from_facts(churn.reference);
    EXPECT_FALSE(action.message.empty());
    EXPECT_EQ(action.tool_calls.size(), action.execution_graph.node_count());
    EXPECT_TRUE(
        churn.task.grader().grade(graph_solution_facts(action.execution_graph)).passed);
}

TEST(SolutionGraph, AttachSetsAGradeableActionOnTheCase) {
    zetta::agent::AdmissionCase search = zetta::agent::search_solution_case();
    zetta::agent::attach_reference_action(search);
    ASSERT_TRUE(search.reference_action.has_value());
    EXPECT_TRUE(search.task.grader()
                    .grade(graph_solution_facts(search.reference_action->execution_graph))
                    .passed);
}

// The SFT training example targets the ACTION, not the facts (#873): the prompt is the Studio
// system prompt + the ask; the target parses back to a StudioAction whose pipeline passes the
// grader. This is what training now learns to emit.
TEST(SolutionGraph, RenderActionExampleTargetsTheAction) {
    zetta::agent::AdmissionCase churn = zetta::agent::churn_solution_case();
    zetta::agent::attach_reference_action(churn);
    const auto [prompt, target] = zetta::agent::render_action_example(churn);

    // Prompt: the Studio identity + the task's ask.
    EXPECT_NE(prompt.find("Orama Studio"), std::string::npos);
    EXPECT_NE(prompt.find("churn"), std::string::npos);

    // Target: the DECISION — mode, message, tool calls — with no execution_graph. Node ids are
    // random UUIDs, so asking a model to write them teaches it only what a UUID looks like.
    const auto action = zetta::agent::parse_studio_action(target);
    ASSERT_TRUE(action);
    EXPECT_FALSE(action->tool_calls.empty()) << "the target must carry the steps to call";
    EXPECT_TRUE(action->execution_graph.nodes().empty())
        << "the target must NOT contain a graph; the harness builds it";
    EXPECT_EQ(target.find("execution_graph"), std::string::npos);

    // And executing those calls the way the harness does must produce a pipeline that passes the
    // task's grader — the whole loop, not just the model's half of it.
    const zetta::ir::ExecutionGraph built =
        zetta::agent::build_pipeline_from_tool_calls(action->tool_calls);
    EXPECT_TRUE(churn.task.grader().grade(graph_solution_facts(built)).passed);
}

// Acceptance for the reshape (guarded on ZETTA_CORPUS): EVERY task in the real corpus gets a
// reference action whose pipeline passes that task's own grader — the whole corpus becomes
// actions, discrimination intact. Skipped in CI (no corpus file); run with
// ZETTA_CORPUS=data/base-agent-training-set.jsonl.
TEST(SolutionGraph, EveryCorpusTaskBecomesAGradeableAction) {
    const char* path = std::getenv("ZETTA_CORPUS");
    if (path == nullptr) GTEST_SKIP() << "set ZETTA_CORPUS to a corpus JSONL to run";
    std::ifstream file(path);
    ASSERT_TRUE(file.good()) << "cannot open " << path;
    std::stringstream buffer;
    buffer << file.rdbuf();
    std::string error;
    auto corpus = zetta::agent::load_corpus_jsonl(buffer.str(), &error);
    ASSERT_TRUE(corpus.has_value()) << error;

    int passed = 0;
    for (zetta::agent::AdmissionCase& admission_case : *corpus) {
        zetta::agent::attach_reference_action(admission_case);
        ASSERT_TRUE(admission_case.reference_action.has_value());
        const bool ok = admission_case.task.grader()
                            .grade(graph_solution_facts(
                                admission_case.reference_action->execution_graph))
                            .passed;
        EXPECT_TRUE(ok) << "task '" << admission_case.task.id()
                        << "' action pipeline must pass its grader";
        if (ok) ++passed;
    }
    std::cerr << "[reshape] " << passed << "/" << corpus->size()
              << " corpus tasks became gradeable actions\n";
    EXPECT_EQ(passed, static_cast<int>(corpus->size()));
}

// The two encodings of a pipeline — the graph, and the tool calls that build it — must be
// inverses. A corpus that writes them independently lets them disagree, and the v1 corpus did:
// 91% of its action cases carried a real graph with an empty `tool_calls`, teaching the agent
// that an action calls nothing. This pins the round trip so that cannot come back.
TEST(SolutionGraph, ToolCallsAndPipelineAreInverses) {
    zetta::agent::EndState facts;
    facts.set("connect", true);
    facts.set("clean", true);
    facts.set("model", true);
    facts.set("serve", true);
    facts.set("leakage_free", true);
    facts.set("model_kind", std::string("gbdt"));
    const zetta::ir::ExecutionGraph graph = zetta::agent::build_solution_graph(facts);
    ASSERT_GT(graph.node_count(), 1u) << "the fixture needs a real multi-step pipeline";

    const std::vector<zetta::agent::ToolCall> calls = zetta::agent::tool_calls_from_graph(graph);
    ASSERT_EQ(calls.size(), graph.nodes().size()) << "one call per pipeline step";

    const zetta::ir::ExecutionGraph rebuilt = zetta::agent::build_pipeline_from_tool_calls(calls);
    ASSERT_EQ(rebuilt.nodes().size(), graph.nodes().size());
    for (std::size_t index = 0; index < graph.nodes().size(); ++index) {
        EXPECT_EQ(rebuilt.nodes()[index]->kind, graph.nodes()[index]->kind);
        EXPECT_EQ(rebuilt.nodes()[index]->name, graph.nodes()[index]->name);
        EXPECT_EQ(rebuilt.nodes()[index]->attributes, graph.nodes()[index]->attributes);
    }
    // The facts survive the round trip, so a rebuilt pipeline still passes its task's grader.
    const zetta::agent::EndState recovered = zetta::agent::graph_solution_facts(rebuilt);
    EXPECT_TRUE(recovered.matches("leakage_free", true));
    EXPECT_TRUE(recovered.matches("model_kind", std::string("gbdt")));
}

}  // namespace

// B1c (#983) — response-grading facts must not reach the graph — is DEFERRED, not forgotten.
//
// `build_solution_graph` sweeps every unrecognised fact into an attribute bag on the serve
// node, so `grounded`, `numbers_from_context` and `built_nothing` — judgements about the TURN —
// ride on the artifact that deploys. That is a category error, and the same silent absorption
// is how `model_kind` looked wired for as long as it did.
//
// The straightforward fix (deny those keys at sweep time) breaks the round-trip contract this
// file owes the corpus: `graph_solution_facts(build_solution_graph(required))` must recover
// the facts a reference requires, and action cases (bare/affirm, follow/build, follow/modify)
// fail their own graders when keys are dropped. Fixing it properly means separating "facts the
// grader projects from a response" from "facts a graph must round-trip" at the type level
// rather than by key name — a change to EndState's contract, not a filter.
//
// Tracked on https://github.com/oramasearch/zetta/issues/984. B1a (model_kind rides the model
// step) and B1b (the correctness gate refuses an unimplemented methodology) are landed and
// carry the load; this is hygiene on top of them.
