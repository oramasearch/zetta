// SPDX-License-Identifier: MIT

#include "zetta/billing/plan.hpp"

#include "zetta/cluster/replication.hpp"

#include "../catalog.hpp"

#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

namespace zetta::billing {
namespace fs = std::filesystem;

namespace {

// The Plan JSON Schema (draft-07). `name` is required + non-empty; `quotas` is a
// free-form object of numeric limits — so a malformed record fails validation at
// the boundary.
const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.Plan",
      "type": "object",
      "properties": {
        "id":           { "type": "string" },
        "name":         { "type": "string", "minLength": 1 },
        "tier":         { "type": "string" },
        "quotas": {
          "type": "object",
          "additionalProperties": { "type": "number" }
        },
        "period_start": { "type": "string" },
        "created":      { "type": "string" },
        "updated":      { "type": "string" }
      },
      "required": ["name"],
      "additionalProperties": false
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

nlohmann::json to_json(const PlanValue& value) {
    return nlohmann::json{
        {"name", value.name},
        {"tier", value.tier},
        {"quotas", value.quotas},
        {"period_start", value.period_start},
        {"created", value.created},
        {"updated", value.updated},
    };
}

const nlohmann::json& plan_value_schema() { return schema_document(); }

bool validate_plan_value(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<PlanValue> parse_plan_value(const nlohmann::json& value,
                                          std::string* error) {
    if (!validate_plan_value(value, error)) return std::nullopt;
    PlanValue parsed;
    parsed.name = value.value("name", std::string{});
    parsed.tier = value.value("tier", std::string{});
    if (const auto it = value.find("quotas"); it != value.end()) {
        parsed.quotas = *it;
    }
    parsed.period_start = value.value("period_start", std::string{});
    parsed.created = value.value("created", std::string{});
    parsed.updated = value.value("updated", std::string{});
    return parsed;
}

// ----- Plan ------------------------------------------------------------------

Plan::Plan(std::string id, PlanValue value, fs::path dir,
           zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {}

void Plan::set_content(std::string name, std::string tier, nlohmann::json quotas,
                       std::string period_start, std::string updated) {
    value_.name = std::move(name);
    value_.tier = std::move(tier);
    value_.quotas = std::move(quotas);
    value_.period_start = std::move(period_start);
    value_.updated = std::move(updated);
    capture_and_persist();
}

void Plan::apply_value(PlanValue value) {
    value_ = std::move(value);
    persist_meta();
}

void Plan::capture_and_persist() {
    if (replication_) replication_->set(catalog::plan_key(id_), metadata());
    persist_meta();
}

nlohmann::json Plan::metadata() const { return to_json(value_); }

void Plan::persist_meta() const {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    nlohmann::json meta = metadata();
    meta["id"] = id_;
    std::ofstream out(dir_ / "plan.json", std::ios::trunc);
    out << meta.dump(2) << '\n';
}

}  // namespace zetta::billing
