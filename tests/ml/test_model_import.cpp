// lake#681: models trained elsewhere (ONNX tree ensembles, XGBoost JSON and UBJSON,
// LightGBM text) read into the engine's tree model, checked against the source
// library's own predictions; and the engine's models written back out as ONNX.
// Fixtures: tests/fixtures/model_import/, regenerated offline by
// scripts/gen-model-import-fixtures.py. docs/ml/model-import.md.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/model_import.hpp"

namespace {

using nlohmann::json;
using zetta::ml::GbdtModel;
using zetta::ml::ImportedModel;
using zetta::ml::ModelFileFormat;
using zetta::ml::ModelImportOptions;

const std::filesystem::path kFixtures = std::filesystem::path(ZETTA_TEST_FIXTURES_DIR) / "model_import";

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

json expected_of(const std::string& name) {
    return json::parse(read_file(kFixtures / (name + ".expected.json")));
}

std::vector<double> row_of(const json& row) {
    std::vector<double> values;
    for (const json& value : row) {
        values.push_back(value.is_null() ? std::numeric_limits<double>::quiet_NaN()
                                         : value.get<double>());
    }
    return values;
}

struct Case {
    std::string name;
    std::string file;
    ModelFileFormat format;
};

const std::vector<Case>& cases() {
    static const std::vector<Case> all{
        {"sklearn_gbc", "sklearn_gbc.onnx", ModelFileFormat::Onnx},
        {"sklearn_gbr", "sklearn_gbr.onnx", ModelFileFormat::Onnx},
        {"sklearn_rfr", "sklearn_rfr.onnx", ModelFileFormat::Onnx},
        {"sklearn_hgb", "sklearn_hgb.onnx", ModelFileFormat::Onnx},
        {"xgboost_binary", "xgboost_binary.json", ModelFileFormat::XgboostJson},
        {"xgboost_binary_ubj", "xgboost_binary_ubj.ubj", ModelFileFormat::XgboostUbjson},
        {"xgboost_regression", "xgboost_regression.json", ModelFileFormat::XgboostJson},
        {"xgboost_binary_onnx", "xgboost_binary_onnx.onnx", ModelFileFormat::Onnx},
        {"lightgbm_binary", "lightgbm_binary.txt", ModelFileFormat::LightgbmText},
        {"lightgbm_regression", "lightgbm_regression.txt", ModelFileFormat::LightgbmText},
        {"lightgbm_binary_onnx", "lightgbm_binary_onnx.onnx", ModelFileFormat::Onnx},
    };
    return all;
}

ImportedModel import_case(const Case& each) {
    const json expected = expected_of(each.name);
    ModelImportOptions options;
    if (each.format == ModelFileFormat::Onnx) {
        options.feature_names = expected.at("feature_names").get<std::vector<std::string>>();
    }
    std::string error;
    std::optional<ImportedModel> imported =
        zetta::ml::import_model(read_file(kFixtures / each.file), options, &error);
    EXPECT_TRUE(imported) << each.name << ": " << error;
    return imported ? std::move(*imported) : ImportedModel{};
}

// The score the engine gives: a classifier's probability, a regressor's value.
double score(const GbdtModel& model, const std::vector<double>& row) {
    return model.predict_proba(row);
}

TEST(ModelImport, EveryFormatScoresAsItsSourceLibraryDoes) {
    for (const Case& each : cases()) {
        SCOPED_TRACE(each.name);
        const json expected = expected_of(each.name);
        const ImportedModel imported = import_case(each);
        EXPECT_EQ(imported.format, each.format);
        EXPECT_EQ(imported.kind, expected.at("kind").get<std::string>());
        EXPECT_EQ(imported.model.feature_names,
                  expected.at("feature_names").get<std::vector<std::string>>());
        ASSERT_FALSE(imported.model.trees.empty());
        const json& rows = expected.at("rows");
        const json& scores = expected.at("expected");
        ASSERT_EQ(rows.size(), scores.size());
        for (std::size_t index = 0; index < rows.size(); ++index) {
            const double reference = scores[index].get<double>();
            // The libraries sum leaves in 32-bit floats; every row takes the same path.
            const double tolerance = 1e-5 * std::max(1.0, std::abs(reference));
            EXPECT_NEAR(score(imported.model, row_of(rows[index])), reference, tolerance)
                << "row " << index;
        }
        // It is the engine's own model: it round-trips through the weights format.
        const std::optional<GbdtModel> reread = GbdtModel::from_json(imported.model.to_json());
        ASSERT_TRUE(reread);
        EXPECT_EQ(reread->to_json(), imported.model.to_json());
    }
}

TEST(ModelImport, MissingValuesGoWhereTheSourceSendsThem) {
    // The XGBoost and LightGBM fixtures were trained with spend missing in some rows;
    // those rows are in the checked set, so their default directions are exercised.
    std::size_t missing_rows = 0;
    for (const json& row : expected_of("xgboost_binary").at("rows")) {
        if (row[1].is_null()) ++missing_rows;
    }
    EXPECT_GT(missing_rows, 0u);
    bool any_left = false;
    for (const Case& each : cases()) {
        for (const auto& tree : import_case(each).model.trees) {
            for (const auto& node : tree.nodes) any_left = any_left || node.missing_left;
        }
    }
    EXPECT_TRUE(any_left);
}

TEST(ModelImport, AFloatSplitBecomesTheExactDoubleBound) {
    std::mt19937_64 random(681);
    std::uniform_real_distribution<double> magnitude(-40.0, 40.0);
    for (int trial = 0; trial < 20000; ++trial) {
        const auto upper = static_cast<float>(std::ldexp(magnitude(random) / 40.0, static_cast<int>(magnitude(random))));
        const double bound = zetta::ml::float_less_bound(upper);
        const float below = std::nextafter(upper, -std::numeric_limits<float>::infinity());
        // Probe the doubles around the boundary: the midpoint and its neighbours.
        const double middle = (static_cast<double>(below) + static_cast<double>(upper)) / 2.0;
        for (const double x : {middle, std::nextafter(middle, -1e300), std::nextafter(middle, 1e300),
                               static_cast<double>(below), static_cast<double>(upper)}) {
            EXPECT_EQ(static_cast<float>(x) < upper, x < bound)
                << "upper " << upper << " x " << x;
        }
    }
    EXPECT_EQ(zetta::ml::float_less_bound(-std::numeric_limits<float>::infinity()),
              -std::numeric_limits<double>::infinity());
}

TEST(ModelImport, WhatCannotBeConvertedExactlyIsRefusedByName) {
    const auto refusal = [](const std::string& bytes) {
        std::string error;
        EXPECT_FALSE(zetta::ml::import_model(bytes, {}, &error));
        return error;
    };
    EXPECT_NE(refusal(read_file(kFixtures / "sklearn_rfc.onnx")).find("averages probabilities"),
              std::string::npos);
    EXPECT_NE(refusal(read_file(kFixtures / "sklearn_scaler_pipeline.onnx")).find("Scaler"),
              std::string::npos);
    EXPECT_NE(refusal(read_file(kFixtures / "sklearn_mlp.onnx")).find("not a tree ensemble"),
              std::string::npos);
    EXPECT_NE(refusal(std::string("\x80\x04\x95\x10", 4) + "sklearn").find("pickle"),
              std::string::npos);
    EXPECT_NE(refusal("PK\x03\x04 a zip").find("ONNX"), std::string::npos);
    EXPECT_NE(refusal("{\"a\": 1}").find("not a model"), std::string::npos);
    EXPECT_NE(refusal("").find("empty"), std::string::npos);

    // A multiclass XGBoost model, an exponential objective, and a categorical LightGBM split.
    json xgboost = json::parse(read_file(kFixtures / "xgboost_binary.json"));
    xgboost["learner"]["objective"]["name"] = "multi:softprob";
    EXPECT_NE(refusal(xgboost.dump()).find("multi:softprob"), std::string::npos);
    xgboost["learner"]["objective"]["name"] = "count:poisson";
    EXPECT_NE(refusal(xgboost.dump()).find("count:poisson"), std::string::npos);
    std::string lightgbm = read_file(kFixtures / "lightgbm_binary.txt");
    lightgbm.replace(lightgbm.find("decision_type=2"), 15, "decision_type=1");
    EXPECT_NE(refusal(lightgbm).find("categorical"), std::string::npos);

    // Naming the wrong number of columns.
    std::string error;
    ModelImportOptions options;
    options.feature_names = {"only_one"};
    EXPECT_FALSE(zetta::ml::import_model(read_file(kFixtures / "sklearn_gbr.onnx"), options, &error));
    EXPECT_NE(error.find("feature_names"), std::string::npos);
}

// lake#780: the objective line LightGBM writes carries the objective's parameters
// (`objective=binary sigmoid:1`). The reader once split a temporary copy of that
// line into views, which dangled once the copy was freed: a line too long for the
// string's inline buffer was read from freed memory, refused, and the refusal quoted
// the freed bytes. The lines below are long enough to live on the heap.
TEST(ModelImport, ALightGbmObjectiveWithParametersIsRead) {
    const std::string original = read_file(kFixtures / "lightgbm_binary.txt");
    const json expected = expected_of("lightgbm_binary");
    const auto names = expected.at("feature_names").get<std::vector<std::string>>();
    const std::string written = "objective=binary sigmoid:1\n";
    const auto with_objective = [&](const std::string& line) {
        std::string text = original;
        text.replace(text.find(written), written.size(), line + "\n");
        return text;
    };
    const std::vector<double> row = row_of(expected.at("rows")[0]);
    const double reference = expected.at("expected")[0].get<double>();

    for (const std::string line : {"objective=binary sigmoid:1.000000000000000000000000000000",
                                   "objective=cross_entropy sigmoid:1.0000000000000000000000",
                                   "objective=binary sigmoid:1.00000000000000000000 extra:words"}) {
        SCOPED_TRACE(line);
        std::string error;
        const std::optional<ImportedModel> imported =
            zetta::ml::import_model(with_objective(line), {}, &error);
        ASSERT_TRUE(imported) << error;
        EXPECT_EQ(imported->kind, "classifier");
        EXPECT_EQ(imported->model.feature_names, names);
        EXPECT_NEAR(score(imported->model, row), reference, 1e-5);
    }

    // The sigmoid's value is read, not defaulted: sigmoid:2 doubles the margin.
    std::string error;
    const std::optional<ImportedModel> doubled = zetta::ml::import_model(
        with_objective("objective=binary sigmoid:2.0000000000000000000000000"), {}, &error);
    ASSERT_TRUE(doubled) << error;
    const double margin = std::log(reference / (1.0 - reference));
    EXPECT_NEAR(score(doubled->model, row), 1.0 / (1.0 + std::exp(-2.0 * margin)), 1e-5);

    // A refused objective is quoted as written, never from freed memory.
    EXPECT_FALSE(zetta::ml::import_model(
        with_objective("objective=lambdarank lambdarank_truncation_level:30 label_gain:0,1,3"), {},
        &error));
    EXPECT_NE(error.find("the objective is 'lambdarank'"), std::string::npos) << error;
}

TEST(ModelImport, AnOnnxModelWithoutNamesReadsF0Onward) {
    std::string error;
    const auto imported =
        zetta::ml::import_model(read_file(kFixtures / "sklearn_gbr.onnx"), {}, &error);
    ASSERT_TRUE(imported) << error;
    EXPECT_EQ(imported->model.feature_names, (std::vector<std::string>{"f0", "f1", "f2", "f3"}));
}

TEST(ModelImport, ExportedOnnxScoresAsTheEngineDoes) {
    // Every imported fixture, written as ONNX and read back, scores exactly as the
    // engine's model; with ZETTA_MODEL_EXPORT_DIR set the files and the engine's own
    // scores are written there for scripts/gen-model-import-fixtures.py
    // --check-exports, which scores them with onnxruntime.
    const char* export_dir = std::getenv("ZETTA_MODEL_EXPORT_DIR");
    for (const Case& each : cases()) {
        SCOPED_TRACE(each.name);
        const ImportedModel imported = import_case(each);
        std::string error;
        const std::optional<std::string> onnx = zetta::ml::export_onnx(imported.model, &error);
        ASSERT_TRUE(onnx) << error;
        const auto reread = zetta::ml::import_model(*onnx, {}, &error);
        ASSERT_TRUE(reread) << error;
        EXPECT_EQ(reread->format, ModelFileFormat::Onnx);
        EXPECT_EQ(reread->kind, imported.kind);
        EXPECT_EQ(reread->model.feature_names, imported.model.feature_names);
        const json expected = expected_of(each.name);
        json engine_scores = json::array();
        for (const json& row : expected.at("rows")) {
            const double engine = score(imported.model, row_of(row));
            EXPECT_DOUBLE_EQ(score(reread->model, row_of(row)), engine);
            engine_scores.push_back(engine);
        }
        if (export_dir != nullptr) {
            const std::filesystem::path directory(export_dir);
            std::filesystem::create_directories(directory);
            std::ofstream(directory / (each.name + ".onnx"), std::ios::binary) << *onnx;
            std::ofstream(directory / (each.name + ".engine.json"))
                << json{{"kind", imported.kind},
                        {"rows", expected.at("rows")},
                        {"expected", engine_scores}}
                       .dump();
        }
    }
}

TEST(ModelImport, AnIsotonicCalibrationOrARankerIsNotWrittenAsOnnx) {
    GbdtModel model = import_case(cases().front()).model;
    zetta::ml::Calibrator isotonic;
    isotonic.method = zetta::ml::CalibrationMethod::Isotonic;
    isotonic.iso_bounds = {0.0};
    isotonic.iso_values = {0.5};
    model.calibration = isotonic;
    std::string error;
    EXPECT_FALSE(zetta::ml::export_onnx(model, &error));
    EXPECT_NE(error.find("isotonic"), std::string::npos);
    model.calibration.reset();
    model.objective = "lambdamart";
    EXPECT_FALSE(zetta::ml::export_onnx(model, &error));
}

TEST(ModelImport, APlattCalibrationIsFoldedIntoTheOnnxLeaves) {
    GbdtModel model = import_case(cases().front()).model;
    zetta::ml::Calibrator platt;
    platt.platt_a = 0.7;
    platt.platt_b = -0.2;
    model.calibration = platt;
    std::string error;
    const auto onnx = zetta::ml::export_onnx(model, &error);
    ASSERT_TRUE(onnx) << error;
    const auto reread = zetta::ml::import_model(*onnx, {}, &error);
    ASSERT_TRUE(reread) << error;
    for (const json& row : expected_of("sklearn_gbc").at("rows")) {
        EXPECT_NEAR(reread->model.predict_proba(row_of(row)), model.predict_proba(row_of(row)), 1e-12);
    }
}

TEST(ModelImport, MissingLeftRoundTripsAndATrainedBodyIsUnchanged) {
    GbdtModel model;
    model.feature_names = {"x"};
    zetta::ml::RegressionTree tree;
    tree.nodes = {{.feature = 0, .threshold = 1.0, .left = 1, .right = 2, .missing_left = true},
                  {.value = -1.0},
                  {.value = 1.0}};
    model.trees.push_back(tree);
    const double nan = std::numeric_limits<double>::quiet_NaN();
    EXPECT_LT(model.predict_raw({nan}), 0.0);
    const json body = model.to_json();
    EXPECT_TRUE(body["trees"][0]["nodes"][0].at("missing_left").get<bool>());
    EXPECT_FALSE(body["trees"][0]["nodes"][1].contains("missing_left"));
    const auto reread = GbdtModel::from_json(body);
    ASSERT_TRUE(reread);
    EXPECT_LT(reread->predict_raw({nan}), 0.0);
    model.trees[0].nodes[0].missing_left = false;
    EXPECT_GT(model.predict_raw({nan}), 0.0);  // as every trained model: NaN goes right
    EXPECT_FALSE(model.to_json()["trees"][0]["nodes"][0].contains("missing_left"));
}

}  // namespace
