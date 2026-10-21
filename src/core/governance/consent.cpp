#include "zetta/governance/consent.hpp"

#include <algorithm>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/governance/credential_redaction.hpp"
#include "zetta/util/timestamp.hpp"
#include "zetta/util/uuid.hpp"

namespace zetta::governance {

std::string_view to_string(ConsentStatus status) {
    switch (status) {
        case ConsentStatus::Pending: return "pending";
        case ConsentStatus::Denied: return "denied";
        case ConsentStatus::Approved: return "approved";
        case ConsentStatus::Executed: return "executed";
        case ConsentStatus::Failed: return "failed";
        case ConsentStatus::Expired: return "expired";
    }
    return "pending";
}

ConsentStatus ConsentChallenge::status_at(std::string_view now) const {
    // Stamps are fixed-width ISO-8601 UTC, so chronology is a plain
    // lexicographic compare (the Capability::expired convention).
    if (status == ConsentStatus::Pending && !expires_at.empty() &&
        now >= expires_at) {
        return ConsentStatus::Expired;
    }
    return status;
}

nlohmann::json to_json(const ConsentChallenge& challenge) {
    nlohmann::json value;
    value["id"] = challenge.id;
    value["capability"] = challenge.capability;
    // The approver sees the exact payload — except a credential given inline
    // (a connector's `token`), which the challenge holds in memory to execute
    // but never renders: the listing reaches a different principal, and the
    // refusal is returned and logged like any error (#74).
    value["input"] = redact_credentials(challenge.input);
    value["action"] = {{"summary", challenge.action.summary},
                       {"reads", challenge.action.reads},
                       {"runs", challenge.action.runs}};
    value["requested_by"] =
        challenge.requested_by.empty() ? "local" : challenge.requested_by;
    if (!challenge.requested_principal.empty()) {
        value["requested_principal"] = challenge.requested_principal;
    }
    value["created_at"] = challenge.created_at;
    value["expires_at"] = challenge.expires_at;
    value["status"] =
        std::string(to_string(challenge.status_at(zetta::util::now_iso8601())));
    if (!challenge.decided_by.empty()) value["decided_by"] = challenge.decided_by;
    if (!challenge.decided_at.empty()) value["decided_at"] = challenge.decided_at;
    if (!challenge.result.is_null()) {
        value["result"] = redact_credentials(challenge.result);
    }
    if (!challenge.detail.empty()) value["detail"] = challenge.detail;
    return value;
}

const nlohmann::json& consent_challenge_schema() {
    // Schema-as-code: this accessor is the source of truth;
    // schemas/consent_challenge.schema.json is generated from it and
    // drift-checked by tests/test_schemas.cpp.
    static const nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "ConsentChallenge"},
        {"description",
         "One remote consent challenge for a cloud-touching capability "
         "(#621): the frozen request, exactly what would be read/run, and "
         "the decision trail. Ephemeral and per-node — never replicated, "
         "never remembered."},
        {"type", "object"},
        {"required", nlohmann::json::array({"id", "capability", "input",
                                            "action", "requested_by",
                                            "created_at", "expires_at",
                                            "status"})},
        {"additionalProperties", false},
        {"properties",
         {{"id", {{"type", "string"}, {"minLength", 1}}},
          {"capability", {{"type", "string"}, {"minLength", 1}}},
          {"input",
           {{"description",
             "The frozen, schema-validated input approval would execute"}}},
          {"action",
           {{"type", "object"},
            {"required", nlohmann::json::array({"summary", "reads", "runs"})},
            {"additionalProperties", false},
            {"properties",
             {{"summary", {{"type", "string"}, {"minLength", 1}}},
              {"reads",
               {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"runs",
               {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}},
          {"requested_by", {{"type", "string"}, {"minLength", 1}}},
          {"requested_principal", {{"type", "string"}}},
          {"created_at", {{"type", "string"}}},
          {"expires_at", {{"type", "string"}}},
          {"status",
           {{"type", "string"},
            {"enum", nlohmann::json::array({"pending", "denied", "approved",
                                            "executed", "failed",
                                            "expired"})}}},
          {"decided_by", {{"type", "string"}}},
          {"decided_at", {{"type", "string"}}},
          {"result", {{"description", "The executed action's output"}}},
          {"detail", {{"type", "string"}}}}}};
    return schema;
}

namespace {

struct collecting_error_handler : nlohmann::json_schema::error_handler {
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&, const std::string& message) override {
        if (failed) return;
        failed = true;
        reason = pointer.to_string().empty()
                     ? message
                     : pointer.to_string() + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

nlohmann::json_schema::json_validator& schema_validator() {
    static nlohmann::json_schema::json_validator validator = [] {
        nlohmann::json_schema::json_validator built;
        built.set_root_schema(consent_challenge_schema());
        return built;
    }();
    return validator;
}

}  // namespace

bool validate_consent_challenge(const nlohmann::json& value,
                                std::string* error) {
    collecting_error_handler handler;
    schema_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

// ----- ConsentLedger ---------------------------------------------------------

ConsentLedger::ConsentLedger(long long time_to_live_seconds,
                             std::size_t max_entries)
    : time_to_live_seconds_(time_to_live_seconds), max_entries_(max_entries) {}

std::shared_ptr<const ConsentChallenge> ConsentLedger::create(
    std::string capability, nlohmann::json input, ConsentAction action,
    std::string requested_by, std::string requested_principal) {
    auto challenge = std::make_shared<ConsentChallenge>();
    challenge->id = zetta::util::new_uuid();
    challenge->capability = std::move(capability);
    challenge->input = std::move(input);
    challenge->action = std::move(action);
    challenge->requested_by = std::move(requested_by);
    challenge->requested_principal = std::move(requested_principal);
    challenge->created_at = zetta::util::now_iso8601();
    // iso8601_seconds_ago with a negative offset lands the stamp TTL seconds
    // in the future — the expiry cutoff, compared lexicographically.
    challenge->expires_at =
        zetta::util::iso8601_seconds_ago(-time_to_live_seconds_);
    entries_.push_back(challenge);
    enforce_capacity();
    return challenge;
}

std::shared_ptr<ConsentChallenge> ConsentLedger::find_mutable(
    const std::string& id) {
    auto found = std::find_if(entries_.begin(), entries_.end(),
                              [&](const auto& entry) { return entry->id == id; });
    return found == entries_.end() ? nullptr : *found;
}

std::shared_ptr<const ConsentChallenge> ConsentLedger::find(
    const std::string& id) const {
    auto found = std::find_if(entries_.begin(), entries_.end(),
                              [&](const auto& entry) { return entry->id == id; });
    return found == entries_.end() ? nullptr : *found;
}

std::vector<std::shared_ptr<const ConsentChallenge>> ConsentLedger::all() const {
    return {entries_.begin(), entries_.end()};
}

std::vector<std::shared_ptr<const ConsentChallenge>> ConsentLedger::pending()
    const {
    const std::string now = zetta::util::now_iso8601();
    std::vector<std::shared_ptr<const ConsentChallenge>> open;
    for (const auto& entry : entries_) {
        if (entry->status_at(now) == ConsentStatus::Pending) open.push_back(entry);
    }
    return open;
}

AccessDecision ConsentLedger::deny(const std::string& id,
                                   std::string decided_by, std::string reason) {
    auto challenge = find_mutable(id);
    if (!challenge) {
        return {false, "no consent challenge with id '" + id + "'"};
    }
    const std::string now = zetta::util::now_iso8601();
    const ConsentStatus current = challenge->status_at(now);
    if (current != ConsentStatus::Pending) {
        return {false, "consent challenge '" + id + "' is " +
                           std::string(to_string(current)) +
                           " and can no longer be denied"};
    }
    challenge->status = ConsentStatus::Denied;
    challenge->decided_by = std::move(decided_by);
    challenge->decided_at = now;
    challenge->detail = std::move(reason);
    return {true, "denied"};
}

AccessDecision ConsentLedger::consume_for_execution(const std::string& id,
                                                    std::string decided_by) {
    auto challenge = find_mutable(id);
    if (!challenge) {
        return {false, "no consent challenge with id '" + id + "'"};
    }
    const std::string now = zetta::util::now_iso8601();
    const ConsentStatus current = challenge->status_at(now);
    if (current != ConsentStatus::Pending) {
        // The one-time-use guarantee: an already-decided (or expired)
        // challenge can never execute (again).
        return {false, "consent challenge '" + id + "' is " +
                           std::string(to_string(current)) +
                           "; a challenge is approvable exactly once, while "
                           "pending and unexpired"};
    }
    challenge->status = ConsentStatus::Approved;
    challenge->decided_by = std::move(decided_by);
    challenge->decided_at = now;
    return {true, "approved"};
}

void ConsentLedger::record_outcome(const std::string& id, bool ok,
                                   nlohmann::json result, std::string detail) {
    auto challenge = find_mutable(id);
    if (!challenge || challenge->status != ConsentStatus::Approved) return;
    challenge->status = ok ? ConsentStatus::Executed : ConsentStatus::Failed;
    challenge->result = std::move(result);
    challenge->detail = std::move(detail);
}

void ConsentLedger::enforce_capacity() {
    if (entries_.size() <= max_entries_) return;
    // Drop the oldest decided challenge first; only when every entry is still
    // pending does the oldest pending one go (a challenge flood cannot pin
    // the ledger's memory).
    const std::string now = zetta::util::now_iso8601();
    auto decided = std::find_if(entries_.begin(), entries_.end(),
                                [&](const auto& entry) {
                                    return entry->status_at(now) !=
                                           ConsentStatus::Pending;
                                });
    entries_.erase(decided == entries_.end() ? entries_.begin() : decided);
}

}  // namespace zetta::governance
