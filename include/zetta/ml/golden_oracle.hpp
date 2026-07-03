#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

// The committed **golden numeric oracle** for the native churn featurize -> train ->
// TreeSHAP pipeline (the RSK-8 correctness oracle, #395; part of the native
// distributed accelerated-compute pass for UC-1 churn, #264).
//
// **No foreign runtime, ever.** The native algorithms (featurize, GBDT, TreeSHAP) are
// reimplemented as distributed C++ over the ComputeDevice seam (ADR-17 — no Python, no
// XGBoost, in the engine, the build, or CI). This file is the *format* for proving that
// reimplementation is numerically correct **without** running any reference at test
// time: a small set of expected featurize / train / SHAP outputs, generated once
// offline by zetta's own native engine and **checked in as static data**
// (tests/fixtures/golden/churn_oracle.json). The generator is not a repo/build/CI
// dependency; tests/test_native_oracle.cpp loads this file and asserts native outputs
// match within tolerance.
//
// The committed numbers are pinned regression anchors. Their *correctness* (vs. merely
// "the bits didn't change") is established alongside them by the oracle's other two
// layers: the analytic closed-form micro-tests (hand-computable GBDT splits + SHAP phi),
// the SHAP **efficiency invariant** (`base_value + sum(values) == predicted`, a
// mathematical identity the committed values must satisfy), and the
// **single-node == multi-node == different-shard-count** equivalence layer.
//
// **Validated at the boundary like load_churn_dataset.** The loader is non-throwing
// (returns an error string, never throws across the zetta-core seam) and
// schema-validates the document against schemas/golden_oracle.schema.json (draft-07,
// the pboettch validator embedded in src/core/ml/golden_oracle.cpp), then enforces the
// cross-field structural rules the schema can't express (array lengths agree across
// entities / feature_names). A malformed fixture fails the load with a path-qualified
// reason instead of silently passing a vacuous test. See docs/ml/native-oracle.md.
namespace zetta::ml {

// The expected leakage-free feature table: one row per account in the entity universe,
// ascending key order. `features[column][row]` is aligned to `feature_names` then
// `entities`; `labels[row]` is the training target.
struct GoldenFeaturize {
    std::vector<std::int64_t> entities;
    std::vector<std::string> feature_names;
    std::vector<std::vector<double>> features;  // [column][row]
    std::string label_name;
    std::vector<double> labels;
};

// The expected trained-GBDT outputs over the feature table under the canonical
// hyperparameters (the test owns the exact GbdtParams; this pins their result).
struct GoldenTrain {
    std::vector<std::string> feature_names;
    double base_score = 0.0;
    std::int64_t tree_count = 0;
    double auc = 0.0;
    double log_loss = 0.0;
    double accuracy = 0.0;
    std::vector<double> feature_importances;  // per feature, feature_names order
    std::vector<double> probabilities;        // per entity, featurize entity order
};

// One entity's expected exact TreeSHAP attribution (raw / margin space).
// Efficiency: `base_value + sum(values) == predicted`.
struct GoldenEntityShap {
    std::int64_t entity = 0;
    double predicted = 0.0;
    std::string dominant;        // the entity's dominant driver (largest |phi|)
    std::vector<double> values;  // phi per feature, feature_names order
};

// The expected TreeSHAP outputs over the trained model.
struct GoldenShap {
    std::vector<std::string> feature_names;
    double base_value = 0.0;            // the model's cover-weighted expected raw score
    std::vector<double> mean_abs_shap;  // global mean |phi| per feature
    std::string top_driver;             // the feature with the largest mean |phi|
    std::vector<GoldenEntityShap> per_entity;
};

// The whole committed oracle: the three pipeline stages' expected outputs.
struct GoldenOracle {
    int schema_version = 1;
    std::string dataset;      // identifies the fixture the values were generated from
    std::string description;  // free-form provenance note
    GoldenFeaturize featurize;
    GoldenTrain train;
    GoldenShap shap;

    // The JSON form, mirroring schemas/golden_oracle.schema.json. Used by the offline
    // generator (the env-gated write mode in the oracle test) to emit the committed file.
    nlohmann::json to_json() const;
};

// Result of loading a golden oracle. Non-throwing: `error` is empty on success.
struct GoldenOracleResult {
    GoldenOracle oracle;
    std::string error;
    bool ok() const { return error.empty(); }
};

// The golden-oracle JSON Schema (draft-07), exposed for tests/introspection.
const nlohmann::json& golden_oracle_schema();

// Load + schema-validate a golden oracle from `path`. Validates against the embedded
// draft-07 schema at the boundary, then checks the cross-field structural rules (every
// feature column / SHAP value vector has one entry per entity / feature). The first
// failure sets `error` with a path-qualified reason. Never throws.
GoldenOracleResult load_golden_oracle(const std::filesystem::path& path);

}  // namespace zetta::ml
