#include "zetta/ml/golden_oracle.hpp"

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::ml {
namespace fs = std::filesystem;

namespace {

// ---- the draft-07 schema, embedded -----------------------------------------
// This embedded schema is the source of truth (the validator is PRIVATE to
// zetta-core, like the Run / churn-dataset path); schemas/golden_oracle.schema.json
// is generated from it (tests/test_schemas.cpp) and a drift check keeps it in sync.
const nlohmann::json& oracle_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.ml.GoldenOracle",
      "type": "object",
      "required": ["schema_version", "dataset", "featurize", "train", "shap"],
      "additionalProperties": false,
      "properties": {
        "schema_version": { "type": "integer", "minimum": 1 },
        "dataset": { "type": "string", "minLength": 1 },
        "description": { "type": "string" },
        "featurize": {
          "type": "object",
          "required": ["entities", "feature_names", "features", "label_name", "labels"],
          "additionalProperties": false,
          "properties": {
            "entities": { "type": "array", "items": { "type": "integer" } },
            "feature_names": { "type": "array", "items": { "type": "string", "minLength": 1 } },
            "features": { "type": "array", "items": { "type": "array", "items": { "type": "number" } } },
            "label_name": { "type": "string", "minLength": 1 },
            "labels": { "type": "array", "items": { "type": "number" } }
          }
        },
        "train": {
          "type": "object",
          "required": ["feature_names", "base_score", "tree_count", "auc",
                       "log_loss", "accuracy", "feature_importances", "probabilities"],
          "additionalProperties": false,
          "properties": {
            "feature_names": { "type": "array", "items": { "type": "string", "minLength": 1 } },
            "base_score": { "type": "number" },
            "tree_count": { "type": "integer", "minimum": 0 },
            "auc": { "type": "number" },
            "log_loss": { "type": "number" },
            "accuracy": { "type": "number" },
            "feature_importances": { "type": "array", "items": { "type": "number" } },
            "probabilities": { "type": "array", "items": { "type": "number" } }
          }
        },
        "shap": {
          "type": "object",
          "required": ["feature_names", "base_value", "mean_abs_shap", "top_driver", "per_entity"],
          "additionalProperties": false,
          "properties": {
            "feature_names": { "type": "array", "items": { "type": "string", "minLength": 1 } },
            "base_value": { "type": "number" },
            "mean_abs_shap": { "type": "array", "items": { "type": "number" } },
            "top_driver": { "type": "string" },
            "per_entity": {
              "type": "array",
              "items": {
                "type": "object",
                "required": ["entity", "predicted", "dominant", "values"],
                "additionalProperties": false,
                "properties": {
                  "entity": { "type": "integer" },
                  "predicted": { "type": "number" },
                  "dominant": { "type": "string" },
                  "values": { "type": "array", "items": { "type": "number" } }
                }
              }
            }
          }
        }
      }
    })JSONSCHEMA");
    return schema;
}

// One validator, compiled once (the expensive step), then cheap to run.
nlohmann::json_schema::json_validator& oracle_validator() {
    static nlohmann::json_schema::json_validator validator(oracle_schema());
    return validator;
}

// Collects the first validation failure via the error-handler overload, so a
// validation failure never crosses the validator-library boundary as an exception
// (the library's recommended path, robust regardless of how each side is compiled).
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

// "" on success, else the first validation failure reason.
std::string validation_error(const nlohmann::json& value) {
    collecting_error_handler handler;
    oracle_validator().validate(value, handler);
    return handler.failed ? handler.reason : std::string();
}

}  // namespace

const nlohmann::json& golden_oracle_schema() { return oracle_schema(); }

nlohmann::json GoldenOracle::to_json() const {
    nlohmann::json doc;
    doc["schema_version"] = schema_version;
    doc["dataset"] = dataset;
    if (!description.empty()) doc["description"] = description;

    nlohmann::json f;
    f["entities"] = featurize.entities;
    f["feature_names"] = featurize.feature_names;
    f["features"] = featurize.features;
    f["label_name"] = featurize.label_name;
    f["labels"] = featurize.labels;
    doc["featurize"] = std::move(f);

    nlohmann::json t;
    t["feature_names"] = train.feature_names;
    t["base_score"] = train.base_score;
    t["tree_count"] = train.tree_count;
    t["auc"] = train.auc;
    t["log_loss"] = train.log_loss;
    t["accuracy"] = train.accuracy;
    t["feature_importances"] = train.feature_importances;
    t["probabilities"] = train.probabilities;
    doc["train"] = std::move(t);

    nlohmann::json s;
    s["feature_names"] = shap.feature_names;
    s["base_value"] = shap.base_value;
    s["mean_abs_shap"] = shap.mean_abs_shap;
    s["top_driver"] = shap.top_driver;
    nlohmann::json per = nlohmann::json::array();
    for (const GoldenEntityShap& entity : shap.per_entity) {
        per.push_back({{"entity", entity.entity},
                       {"predicted", entity.predicted},
                       {"dominant", entity.dominant},
                       {"values", entity.values}});
    }
    s["per_entity"] = std::move(per);
    doc["shap"] = std::move(s);
    return doc;
}

namespace {

// Populate the structs from an already schema-validated document.
GoldenOracle parse_validated(const nlohmann::json& doc) {
    GoldenOracle oracle;
    oracle.schema_version = doc.at("schema_version").get<int>();
    oracle.dataset = doc.at("dataset").get<std::string>();
    if (doc.contains("description"))
        oracle.description = doc.at("description").get<std::string>();

    const nlohmann::json& f = doc.at("featurize");
    oracle.featurize.entities = f.at("entities").get<std::vector<std::int64_t>>();
    oracle.featurize.feature_names =
        f.at("feature_names").get<std::vector<std::string>>();
    oracle.featurize.features =
        f.at("features").get<std::vector<std::vector<double>>>();
    oracle.featurize.label_name = f.at("label_name").get<std::string>();
    oracle.featurize.labels = f.at("labels").get<std::vector<double>>();

    const nlohmann::json& t = doc.at("train");
    oracle.train.feature_names = t.at("feature_names").get<std::vector<std::string>>();
    oracle.train.base_score = t.at("base_score").get<double>();
    oracle.train.tree_count = t.at("tree_count").get<std::int64_t>();
    oracle.train.auc = t.at("auc").get<double>();
    oracle.train.log_loss = t.at("log_loss").get<double>();
    oracle.train.accuracy = t.at("accuracy").get<double>();
    oracle.train.feature_importances =
        t.at("feature_importances").get<std::vector<double>>();
    oracle.train.probabilities = t.at("probabilities").get<std::vector<double>>();

    const nlohmann::json& s = doc.at("shap");
    oracle.shap.feature_names = s.at("feature_names").get<std::vector<std::string>>();
    oracle.shap.base_value = s.at("base_value").get<double>();
    oracle.shap.mean_abs_shap = s.at("mean_abs_shap").get<std::vector<double>>();
    oracle.shap.top_driver = s.at("top_driver").get<std::string>();
    for (const nlohmann::json& entity : s.at("per_entity")) {
        GoldenEntityShap row;
        row.entity = entity.at("entity").get<std::int64_t>();
        row.predicted = entity.at("predicted").get<double>();
        row.dominant = entity.at("dominant").get<std::string>();
        row.values = entity.at("values").get<std::vector<double>>();
        oracle.shap.per_entity.push_back(std::move(row));
    }
    return oracle;
}

// The cross-field rules the JSON Schema can't express (array lengths agree across
// entities / feature_names). "" on success, else the first violation.
std::string structural_error(const GoldenOracle& oracle) {
    const GoldenFeaturize& f = oracle.featurize;
    const std::size_t entities = f.entities.size();
    if (f.features.size() != f.feature_names.size())
        return "/featurize/features: one column per feature_names entry expected";
    for (std::size_t c = 0; c < f.features.size(); ++c) {
        if (f.features[c].size() != entities)
            return "/featurize/features/" + std::to_string(c) +
                   ": one value per entity expected";
    }
    if (f.labels.size() != entities)
        return "/featurize/labels: one label per entity expected";

    const GoldenTrain& t = oracle.train;
    if (t.feature_importances.size() != t.feature_names.size())
        return "/train/feature_importances: one value per feature expected";
    if (t.probabilities.size() != entities)
        return "/train/probabilities: one probability per entity expected";

    const GoldenShap& s = oracle.shap;
    if (s.mean_abs_shap.size() != s.feature_names.size())
        return "/shap/mean_abs_shap: one value per feature expected";
    for (std::size_t r = 0; r < s.per_entity.size(); ++r) {
        if (s.per_entity[r].values.size() != s.feature_names.size())
            return "/shap/per_entity/" + std::to_string(r) +
                   "/values: one phi per feature expected";
    }
    return std::string();
}

}  // namespace

GoldenOracleResult load_golden_oracle(const fs::path& path) {
    GoldenOracleResult result;

    std::ifstream stream(path);
    if (!stream) {
        result.error = "cannot open golden oracle: " + path.string();
        return result;
    }
    std::ostringstream buffer;
    buffer << stream.rdbuf();

    nlohmann::json doc =
        nlohmann::json::parse(buffer.str(), /*cb=*/nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded()) {
        result.error = path.string() + ": invalid JSON";
        return result;
    }
    if (const std::string reason = validation_error(doc); !reason.empty()) {
        result.error = path.string() + reason;
        return result;
    }

    result.oracle = parse_validated(doc);
    if (const std::string reason = structural_error(result.oracle); !reason.empty()) {
        result.oracle = GoldenOracle{};
        result.error = path.string() + reason;
    }
    return result;
}

}  // namespace zetta::ml
