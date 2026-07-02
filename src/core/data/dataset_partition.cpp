#include "zetta/data/dataset_partition.hpp"

#include "zetta/cluster/replication.hpp"

#include "../catalog.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::data {
namespace fs = std::filesystem;

namespace {

// The DatasetPartition JSON Schema (draft-07). One declarative source of truth for
// the value shape carried by partition.json (with an optional `id`) and the
// `part:<uuid>` CRDT value (without it). The shape is sealed
// (additionalProperties: false) and `table` is required, so a malformed record
// fails validation at the boundary instead of corrupting the tree. `stats` is a
// free-form object (per-column pruning stats); `owner_nodes` is the set of node ids
// holding a copy of the body. This embedded schema is the source of truth;
// schemas/dataset_partition.schema.json is generated from it (tests/test_schemas.cpp)
// and a drift check keeps it in sync.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.DatasetPartition",
      "type": "object",
      "properties": {
        "id":       { "type": "string" },
        "table":    { "type": "string" },
        "blob_ref": { "type": "string" },
        "row_range": {
          "type": "object",
          "properties": {
            "start": { "type": "integer", "minimum": 0 },
            "end":   { "type": "integer", "minimum": 0 }
          },
          "required": ["start", "end"],
          "additionalProperties": false
        },
        "stats":       { "type": "object" },
        "owner_nodes": { "type": "array", "items": { "type": "string" } },
        "created":     { "type": "string" }
      },
      "required": ["table"],
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

nlohmann::json to_json(const DatasetPartitionValue& value) {
    return nlohmann::json{
        {"table", value.table},
        {"blob_ref", value.blob_ref},
        {"row_range",
         {{"start", value.row_range.start}, {"end", value.row_range.end}}},
        {"stats", value.stats},
        {"owner_nodes", value.owner_nodes},
        {"created", value.created}};
}

const nlohmann::json& dataset_partition_value_schema() {
    return schema_document();
}

bool validate_dataset_partition_value(const nlohmann::json& value,
                                      std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<DatasetPartitionValue> parse_dataset_partition_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_dataset_partition_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the required `table`, so the
    // extraction below is total: missing optional fields fall back to defaults.
    DatasetPartitionValue parsed;
    parsed.table = value.value("table", std::string{});
    parsed.blob_ref = value.value("blob_ref", std::string{});
    if (const auto it = value.find("row_range"); it != value.end()) {
        parsed.row_range.start = it->value("start", std::uint64_t{0});
        parsed.row_range.end = it->value("end", std::uint64_t{0});
    }
    if (const auto it = value.find("stats"); it != value.end()) {
        parsed.stats = *it;
    }
    if (const auto it = value.find("owner_nodes"); it != value.end()) {
        parsed.owner_nodes = it->get<std::vector<std::string>>();
    }
    parsed.created = value.value("created", std::string{});
    return parsed;
}

// ----- DatasetPartition ------------------------------------------------------

DatasetPartition::DatasetPartition(std::string id, DatasetPartitionValue value,
                                   fs::path dir, zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {}

void DatasetPartition::apply_value(DatasetPartitionValue value) {
    value_ = std::move(value);
    persist_meta();  // projection only — no capture (no feedback loop)
}

void DatasetPartition::set_owner_nodes(std::vector<std::string> owner_nodes) {
    value_.owner_nodes = std::move(owner_nodes);
    // CRDT first (the source of truth), then the on-disk projection — the same
    // order as Run::set_status. A standalone partition (no replication) just
    // persists the projection.
    if (replication_) {
        replication_->set(catalog::partition_key(id_), to_json(value_));
    }
    persist_meta();
}

nlohmann::json DatasetPartition::metadata() const { return to_json(value_); }

void DatasetPartition::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "partition.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::data