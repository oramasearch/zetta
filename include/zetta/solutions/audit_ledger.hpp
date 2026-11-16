// SPDX-License-Identifier: MIT

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <nlohmann/json.hpp>

namespace zetta::solutions {

enum class LedgerEntryKind { Action, Outcome };
enum class ActionActorKind { Agent, Human };
enum class OutcomeSource { Connector, Manual };

std::string_view to_string(LedgerEntryKind kind);
std::string_view to_string(ActionActorKind kind);
std::string_view to_string(OutcomeSource source);

std::optional<LedgerEntryKind> ledger_entry_kind_from(std::string_view name);
std::optional<ActionActorKind> action_actor_kind_from(std::string_view name);
std::optional<OutcomeSource> outcome_source_from(std::string_view name);

// One immutable score -> decision -> action record. `action` is intentionally
// open vocabulary so a deliberate `no_contact` is recorded like any other
// action instead of disappearing from the reward stream.
struct ActionLedgerValue {
    std::string subject;
    std::string score_id;
    double score = 0.0;
    std::string decision;
    std::string action;
    ActionActorKind actor_kind = ActionActorKind::Agent;
    std::string actor;
    std::string model_version;
    std::string policy_version;
    std::string agent_version;
    std::string cell;
    double cost = 0.0;
    std::string currency;
    std::string score_at;
    std::string decision_at;
    std::string action_at;
    std::string recorded_at;
};

// One immutable outcome observation. Multiple connector or manual observations
// may reference the same action; each is appended as its own record.
struct OutcomeLedgerValue {
    std::string action_id;
    std::string outcome;
    double value = 0.0;
    std::string currency;
    OutcomeSource source = OutcomeSource::Manual;
    std::string occurred_at;
    std::string recorded_at;
};

using LedgerEntryValue = std::variant<ActionLedgerValue, OutcomeLedgerValue>;

nlohmann::json to_json(const ActionLedgerValue& value);
nlohmann::json to_json(const OutcomeLedgerValue& value);
nlohmann::json to_json(const LedgerEntryValue& value);

// The embedded schema is the source of truth for both action and outcome
// records. It validates disk and CRDT input before either enters the node tree.
const nlohmann::json& audit_ledger_entry_schema();
bool validate_audit_ledger_entry(const nlohmann::json& value,
                                 std::string* error = nullptr);
std::optional<LedgerEntryValue> parse_audit_ledger_entry(
    const nlohmann::json& value, std::string* error = nullptr);

// An immutable append-only projection of one `ledger:<uuid>` CRDT entry. There
// are deliberately no mutators and no delete API. Contract:
// docs/solutions/audit-ledger.md (#1107).
class LedgerEntry {
public:
    LedgerEntry(std::string id, LedgerEntryValue value,
                std::filesystem::path dir);

    const std::string& id() const {
        return id_;
    }
    LedgerEntryKind kind() const;
    const LedgerEntryValue& value() const {
        return value_;
    }
    std::string_view recorded_at() const;
    std::string_view action_id() const;
    std::string_view subject() const;
    std::string_view cell() const;

    const std::filesystem::path& dir() const {
        return dir_;
    }
    std::filesystem::path file() const {
        return dir_ / (id_ + ".json");
    }

    nlohmann::json metadata() const;
    void persist_meta() const;

private:
    std::string id_;
    LedgerEntryValue value_;
    std::filesystem::path dir_;
};

}  // namespace zetta::solutions
