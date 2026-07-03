#include "zetta/ml/model.hpp"

#include "zetta/cluster/replication.hpp"
#include "zetta/util/sha256.hpp"

#include "../catalog.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::ml {
namespace fs = std::filesystem;

namespace {

// The Model JSON Schema (draft-07). One declarative source of truth for the
// value shape carried by model.json (with an optional `id`) and the
// `model:<uuid>` CRDT value (without it). `kind` is required + non-empty,
// `metrics` are numeric, and `weights` (when present) must carry both size +
// hash — so a malformed record fails validation at the boundary.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.Model",
      "type": "object",
      "properties": {
        "id":      { "type": "string" },
        "name":    { "type": "string" },
        "kind":    { "type": "string", "minLength": 1 },
        "project": { "type": "string" },
        "run":     { "type": "string" },
        "feature_schema": { "type": "object" },
        "metrics": {
          "type": "object",
          "additionalProperties": { "type": "number" }
        },
        "created": { "type": "string" },
        "weights": {
          "type": "object",
          "properties": {
            "size":   { "type": "integer", "minimum": 0 },
            "sha256": { "type": "string" }
          },
          "required": ["size", "sha256"],
          "additionalProperties": false
        },
        "exposed": { "type": "boolean" }
      },
      "required": ["kind"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
// validate() is then cheap and runs at every boundary.
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. Using the error-handler overload of
// validate() (rather than the throwing one) keeps validation failures from
// crossing the validator-library boundary as exceptions — the library's
// recommended path, and robust regardless of how each side is compiled.
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

}  // namespace

nlohmann::json to_json(const ModelValue& value) {
    nlohmann::json document{
        {"name", value.name},
        {"kind", value.kind},
        {"project", value.project},
        {"run", value.run},
        {"feature_schema", value.feature_schema},
        {"metrics", value.metrics},
        {"created", value.created},
    };
    if (value.weights) {
        document["weights"] = {{"size", value.weights->size},
                               {"sha256", value.weights->sha256}};
    }
    // Emitted only when set, so records written before serving existed (#648)
    // and never-exposed models keep byte-identical metadata.
    if (value.exposed) document["exposed"] = true;
    return document;
}

const nlohmann::json& model_value_schema() { return schema_document(); }

bool validate_model_value(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<ModelValue> parse_model_value(const nlohmann::json& value,
                                            std::string* error) {
    if (!validate_model_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the required `kind`, so the
    // extraction below is total: missing optional fields fall back to defaults.
    ModelValue parsed;
    parsed.name = value.value("name", std::string{});
    parsed.kind = value.value("kind", std::string{});
    parsed.project = value.value("project", std::string{});
    parsed.run = value.value("run", std::string{});
    if (const auto it = value.find("feature_schema"); it != value.end()) {
        parsed.feature_schema = *it;
    }
    if (const auto it = value.find("metrics"); it != value.end()) {
        parsed.metrics = *it;
    }
    parsed.created = value.value("created", std::string{});
    if (const auto it = value.find("weights"); it != value.end()) {
        ModelWeights weights;
        weights.size = it->value("size", std::uintmax_t{0});
        weights.sha256 = it->value("sha256", std::string{});
        parsed.weights = weights;
    }
    parsed.exposed = value.value("exposed", false);
    return parsed;
}

// ----- Model -----------------------------------------------------------------

Model::Model(std::string id, ModelValue value, fs::path dir,
             zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {}

bool Model::has_weights_body() const {
    std::error_code ec;
    return fs::exists(weights_path(), ec);
}

std::optional<std::string> Model::read_weights() const {
    std::ifstream in(weights_path(), std::ios::binary);
    if (!in) return std::nullopt;
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

bool Model::write_weights_body(std::string_view bytes) const {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    const fs::path path = weights_path();
    const fs::path tmp = path.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out.good()) return false;
    }
    fs::rename(tmp, path, ec);  // atomic publish
    return !ec;
}

bool Model::set_weights(std::string_view bytes) {
    if (!write_weights_body(bytes)) return false;
    ModelWeights weights;
    weights.size = bytes.size();
    weights.sha256 = zetta::util::sha256_hex(bytes);
    value_.weights = weights;
    capture_and_persist();
    return true;
}

void Model::set_metrics(nlohmann::json metrics) {
    value_.metrics = std::move(metrics);
    capture_and_persist();
}

void Model::set_exposed(bool exposed) {
    if (value_.exposed == exposed) return;  // idempotent: no capture, no churn
    value_.exposed = exposed;
    capture_and_persist();
}

void Model::apply_value(ModelValue value) {
    value_ = std::move(value);
    persist_meta();  // metadata projection only — no capture, body untouched
}

void Model::capture_and_persist() {
    // CRDT first (the source of truth), then the on-disk projection.
    if (replication_) replication_->set(catalog::model_key(id_), metadata());
    persist_meta();
}

std::vector<ModelDriver> ranked_drivers(const nlohmann::json& feature_schema) {
    std::vector<ModelDriver> drivers;
    if (!feature_schema.is_object()) return drivers;
    const auto it = feature_schema.find("drivers");
    if (it == feature_schema.end() || !it->is_array()) return drivers;

    // Defensive read: keep only well-shaped entries ({ feature: string,
    // importance: number }); anything else is silently skipped (feature_schema is
    // free-form, so we never trust its inner shape blindly — but we also never
    // throw on it).
    for (const nlohmann::json& entry : *it) {
        if (!entry.is_object()) continue;
        const auto feature = entry.find("feature");
        const auto importance = entry.find("importance");
        if (feature == entry.end() || !feature->is_string()) continue;
        if (importance == entry.end() || !importance->is_number()) continue;
        drivers.push_back(
            ModelDriver{feature->get<std::string>(), importance->get<double>()});
    }

    // Rank by influence magnitude (|importance|) descending — strongest first —
    // with the feature name as a stable tiebreak so the order is deterministic.
    std::stable_sort(drivers.begin(), drivers.end(),
                     [](const ModelDriver& lhs, const ModelDriver& rhs) {
                         const double left = std::abs(lhs.importance);
                         const double right = std::abs(rhs.importance);
                         if (left != right) return left > right;
                         return lhs.feature < rhs.feature;
                     });
    return drivers;
}

nlohmann::json Model::metadata() const { return to_json(value_); }

void Model::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "model.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::ml