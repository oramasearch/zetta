#include "zetta/audit_event.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta {
namespace fs = std::filesystem;

namespace {

// The AuditEvent JSON Schema (draft-07). One declarative source of truth for the
// value shape carried by audit/<uuid>.json (with an optional `id`) and the
// `audit:<uuid>` CRDT value (without it). `action` is a closed enum and the
// shape is sealed (additionalProperties: false), so a malformed record fails
// validation at the boundary instead of corrupting the trail. Mirrors
// schemas/audit_event.schema.json — keep the two in sync.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.AuditEvent",
      "type": "object",
      "properties": {
        "id":      { "type": "string" },
        "action":  { "type": "string",
                     "enum": ["graph_created", "graph_dropped",
                              "graph_deployed", "data_access",
                              "job_created", "job_finished",
                              "key_issued", "key_revoked",
                              "provision_requested"] },
        "actor":   { "type": "string" },
        "target":  { "type": "string" },
        "allowed": { "type": "boolean" },
        "detail":  { "type": "string" },
        "time":    { "type": "string" }
      },
      "required": ["action", "time"],
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

std::string_view to_string(AuditAction action) {
    switch (action) {
        case AuditAction::GraphCreated:  return "graph_created";
        case AuditAction::GraphDropped:  return "graph_dropped";
        case AuditAction::GraphDeployed: return "graph_deployed";
        case AuditAction::DataAccess:    return "data_access";
        case AuditAction::JobCreated:    return "job_created";
        case AuditAction::JobFinished:   return "job_finished";
        case AuditAction::KeyIssued:     return "key_issued";
        case AuditAction::KeyRevoked:    return "key_revoked";
        case AuditAction::ProvisionRequested: return "provision_requested";
    }
    return "data_access";
}

std::optional<AuditAction> audit_action_from(std::string_view name) {
    if (name == "graph_created")  return AuditAction::GraphCreated;
    if (name == "graph_dropped")  return AuditAction::GraphDropped;
    if (name == "graph_deployed") return AuditAction::GraphDeployed;
    if (name == "data_access")    return AuditAction::DataAccess;
    if (name == "job_created")    return AuditAction::JobCreated;
    if (name == "job_finished")   return AuditAction::JobFinished;
    if (name == "key_issued")     return AuditAction::KeyIssued;
    if (name == "key_revoked")    return AuditAction::KeyRevoked;
    if (name == "provision_requested") return AuditAction::ProvisionRequested;
    return std::nullopt;
}

nlohmann::json to_json(const AuditEventValue& value) {
    return nlohmann::json{{"action", to_string(value.action)},
                          {"actor", value.actor},
                          {"target", value.target},
                          {"allowed", value.allowed},
                          {"detail", value.detail},
                          {"time", value.time}};
}

const nlohmann::json& audit_event_value_schema() { return schema_document(); }

bool validate_audit_event_value(const nlohmann::json& value,
                                std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<AuditEventValue> parse_audit_event_value(
    const nlohmann::json& value, std::string* error) {
    if (!validate_audit_event_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the action enum, so extraction
    // is total: missing optional fields fall back to defaults.
    AuditEventValue parsed;
    parsed.action =
        audit_action_from(value.value("action", std::string{"data_access"}))
            .value_or(AuditAction::DataAccess);
    parsed.actor = value.value("actor", std::string{});
    parsed.target = value.value("target", std::string{});
    parsed.allowed = value.value("allowed", true);
    parsed.detail = value.value("detail", std::string{});
    parsed.time = value.value("time", std::string{});
    return parsed;
}

// ----- AuditEvent ------------------------------------------------------------

AuditEvent::AuditEvent(std::string id, AuditEventValue value, fs::path dir)
    : id_(std::move(id)), value_(std::move(value)), dir_(std::move(dir)) {}

nlohmann::json AuditEvent::metadata() const { return to_json(value_); }

void AuditEvent::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(file(), std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta
