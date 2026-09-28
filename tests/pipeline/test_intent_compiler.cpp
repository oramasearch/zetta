#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/intent_compiler.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"
#include "zetta/pipeline/project_input.hpp"

using zetta::pipeline::compile_intent;
using zetta::pipeline::CompiledIntent;
using zetta::pipeline::IntentShape;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// True when the graph has at least one node of every pipeline stage — i.e. it is
// a complete connect→clean→model→index→serve→act skeleton.
bool covers_all_stages(const ExecutionGraph& graph) {
    for (const StepKind kind : zetta::ir::all_step_kinds()) {
        if (graph.nodes_of_kind(kind).empty()) return false;
    }
    return true;
}

}  // namespace

// A churn-flavored intent compiles to the churn skeleton: a complete six-stage
// pipeline (the acceptance — a well-formed ExecutionGraph from an intent).
TEST(IntentCompilerTest, ChurnIntentProducesFullChurnSkeleton) {
    const auto compiled = compile_intent(
        "Score every active account for churn risk and tell me why.");
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->shape, IntentShape::Churn);
    EXPECT_EQ(compiled->methodology, "M-1");

    const ExecutionGraph& graph = compiled->graph;
    EXPECT_FALSE(graph.empty());
    EXPECT_TRUE(covers_all_stages(graph));  // connect…act, index included
    // Two connect sources feed the feature build; the pipeline ends in an action.
    EXPECT_EQ(graph.nodes_of_kind(StepKind::Connect).size(), 2u);
    EXPECT_EQ(graph.nodes_of_kind(StepKind::Act).size(), 1u);
    EXPECT_TRUE(graph.has_node("train_churn_model"));
    EXPECT_EQ(graph.find_node("train_churn_model")->attributes.at("model_kind"),
              "gbdt");
}

// Various phrasings of the churn shape are all recognized (the heuristic is the
// transparent, signal-word mapping — not language understanding).
TEST(IntentCompilerTest, RecognizesChurnSynonyms) {
    for (const char* intent : {"reduce customer attrition",
                               "predict subscriber cancellation",
                               "improve retention of paying users",
                               "who is about to lapse on renewal?"}) {
        const auto compiled = compile_intent(intent);
        ASSERT_TRUE(compiled) << intent;
        EXPECT_EQ(compiled->shape, IntentShape::Churn) << intent;
    }
}

TEST(IntentCompilerTest, ExplicitUnavailableMethodologyIsRefusedWithItsIssue) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-3";
    std::string error;
    EXPECT_FALSE(compile_intent("Group customers", options, &error));
    EXPECT_NE(error.find("Segmentation / clustering (M-3)"), std::string::npos);
    EXPECT_NE(error.find("1042"), std::string::npos);
}

TEST(IntentCompilerTest, PrimarySolutionBuildsItsMethodologyShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.solution = "S-1";
    const auto compiled = compile_intent("Build a retention plan", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-1");
    EXPECT_TRUE(compiled->graph.has_node("train_churn_model"));
}

TEST(IntentCompilerTest, CausalMethodologyAndSolutionBuildUpliftShape) {
    for (const bool by_solution : {false, true}) {
        zetta::pipeline::IntentCompileOptions options;
        if (by_solution) {
            options.solution = "S-11";
        } else {
            options.methodology = "M-6";
        }
        options.hints = {{"source", "experiments"},
                         {"table", "population"},
                         {"treatment", "offer"},
                         {"outcome", "converted"}};
        const auto compiled = compile_intent("Decide who to treat", options);
        ASSERT_TRUE(compiled);
        EXPECT_EQ(compiled->methodology, "M-6");
        const auto models =
            compiled->graph.nodes_of_kind(zetta::ir::StepKind::Model);
        ASSERT_EQ(models.size(), 1u);
        EXPECT_EQ(models.front()->attributes.at("model_kind"), "uplift");
        EXPECT_TRUE(zetta::pipeline::default_gate().run(compiled->graph).passed());
    }
}

TEST(IntentCompilerTest, SegmentationKeywordIsRefusedWithItsIssue) {
    std::string error;
    EXPECT_FALSE(compile_intent("Cluster customers into personas", &error));
    EXPECT_NE(error.find("Segmentation / clustering (M-3)"), std::string::npos);
    EXPECT_NE(error.find("1042"), std::string::npos);
}

TEST(IntentCompilerTest, UnrecognizedNonPredictiveIntentListsWhatRuns) {
    std::string error;
    EXPECT_FALSE(
        compile_intent("Summarize incoming support tickets by topic.", &error));
    EXPECT_NE(error.find("Runnable methodologies"), std::string::npos);
    EXPECT_NE(error.find("Tabular supervised learning (M-1)"),
              std::string::npos);
}

// Empty / whitespace-only intent carries no meaning → no graph (mirrors
// Project::capture_intent), with a reason set.
TEST(IntentCompilerTest, EmptyIntentIsRejected) {
    std::string error;
    EXPECT_FALSE(compile_intent("", &error));
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(compile_intent("   \t\n  "));
}

// The produced skeleton is well-formed: it round-trips through the IR's JSON
// trust boundary (ir::to_json → ir::from_json) unchanged. This is what "editable,
// not a black box" rests on — it is the same record the workspace stores + edits.
TEST(IntentCompilerTest, SkeletonRoundTripsThroughJsonBoundary) {
    const auto compiled = compile_intent("forecast churn for our accounts");
    ASSERT_TRUE(compiled);

    const nlohmann::json document = zetta::ir::to_json(compiled->graph);
    std::string error;
    const std::optional<ExecutionGraph> reparsed =
        zetta::ir::from_json(document, &error);
    ASSERT_TRUE(reparsed) << error;
    EXPECT_EQ(zetta::ir::to_json(*reparsed), document);  // unchanged
}

// A freshly compiled skeleton passes the baseline correctness gate (its checks
// are opt-in annotations the skeleton doesn't carry), so it is deployable after
// review without further wiring.
TEST(IntentCompilerTest, SkeletonPassesDefaultGate) {
    const auto compiled = compile_intent("retention scoring for subscribers");
    ASSERT_TRUE(compiled);
    EXPECT_TRUE(zetta::pipeline::default_gate().run(compiled->graph).passed());
}

// Compilation is deterministic: the same intent yields an identical graph.
TEST(IntentCompilerTest, IsDeterministic) {
    const auto first = compile_intent("churn prediction");
    const auto second = compile_intent("churn prediction");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_EQ(zetta::ir::to_json(first->graph),
              zetta::ir::to_json(second->graph));
}

// The Project bridge: a captured intent input compiles to a stored, named,
// editable ProjectGraph (the stored-intent → workspace path the issue asks for).
TEST(IntentCompilerTest, ProjectCompilesStoredIntentToGraph) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto project = node.create_project("churn");
    ASSERT_TRUE(project);
    auto intent = project->capture_intent(
        "Flag accounts likely to churn and notify our retention team.");
    ASSERT_TRUE(intent);

    auto graph = project->compile_intent_to_graph(intent->id());
    ASSERT_TRUE(graph);
    EXPECT_EQ(graph->name(), "churn skeleton");  // shape-derived default name
    EXPECT_TRUE(covers_all_stages(graph->graph()));

    // It is a real stored graph: findable by name and listed by the project, so
    // the Graph Workspace renders + edits it like any other.
    EXPECT_TRUE(project->find_graph("churn skeleton"));
    EXPECT_EQ(project->graph_count(), 1u);

    // The intent input is left intact — it is an immutable provenance record.
    EXPECT_TRUE(project->find_input(intent->id()));
}

// The bridge rejects bad inputs cleanly: an unknown id, and a custom name that
// collides with an existing graph.
TEST(IntentCompilerTest, ProjectBridgeRejectsBadInputs) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    EXPECT_FALSE(project->compile_intent_to_graph("no-such-input"));

    auto intent = project->capture_intent("churn risk scoring");
    ASSERT_TRUE(intent);
    ASSERT_TRUE(project->compile_intent_to_graph(intent->id(), "my graph"));
    // Same name again → create_graph collision → empty handle.
    EXPECT_FALSE(project->compile_intent_to_graph(intent->id(), "my graph"));
}
