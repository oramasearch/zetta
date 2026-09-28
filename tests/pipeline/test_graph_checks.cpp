#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/gate.hpp"
#include "zetta/agent/solution_graph.hpp"
#include "zetta/agent/end_state.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/pipeline/validation.hpp"

using zetta::pipeline::CheckResult;
using zetta::pipeline::CheckStatus;
using zetta::pipeline::CausalUpliftCheck;
using zetta::pipeline::BanditPolicyCheck;
using zetta::pipeline::CorrectnessGate;
using zetta::pipeline::DataLeakageCheck;
using zetta::pipeline::GraphEdgeTimeCheck;
using zetta::pipeline::LanguageGroundingCheck;
using zetta::pipeline::LanguageTrainingAccessCheck;
using zetta::pipeline::EnsembleOutOfFoldCheck;
using zetta::pipeline::SchemaTypeCheck;
using zetta::pipeline::SnapshotLineageCheck;
using zetta::pipeline::SurvivalCensoringCheck;
using zetta::pipeline::SpeechConsentCheck;
using zetta::pipeline::SpeechLocalAudioCheck;
using zetta::pipeline::SpeechPiiRedactionCheck;
using zetta::pipeline::ValidationReport;
using zetta::pipeline::VisionSourceSplitCheck;
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
                        {"source_kind", "snapshot"},
                        {"snapshot_as_of", "2024-01-01"},
                        {"schema", {{"customer_id", "int"},
                                    {"revenue", "float"},
                                    {"signup_date", "timestamp"}}}},
                   "n-load");
    graph.add_node(StepKind::Clean, "dedupe", json::object(), "n-clean");
    graph.add_node(
        StepKind::Model, "churn_model",
        json{{"as_of", "2024-01-01"},
             {"feature_window_end", "2024-01-01"},
             {"label_window_start", "2024-01-08"},
             {"blackout_gap_seconds", 604800},
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

ExecutionGraph entity_resolution_graph() {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "resolve entities",
        json{{"model_kind", "entity_resolution"},
             {"blocking_keys", {"email", "phone", "name"}},
             {"match_threshold", 0.82},
             {"review_threshold", 0.55},
             {"conflict_policy", "reject_verified_identifier_conflicts"}},
        "resolve");
    return graph;
}

ExecutionGraph uplift_graph(json overrides = json::object()) {
    json attributes = {
        {"model_kind", "uplift"},
        {"treatment", "treated"},
        {"outcome", "converted"},
        {"label", "converted"},
        {"treatment_time", "2024-01-15"},
        {"as_of", "2024-01-15"},
        {"overlap_lower", 0.05},
        {"overlap_upper", 0.95},
        {"features",
         json::array({json{{"name", "tenure"},
                            {"data_as_of", "2024-01-14"}}})}};
    for (auto& [key, value] : overrides.items()) {
        attributes[key] = value;
    }
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "uplift", std::move(attributes),
                   "uplift-model");
    return graph;
}

ExecutionGraph rank_graph() {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "rank leads",
        json{{"model_kind", "rank"},
             {"group_column", "rep_id"},
             {"split_strategy", "group"},
             {"feature_availability", "ranking_time"}},
        "rank-model");
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

TEST(SnapshotLineageCheck, CleanGraphPasses) {
    const CheckResult result = SnapshotLineageCheck{}.evaluate(clean_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(SnapshotLineageCheck, RefusesFeatureLineageFromALiveTable) {
    ExecutionGraph graph = clean_graph();
    graph.find_node("n-load")->attributes["source_kind"] = "live";
    graph.find_node("n-load")->attributes.erase("snapshot_as_of");

    const CheckResult result = SnapshotLineageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("live-table"), std::string::npos)
        << result.reason;
}

TEST(SnapshotLineageCheck, RefusesSnapshotAfterPredictionTime) {
    ExecutionGraph graph = clean_graph();
    graph.find_node("n-load")->attributes["snapshot_as_of"] = "2024-01-02";

    const CheckResult result = SnapshotLineageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("after prediction"), std::string::npos)
        << result.reason;
}

TEST(SnapshotLineageCheck, RefusesLabelWindowInsideBlackoutGap) {
    ExecutionGraph graph = clean_graph();
    graph.find_node("n-model")->attributes["label_window_start"] =
        "2024-01-07";

    const CheckResult result = SnapshotLineageCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("blackout gap"), std::string::npos)
        << result.reason;
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

TEST(SurvivalGraphCheck, RefusesMissingCensoringGuarantees) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "survival",
                   json{{"model_kind", "survival"},
                        {"duration_column", "duration"},
                        {"event_indicator", "event"}},
                   "survival");

    const CheckResult result = SurvivalCensoringCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("censoring_handled"), std::string::npos);
}

TEST(SurvivalGraphCheck, RefusesFeatureAfterCutoff) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "survival",
        json{{"model_kind", "survival"},
             {"as_of", "2024-01-01"},
             {"features", json::array({json{{"name", "future_usage"},
                                              {"data_as_of", "2024-02-01"}}})}},
        "survival");

    EXPECT_EQ(DataLeakageCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(SurvivalGraphCheck, AcceptsRequiredCensoringGuarantees) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "survival",
                   json{{"model_kind", "survival"},
                        {"duration_column", "duration"},
                        {"event_indicator", "event"},
                        {"censoring_handled", true},
                        {"stop_after_event", true},
                        {"point_in_time_features", true}},
                   "survival");

    EXPECT_EQ(SurvivalCensoringCheck{}.evaluate(graph).status,
              CheckStatus::Pass);
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

TEST(EntityResolutionGraphCheck, RefusesMissingBlockingKeys) {
    ExecutionGraph graph = entity_resolution_graph();
    graph.find_node("resolve")->attributes.erase("blocking_keys");

    const CheckResult result =
        zetta::pipeline::EntityResolutionGraphCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("blocking_keys"), std::string::npos);
}

TEST(EntityResolutionGraphCheck, RefusesUnstatedThresholds) {
    ExecutionGraph graph = entity_resolution_graph();
    graph.find_node("resolve")->attributes.erase("review_threshold");

    const CheckResult result =
        zetta::pipeline::EntityResolutionGraphCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("threshold"), std::string::npos);
}

TEST(EntityResolutionGraphCheck, RefusesUnsafeConflictPolicy) {
    ExecutionGraph graph = entity_resolution_graph();
    graph.find_node("resolve")->attributes["conflict_policy"] = "allow";

    const CheckResult result =
        zetta::pipeline::EntityResolutionGraphCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("verified identifier"), std::string::npos);
}

TEST(EntityResolutionGraphCheck, AcceptsTheM2Profile) {
    const CheckResult result =
        zetta::pipeline::EntityResolutionGraphCheck{}.evaluate(
            entity_resolution_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(CausalUpliftGraphCheck, CleanGraphPasses) {
    const CheckResult result = CausalUpliftCheck{}.evaluate(uplift_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(CausalUpliftGraphCheck, MissingTreatmentIsRefused) {
    ExecutionGraph graph = uplift_graph();
    graph.find_node("uplift-model")->attributes.erase("treatment");
    const CheckResult result = CausalUpliftCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("treatment and outcome"), std::string::npos);
}

TEST(CausalUpliftGraphCheck, PostTreatmentFeatureIsRefused) {
    const CheckResult result = CausalUpliftCheck{}.evaluate(uplift_graph(
        {{"features", json::array({json{{"name", "after_contact"},
                                        {"data_as_of", "2024-01-16"}}})}}));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("after treatment"), std::string::npos);
}

TEST(CausalUpliftGraphCheck, OutcomeNamedFeatureIsRefused) {
    const CheckResult result = CausalUpliftCheck{}.evaluate(uplift_graph(
        {{"features", json::array({json{{"name", "converted_30d"}}})}}));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("named like outcome"), std::string::npos);
}

TEST(CausalUpliftGraphCheck, UnsafeOverlapBoundsAreRefused) {
    const CheckResult result =
        CausalUpliftCheck{}.evaluate(uplift_graph({{"overlap_lower", 0.01}}));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("overlap"), std::string::npos);
}
TEST(GraphCheck, EdgeTimeFilterPassesWhenItMatchesTheModelCutoff) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "edges", json::object(), "source");
    graph.add_node(StepKind::Clean, "filter future edges",
                   json{{"filter_time_column", "time"},
                        {"prediction_time", 10.0},
                        {"exclude_after_prediction_time", true}},
                   "filter");
    graph.add_node(StepKind::Model, "graph model",
                   json{{"model_kind", "graph"},
                        {"time_column", "time"},
                        {"prediction_time", 10.0}},
                   "model");
    graph.add_edge("source", "filter");
    graph.add_edge("filter", "model");
    EXPECT_EQ(GraphEdgeTimeCheck{}.evaluate(graph).status, CheckStatus::Pass);
}

TEST(GraphCheck, RefusesAGraphWithoutAPredictionCutoff) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "graph model",
                   json{{"model_kind", "graph"}, {"time_column", "time"}},
                   "model");
    EXPECT_EQ(GraphEdgeTimeCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(GraphCheck, RefusesADisabledEdgeTimeFilter) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "filter future edges",
                   json{{"filter_time_column", "time"},
                        {"prediction_time", 10.0},
                        {"exclude_after_prediction_time", false}},
                   "filter");
    graph.add_node(StepKind::Model, "graph model",
                   json{{"model_kind", "graph"},
                        {"time_column", "time"},
                        {"prediction_time", 10.0}},
                   "model");
    graph.add_edge("filter", "model");
    EXPECT_EQ(GraphEdgeTimeCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(GraphCheck, RefusesAFilterUsingAnotherCutoff) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "filter future edges",
                   json{{"filter_time_column", "time"},
                        {"prediction_time", 11.0},
                        {"exclude_after_prediction_time", true}},
                   "filter");
    graph.add_node(StepKind::Model, "graph model",
                   json{{"model_kind", "graph"},
                        {"time_column", "time"},
                        {"prediction_time", 10.0}},
                   "model");
    graph.add_edge("filter", "model");
    EXPECT_EQ(GraphEdgeTimeCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(GraphCheck, RefusesADisconnectedEdgeTimeFilter) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Clean, "unrelated filter",
                   json{{"filter_time_column", "time"},
                        {"prediction_time", 10.0},
                        {"exclude_after_prediction_time", true}},
                   "filter");
    graph.add_node(StepKind::Model, "graph model",
                   json{{"model_kind", "graph"},
                        {"time_column", "time"},
                        {"prediction_time", 10.0}},
                   "model");
    EXPECT_EQ(GraphEdgeTimeCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(VisionSourceSplitCheck, AcceptsANamedSourceDisjointHoldout) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "vision",
                   {{"model_kind", "vision"},
                    {"source_disjoint", true},
                    {"source_column", "camera_id"}});
    const CheckResult result = VisionSourceSplitCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(VisionSourceSplitCheck, RefusesFrameLevelSplits) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "vision",
                   {{"model_kind", "vision"},
                    {"source_disjoint", false},
                    {"source_column", "camera_id"}});
    const CheckResult result = VisionSourceSplitCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("frames"), std::string::npos) << result.reason;
}

TEST(VisionSourceSplitCheck, RefusesAnUnnamedSourceGroup) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "vision",
                   {{"model_kind", "computer_vision"},
                    {"source_disjoint", true}});
    const CheckResult result = VisionSourceSplitCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("source_column"), std::string::npos)
        << result.reason;
}

TEST(LanguageGroundingGraphCheck, RefusesAnswersWithoutCitationPolicy) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "language",
                   {{"model_kind", "language"},
                    {"refuse_without_retrieval", true}},
                   "language");
    const CheckResult result = LanguageGroundingCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("citations"), std::string::npos);
}

TEST(LanguageGroundingGraphCheck, RefusesAnswersWithoutEmptyRetrievalPolicy) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "language",
                   {{"model_kind", "language"},
                    {"citations_required", true}},
                   "language");
    const CheckResult result = LanguageGroundingCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("refuse"), std::string::npos);
}

TEST(LanguageTrainingAccessGraphCheck, RefusesUncheckedTrainingText) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "text",
                   {{"caller_read_required", true}}, "text");
    graph.add_node(StepKind::Model, "language",
                   {{"model_kind", "language"},
                    {"citations_required", true},
                    {"refuse_without_retrieval", true}},
                   "language");
    graph.add_edge("text", "language");
    const CheckResult result = LanguageTrainingAccessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("authorize"), std::string::npos);
}

TEST(LanguageTrainingAccessGraphCheck, RefusesUnguardedTextSource) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "text", {}, "text");
    graph.add_node(StepKind::Model, "language",
                   {{"model_kind", "language"},
                    {"governance_check", true},
                    {"citations_required", true},
                    {"refuse_without_retrieval", true}},
                   "language");
    graph.add_edge("text", "language");
    const CheckResult result = LanguageTrainingAccessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("caller-read"), std::string::npos);
}
// ---- bandit_policy ----------------------------------------------------------

TEST(BanditPolicyCheck, RefusesMissingExplorationFloor) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "policy",
                   json{{"model_kind", "bandit"},
                        {"default_arm_cap", 1.0}},
                   "policy");
    const CheckResult result = BanditPolicyCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("exploration_floor"), std::string::npos);
}

TEST(BanditPolicyCheck, RefusesAnArmCapBelowTheExplorationFloor) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "policy",
                   json{{"model_kind", "bandit"},
                        {"arms", {"control", "guardrail"}},
                        {"exploration_floor", 0.1},
                        {"default_arm_cap", 1.0},
                        {"arm_caps", {{"guardrail", 0.05}}}},
                   "policy");
    const CheckResult result = BanditPolicyCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("guardrail"), std::string::npos);
}

TEST(BanditPolicyCheck, RefusesCapsThatCannotCarryTotalProbability) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "policy",
                   json{{"model_kind", "bandit"},
                        {"arms", {"a", "b"}},
                        {"exploration_floor", 0.1},
                        {"default_arm_cap", 0.4},
                        {"arm_caps", json::object()}},
                   "policy");
    const CheckResult result = BanditPolicyCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("total probability"), std::string::npos);
}

TEST(BanditPolicyCheck, AcceptsFeasibleFloorsAndCapsForEveryArm) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "policy",
                   json{{"model_kind", "bandit"},
                        {"arms", {"control", "best", "guardrail"}},
                        {"exploration_floor", 0.1},
                        {"default_arm_cap", 0.8},
                        {"arm_caps", {{"guardrail", 0.2}}}},
                   "policy");
    EXPECT_EQ(BanditPolicyCheck{}.evaluate(graph).status, CheckStatus::Pass);
}
// ---- end to end through the gate (RULE-1) ----------------------------------

TEST(GraphChecksThroughGate, DefaultGateShipsTheConcreteChecks) {
    const CorrectnessGate gate = zetta::pipeline::default_gate();
    ASSERT_EQ(gate.check_count(), 29u);
    EXPECT_EQ(gate.checks()[0]->name(), "non_empty_graph");
    EXPECT_EQ(gate.checks()[1]->name(), "schema_type");
    EXPECT_EQ(gate.checks()[2]->name(), "data_leakage");
    EXPECT_EQ(gate.checks()[3]->name(), "snapshot_lineage");
    EXPECT_EQ(gate.checks()[4]->name(), "temporal_validation");
    EXPECT_EQ(gate.checks()[5]->name(), "entity_resolution_profile");
    EXPECT_EQ(gate.checks()[6]->name(), "segmentation");
    EXPECT_EQ(gate.checks()[7]->name(), "forecast_correctness");
    EXPECT_EQ(gate.checks()[8]->name(), "survival_censoring");
    EXPECT_EQ(gate.checks()[9]->name(), "causal_uplift");
    EXPECT_EQ(gate.checks()[10]->name(), "anomaly_correctness");
    EXPECT_EQ(gate.checks()[11]->name(), "next_best_action_correctness");
    EXPECT_EQ(gate.checks()[12]->name(), "rank_correctness");
    EXPECT_EQ(gate.checks()[13]->name(), "language_grounding");
    EXPECT_EQ(gate.checks()[14]->name(), "language_training_access");
    EXPECT_EQ(gate.checks()[15]->name(), "document_review");
    EXPECT_EQ(gate.checks()[16]->name(), "graph_edge_time");
    EXPECT_EQ(gate.checks()[17]->name(), "vision_source_split");
    EXPECT_EQ(gate.checks()[18]->name(), "speech_consent");
    EXPECT_EQ(gate.checks()[19]->name(), "speech_pii_redaction");
    EXPECT_EQ(gate.checks()[20]->name(), "speech_local_audio");
    EXPECT_EQ(gate.checks()[21]->name(), "bandit_policy");
    EXPECT_EQ(gate.checks()[22]->name(), "optimization_contract");
    EXPECT_EQ(gate.checks()[23]->name(), "uncertainty_validation");
    EXPECT_EQ(gate.checks()[24]->name(), "ensemble_out_of_fold");
    EXPECT_EQ(gate.checks()[25]->name(), "label_efficiency_gold_isolation");
    EXPECT_EQ(gate.checks()[26]->name(), "privacy_budget");
    EXPECT_EQ(gate.checks()[27]->name(), "streaming_correctness");
    EXPECT_EQ(gate.checks()[28]->name(), "entity_resolution");
}

TEST(RankGraphCheck, CorrectRankGraphPasses) {
    const CheckResult result =
        zetta::pipeline::RankCorrectnessCheck{}.evaluate(rank_graph());
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(RankGraphCheck, MissingGroupColumnFails) {
    ExecutionGraph graph = rank_graph();
    graph.find_node("rank-model")->attributes.erase("group_column");
    const CheckResult result =
        zetta::pipeline::RankCorrectnessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("group_column"), std::string::npos);
}

TEST(RankGraphCheck, RowSplitFails) {
    ExecutionGraph graph = rank_graph();
    graph.find_node("rank-model")->attributes["split_strategy"] = "row";
    const CheckResult result =
        zetta::pipeline::RankCorrectnessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("split_strategy"), std::string::npos);
}

TEST(RankGraphCheck, FeaturesUnavailableAtRankingTimeFail) {
    ExecutionGraph graph = rank_graph();
    graph.find_node("rank-model")->attributes["feature_availability"] =
        "after_outcome";
    const CheckResult result =
        zetta::pipeline::RankCorrectnessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("ranking_time"), std::string::npos);
}

namespace {

ExecutionGraph document_graph(nlohmann::json attributes) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "documents", {}, "documents");
    graph.add_node(StepKind::Model, "extract", std::move(attributes),
                   "extract");
    graph.add_node(StepKind::Act, "review", json{{"route", "review_queue"}},
                   "review");
    graph.add_edge("documents", "extract");
    graph.add_edge("extract", "review");
    return graph;
}

json valid_document_attributes() {
    return {{"model_kind", "document_extraction"},
            {"confidence_required", true},
            {"retain_evidence", true},
            {"route_low_confidence_to_review", true},
            {"review_threshold", 0.8}};
}

}  // namespace

TEST(DocumentReviewCheck, RefusesFieldsWithoutConfidence) {
    json attributes = valid_document_attributes();
    attributes["confidence_required"] = false;
    const CheckResult result =
        zetta::pipeline::DocumentReviewCheck{}.evaluate(
            document_graph(std::move(attributes)));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("confidence"), std::string::npos);
}

TEST(DocumentReviewCheck, RefusesMistypedConfidenceRuleWithoutThrowing) {
    json attributes = valid_document_attributes();
    attributes["confidence_required"] = "yes";
    const CheckResult result =
        zetta::pipeline::DocumentReviewCheck{}.evaluate(
            document_graph(std::move(attributes)));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("confidence"), std::string::npos);
}

TEST(DocumentReviewCheck, RefusesFieldsWithoutSourceEvidence) {
    json attributes = valid_document_attributes();
    attributes["retain_evidence"] = false;
    const CheckResult result =
        zetta::pipeline::DocumentReviewCheck{}.evaluate(
            document_graph(std::move(attributes)));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("page and box"), std::string::npos);
}

TEST(DocumentReviewCheck, RefusesSilentLowConfidenceAcceptance) {
    json attributes = valid_document_attributes();
    attributes["route_low_confidence_to_review"] = false;
    const CheckResult result =
        zetta::pipeline::DocumentReviewCheck{}.evaluate(
            document_graph(std::move(attributes)));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("routed to review"), std::string::npos);
}

TEST(DocumentReviewCheck, RefusesMissingReviewQueueStep) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "extract", valid_document_attributes(),
                   "extract");
    const CheckResult result =
        zetta::pipeline::DocumentReviewCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("review_queue"), std::string::npos);
}

TEST(DocumentReviewCheck, RefusesInvalidReviewThreshold) {
    json attributes = valid_document_attributes();
    attributes["review_threshold"] = 1.1;
    const CheckResult result =
        zetta::pipeline::DocumentReviewCheck{}.evaluate(
            document_graph(std::move(attributes)));
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("between 0 and 1"), std::string::npos);
}

TEST(SpeechGraphCheck, RefusesMissingConsentContract) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "speech",
                   json{{"model_kind", "speech"},
                        {"redact_pii", true},
                        {"audio_local_only", true}});
    EXPECT_EQ(SpeechConsentCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(SpeechGraphCheck, RefusesDisabledPiiRedaction) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "speech",
                   json{{"model_kind", "speech"},
                        {"consent_required", true},
                        {"consent_column", "consent"},
                        {"redact_pii", false},
                        {"audio_local_only", true}});
    EXPECT_EQ(SpeechPiiRedactionCheck{}.evaluate(graph).status,
              CheckStatus::Fail);
}

TEST(SpeechGraphCheck, RefusesNonLocalAudio) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "speech",
                   json{{"model_kind", "speech"},
                        {"consent_required", true},
                        {"consent_column", "consent"},
                        {"redact_pii", true},
                        {"audio_local_only", false}});
    EXPECT_EQ(SpeechLocalAudioCheck{}.evaluate(graph).status, CheckStatus::Fail);
}

TEST(LabelEfficiencyGoldIsolationCheck, RefusesWeakLabelsAsGold) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "label model",
        json{{"model_kind", "label_efficiency"},
             {"weak_labels_are_gold", true},
             {"gold_split", "disjoint"}},
        "label-model");

    const CheckResult result =
        zetta::pipeline::LabelEfficiencyGoldIsolationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("never be treated as gold"),
              std::string::npos);
}

TEST(LabelEfficiencyGoldIsolationCheck, RefusesOverlappingGoldSplit) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "label model",
        json{{"model_kind", "weak_supervision"},
             {"weak_labels_are_gold", false},
             {"gold_split", "overlapping"}},
        "label-model");

    const CheckResult result =
        zetta::pipeline::LabelEfficiencyGoldIsolationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("must be disjoint"), std::string::npos);
}

TEST(LabelEfficiencyGoldIsolationCheck, AcceptsDisjointGoldSplit) {
    ExecutionGraph graph;
    graph.add_node(
        StepKind::Model, "label model",
        json{{"model_kind", "active_learning"},
             {"weak_labels_are_gold", false},
             {"gold_split", "disjoint"}},
        "label-model");

    const CheckResult result =
        zetta::pipeline::LabelEfficiencyGoldIsolationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Pass);
}

TEST(PrivacyBudgetGraphCheck, RefusesAMissingBudget) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "model",
                   {{"model_kind", "synthetic_private"}}, "model");
    const CheckResult result =
        zetta::pipeline::PrivacyBudgetCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("epsilon"), std::string::npos);
}

TEST(PrivacyBudgetGraphCheck, RefusesABudgetAboveItsMaximum) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "model",
        {{"model_kind", "synthetic_private"},
         {"epsilon", 9.0},
         {"delta", 1e-6},
         {"maximum_epsilon", 8.0},
         {"maximum_delta", 1e-5},
         {"noise", "gaussian"},
         {"clip_min", 0.0},
         {"clip_max", 1.0},
         {"privacy_evaluation", true}}, "model");
    EXPECT_EQ(zetta::pipeline::PrivacyBudgetCheck{}.evaluate(graph).status,
              CheckStatus::Fail);
}

TEST(PrivacyBudgetGraphCheck, RefusesPublishingWithoutPrivacyEvaluation) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Model, "model",
        {{"model_kind", "synthetic_private"},
         {"epsilon", 4.0},
         {"delta", 1e-6},
         {"maximum_epsilon", 8.0},
         {"maximum_delta", 1e-5},
         {"noise", "gaussian"},
         {"clip_min", 0.0},
         {"clip_max", 1.0},
         {"privacy_evaluation", false}}, "model");
    EXPECT_EQ(zetta::pipeline::PrivacyBudgetCheck{}.evaluate(graph).status,
              CheckStatus::Fail);
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

TEST(MethodologyCheck, AnUnknownModelKindFailsAndSaysWhatToDo) {
    zetta::ir::ExecutionGraph graph;
    const auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    const auto model = graph.add_node(zetta::ir::StepKind::Model, "model",
                                      nlohmann::json{{"model_kind", "unregistered_model_kind"}});
    graph.add_edge(connect->id, model->id);

    const zetta::pipeline::CheckResult result =
        zetta::pipeline::MethodologyCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("unregistered_model_kind"),
              std::string::npos)
        << "the reason must name the methodology that was refused";
    EXPECT_NE(result.reason.find("Implemented today:"), std::string::npos);
    EXPECT_NE(result.reason.find("#984"), std::string::npos);
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
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "uplift"),
              implemented.end()) << "M-6 causal uplift ships today";
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "survival"),
              implemented.end())
        << "M-5 survival ships today";
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "graph"),
              implemented.end()) << "M-12 graph ML ships today";
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "optimization"),
              implemented.end())
        << "M-16 optimization ships today";
    for (const char* shipped : {"speech", "audio", "bandit"}) {
        EXPECT_NE(std::find(implemented.begin(), implemented.end(), shipped),
                  implemented.end()) << shipped;
    }
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "bayesian"),
              implemented.end())
        << "M-17 Bayesian uncertainty ships today";
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "ensemble"),
              implemented.end())
        << "M-18 out-of-fold stacking ships today";
    EXPECT_NE(std::find(implemented.begin(), implemented.end(),
                        "label_efficiency"),
              implemented.end())
        << "M-19 label efficiency ships today";
    for (const zetta::ml::Methodology& methodology : zetta::ml::methodologies()) {
        EXPECT_TRUE(methodology.runnable) << methodology.id;
        for (const std::string& kind : methodology.model_kinds) {
            EXPECT_NE(std::find(implemented.begin(), implemented.end(), kind),
                      implemented.end()) << methodology.id << ": " << kind;
        }
    }
    EXPECT_EQ(std::find(implemented.begin(), implemented.end(), "unregistered_model_kind"),
              implemented.end());
}

namespace {

zetta::ir::ExecutionGraph optimization_graph(nlohmann::json attributes) {
  zetta::ir::ExecutionGraph graph;
  graph.add_node(zetta::ir::StepKind::Connect, "scenario table", {}, "source");
  graph.add_node(zetta::ir::StepKind::Model, "optimization",
                 std::move(attributes), "model");
  graph.add_edge("source", "model");
  return graph;
}

// --- UncertaintyValidationCheck (#1056, M-17) -------------------------------

zetta::ir::ExecutionGraph uncertainty_graph(nlohmann::json attributes) {
    zetta::ir::ExecutionGraph graph;
    const auto connect = graph.add_node(zetta::ir::StepKind::Connect, "source");
    const auto model = graph.add_node(zetta::ir::StepKind::Model, "model",
                                      std::move(attributes));
    graph.add_edge(connect->id, model->id);
    return graph;
}

} // namespace

TEST(OptimizationGraphCheck, CompleteContractPasses) {
  const auto graph = optimization_graph({{"model_kind", "optimization"},
                                         {"scenario_input", true},
                                         {"verify_constraints", true},
                                         {"report_solver_status", true}});
  const auto result =
      zetta::pipeline::OptimizationContractCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Pass) << result.reason;
}

TEST(OptimizationGraphCheck, PointForecastShapeIsRefused) {
  const auto graph = optimization_graph({{"model_kind", "optimization"},
                                         {"verify_constraints", true},
                                         {"report_solver_status", true}});
  const auto result =
      zetta::pipeline::OptimizationContractCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
  EXPECT_NE(result.reason.find("scenario or quantile"), std::string::npos)
      << result.reason;
}

TEST(OptimizationGraphCheck, UnverifiedPlanIsRefused) {
  const auto graph = optimization_graph({{"model_kind", "optimization"},
                                         {"scenario_input", true},
                                         {"report_solver_status", true}});
  const auto result =
      zetta::pipeline::OptimizationContractCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
  EXPECT_NE(result.reason.find("independently verify"), std::string::npos)
      << result.reason;
}

TEST(OptimizationGraphCheck, MissingSolverStatusIsRefused) {
  const auto graph = optimization_graph({{"model_kind", "optimization"},
                                         {"scenario_input", true},
                                         {"verify_constraints", true}});
  const auto result =
      zetta::pipeline::OptimizationContractCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
  EXPECT_NE(result.reason.find("solver status"), std::string::npos)
      << result.reason;
}

TEST(UncertaintyValidationCheck, MeasuredCoverageAndAssumptionPass) {
    const auto graph = uncertainty_graph(
        {{"model_kind", "bayesian"},
         {"uncertainty_kind", "hierarchical"},
         {"nominal_coverage", 0.9},
         {"coverage_checked", true},
         {"exchangeability_assumption", "rows are exchangeable within group"}});
    const auto result =
        zetta::pipeline::UncertaintyValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Pass)
        << result.reason;
}

TEST(UncertaintyValidationCheck, UnmeasuredCoverageFails) {
    const auto graph = uncertainty_graph(
        {{"model_kind", "bayesian"},
         {"uncertainty_kind", "hierarchical"},
         {"nominal_coverage", 0.9},
         {"exchangeability_assumption", "rows are exchangeable within group"}});
    const auto result =
        zetta::pipeline::UncertaintyValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("coverage_checked"), std::string::npos);
}

TEST(UncertaintyValidationCheck, MissingExchangeabilityStatementFails) {
    const auto graph = uncertainty_graph(
        {{"model_kind", "bayesian"},
         {"uncertainty_kind", "hierarchical"},
         {"nominal_coverage", 0.9},
         {"coverage_checked", true}});
    const auto result =
        zetta::pipeline::UncertaintyValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("exchangeability"), std::string::npos);
}

TEST(UncertaintyValidationCheck, OverlappingConformalCalibrationFails) {
    const auto graph = uncertainty_graph(
        {{"model_kind", "bayesian"},
         {"uncertainty_kind", "split_conformal_regression"},
         {"nominal_coverage", 0.9},
         {"coverage_checked", true},
         {"exchangeability_assumption", "rows are exchangeable"},
         {"calibration_disjoint", false}});
    const auto result =
        zetta::pipeline::UncertaintyValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("disjoint"), std::string::npos);
}

TEST(UncertaintyValidationCheck, UnknownUncertaintyShapeFailsForAnAlias) {
    const auto graph = uncertainty_graph(
        {{"model_kind", "probabilistic"},
         {"uncertainty_kind", "unmeasured_guess"},
         {"nominal_coverage", 0.9},
         {"coverage_checked", true},
         {"exchangeability_assumption", "rows are exchangeable"}});
    const auto result =
        zetta::pipeline::UncertaintyValidationCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("uncertainty_kind"), std::string::npos);
}

// --- EnsembleOutOfFoldCheck (#1057) -----------------------------------------

namespace {

ExecutionGraph ensemble_graph(nlohmann::json attributes) {
    ExecutionGraph graph;
    const auto source = graph.add_node(StepKind::Connect, "source");
    const auto model =
        graph.add_node(StepKind::Model, "ensemble", std::move(attributes));
    graph.add_edge(source->id, model->id);
    return graph;
}

}  // namespace

TEST(EnsembleOutOfFoldCheck, CorrectProfilePasses) {
    const auto graph = ensemble_graph(
        {{"model_kind", "ensemble"},
         {"stacking_predictions", "out_of_fold"},
         {"folds", 5},
         {"base_refit", "all_training"},
         {"time_column", "observed_at"},
         {"fold_strategy", "time_ordered"}});
    const CheckResult result = EnsembleOutOfFoldCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, CheckStatus::Pass) << result.reason;
}

TEST(EnsembleOutOfFoldCheck, InSampleStackingFails) {
    const auto graph = ensemble_graph(
        {{"model_kind", "ensemble"},
         {"stacking_predictions", "in_sample"},
         {"folds", 5},
         {"base_refit", "all_training"}});
    const CheckResult result = EnsembleOutOfFoldCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("out_of_fold"), std::string::npos);
}

TEST(EnsembleOutOfFoldCheck, TooFewFoldsFail) {
    const auto graph = ensemble_graph(
        {{"model_kind", "ensemble"},
         {"stacking_predictions", "out_of_fold"},
         {"folds", 1},
         {"base_refit", "all_training"}});
    const CheckResult result = EnsembleOutOfFoldCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("at least 2"), std::string::npos);
}

TEST(EnsembleOutOfFoldCheck, TimeDataWithContentFoldsFails) {
    const auto graph = ensemble_graph(
        {{"model_kind", "ensemble"},
         {"stacking_predictions", "out_of_fold"},
         {"folds", 5},
         {"base_refit", "all_training"},
         {"time_column", "observed_at"},
         {"fold_strategy", "entity_hash"}});
    const CheckResult result = EnsembleOutOfFoldCheck{}.evaluate(graph);
    ASSERT_EQ(result.status, CheckStatus::Fail);
    EXPECT_NE(result.reason.find("time_ordered"), std::string::npos);
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

TEST(StreamingGraphCheck, CorrectEventTimeAndPrequentialPolicyPasses) {
    const auto graph = model_graph(
        {{"model_kind", "streaming"},
         {"event_time_ordered", true},
         {"allowed_lateness", 5.0},
         {"future_event_policy", "buffer_until_watermark"},
         {"label_time_policy", "not_before_event_time"},
         {"prequential", true}});
    const auto result =
        zetta::pipeline::StreamingCorrectnessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Pass)
        << result.reason;
}

TEST(StreamingGraphCheck, RefusesAStreamingModelWithoutEventTimeOrdering) {
    const auto graph = model_graph(
        {{"model_kind", "streaming"},
         {"allowed_lateness", 5.0},
         {"future_event_policy", "buffer_until_watermark"},
         {"label_time_policy", "not_before_event_time"},
         {"prequential", true}});
    const auto result =
        zetta::pipeline::StreamingCorrectnessCheck{}.evaluate(graph);
    EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(result.reason.find("event_time_ordered"), std::string::npos);
}

TEST(StreamingGraphCheck, RefusesEarlyLabelsAndTrainThenTestMetrics) {
    const auto early_label = model_graph(
        {{"model_kind", "streaming"},
         {"event_time_ordered", true},
         {"allowed_lateness", 5.0},
         {"future_event_policy", "buffer_until_watermark"},
         {"label_time_policy", "use_on_arrival"},
         {"prequential", true}});
    const auto early_result =
        zetta::pipeline::StreamingCorrectnessCheck{}.evaluate(early_label);
    EXPECT_EQ(early_result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(early_result.reason.find("label_time_policy"), std::string::npos);

    const auto train_then_test = model_graph(
        {{"model_kind", "streaming"},
         {"event_time_ordered", true},
         {"allowed_lateness", 5.0},
         {"future_event_policy", "buffer_until_watermark"},
         {"label_time_policy", "not_before_event_time"},
         {"prequential", false}});
    const auto metric_result =
        zetta::pipeline::StreamingCorrectnessCheck{}.evaluate(train_then_test);
    EXPECT_EQ(metric_result.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(metric_result.reason.find("prequential"), std::string::npos);
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

// Availability and temporal checks accept M-4. The default gate additionally
// validates its complete executable shape through ForecastGraphCheck.
TEST(MethodologyEndToEnd,
     AForecastHasARegistryRunPathAndStillRequiresTemporalValidation) {
    zetta::agent::EndState facts;
    facts.set("connect", true);
    facts.set("model", true);
    facts.set("serve", true);
    facts.set("model_kind", std::string("forecast"));
    facts.set("temporal_validation", true);

    const auto graph = zetta::agent::build_solution_graph(facts);
    EXPECT_FALSE(gate_over(graph).block_deploy);

    // And the profile is still enforced: the same methodology WITHOUT temporal
    // validation, or WITH calibration, is refused.
    zetta::agent::EndState uncalibrated_but_unvalidated;
    uncalibrated_but_unvalidated.set("connect", true);
    uncalibrated_but_unvalidated.set("model", true);
    uncalibrated_but_unvalidated.set("serve", true);
    uncalibrated_but_unvalidated.set("model_kind", std::string("forecast"));
    EXPECT_TRUE(gate_over(zetta::agent::build_solution_graph(
                              uncalibrated_but_unvalidated))
                    .block_deploy)
        << "shipping the run path must not relax the correctness profile";
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

// --- #983 acceptance criteria, demonstrated -------------------------------------------------
//
// The epic lists six behavioural criteria. Four are decidable at the engine level and are
// demonstrated here; two (the three-turn walk, and an agent CALLING these tools rather than
// describing them) need a trained checkpoint and a live node, and are not claimed by this test.
//
// Naming that split matters: a green engine test is not evidence the agent does the right thing,
// and this epic has already had one acceptance test pass a model that was differently wrong each
// turn.

// Criterion 1: a forecasting ask produces the forecasting pipeline; a churn ask still produces
// the calibrated one. Before this epic both produced byte-identical graphs.
TEST(EpicAcceptance, ForecastAndChurnProduceDifferentVerdicts) {
    const auto build = [](const std::string& model_kind, bool calibrated, bool temporal) {
        zetta::agent::EndState facts;
        facts.set("connect", true);
        facts.set("clean", true);
        facts.set("model", true);
        facts.set("serve", true);
        facts.set("model_kind", model_kind);
        if (calibrated) facts.set("calibrated", true);
        if (temporal) facts.set("temporal_validation", true);
        return zetta::agent::build_solution_graph(facts);
    };

    const auto churn = build("gbdt", true, false);
    const auto forecast = build("forecast", false, true);

    EXPECT_FALSE(gate_over(churn).block_deploy) << "churn calibrates and is accepted";
    EXPECT_FALSE(gate_over(forecast).block_deploy)
        << "M-4 has a registry runner and validates temporally";

    // And they are not interchangeable — the neighbouring shape under the other name is refused.
    EXPECT_TRUE(gate_over(build("forecast", true, true)).block_deploy)
        << "the churn recipe under a forecasting name must be blocked";
    EXPECT_TRUE(gate_over(build("forecast", false, false)).block_deploy)
        << "a forecast that is not validated over time must be blocked";
}

// Criterion 6: the corpus invariants still pass. Asserted by CorpusInvariants.* running green;
// recorded here so the epic's acceptance is readable in one place rather than inferred from a
// suite name.
TEST(EpicAcceptance, TheImplementedMethodologySetIsHonest) {
    const std::vector<std::string>& implemented =
        zetta::pipeline::MethodologyCheck::implemented();
    // What ships, ships. What does not, is refused rather than silently run as something else.
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "gbdt"), implemented.end());
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "uplift"),
              implemented.end());
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "survival"),
              implemented.end());
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "graph"), implemented.end());
    EXPECT_NE(std::find(implemented.begin(), implemented.end(), "bandit"),
              implemented.end());
    for (const char* shipped : {"speech", "audio", "optimization"}) {
        EXPECT_NE(std::find(implemented.begin(), implemented.end(), shipped),
                  implemented.end()) << shipped;
    }
    EXPECT_NE(std::find(implemented.begin(), implemented.end(),
                        "label_efficiency"),
              implemented.end());
    for (const zetta::ml::Methodology& methodology : zetta::ml::methodologies()) {
        EXPECT_TRUE(methodology.runnable) << methodology.id;
        for (const std::string& kind : methodology.model_kinds) {
            EXPECT_NE(std::find(implemented.begin(), implemented.end(), kind),
                      implemented.end()) << methodology.id << ": " << kind;
        }
    }
    EXPECT_EQ(std::find(implemented.begin(), implemented.end(), "unregistered_model_kind"),
              implemented.end());
}
