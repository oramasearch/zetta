#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/gate.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/agent/end_state.hpp"
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

// --- MethodologyCheck (#984) --------------------------------------------------------------
//
// A model step may not name a methodology the engine cannot execute. Before `model_kind` rode
// the model step it was a string in a bag nothing read, so a graph could name `forecast` and
// silently run the churn path. A silent wrong pipeline is the product lying fluently; a loud
// refusal is the honest outcome, and this check is what makes it loud.

TEST(MethodologyCheck, AnImplementedMethodologyPasses) {
    zetta::ir::ExecutionGraph graph;
    const auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    const auto model = graph.add_node(zetta::ir::StepKind::Model, "model",
                                      nlohmann::json{{"model_kind", "gbdt"}});
    graph.add_edge(connect->id, model->id);

    const zetta::pipeline::CheckResult result =
        zetta::pipeline::MethodologyCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Pass) << result.reason;
}

TEST(MethodologyCheck, AnUnimplementedMethodologyFailsAndSaysWhatToDo) {
    zetta::ir::ExecutionGraph graph;
    const auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    const auto model = graph.add_node(zetta::ir::StepKind::Model, "model",
                                      nlohmann::json{{"model_kind", "forecast"}});
    graph.add_edge(connect->id, model->id);

    const zetta::pipeline::CheckResult result =
        zetta::pipeline::MethodologyCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("forecast"), std::string::npos)
        << "the reason must name the methodology that was refused";
    EXPECT_NE(result.reason.find("gbdt"), std::string::npos)
        << "the reason must name what IS implemented, so the author knows the way forward";
}

TEST(MethodologyCheck, AModelStepWithNoMethodologyIsSkippedNotFailed) {
    // Opt-in, exactly like the other checks here: a graph that carries no annotation must not
    // be penalised for it, or every pre-existing pipeline breaks the day this check lands.
    zetta::ir::ExecutionGraph graph;
    const auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    const auto model = graph.add_node(zetta::ir::StepKind::Model, "model");
    graph.add_edge(connect->id, model->id);

    EXPECT_EQ(zetta::pipeline::MethodologyCheck{}.evaluate(graph).status,
              zetta::pipeline::CheckStatus::Pass);
}

TEST(MethodologyCheck, TheImplementedSetIsWhatTheEngineActuallyHas) {
    // This asserts the set is HONEST, not that it is large. It widens only as each M-n lands
    // (#984); widening it early is exactly how the agent learns to propose work the engine
    // silently will not do.
    const std::vector<std::string>& implemented =
        zetta::pipeline::MethodologyCheck::implemented();
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "gbdt"), implemented.end())
        << "M-1 tabular supervised ships today";
    for (const char* unshipped : {"forecast", "anomaly", "uplift", "survival", "cluster"}) {
        EXPECT_EQ(std::find(implemented.begin(), implemented.end(), unshipped), implemented.end())
            << unshipped << " is not implemented; listing it here would make the check a lie";
    }
}

// --- TemporalValidationCheck (#984, M-4) ---------------------------------------------------
//
// This is the check that makes forecasting a different METHODOLOGY rather than churn with
// different nouns, and it is the first place the engine can tell the two apart.

namespace {

zetta::ir::ExecutionGraph model_graph(nlohmann::json model_attributes) {
    zetta::ir::ExecutionGraph graph;
    const auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    const auto model =
        graph.add_node(zetta::ir::StepKind::Model, "model", std::move(model_attributes));
    graph.add_edge(connect->id, model->id);
    return graph;
}

}  // namespace

TEST(TemporalValidationCheck, AForecastValidatedOverTimePasses) {
    const auto graph =
        model_graph({{"model_kind", "forecast"}, {"temporal_validation", true}});
    const auto result = zetta::pipeline::TemporalValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Pass) << result.reason;
}

TEST(TemporalValidationCheck, AForecastWithoutTemporalValidationFails) {
    const auto graph = model_graph({{"model_kind", "forecast"}});
    const auto result = zetta::pipeline::TemporalValidationCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("temporal_validation"), std::string::npos)
        << "the reason must name what to add";
}

// The counter-intuitive half. Calibration is correct and necessary for churn; on a forecast it is
// the churn recipe applied to a problem it does not fit — there is no probability to calibrate,
// and fitting a calibrator on rows held out by CONTENT re-introduces the leak temporal validation
// exists to prevent, wearing a respectable name.
TEST(TemporalValidationCheck, AForecastThatCalibratesFails) {
    const auto graph = model_graph(
        {{"model_kind", "forecast"}, {"temporal_validation", true}, {"calibrated", true}});
    const auto result = zetta::pipeline::TemporalValidationCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("calibrat"), std::string::npos);
    EXPECT_NE(result.reason.find("backtest"), std::string::npos)
        << "the reason must say what to do instead, not only what is wrong";
}

// The other side of the same coin: churn MUST still be allowed to calibrate. A check that made
// calibration suspicious everywhere would break the one methodology that ships.
TEST(TemporalValidationCheck, ChurnMayStillCalibrate) {
    const auto graph = model_graph({{"model_kind", "gbdt"}, {"calibrated", true}});
    const auto result = zetta::pipeline::TemporalValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Pass) << result.reason;
}

TEST(TemporalValidationCheck, AModelWithNoMethodologyIsSkipped) {
    const auto graph = model_graph(nlohmann::json::object());
    EXPECT_EQ(zetta::pipeline::TemporalValidationCheck{}.evaluate(graph).status,
              zetta::pipeline::CheckStatus::Pass);
}

// The two checks together are what the corpus's neighbouring-solution negative control needs:
// the forecasting shape and the churn shape must not both pass the same gate.
TEST(TemporalValidationCheck, TheChurnShapeAndTheForecastShapeAreNotInterchangeable) {
    const auto churn_shape = model_graph({{"model_kind", "gbdt"}, {"calibrated", true}});
    const auto forecast_shape =
        model_graph({{"model_kind", "forecast"}, {"temporal_validation", true}});

    const zetta::pipeline::TemporalValidationCheck temporal;
    EXPECT_EQ(temporal.evaluate(churn_shape).status, zetta::pipeline::CheckStatus::Pass);
    EXPECT_EQ(temporal.evaluate(forecast_shape).status, zetta::pipeline::CheckStatus::Pass);

    // Now swap the methodologies onto the wrong shapes. Both must be refused.
    const auto churn_shape_as_forecast =
        model_graph({{"model_kind", "forecast"}, {"calibrated", true}});
    EXPECT_EQ(temporal.evaluate(churn_shape_as_forecast).status,
              zetta::pipeline::CheckStatus::Fail)
        << "the churn recipe under a forecasting name is the exact confusion the corpus's "
           "neighbouring-solution control is trying to teach against";
}

// --- B7: a methodology travels intact from facts to gate verdict ---------------------------
//
// The end-to-end property #983 turns on: an ask for one methodology must produce that
// methodology's pipeline, and the neighbouring one must be refused. Before this epic the two
// were indistinguishable — `model_kind` never reached the IR, so a forecasting graph and a churn
// graph were byte-identical and no check could tell them apart.
//
// This composes the whole chain: agent facts → build_solution_graph → the correctness gate.

namespace {

zetta::pipeline::ValidationReport gate_over(const zetta::ir::ExecutionGraph& graph) {
    zetta::pipeline::CorrectnessGate gate;
    gate.add_check(std::make_shared<zetta::pipeline::MethodologyCheck>());
    gate.add_check(std::make_shared<zetta::pipeline::TemporalValidationCheck>());
    return gate.run(graph);
}

}  // namespace

TEST(MethodologyEndToEnd, TheChurnShapeSurvivesFactsToGateVerdict) {
    zetta::agent::EndState facts;
    facts.set("connect", true);
    facts.set("clean", true);
    facts.set("model", true);
    facts.set("serve", true);
    facts.set("model_kind", std::string("gbdt"));
    facts.set("calibrated", true);

    const auto graph = zetta::agent::build_solution_graph(facts);
    const auto report = gate_over(graph);
    EXPECT_FALSE(report.block_deploy)
        << "the one methodology that ships must still pass end to end";
}

// The honest state of M-4 today. Its primitives exist and are tested — temporal split, backtest,
// lag features, the squared-error objective — but nothing wires them into a run, so `forecast` is
// deliberately NOT in MethodologyCheck::implemented(). The gate therefore refuses it, loudly,
// with a reason that names what is implemented.
//
// A loud refusal is the honest outcome and the whole point of B1b: the alternative is the agent
// proposing a forecast the engine silently runs as a classifier.
TEST(MethodologyEndToEnd, AForecastIsRefusedUntilItsRunPathExists) {
    zetta::agent::EndState facts;
    facts.set("connect", true);
    facts.set("model", true);
    facts.set("serve", true);
    facts.set("model_kind", std::string("forecast"));
    facts.set("temporal_validation", true);

    const auto graph = zetta::agent::build_solution_graph(facts);
    const auto report = gate_over(graph);
    EXPECT_TRUE(report.block_deploy)
        << "a methodology with no run path must be refused, not silently run as something else";

    bool named_the_way_forward = false;
    for (const auto& result : report.checks) {
        if (result.status == zetta::pipeline::CheckStatus::Fail &&
            result.reason.find("gbdt") != std::string::npos) {
            named_the_way_forward = true;
        }
    }
    EXPECT_TRUE(named_the_way_forward)
        << "the refusal must say what IS implemented, or the author only learns they are stuck";
}

// And the property that makes the corpus's neighbouring-solution negative control meaningful:
// the two shapes are not interchangeable. Swap the methodologies and the verdicts swap with them.
TEST(MethodologyEndToEnd, TheTwoShapesAreNotInterchangeable) {
    const auto build = [](const std::string& model_kind, bool calibrated) {
        zetta::agent::EndState facts;
        facts.set("connect", true);
        facts.set("model", true);
        facts.set("serve", true);
        facts.set("model_kind", model_kind);
        facts.set("temporal_validation", true);
        if (calibrated) facts.set("calibrated", true);
        return zetta::agent::build_solution_graph(facts);
    };

    // Churn calibrates and is fine. The identical shape under a forecasting name is not.
    EXPECT_FALSE(gate_over(build("gbdt", true)).block_deploy);
    EXPECT_TRUE(gate_over(build("forecast", true)).block_deploy)
        << "the churn recipe wearing a forecasting name is the exact confusion the corpus's "
           "neighbouring-solution control exists to teach against, and the gate must catch it";
}
