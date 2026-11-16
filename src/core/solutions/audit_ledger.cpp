// SPDX-License-Identifier: MIT

#include "zetta/solutions/audit_ledger.hpp"

#include <fstream>
#include <utility>

#include <nlohmann/json-schema.hpp>

namespace zetta::solutions {
namespace fs = std::filesystem;

namespace {

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "zetta.AuditLedgerEntry",
      "description": "One immutable action or outcome in the solutions reward ledger (#1107).",
      "oneOf": [
        {
          "type": "object",
          "required": ["kind", "subject", "score_id", "score", "decision",
                       "action", "actor_kind", "actor", "model_version",
                       "policy_version", "agent_version", "cell", "cost",
                       "currency", "score_at", "decision_at", "action_at",
                       "recorded_at"],
          "additionalProperties": false,
          "properties": {
            "id": { "type": "string" },
            "kind": { "const": "action" },
            "subject": { "type": "string", "minLength": 1 },
            "score_id": { "type": "string", "minLength": 1 },
            "score": { "type": "number" },
            "decision": { "type": "string", "minLength": 1 },
            "action": { "type": "string", "minLength": 1 },
            "actor_kind": { "type": "string", "enum": ["agent", "human"] },
            "actor": { "type": "string", "minLength": 1 },
            "model_version": { "type": "string", "minLength": 1 },
            "policy_version": { "type": "string", "minLength": 1 },
            "agent_version": { "type": "string", "minLength": 1 },
            "cell": { "type": "string", "minLength": 1 },
            "cost": { "type": "number", "minimum": 0 },
            "currency": { "type": "string", "minLength": 1 },
            "score_at": { "type": "string", "minLength": 1 },
            "decision_at": { "type": "string", "minLength": 1 },
            "action_at": { "type": "string", "minLength": 1 },
            "recorded_at": { "type": "string", "minLength": 1 }
          }
        },
        {
          "type": "object",
          "required": ["kind", "action_id", "outcome", "value", "currency",
                       "source", "occurred_at", "recorded_at"],
          "additionalProperties": false,
          "properties": {
            "id": { "type": "string" },
            "kind": { "const": "outcome" },
            "action_id": { "type": "string", "minLength": 1 },
            "outcome": { "type": "string", "minLength": 1 },
            "value": { "type": "number" },
            "currency": { "type": "string", "minLength": 1 },
            "source": { "type": "string", "enum": ["connector", "manual"] },
            "occurred_at": { "type": "string", "minLength": 1 },
            "recorded_at": { "type": "string", "minLength": 1 }
          }
        }
      ]
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator(schema_document());
    return validator;
}

class CollectingErrorHandler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&,
               const std::string& message) override {
        if (failed) {
            return;
        }
        failed = true;
        const std::string location = pointer.to_string();
        reason = (location.empty() ? "/" : location) + ": " + message;
    }

    bool failed = false;
    std::string reason;
};

}  // namespace

std::string_view to_string(LedgerEntryKind kind) {
    switch (kind) {
        case LedgerEntryKind::Action:
            return "action";
        case LedgerEntryKind::Outcome:
            return "outcome";
    }
    return "action";
}

std::string_view to_string(ActionActorKind kind) {
    switch (kind) {
        case ActionActorKind::Agent:
            return "agent";
        case ActionActorKind::Human:
            return "human";
    }
    return "agent";
}

std::string_view to_string(OutcomeSource source) {
    switch (source) {
        case OutcomeSource::Connector:
            return "connector";
        case OutcomeSource::Manual:
            return "manual";
    }
    return "manual";
}

std::optional<LedgerEntryKind> ledger_entry_kind_from(std::string_view name) {
    if (name == "action") {
        return LedgerEntryKind::Action;
    }
    if (name == "outcome") {
        return LedgerEntryKind::Outcome;
    }
    return std::nullopt;
}

std::optional<ActionActorKind> action_actor_kind_from(std::string_view name) {
    if (name == "agent") {
        return ActionActorKind::Agent;
    }
    if (name == "human") {
        return ActionActorKind::Human;
    }
    return std::nullopt;
}

std::optional<OutcomeSource> outcome_source_from(std::string_view name) {
    if (name == "connector") {
        return OutcomeSource::Connector;
    }
    if (name == "manual") {
        return OutcomeSource::Manual;
    }
    return std::nullopt;
}

nlohmann::json to_json(const ActionLedgerValue& value) {
    return nlohmann::json{
        {"kind", "action"},
        {"subject", value.subject},
        {"score_id", value.score_id},
        {"score", value.score},
        {"decision", value.decision},
        {"action", value.action},
        {"actor_kind", to_string(value.actor_kind)},
        {"actor", value.actor},
        {"model_version", value.model_version},
        {"policy_version", value.policy_version},
        {"agent_version", value.agent_version},
        {"cell", value.cell},
        {"cost", value.cost},
        {"currency", value.currency},
        {"score_at", value.score_at},
        {"decision_at", value.decision_at},
        {"action_at", value.action_at},
        {"recorded_at", value.recorded_at}};
}

nlohmann::json to_json(const OutcomeLedgerValue& value) {
    return nlohmann::json{{"kind", "outcome"},
                          {"action_id", value.action_id},
                          {"outcome", value.outcome},
                          {"value", value.value},
                          {"currency", value.currency},
                          {"source", to_string(value.source)},
                          {"occurred_at", value.occurred_at},
                          {"recorded_at", value.recorded_at}};
}

nlohmann::json to_json(const LedgerEntryValue& value) {
    return std::visit([](const auto& entry) {
        return to_json(entry);
    }, value);
}

const nlohmann::json& audit_ledger_entry_schema() {
    return schema_document();
}

bool validate_audit_ledger_entry(const nlohmann::json& value,
                                 std::string* error) {
    CollectingErrorHandler handler;
    schema_validator().validate(value, handler);
    if (!handler.failed) {
        return true;
    }
    if (error != nullptr) {
        *error = handler.reason;
    }
    return false;
}

std::optional<LedgerEntryValue> parse_audit_ledger_entry(
    const nlohmann::json& value, std::string* error) {
    if (!validate_audit_ledger_entry(value, error)) {
        return std::nullopt;
    }
    const LedgerEntryKind kind = ledger_entry_kind_from(
        value.at("kind").get<std::string>()).value();
    if (kind == LedgerEntryKind::Action) {
        ActionLedgerValue parsed;
        parsed.subject = value.at("subject").get<std::string>();
        parsed.score_id = value.at("score_id").get<std::string>();
        parsed.score = value.at("score").get<double>();
        parsed.decision = value.at("decision").get<std::string>();
        parsed.action = value.at("action").get<std::string>();
        parsed.actor_kind = action_actor_kind_from(
            value.at("actor_kind").get<std::string>()).value();
        parsed.actor = value.at("actor").get<std::string>();
        parsed.model_version = value.at("model_version").get<std::string>();
        parsed.policy_version = value.at("policy_version").get<std::string>();
        parsed.agent_version = value.at("agent_version").get<std::string>();
        parsed.cell = value.at("cell").get<std::string>();
        parsed.cost = value.at("cost").get<double>();
        parsed.currency = value.at("currency").get<std::string>();
        parsed.score_at = value.at("score_at").get<std::string>();
        parsed.decision_at = value.at("decision_at").get<std::string>();
        parsed.action_at = value.at("action_at").get<std::string>();
        parsed.recorded_at = value.at("recorded_at").get<std::string>();
        return LedgerEntryValue{std::move(parsed)};
    }

    OutcomeLedgerValue parsed;
    parsed.action_id = value.at("action_id").get<std::string>();
    parsed.outcome = value.at("outcome").get<std::string>();
    parsed.value = value.at("value").get<double>();
    parsed.currency = value.at("currency").get<std::string>();
    parsed.source = outcome_source_from(
        value.at("source").get<std::string>()).value();
    parsed.occurred_at = value.at("occurred_at").get<std::string>();
    parsed.recorded_at = value.at("recorded_at").get<std::string>();
    return LedgerEntryValue{std::move(parsed)};
}

LedgerEntry::LedgerEntry(std::string id, LedgerEntryValue value, fs::path dir)
    : id_(std::move(id)), value_(std::move(value)), dir_(std::move(dir)) {
}

LedgerEntryKind LedgerEntry::kind() const {
    if (std::holds_alternative<ActionLedgerValue>(value_)) {
        return LedgerEntryKind::Action;
    }
    return LedgerEntryKind::Outcome;
}

std::string_view LedgerEntry::recorded_at() const {
    return std::visit([](const auto& entry) -> std::string_view {
        return entry.recorded_at;
    }, value_);
}

std::string_view LedgerEntry::action_id() const {
    const auto* outcome = std::get_if<OutcomeLedgerValue>(&value_);
    if (outcome == nullptr) {
        return {};
    }
    return outcome->action_id;
}

std::string_view LedgerEntry::subject() const {
    const auto* action = std::get_if<ActionLedgerValue>(&value_);
    if (action == nullptr) {
        return {};
    }
    return action->subject;
}

std::string_view LedgerEntry::cell() const {
    const auto* action = std::get_if<ActionLedgerValue>(&value_);
    if (action == nullptr) {
        return {};
    }
    return action->cell;
}

nlohmann::json LedgerEntry::metadata() const {
    return to_json(value_);
}

void LedgerEntry::persist_meta() const {
    nlohmann::json document = metadata();
    document["id"] = id_;
    std::ofstream output(file(), std::ios::trunc);
    output << document.dump(2) << '\n';
}

}  // namespace zetta::solutions
