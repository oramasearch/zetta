#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/access_key.hpp"  // AccessDecision

// Remote consent for cloud-touching actions (#621; docs/api/consent.md,
// docs/api/registry-design.md → Consent). A `requires_consent` capability
// NEVER executes on first call: it returns a structured **consent challenge**
// naming exactly what would be read/run (the Cloud Inspector grammar) plus a
// one-time challenge id. Confirmation must come from a **different,
// approval-capable principal** — an approver key, or a human in the headed
// app's approval surface — the calling key repeating the call can never
// satisfy it. Approvals are per-action, NEVER remembered, audited on both
// sides.
//
// This header is the governance half: the challenge record and the ledger
// that holds it. The protocol half (issuing challenges, enforcing the
// distinctly-privileged approval, executing the approved action exactly once)
// lives on the capability registry (include/zetta/api/registry.hpp).
namespace zetta::governance {

// What a consent-gated action WOULD do, named up front so consent is informed
// — the same grammar the headed Cloud Inspector's gate shows (config paths the
// provider CLI may read; the exact commands / action that would run). zetta
// itself never reads or prints the named config files; they are listed so the
// approver sees what the CLI may touch.
struct ConsentAction {
    std::string summary;             // one human sentence: what + the cost/side effect
    std::vector<std::string> reads;  // config/credential paths the provider CLI may read
    std::vector<std::string> runs;   // the read-only / billable commands that would run

    bool operator==(const ConsentAction&) const = default;
};

// The lifecycle of one challenge. `Expired` is computed lazily against the
// challenge's TTL — an expired challenge can never be approved. There is no
// "approved and reusable" state by design: approval consumes the challenge
// (per-action, never remembered).
enum class ConsentStatus {
    Pending,   // issued, awaiting a distinctly-privileged decision
    Denied,    // refused (or withdrawn); the action never ran
    Approved,  // approval consumed the challenge; the action is executing
    Executed,  // the named action ran exactly once; `result` holds its output
    Failed,    // approved, but the action's execution failed; `detail` says why
    Expired,   // the TTL elapsed before a decision (computed, never stored)
};

std::string_view to_string(ConsentStatus status);

// One consent challenge: the frozen request (capability + validated input),
// the named action, who asked, and the decision trail. The `input` is frozen
// at challenge time so approval executes **exactly** the named action — a
// caller cannot swap the payload between challenge and approval.
struct ConsentChallenge {
    std::string id;                   // one-time challenge id (uuid)
    std::string capability;           // the registry entry this gates
    nlohmann::json input;             // the frozen, schema-validated input
    ConsentAction action;             // exactly what would be read/run
    std::string requested_by;         // acting AccessKey id; empty = the local session
    std::string requested_principal;  // the requesting key's principal id; empty = local
    // `requested_by` is a token-resolved USER id, not an AccessKey id (#999):
    // approval re-authorizes and executes as that user, through the user path.
    bool requested_is_user = false;
    std::string created_at;           // ISO-8601 UTC
    std::string expires_at;           // ISO-8601 UTC; pending past this = expired
    ConsentStatus status = ConsentStatus::Pending;
    std::string decided_by;           // approver key id, or "headed app"
    std::string decided_at;           // ISO-8601 UTC of the decision
    nlohmann::json result;            // the executed action's output (Executed only)
    std::string detail;               // deny reason / execution failure / notes

    // The status this challenge reports as of `now` — `Pending` past its
    // expiry reads as `Expired`; decided states are unaffected by time.
    ConsentStatus status_at(std::string_view now) const;
};

// Serialize a challenge to its JSON shape — the structured consent request the
// first call returns, the `system.consent_requests` row, and the shape the
// embedded schema validates. The frozen `input` rides along so the approver
// sees the exact payload that would execute.
nlohmann::json to_json(const ConsentChallenge& challenge);

// The ConsentChallenge JSON Schema (draft-07) — schema-as-code, the single
// source of truth for the challenge shape. Mirrors
// schemas/consent_challenge.schema.json (generated + drift-checked).
const nlohmann::json& consent_challenge_schema();

// Validate a challenge JSON value against the schema. The challenge is
// produced locally (never parsed from untrusted input — it is ephemeral and
// never replicated), so this is the contract check for to_json + tests.
bool validate_consent_challenge(const nlohmann::json& value,
                                std::string* error = nullptr);

// The per-node ledger of consent challenges. **Ephemeral, owner-thread-only,
// and never persisted or replicated** — consent is per-action and never
// remembered (the cloud hard rule), so challenges deliberately do not survive
// a restart and never gossip to peers. The Node owns one
// (Node::consent_ledger()); `system.consent_requests` and the headed approval
// surface read it live (RULE-7).
class ConsentLedger {
public:
    // `time_to_live_seconds` bounds how long a pending challenge stays
    // approvable; `max_entries` caps the ledger (oldest *decided* challenges
    // are dropped first, then oldest pending — a request flood cannot grow it
    // unboundedly).
    explicit ConsentLedger(long long time_to_live_seconds = 600,
                           std::size_t max_entries = 256);

    // Issue a new challenge for (capability, input, action) from
    // `requested_by` / `requested_principal`. Stamps a fresh one-time id,
    // created_at = now, expires_at = now + TTL. Returns the stored challenge.
    std::shared_ptr<const ConsentChallenge> create(std::string capability,
                                                   nlohmann::json input,
                                                   ConsentAction action,
                                                   std::string requested_by,
                                                   std::string requested_principal,
                                                   bool requested_is_user = false);

    std::shared_ptr<const ConsentChallenge> find(const std::string& id) const;

    // Every challenge, oldest first (the system.consent_requests source).
    std::vector<std::shared_ptr<const ConsentChallenge>> all() const;

    // Pending, unexpired challenges — what an approval surface lists.
    std::vector<std::shared_ptr<const ConsentChallenge>> pending() const;

    // Refuse a pending challenge. Allowed while pending and unexpired;
    // records who decided and why. The action never runs.
    AccessDecision deny(const std::string& id, std::string decided_by,
                        std::string reason);

    // Consume a pending, unexpired challenge for execution: Pending →
    // Approved, exactly once. Any other state (already decided, expired,
    // unknown id) is refused with the reason — this is the one-time-use
    // guarantee (a replayed approval can never execute twice).
    AccessDecision consume_for_execution(const std::string& id,
                                         std::string decided_by);

    // Record the executed action's outcome on an Approved challenge:
    // Approved → Executed (ok) or Failed (!ok, `detail` = the engine reason).
    void record_outcome(const std::string& id, bool ok, nlohmann::json result,
                        std::string detail);

    std::size_t size() const { return entries_.size(); }

private:
    std::shared_ptr<ConsentChallenge> find_mutable(const std::string& id);
    void enforce_capacity();

    long long time_to_live_seconds_;
    std::size_t max_entries_;
    std::vector<std::shared_ptr<ConsentChallenge>> entries_;  // insertion order
};

}  // namespace zetta::governance
