#include <gtest/gtest.h>

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/segmentation_methodology.hpp"
#include "zetta/pipeline/graph_checks.hpp"
#include "zetta/pipeline/intent_compiler.hpp"

namespace {
using Json = nlohmann::json;

zetta::ir::ExecutionGraph shape(Json hints = Json::object()) {
    zetta::ml::ShapeRequest request;
    request.hints = std::move(hints);
    return zetta::ml::find_methodology("M-3")->build_shape(request);
}

void refuses(Json hints, const std::string &reason) {
    const auto graph = shape(std::move(hints));
    const auto checked = zetta::pipeline::SegmentationCheck{}.evaluate(graph);
    EXPECT_EQ(checked.status, zetta::pipeline::CheckStatus::Fail);
    EXPECT_NE(checked.reason.find(reason), std::string::npos) << checked.reason;
    EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}
} // namespace

TEST(SegmentationMethodology,
     ExplicitSolutionAndIntentBuildTheSameOwnPipeline) {
    for (const auto *selector : {"M-3", "S-9", ""}) {
        zetta::pipeline::IntentCompileOptions options;
        if (std::string(selector) == "M-3") {
            options.methodology = selector;
        } else {
            options.solution = selector;
        }
        options.hints = {{"source", "crm"}, {"table", "customers"}};
        std::string error;
        const auto compiled = zetta::pipeline::compile_intent(
            "segment my customers into groups", options, &error);
        ASSERT_TRUE(compiled) << error;
        EXPECT_EQ(compiled->methodology, "M-3");
        EXPECT_EQ(compiled->graph.node_count(), 5u);
        EXPECT_EQ(
            compiled->graph.find_node("load_entities")->attributes.at("source"),
            "crm");
        EXPECT_FALSE(compiled->graph.has_node("train_churn_model"));
        EXPECT_FALSE(
            zetta::pipeline::default_gate().run(compiled->graph).block_deploy);
    }
}

TEST(SegmentationGraphCheck, RefusesLabelAndOutcomeFeatures) {
    for (const auto *column : {"label", "target", "outcome", "OUTCOME"}) {
        refuses({{"feature_columns", {"spend", column}}}, column);
    }
    refuses({{"label_column", "cancelled"},
             {"feature_columns", {"spend", "cancelled"}}},
            "cancelled");
    refuses(
        {{"label_column", "cancelled"}, {"categorical_columns", {"cancelled"}}},
        "cancelled");
    auto graph = shape({{"feature_columns", {"spend", "cancelled"}}});
    graph.find_node("train_segments")->attributes["outcome"] = "cancelled";
    EXPECT_EQ(zetta::pipeline::SegmentationCheck{}.evaluate(graph).status,
              zetta::pipeline::CheckStatus::Fail);
}

TEST(SegmentationGraphCheck, RefusesEntityKeyAndOtherIdentifierFeatures) {
    refuses({{"entity_column", "outcome"}}, "entity key");
    refuses({{"entity_column", "customer"}, {"label_column", "CUSTOMER"}},
            "entity key");
    refuses({{"entity_column", "customer"},
             {"feature_columns", {"customer", "spend"}}},
            "customer");
    refuses({{"feature_columns", {"customer_id", "spend"}}}, "customer_id");
    refuses(
        {{"entity_column", "customer"}, {"categorical_columns", {"customer"}}},
        "customer");
}

TEST(SegmentationGraphCheck,
     RefusesLocalScalingAndDifferentSamplePreprocessing) {
    refuses({{"scaling", "partition_local"}}, "scaling");
    refuses({{"encoding", "unbounded_one_hot"}}, "encoding");
    auto graph = shape();
    graph.find_node("prepare_segments")->attributes["segmentation"]["scaling"] =
        "none";
    EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}

TEST(SegmentationGraphCheck, RefusesUnstatedKSelectionAndWeakenedMinimumShare) {
    refuses({{"selection", "inertia"}}, "selection");
    refuses({{"minimum_segment_share", 0.001}}, "minimum_segment_share");
    refuses({{"minimum_k", 6}, {"maximum_k", 3}}, "maximum_k");
    refuses({{"restarts", 1}}, "restarts");
}

TEST(SegmentationGraphCheck, RefusesMalformedParametersAndImplicitTimeCutoff) {
    refuses({{"seed", "random"}}, "seed");
    refuses({{"feature_columns", "spend"}}, "feature_columns");
    refuses({{"minimum_k", -3}}, "minimum_k");
    refuses({{"time_column", "snapshot"}}, "as_of");
    refuses({{"as_of", 100}}, "time_column");
    refuses({{"center_shift_tolerance", 0}}, "center_shift_tolerance");
}

TEST(SegmentationGraphCheck, AcceptsIsoOrEpochTimeCutoffs) {
    for (const nlohmann::json& cutoff :
         {nlohmann::json("2026-09-01T00:00:00Z"),
          nlohmann::json(1788220800.0)}) {
        const auto graph = shape({{"time_column", "observed_at"},
                                  {"as_of", cutoff}});
        const auto checked =
            zetta::pipeline::SegmentationCheck{}.evaluate(graph);
        EXPECT_EQ(checked.status, zetta::pipeline::CheckStatus::Pass)
            << cutoff.dump() << ": " << checked.reason;
    }
}

TEST(SegmentationGraphCheck,
     RefusesDisconnectedOrMissingOutputsAndEntityClustering) {
    auto graph = shape();
    graph.remove_edge("train_segments", "write_profiles");
    EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
    graph = shape();
    graph.find_node("prepare_segments")->attributes = nullptr;
    EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
    graph = shape();
    graph.remove_node("write_assignments");
    EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
    graph = shape();
    graph.find_node("prepare_segments")->attributes["entity_key"] = "clustered";
    EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}
