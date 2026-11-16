// SPDX-License-Identifier: MIT

#include "zetta/data/dataset_manifest.hpp"

#include <utility>

#include <nlohmann/json-schema.hpp>

namespace zetta::data {
namespace {

// The DatasetManifest JSON Schema (draft-07). One declarative source of truth for
// the value shape carried by the `mfst:<table-id>` CRDT value and by the
// `manifest` block of a table's table.json. Sealed (additionalProperties: false)
// with `table` and `partitions` required, so a malformed record is rejected at
// the boundary rather than becoming an authority the read path fails queries
// against. This embedded schema is the source of truth;
// schemas/dataset_manifest.schema.json is generated from it (tests/test_schemas.cpp)
// and a drift check keeps it in sync.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.DatasetManifest",
      "description": "How many partitions a finished dataset HAS (#837) — the authority a reader compares its visible part: entries against, so an incomplete catalog fails the query instead of answering short.",
      "type": "object",
      "properties": {
        "table":      { "type": "string", "minLength": 1 },
        "partitions": { "type": "integer", "minimum": 0 },
        "rows":       { "type": "integer", "minimum": 0 },
        "bytes":      { "type": "integer", "minimum": 0 },
        "partition_ids": { "type": "array", "items": { "type": "string" } },
        "created":    { "type": "string" }
      },
      "required": ["table", "partitions"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure through the error-handler overload, so
// nothing crosses the validator-library boundary as an exception.
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

nlohmann::json to_json(const DatasetManifestValue& value) {
    nlohmann::json document{{"table", value.table},
                            {"partitions", value.partitions},
                            {"rows", value.rows},
                            {"bytes", value.bytes},
                            {"created", value.created}};
    // Omitted when the writer did not know every id (a co-launched ingest agrees
    // on COUNTS through the allreduce, not on a list), so the absence says
    // "counts only" rather than "no partitions".
    if (!value.partition_ids.empty()) {
        document["partition_ids"] = value.partition_ids;
    }
    return document;
}

const nlohmann::json& dataset_manifest_value_schema() { return schema_document(); }

bool validate_dataset_manifest_value(const nlohmann::json& value,
                                     std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<DatasetManifestValue> parse_dataset_manifest_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_dataset_manifest_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the required fields, so the
    // extraction below is total: missing optional fields fall back to defaults.
    DatasetManifestValue parsed;
    parsed.table = value.value("table", std::string{});
    parsed.partitions = value.value("partitions", std::uint64_t{0});
    parsed.rows = value.value("rows", std::uint64_t{0});
    parsed.bytes = value.value("bytes", std::uint64_t{0});
    if (const auto it = value.find("partition_ids"); it != value.end()) {
        parsed.partition_ids = it->get<std::vector<std::string>>();
    }
    parsed.created = value.value("created", std::string{});
    return parsed;
}

std::string catalog_shortfall(const DatasetManifestValue& manifest,
                              std::uint64_t visible) {
    if (visible >= manifest.partitions) return {};
    return "the dataset's partition catalog is incomplete — " +
           std::to_string(visible) + " of " + std::to_string(manifest.partitions) +
           " partitions are visible on this node; the dataset is degraded until "
           "the catalog converges (a partition that has not gossiped here is "
           "invisible, not unreachable, so answering would return fewer rows "
           "with no error)";
}

}  // namespace zetta::data
