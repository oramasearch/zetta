// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/ml/gbdt.hpp"

// Models trained elsewhere, read into the engine's own gradient-boosted tree model,
// and the engine's models written back out (lake#681). See docs/ml/model-import.md.
//
// A customer's tree models come from scikit-learn, XGBoost and LightGBM, saved in
// those libraries' own files or exported to ONNX. Each is converted, when it is
// brought, into a GbdtModel, so score, explain, expose_model and predict take it
// exactly as they take a model trained here. The conversion is exact: every split
// becomes the engine's strict `<` with the double threshold that sends every input
// the way the source sends it (a source that compares 32-bit floats included), and
// a missing value goes the way the source sends it (TreeNode::missing_left).
//
// Every reader is the engine's own (ADR-17: no foreign runtime). ONNX is read with a
// hand-written protobuf wire-format reader (src/core/ml/protobuf_wire.hpp), XGBoost's
// UBJSON with nlohmann::json's UBJSON parser, LightGBM's text model with a parser here.
// What cannot be converted exactly is refused with a reason naming what can.
namespace zetta::ml {

enum class ModelFileFormat {
    Zetta,           // the engine's own JSON weights (GbdtModel::to_json)
    Onnx,            // ai.onnx.ml TreeEnsembleRegressor / TreeEnsembleClassifier
    XgboostJson,     // XGBoost Booster.save_model("model.json")
    XgboostUbjson,   // XGBoost Booster.save_model("model.ubj")
    LightgbmText,    // LightGBM Booster.save_model("model.txt")
};

// "zetta", "onnx", "xgboost_json", "xgboost_ubjson", "lightgbm".
std::string to_string(ModelFileFormat format);
std::optional<ModelFileFormat> model_file_format_from_string(std::string_view name);

// The formats a brought model may be in, as one sentence for a refusal.
std::string supported_model_formats();

// Import resource ceilings (#1419). Native callers may choose smaller or larger
// budgets; feature indices must still fit the engine's int representation.
struct ModelImportLimits {
    std::size_t max_features = 1'048'576;
    std::size_t max_bytes = 1'073'741'824;
};

// Operator settings for register_model: ZETTA_MODEL_IMPORT_MAX_FEATURES and
// ZETTA_MODEL_IMPORT_MAX_BYTES. Invalid or zero values refuse imports.
std::optional<ModelImportLimits> model_import_limits_from_environment(std::string* error);

struct ModelImportOptions {
    // The file's format; detected from its contents when absent.
    std::optional<ModelFileFormat> format;
    // The columns the model reads, in its own order. Required in effect for ONNX,
    // which names no columns (its features are f0, f1, … otherwise); for the other
    // formats it replaces the names the file carries.
    std::vector<std::string> feature_names;
    ModelImportLimits limits;
};

struct ImportedModel {
    GbdtModel model;
    ModelFileFormat format = ModelFileFormat::Zetta;
    std::string kind;  // "classifier" (binary, logistic link) or "regressor"
};

// Read a model file into the engine's model. nullopt with a reason in `error` when
// the file is not one of the supported formats or cannot be converted exactly.
std::optional<ImportedModel> import_model(std::string_view bytes,
                                          const ModelImportOptions& options,
                                          std::string* error);

// Write a gradient-boosted tree model as ONNX: a binary_logistic model as a
// TreeEnsembleClassifier (labels 0 and 1, a logistic link) and a squared_error model
// as a TreeEnsembleRegressor, over one double input `input` [N, features], with
// double thresholds and leaf values (ai.onnx.ml opset 3) so it scores as it does
// here. The feature names ride in the model's metadata (`feature_names`, a JSON
// array). A Platt calibration is folded into the leaves; an isotonic one cannot be
// expressed and is refused, as are other objectives.
std::optional<std::string> export_onnx(const GbdtModel& model, std::string* error);

// The double bound d such that, for every non-NaN double x,
//   static_cast<float>(x) < upper  <=>  x < d.
// How a split that compares 32-bit floats becomes the engine's strict double `<`.
double float_less_bound(float upper);

}  // namespace zetta::ml
