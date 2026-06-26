#include "zetta/run.hpp"

#include "zetta/replication.hpp"
#include "zetta/timestamp.hpp"

#include "catalog.hpp"

#include <algorithm>
#include <exception>
#include <fstream>
#include <mutex>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta {
namespace fs = std::filesystem;

namespace {

// Keep a confidence score inside the [0, 1] range the schema (and the lineage
// contract) require, so our own writes always round-trip through validation.
double clamp_confidence(double confidence) {
    return std::clamp(confidence, 0.0, 1.0);
}

// The Run JSON Schema (draft-07). One declarative source of truth for the value
// shape carried by run.json (with an optional `id`) and the `run:<uuid>` CRDT
// value (without it). `status`/`kind` are closed enums and `confidence` is
// bounded, so malformed records fail validation at the boundary.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.Run",
      "type": "object",
      "properties": {
        "id":         { "type": "string" },
        "project":    { "type": "string" },
        "graph":      { "type": "string" },
        "deployment": { "type": "string" },
        "status":  { "type": "string",
                     "enum": ["pending", "running", "succeeded", "failed"] },
        "created": { "type": "string" },
        "updated": { "type": "string" },
        "logs": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "time":    { "type": "string" },
              "level":   { "type": "string" },
              "message": { "type": "string" }
            },
            "required": ["message"],
            "additionalProperties": false
          }
        },
        "outputs": {
          "type": "array",
          "items": {
            "type": "object",
            "properties": {
              "kind": { "type": "string",
                        "enum": ["dataset", "model", "index"] },
              "name": { "type": "string" },
              "ref":  { "type": "string" }
            },
            "required": ["kind"],
            "additionalProperties": false
          }
        },
        "lineage": {
          "type": "object",
          "properties": {
            "sources":    { "type": "array", "items": { "type": "string" } },
            "steps":      { "type": "array", "items": { "type": "string" } },
            "freshness":  { "type": "string" },
            "confidence": { "type": "number", "minimum": 0, "maximum": 1 }
          },
          "additionalProperties": false
        }
      },
      "required": ["status"],
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

std::string_view to_string(RunStatus status) {
    switch (status) {
        case RunStatus::Pending:   return "pending";
        case RunStatus::Running:   return "running";
        case RunStatus::Succeeded: return "succeeded";
        case RunStatus::Failed:    return "failed";
    }
    return "pending";
}

std::optional<RunStatus> run_status_from_string(std::string_view name) {
    if (name == "pending")   return RunStatus::Pending;
    if (name == "running")   return RunStatus::Running;
    if (name == "succeeded") return RunStatus::Succeeded;
    if (name == "failed")    return RunStatus::Failed;
    return std::nullopt;
}

nlohmann::json to_json(const RunValue& value) {
    nlohmann::json logs = nlohmann::json::array();
    for (const RunLogEntry& entry : value.logs) {
        logs.push_back({{"time", entry.time},
                        {"level", entry.level},
                        {"message", entry.message}});
    }
    nlohmann::json outputs = nlohmann::json::array();
    for (const RunOutput& output : value.outputs) {
        outputs.push_back({{"kind", output.kind},
                           {"name", output.name},
                           {"ref", output.ref}});
    }
    return nlohmann::json{
        {"project", value.project},
        {"graph", value.graph},
        {"deployment", value.deployment},
        {"status", to_string(value.status)},
        {"created", value.created},
        {"updated", value.updated},
        {"logs", std::move(logs)},
        {"outputs", std::move(outputs)},
        {"lineage",
         {{"sources", value.lineage.sources},
          {"steps", value.lineage.steps},
          {"freshness", value.lineage.freshness},
          {"confidence", clamp_confidence(value.lineage.confidence)}}}};
}

const nlohmann::json& run_value_schema() { return schema_document(); }

bool validate_run_value(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<RunValue> parse_run_value(const nlohmann::json& value,
                                        std::string* error) {
    if (!validate_run_value(value, error)) return std::nullopt;

    // The schema has already guaranteed types + the status/kind enums, so the
    // extraction below is total: missing optional fields fall back to defaults.
    RunValue parsed;
    parsed.project = value.value("project", std::string{});
    parsed.graph = value.value("graph", std::string{});
    parsed.deployment = value.value("deployment", std::string{});
    parsed.status =
        run_status_from_string(value.value("status", std::string{"pending"}))
            .value_or(RunStatus::Pending);
    parsed.created = value.value("created", std::string{});
    parsed.updated = value.value("updated", std::string{});

    if (const auto logs = value.find("logs"); logs != value.end()) {
        for (const auto& entry : *logs) {
            parsed.logs.push_back({entry.value("time", std::string{}),
                                   entry.value("level", std::string{}),
                                   entry.value("message", std::string{})});
        }
    }
    if (const auto outputs = value.find("outputs"); outputs != value.end()) {
        for (const auto& output : *outputs) {
            parsed.outputs.push_back({output.value("kind", std::string{}),
                                      output.value("name", std::string{}),
                                      output.value("ref", std::string{})});
        }
    }
    if (const auto lineage = value.find("lineage"); lineage != value.end()) {
        parsed.lineage.sources =
            lineage->value("sources", std::vector<std::string>{});
        parsed.lineage.steps =
            lineage->value("steps", std::vector<std::string>{});
        parsed.lineage.freshness = lineage->value("freshness", std::string{});
        parsed.lineage.confidence =
            clamp_confidence(lineage->value("confidence", 0.0));
    }
    return parsed;
}

// ----- Run -------------------------------------------------------------------

Run::Run(std::string id, RunValue value, fs::path dir,
         ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {
    value_.lineage.confidence = clamp_confidence(value_.lineage.confidence);
}

void Run::set_status(RunStatus status) {
    value_.status = status;
    capture_and_persist();
}

void Run::add_log(RunLogEntry entry) {
    value_.logs.push_back(std::move(entry));
    capture_and_persist();
}

void Run::set_outputs(std::vector<RunOutput> outputs) {
    value_.outputs = std::move(outputs);
    capture_and_persist();
}

void Run::set_lineage(RunLineage lineage) {
    lineage.confidence = clamp_confidence(lineage.confidence);
    value_.lineage = std::move(lineage);
    capture_and_persist();
}

void Run::apply_value(RunValue value) {
    value.lineage.confidence = clamp_confidence(value.lineage.confidence);
    value_ = std::move(value);
    persist_meta();  // projection only — no capture (avoids a feedback loop)
}

void Run::capture_and_persist() {
    value_.updated = now_iso8601();
    // CRDT first (the source of truth), then the on-disk projection.
    if (replication_) replication_->set(catalog::run_key(id_), metadata());
    persist_meta();
}

nlohmann::json Run::metadata() const { return to_json(value_); }

void Run::persist_meta() const {
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "run.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta
