// zetta#1390: a model's split feature indices and child indices are
// checked against the model itself before anything scores or explains it.
//
// A model brought from a file (register_model -> import_model) used to be
// accepted with a split reading a feature past the end of its own feature list,
// or with a child pointing back up the tree. Scoring then read past the feature
// row, TreeSHAP wrote past its feature-count-sized output, and a cycle never
// ended. These crafted models must now be refused at every boundary, and the
// scoring and explanation code must stay inside its buffers even for a model
// built in memory without the check: compiled with -fsanitize=address, the
// in-memory cases read and write nothing out of bounds, where the old code read
// past the row in RegressionTree::predict.
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/model_import.hpp"
#include "zetta/ml/tree_shap.hpp"

namespace {

using nlohmann::json;
using zetta::ml::GbdtModel;
using zetta::ml::ModelImportOptions;
using zetta::ml::RegressionTree;
using zetta::ml::TreeNode;

// One split on `feature` over two leaves, with names {"a", "b"}.
GbdtModel one_split(int feature) {
    GbdtModel model;
    model.objective = "squared_error";
    model.base_score = 0.0;
    model.feature_names = {"a", "b"};
    RegressionTree tree;
    tree.nodes = {TreeNode{.feature = feature, .threshold = 0.5, .left = 1, .right = 2, .cover = 2.0},
                  TreeNode{.feature = -1, .value = -1.0, .cover = 1.0},
                  TreeNode{.feature = -1, .value = 1.0, .cover = 1.0}};
    model.trees = {std::move(tree)};
    return model;
}

std::string read_fixture(const std::string& name) {
    std::ifstream in(std::filesystem::path(ZETTA_TEST_FIXTURES_DIR) / "model_import" / name,
                     std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

TEST(ModelBounds, ASplitPastTheFeatureListIsRefusedByTheWeightsFormat) {
    json body = one_split(0).to_json();
    ASSERT_TRUE(GbdtModel::from_json(body));
    body["trees"][0]["nodes"][0]["feature"] = 7;  // the model names two features
    std::string error;
    EXPECT_FALSE(GbdtModel::from_json(body, &error));
    EXPECT_NE(error.find("feature 7"), std::string::npos) << error;

    // A split on the last feature is fine; one past it is not.
    body["trees"][0]["nodes"][0]["feature"] = 1;
    EXPECT_TRUE(GbdtModel::from_json(body));
    body["trees"][0]["nodes"][0]["feature"] = 2;
    EXPECT_FALSE(GbdtModel::from_json(body));

    // A split with no feature list at all reads nothing that exists.
    body["trees"][0]["nodes"][0]["feature"] = 0;
    body["feature_names"] = json::array();
    EXPECT_FALSE(GbdtModel::from_json(body));
}

TEST(ModelBounds, AChildThatPointsBackUpTheTreeIsRefused) {
    const json good = one_split(0).to_json();
    {  // the root as its own child: a walk that never ends
        json bad = good;
        bad["trees"][0]["nodes"][0]["left"] = 0;
        bad["trees"][0]["nodes"][0]["right"] = 1;
        std::string error;
        EXPECT_FALSE(GbdtModel::from_json(bad, &error));
        EXPECT_NE(error.find("child index"), std::string::npos) << error;
    }
    {  // a deeper node pointing back at the root
        json bad = good;
        bad["trees"][0]["nodes"][1] = {{"feature", 1},   {"threshold", 0.0}, {"left", 0},
                                       {"right", 2},     {"value", 0.0},     {"cover", 1.0}};
        EXPECT_FALSE(GbdtModel::from_json(bad));
    }
}

TEST(ModelBounds, EveryImporterRefusesASplitPastTheFinalFeatureList) {
    // The engine's own weights format, brought as a file.
    json body = one_split(0).to_json();
    body["trees"][0]["nodes"][0]["feature"] = 9;
    std::string error;
    EXPECT_FALSE(zetta::ml::import_model(body.dump(), {}, &error));

    // XGBoost: the file names four features and its splits read all four, but it
    // declares one, and the caller names one. The caller's list replaces the
    // file's after the reader checked the splits against the longer one.
    json xgboost = json::parse(read_fixture("xgboost_binary.json"));
    {
        error.clear();
        ModelImportOptions options;
        ASSERT_TRUE(zetta::ml::import_model(xgboost.dump(), options, &error)) << error;
    }
    xgboost["learner"]["learner_model_param"]["num_feature"] = "1";
    ModelImportOptions shortened;
    shortened.feature_names = {"tenure"};
    error.clear();
    EXPECT_FALSE(zetta::ml::import_model(xgboost.dump(), shortened, &error));
    EXPECT_NE(error.find("feature"), std::string::npos) << error;
}

TEST(ModelBounds, AShortRowReadsZeroForTheFeaturesItLacks) {
    const GbdtModel model = one_split(1);
    ASSERT_TRUE(model.validate_structure());
    // feature 1 missing from the row is read as 0.0, which goes left (< 0.5).
    EXPECT_DOUBLE_EQ(model.predict_raw({5.0}), -1.0);
    EXPECT_DOUBLE_EQ(model.predict_raw({}), -1.0);
    EXPECT_DOUBLE_EQ(model.predict_raw({5.0, 0.0}), model.predict_raw({5.0}));
}

TEST(ModelBounds, AnUncheckedModelBuiltInMemoryStaysInsideItsBuffers) {
    // Built directly, bypassing every boundary: a split on feature 40 of 2.
    const GbdtModel bad_feature = one_split(40);
    EXPECT_FALSE(bad_feature.validate_structure());
    const std::vector<double> row{3.0, 4.0};
    // Scoring reads the missing feature as 0.0 instead of past the row…
    EXPECT_DOUBLE_EQ(bad_feature.predict_raw(row), -1.0);
    // …and TreeSHAP writes nothing past its two-value explanation.
    const zetta::ml::ShapExplanation explanation = zetta::ml::tree_shap(bad_feature, row);
    ASSERT_EQ(explanation.values.size(), 2u);
    for (const double value : explanation.values) EXPECT_TRUE(std::isfinite(value));

    // A tree whose root is its own child: the walk stops instead of looping.
    GbdtModel cyclic = one_split(0);
    cyclic.trees[0].nodes[0].left = 0;
    EXPECT_FALSE(cyclic.validate_structure());
    EXPECT_TRUE(std::isfinite(cyclic.predict_raw({0.0, 0.0})));

    // A child index outside the tree: the walk stops at the edge.
    GbdtModel outside = one_split(0);
    outside.trees[0].nodes[0].left = 99;
    EXPECT_FALSE(outside.validate_structure());
    EXPECT_TRUE(std::isfinite(outside.predict_raw({0.0, 0.0})));
}

TEST(ModelBounds, TrainedAndImportedModelsStillPass) {
    for (const std::string& name :
         {std::string("xgboost_binary.json"), std::string("lightgbm_binary.txt")}) {
        SCOPED_TRACE(name);
        std::string error;
        const auto imported = zetta::ml::import_model(read_fixture(name), {}, &error);
        ASSERT_TRUE(imported) << error;
        EXPECT_TRUE(imported->model.validate_structure(&error)) << error;
    }
    EXPECT_TRUE(one_split(0).validate_structure());
}

}  // namespace
