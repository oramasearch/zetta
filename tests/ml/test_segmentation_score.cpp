// #1497: a segmentation model's weights read back, a row encoded the way
// training encoded it, and placed in its nearest segment. Pure; the API path
// is tests/api/test_segmentation_score.cpp. docs/ml/segmentation.md.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/score_table.hpp"
#include "zetta/ml/explain_row.hpp"
#include "zetta/ml/segmentation.hpp"
#include "zetta/ml/segmentation_score.hpp"
#include "zetta/node.hpp"

namespace {

using Json = nlohmann::json;
using zetta::ml::SegmentationParams;
using zetta::ml::SegmentationRow;

// Two segments far apart on spend and visits, each with its own plan.
std::vector<SegmentationRow> two_groups() {
    std::vector<SegmentationRow> rows;
    std::mt19937_64 generator(1497);
    std::normal_distribution<double> noise(0.0, 0.2);
    for (std::int64_t index = 0; index < 60; ++index) {
        const bool high = index >= 30;
        rows.push_back({.entity = index + 1,
                        .values = {{"spend", (high ? 90.0 : 10.0) + noise(generator)},
                                   {"visits", (high ? 2.0 : 20.0) + noise(generator)},
                                   {"plan", high ? "pro" : "free"}}});
    }
    return rows;
}

SegmentationParams two_group_params() {
    SegmentationParams params;
    params.minimum_k = 2;
    params.maximum_k = 3;
    params.restarts = 3;
    params.seed = 1497;
    params.feature_columns = {"spend", "visits", "plan"};
    params.categorical_columns = {"plan"};
    return params;
}

struct Registered {
    zetta::test::TempDir dir;
    zetta::Node node;
    std::shared_ptr<zetta::ml::Model> model;

    explicit Registered(const std::string& body) : node(config(dir)) {
        node.start();
        zetta::ml::ModelValue value;
        value.kind = "segmentation";
        model = node.create_model(value);
        if (model) model->set_weights(body);
    }
    ~Registered() { node.stop(); }

    static zetta::NodeConfig config(const zetta::test::TempDir& directory) {
        zetta::NodeConfig out;
        out.data_dir = directory.path().string();
        return out;
    }
};

TEST(SegmentationScore, WeightsRoundTripAndRecordEachSegmentsLabel) {
    const auto trained = zetta::ml::train_segmentation(two_groups(), two_group_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    ASSERT_EQ(trained.model.labels.size(), trained.model.centers.size());
    for (std::size_t index = 0; index < trained.profiles.size(); ++index) {
        EXPECT_EQ(trained.model.labels[index], trained.profiles[index].label);
    }
    std::string error;
    const auto loaded =
        zetta::ml::segmentation_model_from_json(trained.model.to_json(), &error);
    ASSERT_TRUE(loaded) << error;
    EXPECT_EQ(loaded->centers, trained.model.centers);
    EXPECT_EQ(loaded->labels, trained.model.labels);
    EXPECT_EQ(zetta::ml::segmentation_source_columns(*loaded),
              (std::vector<std::string>{"spend", "visits", "plan"}));
}

// The rule the score path must keep: every training row lands where training
// put it, at the distance training recorded.
TEST(SegmentationScore, EveryTrainingRowLandsInTheSegmentTrainingWrote) {
    const std::vector<SegmentationRow> rows = two_groups();
    const auto trained = zetta::ml::train_segmentation(rows, two_group_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    EXPECT_EQ(trained.metrics.chosen_k, 2);
    ASSERT_EQ(trained.assignments.size(), rows.size());
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const auto encoded = zetta::ml::encode_segmentation_row(trained.model, rows[index].values);
        ASSERT_TRUE(encoded.ok()) << encoded.error;
        const auto placed = zetta::ml::place_segmentation_point(trained.model, encoded.point);
        EXPECT_EQ(trained.assignments[index].entity, rows[index].entity);
        EXPECT_EQ(placed.segment_id, trained.assignments[index].segment_id) << index;
        EXPECT_NEAR(placed.distance, trained.assignments[index].distance_to_center, 1e-9);
    }
}

TEST(SegmentationScore, RowsScoreThroughTheRegisteredModel) {
    const auto trained = zetta::ml::train_segmentation(two_groups(), two_group_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    Registered registered(trained.model.to_json().dump());
    ASSERT_TRUE(registered.model);

    const std::vector<Json> rows = {{{"spend", 11.0}, {"visits", 19.0}, {"plan", "free"}},
                                    {{"spend", 88.0}, {"visits", 3.0}, {"plan", "pro"}}};
    const auto scored = zetta::ml::score_segmentation_rows(*registered.model, rows);
    ASSERT_TRUE(scored.ok()) << scored.error;
    ASSERT_EQ(scored.assignments.size(), 2u);
    EXPECT_EQ(scored.assignments[0].entity, 0);
    EXPECT_EQ(scored.assignments[1].entity, 1);
    EXPECT_NE(scored.assignments[0].segment_id, scored.assignments[1].segment_id);
    // Same side as a training row of the same group.
    const auto low = zetta::ml::encode_segmentation_row(trained.model, two_groups()[0].values);
    EXPECT_EQ(scored.assignments[0].segment_id,
              zetta::ml::place_segmentation_point(trained.model, low.point).segment_id);
    EXPECT_EQ(scored.labels, trained.model.labels);
    EXPECT_EQ(scored.rows_scored, 2u);

    // The keys a caller gives are kept.
    const auto keyed = zetta::ml::score_segmentation_rows(*registered.model, rows, {41, 42});
    ASSERT_TRUE(keyed.ok()) << keyed.error;
    EXPECT_EQ(keyed.assignments[1].entity, 42);
}

TEST(SegmentationScore, AMissingColumnIsRefusedNamingTheRowAndTheColumns) {
    const auto trained = zetta::ml::train_segmentation(two_groups(), two_group_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    Registered registered(trained.model.to_json().dump());
    const std::vector<Json> rows = {{{"spend", 11.0}, {"visits", 19.0}, {"plan", "free"}},
                                    {{"spend", 88.0}, {"plan", "pro"}}};
    const auto scored = zetta::ml::score_segmentation_rows(*registered.model, rows);
    EXPECT_FALSE(scored.ok());
    EXPECT_EQ(scored.error,
              "score: row 1 has no 'visits'; the model needs spend, visits, plan");
    EXPECT_TRUE(scored.assignments.empty());

    const auto text = zetta::ml::score_segmentation_rows(
        *registered.model, {Json{{"spend", "lots"}, {"visits", 1.0}, {"plan", "pro"}}});
    EXPECT_NE(text.error.find("non-numeric 'spend'"), std::string::npos) << text.error;
}

TEST(SegmentationScore, AnUnseenCategoryTakesTheRareBucket) {
    zetta::ml::SegmentationModel model;
    using Kind = zetta::ml::SegmentationFeatureKind;
    model.features = {{.source = "spend", .encoded_name = "spend", .kind = Kind::Numeric,
                       .location = 10.0, .scale = 2.0},
                      {.source = "plan", .encoded_name = "plan=free", .kind = Kind::Categorical,
                       .level = "free"},
                      {.source = "plan", .encoded_name = "plan=_other",
                       .kind = Kind::Categorical, .level = "_other", .rare_bucket = true}};
    model.centers = {{0.0, 1.0, 0.0}, {5.0, 0.0, 1.0}};
    const auto known = zetta::ml::encode_segmentation_row(model, {{"spend", 14.0}, {"plan", "free"}});
    ASSERT_TRUE(known.ok());
    EXPECT_EQ(known.point, (std::vector<double>{2.0, 1.0, 0.0}));
    const auto unseen =
        zetta::ml::encode_segmentation_row(model, {{"spend", 10.0}, {"plan", "enterprise"}});
    ASSERT_TRUE(unseen.ok());
    EXPECT_EQ(unseen.point, (std::vector<double>{0.0, 0.0, 1.0}));
    // A literal "_other" is not a fitted level here, so it is rare too.
    const auto literal =
        zetta::ml::encode_segmentation_row(model, {{"spend", 10.0}, {"plan", "_other"}});
    EXPECT_EQ(literal.point, (std::vector<double>{0.0, 0.0, 1.0}));
    EXPECT_FALSE(zetta::ml::encode_segmentation_row(model, {{"spend", 10.0}, {"plan", 3}}).ok());
}

TEST(SegmentationScore, ExplainNamesWhatPlacesTheRowAndAddsUp) {
    const auto trained = zetta::ml::train_segmentation(two_groups(), two_group_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    Registered registered(trained.model.to_json().dump());
    const Json row = {{"spend", 12.0}, {"visits", 18.0}, {"plan", "free"}};
    const auto explained = zetta::ml::explain_segmentation_row(*registered.model, row);
    ASSERT_TRUE(explained.ok()) << explained.error;
    ASSERT_TRUE(explained.runner_up_segment_id);
    EXPECT_NE(*explained.runner_up_segment_id, explained.segment_id);
    EXPECT_GT(explained.runner_up_distance, explained.distance);
    ASSERT_EQ(explained.contributions.size(), 3u);
    double sum = 0.0;
    for (const auto& contribution : explained.contributions) sum += contribution.contribution;
    EXPECT_NEAR(sum, explained.runner_up_distance * explained.runner_up_distance -
                         explained.distance * explained.distance,
                1e-9);
    EXPECT_NEAR(explained.residual, 0.0, 1e-9);
    EXPECT_GE(std::abs(explained.contributions[0].contribution),
              std::abs(explained.contributions[1].contribution));
    EXPECT_EQ(explained.label, trained.model.labels[static_cast<std::size_t>(explained.segment_id)]);

    const auto missing = zetta::ml::explain_segmentation_row(*registered.model, {{"spend", 1.0}});
    EXPECT_EQ(missing.error,
              "explain: the row has no 'visits'; the model needs spend, visits, plan");
}

// The defect seen from lake (lake#1243): the GBDT path read a segmentation
// body as trees and said "required property 'objective' not found".
TEST(SegmentationScore, TheTreePathsSayItIsASegmentationModelNotASchemaError) {
    const auto trained = zetta::ml::train_segmentation(two_groups(), two_group_params());
    ASSERT_TRUE(trained.ok()) << trained.status.message;
    Registered registered(trained.model.to_json().dump());
    const Json row = {{"spend", 12.0}, {"visits", 18.0}, {"plan", "free"}};
    const auto scored = zetta::ml::score_rows(*registered.model, {row}, {}, 0);
    EXPECT_NE(scored.error.find("is a segmentation model"), std::string::npos) << scored.error;
    EXPECT_EQ(scored.error.find("objective"), std::string::npos);
    const auto explained = zetta::ml::explain_row(*registered.model, row);
    EXPECT_NE(explained.error.find("is a segmentation model"), std::string::npos);
}

TEST(SegmentationScore, MalformedWeightsAreRefusedInWords) {
    std::string error;
    EXPECT_FALSE(zetta::ml::segmentation_model_from_json(Json{{"objective", "binary"}}, &error));
    EXPECT_EQ(error, "the weights are not a segmentation model's");
    EXPECT_FALSE(zetta::ml::segmentation_model_from_json(
        Json{{"kind", "segmentation"},
             {"features", Json::array({{{"source", "spend"}, {"kind", "numeric"}}})},
             {"centers", Json::array({Json::array({1.0, 2.0})})}},
        &error));
    EXPECT_EQ(error, "a segmentation centre does not have one value per feature");
}

}  // namespace
