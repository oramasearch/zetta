#include <gtest/gtest.h>

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/ml/methodology.hpp"
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

TEST(IntentCompilerTest, UnknownModelKindIsRefused) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "unregistered_model_kind";
    std::string error;
    EXPECT_FALSE(compile_intent("Group customers", options, &error));
    EXPECT_NE(error.find("unknown methodology 'unregistered_model_kind'"),
              std::string::npos);
}

// #1238: every compiled pipeline is named for what was asked, and says which
// template its steps started from; none is named for the template.
TEST(IntentCompilerTest, NamesEveryRunnablePipelineFromItsIntent) {
    for (const zetta::ml::Methodology& methodology : zetta::ml::methodologies()) {
        ASSERT_TRUE(methodology.runnable) << methodology.id;
        zetta::pipeline::IntentCompileOptions options;
        options.methodology = methodology.id;
        const auto compiled = compile_intent("Build the selected pipeline", options);
        ASSERT_TRUE(compiled) << methodology.id;
        EXPECT_EQ(compiled->methodology, methodology.id);
        EXPECT_EQ(compiled->name, "Build the selected pipeline");
        EXPECT_EQ(compiled->template_name,
                  methodology.id == "M-1"
                      ? std::string("the M-1 churn template")
                      : "the " + methodology.id + " " + methodology.name + " template");
    }
    zetta::pipeline::IntentCompileOptions solution;
    solution.solution = "S-1";
    EXPECT_EQ(compile_intent("Keep customers", solution)->template_name, "the S-1 template");
}

TEST(IntentCompilerTest, APipelineNameIsTheIntentsFirstWords) {
    using zetta::pipeline::intent_pipeline_name;
    EXPECT_EQ(intent_pipeline_name("predict which customers will cancel next month."),
              "Predict which customers will cancel next month");
    EXPECT_EQ(intent_pipeline_name("Predict which customers will churn next quarter and rank "
                                   "the at-risk accounts"),
              "Predict which customers will churn next quarter")
        << "eight words at most, never ending on a joining word";
    EXPECT_EQ(intent_pipeline_name("  Flag at-risk accounts!!! (churn) "),
              "Flag at-risk accounts churn");
    EXPECT_EQ(intent_pipeline_name("?!"), "");
    const std::string long_words(70, 'x');
    EXPECT_EQ(intent_pipeline_name(long_words).size(), 60u);
    EXPECT_LE(intent_pipeline_name("supercalifragilistic expialidocious antidisestablishment "
                                   "floccinaucinihilipilification words")
                  .size(),
              60u);
    // No words: the project's name (or the methodology's) instead.
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-1";
    options.project = "retention";
    EXPECT_EQ(compile_intent("... ???", options)->name, "retention pipeline");
}

TEST(IntentCompilerTest, PrimarySolutionBuildsItsMethodologyShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.solution = "S-1";
    const auto compiled = compile_intent("Build a retention plan", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-1");
    EXPECT_TRUE(compiled->graph.has_node("train_churn_model"));
}

TEST(IntentCompilerTest, StreamingBuildsItsOwnEventTimeShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-21";
    options.hints = {{"source", "live"},
                     {"table", "transactions"},
                     {"entity_column", "account_id"},
                     {"time_column", "occurred_at"},
                     {"label_column", "fraud"},
                     {"feature_columns", {"amount"}},
                     {"output_topic", "fraud.scores"}};
    const auto compiled =
        compile_intent("Score transactions as they arrive", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-21");
    EXPECT_EQ(compiled->shape, IntentShape::Generic);
    EXPECT_EQ(compiled->graph.node_count(), 4u);
    EXPECT_TRUE(compiled->graph.has_node("subscribe_events"));
    EXPECT_TRUE(compiled->graph.has_node("window_features"));
    const auto model = compiled->graph.find_node("train_streaming_model");
    ASSERT_TRUE(model);
    EXPECT_EQ(model->attributes.at("model_kind"), "streaming");
    EXPECT_EQ(model->attributes.at("time_column"), "occurred_at");
    EXPECT_TRUE(model->attributes.at("prequential").get<bool>());
    EXPECT_TRUE(zetta::pipeline::default_gate().run(compiled->graph).passed());
}

TEST(IntentCompilerTest, SharedSolutionKeepsItsPrimaryMethodology) {
    zetta::pipeline::IntentCompileOptions options;
    options.solution = "S-10";
    const auto compiled = compile_intent(
        "Create privacy-preserving synthetic training data", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-1");
    EXPECT_TRUE(compiled->graph.has_node("train_churn_model"));
}

TEST(IntentCompilerTest, EnsembleMethodologyBuildsItsOwnTableShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-18";
    options.hints = {{"dataset_path", "datasets.training_rows"},
                     {"entity_column", "account_id"},
                     {"label_column", "converted"},
                     {"feature_columns", {"signal_a", "signal_b"}}};
    const auto compiled = compile_intent("Stack complementary models", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-18");
    EXPECT_EQ(compiled->shape, IntentShape::Generic);
    EXPECT_EQ(compiled->graph.node_count(), 4u);
    EXPECT_TRUE(compiled->graph.has_node("load_ensemble_table"));
    EXPECT_TRUE(compiled->graph.has_node("train_ensemble"));
    EXPECT_FALSE(compiled->graph.has_node("train_churn_model"));
    const auto model = compiled->graph.find_node("train_ensemble");
    ASSERT_TRUE(model);
    EXPECT_EQ(model->attributes.at("model_kind"), "ensemble");
    EXPECT_EQ(model->attributes.at("stacking_predictions"), "out_of_fold");
    EXPECT_TRUE(zetta::pipeline::default_gate().run(compiled->graph).passed());
}

TEST(IntentCompilerTest, ExplicitBayesianMethodologyBuildsItsOwnShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-17";
    options.hints = {{"database", "measurements"},
                     {"table", "store_demand"},
                     {"entity_column", "id"},
                     {"label_column", "demand"},
                     {"group_column", "store"}};
    const auto compiled = compile_intent(
        "Estimate store demand with honest uncertainty", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-17");
    EXPECT_TRUE(compiled->graph.has_node("read_grouped_observations"));
    EXPECT_TRUE(compiled->graph.has_node("fit_hierarchical_posterior"));
    EXPECT_FALSE(compiled->graph.has_node("train_churn_model"));
    EXPECT_EQ(compiled->graph
                  .find_node("fit_hierarchical_posterior")
                  ->attributes.at("model_kind"),
              "bayesian");
    EXPECT_TRUE(zetta::pipeline::default_gate()
                    .run(compiled->graph)
                    .passed());
}

TEST(IntentCompilerTest, StreamingIntentDoesNotOverrideThePrimarySolution) {
    zetta::pipeline::IntentCompileOptions options;
    options.solution = "S-10";
    const auto compiled =
        compile_intent("Score events as they arrive", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-1");
    EXPECT_FALSE(compiled->graph.has_node("train_streaming_model"));
    EXPECT_TRUE(compiled->graph.has_node("train_churn_model"));
}

TEST(IntentCompilerTest, CausalMethodologyIdAndModelKindsBuildUpliftShape) {
    for (const char* methodology : {"M-6", "uplift", "causal"}) {
        zetta::pipeline::IntentCompileOptions options;
        options.methodology = methodology;
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

TEST(IntentCompilerTest, SurvivalBuildsItsOwnRunnableShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-5";
    const auto compiled = compile_intent("When will each subject fail?", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-5");
    EXPECT_TRUE(compiled->graph.has_node("train_survival_model"));
    EXPECT_TRUE(compiled->graph.has_node("write_survival_table"));
    EXPECT_FALSE(compiled->graph.has_node("train_churn_model"));
    EXPECT_TRUE(zetta::pipeline::default_gate().run(compiled->graph).passed());
}

TEST(IntentCompilerTest, GraphMethodologyBuildsItsOwnPipeline) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-12";
    options.hints = {{"source", "fraud"},
                     {"table", "payments"},
                     {"prediction_time", 100.0}};
    const auto compiled =
        compile_intent("Learn risk from payment relationships", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-12");
    EXPECT_TRUE(compiled->graph.has_node("load_graph_edges"));
    EXPECT_TRUE(compiled->graph.has_node("filter_graph_edges"));
    EXPECT_TRUE(compiled->graph.has_node("train_graph_model"));
    EXPECT_FALSE(compiled->graph.has_node("train_churn_model"));
    EXPECT_EQ(compiled->graph.find_node("train_graph_model")
                  ->attributes.at("model_kind"),
              "graph");
    const auto report = zetta::pipeline::default_gate().run(compiled->graph);
    EXPECT_TRUE(report.passed());
}

TEST(IntentCompilerTest, FraudPrimarySolutionSelectsM1) {
    zetta::pipeline::IntentCompileOptions options;
    options.solution = "S-10";
    options.hints = {{"prediction_time", 100.0}};
    const auto compiled = compile_intent("Review transaction fraud", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-1");
    EXPECT_TRUE(compiled->graph.has_node("train_churn_model"));
}

TEST(IntentCompilerTest, DocumentMethodologyBuildsItsOwnRunnableShape) {
    zetta::pipeline::IntentCompileOptions options;
    options.methodology = "M-11";
    options.hints = {{"source", "documents"},
                     {"table", "invoices"},
                     {"fields", {"invoice_number", "total"}},
                     {"review_threshold", 0.85}};
    const auto compiled = compile_intent("extract invoice fields", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-11");
    ASSERT_EQ(compiled->graph.nodes_of_kind(StepKind::Model).size(), 1u);
    const auto model = compiled->graph.nodes_of_kind(StepKind::Model).front();
    EXPECT_EQ(model->attributes.at("model_kind"), "document_extraction");
    EXPECT_EQ(model->attributes.at("fields").size(), 2u);
    EXPECT_EQ(compiled->graph.nodes_of_kind(StepKind::Act).front()
                  ->attributes.at("route"),
              "review_queue");
    EXPECT_EQ(compiled->graph.nodes_of_kind(StepKind::Clean).front()
                  ->attributes.at("ocr"),
              "trocr");
}

TEST(IntentCompilerTest, SegmentationKeywordBuildsItsOwnShape) {
    std::string error;
    const auto compiled =
        compile_intent("segment my customers into groups", &error);
    ASSERT_TRUE(compiled) << error;
    EXPECT_EQ(compiled->methodology, "M-3");
    EXPECT_TRUE(compiled->graph.has_node("train_segments"));
    EXPECT_FALSE(compiled->graph.has_node("train_churn_model"));
    EXPECT_EQ(compiled->graph.node_count(), 5u);
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

    std::string template_name;
    auto graph = project->compile_intent_to_graph(intent->id(), {}, {}, nullptr,
                                                  &template_name);
    ASSERT_TRUE(graph);
    // Named from the intent's words (#1238), with the template it started from.
    EXPECT_EQ(graph->name(), "Flag accounts likely to churn and notify our");
    EXPECT_EQ(template_name, "the M-1 churn template");
    EXPECT_TRUE(covers_all_stages(graph->graph()));

    // It is a real stored graph: findable by name and listed by the project, so
    // the Graph Workspace renders + edits it like any other.
    EXPECT_TRUE(project->find_graph("Flag accounts likely to churn and notify our"));
    EXPECT_EQ(project->graph_count(), 1u);

    // The same intent compiled again gets a numbered name, not a collision.
    auto again = project->compile_intent_to_graph(intent->id());
    ASSERT_TRUE(again);
    EXPECT_EQ(again->name(), "Flag accounts likely to churn and notify our (2)");
    EXPECT_EQ(project->graph_count(), 2u);

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

// #1246: the generic M-1 template is fitted to the dataset it was compiled for —
// the connect step reads the hinted source and table, and the steps are named
// from the label and the table, not the churn demo.
TEST(IntentCompilerTest, TheGenericTemplateIsFittedToTheDatasetHints) {
    zetta::pipeline::IntentCompileOptions options;
    options.hints = {{"source", "billing"},
                     {"table", "subscriptions"},
                     {"entity_column", "subscription_id"},
                     {"label_column", "downgraded"}};
    const auto compiled =
        compile_intent("Predict which subscriptions will downgrade", options);
    ASSERT_TRUE(compiled);
    EXPECT_EQ(compiled->methodology, "M-1");

    const auto connect = compiled->graph.find_node("load_customers");
    ASSERT_TRUE(connect);
    EXPECT_EQ(connect->attributes.at("source"), "billing");
    EXPECT_EQ(connect->attributes.at("table"), "subscriptions");
    EXPECT_EQ(connect->name, "Load subscriptions");
    EXPECT_EQ(compiled->graph.find_node("build_features")->name, "Build downgraded features");
    const auto model = compiled->graph.find_node("train_churn_model");
    ASSERT_TRUE(model);
    EXPECT_EQ(model->name, "Train downgraded model");
    EXPECT_EQ(model->attributes.at("target"), "downgraded");
    EXPECT_EQ(model->attributes.at("note"), "Predict which subscriptions will downgrade");
    EXPECT_EQ(compiled->graph.find_node("index_scores")->name, "Score and index subscriptions");
    EXPECT_EQ(compiled->graph.find_node("serve_scores")->name, "Serve downgraded scores & reasons");
    EXPECT_EQ(compiled->graph.find_node("notify_retention")->name, "Write downgraded scores");
    for (const auto& node : compiled->graph.nodes()) {
        EXPECT_EQ(node->name.find("churn"), std::string::npos) << node->name;
    }
}

TEST(IntentCompilerTest, ASolutionFamilyKeepsItsOwnStepNames) {
    zetta::pipeline::IntentCompileOptions options;
    options.solution = "S-4";
    options.hints = {{"table", "subscriptions"}, {"label_column", "downgraded"}};
    const auto compiled = compile_intent("Prioritize collections", options);
    ASSERT_TRUE(compiled);
    const auto connect = compiled->graph.find_node("load_customers");
    ASSERT_TRUE(connect);
    EXPECT_EQ(connect->name, "Load receivables");
    EXPECT_EQ(connect->attributes.at("table"), "receivables");
}
