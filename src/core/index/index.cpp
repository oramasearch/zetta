#include "zetta/index/index.hpp"

#include "zetta/cluster/replication.hpp"

#include "../catalog.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::index {
namespace fs = std::filesystem;

namespace {

// The Index JSON Schema (draft-07). One declarative source of truth for the value
// shape carried by index.json (with an optional `id`) and the `index:<uuid>` CRDT
// value (without it). `table` / `backend` are required + non-empty, `kind` is a
// closed enum, `fields` is a non-empty array of unique non-empty strings, and the
// shape is sealed (additionalProperties: false) — so a malformed record fails
// validation at the boundary instead of corrupting the tree. This embedded schema
// is the source of truth; schemas/index.schema.json is generated from it
// (tests/test_schemas.cpp) and a drift check keeps it in sync.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.Index",
      "type": "object",
      "properties": {
        "id":      { "type": "string" },
        "table":   { "type": "string", "minLength": 1 },
        "kind":    { "type": "string", "enum": ["search", "vector", "feature"] },
        "backend": { "type": "string", "minLength": 1 },
        "fields": {
          "type": "array",
          "minItems": 1,
          "uniqueItems": true,
          "items": { "type": "string", "minLength": 1 }
        },
        "created": { "type": "string" },
        "exposed": { "type": "boolean" },
        "database_id": { "type": "string", "minLength": 1 }
      },
      "required": ["table", "kind", "backend", "fields"],
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

nlohmann::json to_json(const IndexValue& value) {
    nlohmann::json document{
        {"table", value.table},
        {"kind", to_string(value.kind)},
        {"backend", value.backend},
        {"fields", value.fields},
        {"created", value.created},
    };
    // Emitted only when set, so records written before serving existed (#648)
    // and never-exposed indexes keep byte-identical metadata.
    if (value.exposed) document["exposed"] = true;
    // Likewise only when known (#1428): a record from before the database was
    // persisted keeps its bytes, and resolve_index_source decides it.
    if (!value.database_id.empty()) document["database_id"] = value.database_id;
    return document;
}

const nlohmann::json& index_value_schema() { return schema_document(); }

bool validate_index_value(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<IndexValue> parse_index_value(const nlohmann::json& value,
                                            std::string* error) {
    if (!validate_index_value(value, error)) return std::nullopt;

    // The schema has already guaranteed the required fields + types + the kind
    // enum, so the extraction below is total: the kind name is known-valid, and
    // missing optional fields fall back to defaults.
    IndexValue parsed;
    parsed.table = value.value("table", std::string{});
    parsed.kind = index_kind_from_name(value.value("kind", std::string{}))
                      .value_or(IndexKind::Search);
    parsed.backend = value.value("backend", std::string{});
    parsed.fields = value.value("fields", std::vector<std::string>{});
    parsed.created = value.value("created", std::string{});
    parsed.exposed = value.value("exposed", false);
    parsed.database_id = value.value("database_id", std::string{});
    return parsed;
}

// ----- Index -----------------------------------------------------------------

Index::Index(std::string id, IndexValue value, fs::path dir,
             zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {}

void Index::apply_value(IndexValue value) {
    value_ = std::move(value);
    persist_meta();  // projection only — no capture
}

void Index::set_exposed(bool exposed) {
    if (value_.exposed == exposed) return;  // idempotent: no capture, no churn
    value_.exposed = exposed;
    // CRDT first (the source of truth), then the on-disk projection — the
    // Model::capture_and_persist pattern; an index has no body to touch.
    if (replication_) replication_->set(catalog::index_key(id_), metadata());
    persist_meta();
}

nlohmann::json Index::metadata() const { return to_json(value_); }

void Index::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "index.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::index