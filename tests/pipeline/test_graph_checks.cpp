#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/pipeline/validation.hpp"

using zetta::pipeline::CheckResult;
using zetta::pipeline::CheckStatus;
using zetta::pipeline::CorrectnessGate;
using zetta::pipeline::DataLeakageCheck;
using zetta::pipeline::SchemaTypeCheck;
using zetta::pipeline::ValidationReport;
using zetta::ir::EdgeKind;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using nlohmann::json;

namespace {

// A correct churn-style graph: a Postgres source declares its column profile
// (`schema`), a model declares the typed fields it `requires` and a set of
// `features` all dated at or before the as_of cutoff. Both #309 checks pass it.
ExecutionGraph clean_graph() {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load_orders",
                   json{{"source", "postgres"},
                        {"schema", {{"customer_id", "int"},
                                    {"revenue", "float"},
                                    {"signup_date", "timestamp"}}}},
                   "n-load");
    graph.add_node(StepKind::Clean, "dedupe", json::object(), "n-clean");
    graph.add_node(
        StepKind::Model, "churn_model",
        json{{"as_of", "2024-01-01"},
             {"label", "churned"},
             {"requires", {{"customer_id", "int"}, {"revenue", "float"}}},
             {"features",
              json::array(
                  {json{{"name", "spend_2023"}, {"data_as_of", "2023-12-31"}},
                   json{{"name", "tenure_days"}}})}},
        "n-model");
    graph.add_edge("n-load", "n-clean", EdgeKind::DataFlow);
    graph.add_edge("n-clean", "n-model", EdgeKind::DataFlow);
    return graph;
}

}  // namespace

// ---- schema_type ------------------------------------------------------------

TEST(SchemaTypeCheck, CleanGraphPasses) {
    const SchemaTypeCheck check;
    EXPECT_EQ(check.name(), "schema_type");
    const CheckResult result = check.evaluate(clean_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(SchemaTypeCheck, MissingFieldFailsWithReason) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load",
                   json{{"schema", {{"customer_id", "int"}}}}, "n-load");
    graph.add_node(StepKind::Model, "model",
                   json{{"requires", {{"revenue", "float"}}}}, "n-model");
    graph.add_edge("n-load", "n-model", EdgeKind::DataFlow);

    const CheckResult result = SchemaTypeCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("revenue"), std::string::npos) << result.reason;
    EXPECT_NE(result.reason.find("no upstream"), std::string::npos)
        << result.reason;
}

TEST(SchemaTypeCheck, TypeMismatchFailsWithReason) {
    // The source provides revenue as a string; the model requires it as a float.
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load",
                   json{{"schema", {{"revenue", "string"}}}}, "n-load");
    graph.add_node(StepKind::Model, "model",
                   json{{"requires", {{"revenue", "float"}}}}, "n-model");
    graph.add_edge("n-load", "n-model", EdgeKind::DataFlow);

    const CheckResult result = SchemaTypeCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("type mismatch"), std::string::npos)
        << result.reason;
}

TEST(SchemaTypeCheck, ResolvesTypesThroughTransitiveAncestors) {
    // The source is two data-flow hops away (through an unannotated clean step);
    // the required type must still resolve against the connect step's profile.
    const CheckResult result = SchemaTypeCheck{}.evaluate(clean_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(SchemaTypeCheck, TypeComparisonIsCaseInsensitive) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load",
                   json{{"schema", {{"revenue", "Float"}}}}, "n-load");
    graph.add_node(StepKind::Model, "model",
                   json{{"requires", {{"revenue", "float"}}}}, "n-model");
    graph.add_edge("n-load", "n-model", EdgeKind::DataFlow);

    EXPECT_EQ(SchemaTypeCheck{}.evaluate(graph).status, CheckStatus::Pass);
}

TEST(SchemaTypeCheck, GraphWithoutRequirementsPasses) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load_orders");  // no annotations at all
    const CheckResult result = SchemaTypeCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Pass);
}

TEST(SchemaTypeCheck, MalformedRequiresAnnotationFails) {
    // `requires` must be an object of field -> type string; an array is rejected
    // at the boundary with a clear reason rather than silently misread.
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "model",
                   json{{"requires", json::array({"revenue"})}}, "n-model");

    const CheckResult result = SchemaTypeCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("malformed"), std::string::npos)
        << result.reason;
}

// ---- data_leakage: point-in-time -------------------------------------------

TEST(DataLeakageCheck, CleanGraphPasses) {
    const DataLeakageCheck check;
    EXPECT_EQ(check.name(), "data_leakage");
    const CheckResult result = check.evaluate(clean_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(DataLeakageCheck, FeatureAfterCutoffFailsWithReason) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "churn_model",
        json{{"as_of", "2024-01-01"},
             {"features",
              json::array({json{{"name", "spend_after_cutoff"},
                                 {"data_as_of", "2024-02-01"}}})}},
        "n-model");

    const CheckResult result = DataLeakageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("point-in-time"), std::string::npos)
        << result.reason;
    EXPECT_NE(result.reason.find("spend_after_cutoff"), std::string::npos)
        << result.reason;
}

TEST(DataLeakageCheck, FeatureAtCutoffPasses) {
    // Exactly at the cutoff is allowed (data available "as of" the cutoff).
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "2024-01-01T00:00:00Z"},
             {"features", json::array({json{{"name", "f"},
                                            {"data_as_of",
                                             "2024-01-01T00:00:00Z"}}})}},
        "n-model");
    EXPECT_EQ(DataLeakageCheck{}.evaluate(graph).status, CheckStatus::Pass);
}

TEST(DataLeakageCheck, DateAndDateTimeCutoffsCompareCorrectly) {
    // A date-only cutoff is midnight; a same-day afternoon feature leaks.
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "2024-01-01"},
             {"features", json::array({json{{"name", "f"},
                                            {"data_as_of",
                                             "2024-01-01T13:00:00Z"}}})}},
        "n-model");
    EXPECT_EQ(DataLeakageCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(DataLeakageCheck, FeaturesWithoutCutoffFails) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "model",
                   json{{"features", json::array({json{{"name", "f"}}})}},
                   "n-model");

    const CheckResult result = DataLeakageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("as_of"), std::string::npos) << result.reason;
}

// ---- data_leakage: target leakage ------------------------------------------

TEST(DataLeakageCheck, FeatureNamedAfterLabelFails) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "2024-01-01"},
             {"label", "churned"},
             {"features", json::array({json{{"name", "churned"}}})}},
        "n-model");

    const CheckResult result = DataLeakageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("target leakage"), std::string::npos)
        << result.reason;
}

TEST(DataLeakageCheck, FeatureDerivedFromLabelFails) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "2024-01-01"},
             {"label", "churned"},
             {"features", json::array({json{{"name", "churn_proxy"},
                                            {"derived_from", "label"}}})}},
        "n-model");

    EXPECT_EQ(DataLeakageCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(DataLeakageCheck, FeatureFlaggedLeaksTargetFails) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "2024-01-01"},
             {"features", json::array({json{{"name", "f"},
                                            {"leaks_target", true}}})}},
        "n-model");

    EXPECT_EQ(DataLeakageCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(DataLeakageCheck, GraphWithoutFeaturesPasses) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "model");  // no leakage annotations
    EXPECT_EQ(DataLeakageCheck{}.evaluate(graph).status, CheckStatus::Pass);
}

TEST(DataLeakageCheck, MalformedFeatureAnnotationFails) {
    // A feature item must have a string "name"; omitting it is rejected at the
    // boundary.
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "2024-01-01"},
             {"features", json::array({json{{"data_as_of", "2023-01-01"}}})}},
        "n-model");

    const CheckResult result = DataLeakageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("malformed"), std::string::npos)
        << result.reason;
}

TEST(DataLeakageCheck, MalformedAsOfTimestampFails) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "model",
        json{{"as_of", "not-a-date"},
             {"features", json::array({json{{"name", "f"}}})}},
        "n-model");

    const CheckResult result = DataLeakageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("malformed"), std::string::npos)
        << result.reason;
}

// ---- end to end through the gate (RULE-1) ----------------------------------

TEST(GraphChecksThroughGate, DefaultGateShipsTheConcreteChecks) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    ASSERT_EQ(gate.check_count(), 4u);
    EXPECT_EQ(gate.checks()[0]->name(), "non_empty_graph");
    EXPECT_EQ(gate.checks()[1]->name(), "schema_type");
    EXPECT_EQ(gate.checks()[2]->name(), "data_leakage");
    EXPECT_EQ(gate.checks()[3]->name(), "entity_resolution");
}

TEST(GraphChecksThroughGate, CleanGraphDeploys) {
    const ValidationReport report = zetta::pipeline::default_gate().run(clean_graph());
    EXPECT_TRUE(report.passed()) << report.checks.size();
    EXPECT_FALSE(report.block_deploy);
}

TEST(GraphChecksThroughGate, LeakyGraphBlocksDeploy) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "churn_model",
        json{{"as_of", "2024-01-01"},
             {"features", json::array({json{{"name", "future_spend"},
                                            {"data_as_of", "2024-06-01"}}})}},
        "n-model");

    const ValidationReport report = zetta::pipeline::default_gate().run(graph);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);  // RULE-1
}

TEST(GraphChecksThroughGate, MistypedGraphBlocksDeploy) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "load",
                   json{{"schema", {{"revenue", "string"}}}}, "n-load");
    graph.add_node(StepKind::Model, "model",
                   json{{"requires", {{"revenue", "float"}}}}, "n-model");
    graph.add_edge("n-load", "n-model", EdgeKind::DataFlow);

    const ValidationReport report = zetta::pipeline::default_gate().run(graph);
    EXPECT_FALSE(report.passed());
    EXPECT_TRUE(report.block_deploy);
}
