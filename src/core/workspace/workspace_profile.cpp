// SPDX-License-Identifier: MIT

#include "zetta/workspace/workspace_profile.hpp"

#include "zetta/cluster/replication.hpp"

#include "../catalog.hpp"

#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::workspace {
namespace fs = std::filesystem;

namespace {

// The Workspace Profile JSON Schema (draft-07). One declarative source of truth
// for the value shape carried by workspace_profile.json (with an optional `id`)
// and the `profile:<uuid>` CRDT value (without it). `owner` is required +
// non-empty; `sources` is an array of free-form source refs — so a malformed
// record fails validation at the boundary.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.WorkspaceProfile",
      "type": "object",
      "properties": {
        "id":         { "type": "string" },
        "owner":      { "type": "string", "minLength": 1 },
        "role":       { "type": "string" },
        "daily_work": { "type": "string" },
        "sources": {
          "type": "array",
          "items": { "type": "object" }
        },
        "created": { "type": "string" },
        "updated": { "type": "string" }
      },
      "required": ["owner"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

// A process-wide validator, compiled once from the schema (the expensive step).
nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

// Collects the first validation failure. Using the error-handler overload of
// validate() (rather than the throwing one) keeps validation failures from
// crossing the validator-library boundary as exceptions.
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

nlohmann::json to_json(const WorkspaceProfileValue& value) {
    return nlohmann::json{
        {"owner", value.owner},
        {"role", value.role},
        {"daily_work", value.daily_work},
        {"sources", value.sources},
        {"created", value.created},
        {"updated", value.updated},
    };
}

const nlohmann::json& workspace_profile_value_schema() { return schema_document(); }

bool validate_workspace_profile_value(const nlohmann::json& value,
                                      std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<WorkspaceProfileValue> parse_workspace_profile_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_workspace_profile_value(value, error)) return std::nullopt;

    // The schema has guaranteed types + the required non-empty `owner`, so the
    // extraction below is total: missing optional fields fall back to defaults.
    WorkspaceProfileValue parsed;
    parsed.owner = value.value("owner", std::string{});
    parsed.role = value.value("role", std::string{});
    parsed.daily_work = value.value("daily_work", std::string{});
    if (const auto it = value.find("sources"); it != value.end()) {
        parsed.sources = *it;
    }
    parsed.created = value.value("created", std::string{});
    parsed.updated = value.value("updated", std::string{});
    return parsed;
}

// ----- WorkspaceProfile ------------------------------------------------------

WorkspaceProfile::WorkspaceProfile(std::string id, WorkspaceProfileValue value,
                                   fs::path dir,
                                   zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {}

void WorkspaceProfile::set_content(std::string role, std::string daily_work,
                                   nlohmann::json sources, std::string updated) {
    value_.role = std::move(role);
    value_.daily_work = std::move(daily_work);
    value_.sources = std::move(sources);
    value_.updated = std::move(updated);
    capture_and_persist();
}

void WorkspaceProfile::apply_value(WorkspaceProfileValue value) {
    value_ = std::move(value);
    persist_meta();  // projection only — no capture (reconciliation path)
}

void WorkspaceProfile::capture_and_persist() {
    // CRDT first (the source of truth), then the on-disk projection.
    if (replication_) {
        replication_->set(catalog::workspace_profile_key(id_), metadata());
    }
    persist_meta();
}

nlohmann::json WorkspaceProfile::metadata() const { return to_json(value_); }

void WorkspaceProfile::persist_meta() const {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "workspace_profile.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::workspace
