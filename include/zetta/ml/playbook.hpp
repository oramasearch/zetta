// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The UC-1 churn **retention playbook** (FEAT-6, #397; part of the native
// distributed accelerated-compute pass for UC-1 churn, #264). This is the optional
// *use-case skin* on top of the generic engine: it turns the model's quantitative
// output — a [calibrated risk](gbdt_score.hpp) per account and the
// [per-entity dominant churn driver](tree_shap.hpp) — into a **CRM-ready playbook**
// a retention team acts on, plus an **outcome log** that closes the feedback loop.
//
// **Three layers, with a hard division of labor (the whole point):**
//
//  1. **Deterministic driver -> action -> owner mapping** — a `PlaybookPolicy` (the
//     editable driver dictionary) maps each churn driver (a feature name) to a
//     canonical recommended `play`, an owning role, and a **save-rate prior**
//     `P(save | this driver)`. Transparent + auditable: the same drivers always
//     yield the same play (the heuristic spirit of `compile_intent` /
//     `EntityResolver`).
//  2. **Expected-retained-value priority** — rank the at-risk accounts by
//     `P(churn) x ARR x P(save | dominant driver)`, computed **in code** so it is
//     auditable and never hallucinated. Ranking by `P(churn)` alone wastes effort on
//     cheap-to-lose accounts; this works the highest *recoverable* value first.
//  3. **A constrained narrator behind a seam** (`PlaybookNarrator`) — the narrator
//     writes the human narrative and *selects* the play within a **strict JSON
//     schema** (the production binding is **Claude**; see `ConstrainedNarrator`), but
//     it **never invents numbers**: the probability / ARR / priority are computed
//     above and are absent from the narrator's output schema, so it physically
//     cannot change them. There is always a deterministic **rules-engine fallback**
//     (`RulesEngineNarrator`), so the pipeline runs **end-to-end offline**, with no
//     API key, and is fully testable.
//
// **Numbers are computed; the narrator does only language + play selection.** That
// is what makes the output trustworthy. See docs/use-cases/playbook.md and
// docs/use-cases/uc1.md (4.4-4.7).
//
// Pure + deterministic (given a deterministic narrator) + non-throwing. It adds two
// JSON boundaries: the editable `PlaybookPolicy` (hand-written `from_json`, the
// `FeaturizeSpec` / `ScalingPolicy` pattern, schemas/playbook_policy.schema.json) and
// the strict narrator-output contract (`playbook_entry_schema()`, the pboettch
// draft-07 validator like the Run record, schemas/playbook_entry.schema.json).
namespace zetta::ml {

// --- The owning role + urgency band ------------------------------------------

// The team that owns a retention play. The lowercase name is BOTH the display label
// AND the stored JSON representation, so it is a durable identifier — append new
// roles, never reorder.
enum class PlaybookOwner {
    Csm,             // "csm" — Customer Success Manager
    AccountManager,  // "account_manager"
    Billing,         // "billing"
    Support,         // "support"
    Marketing,       // "marketing"
};

std::string_view to_string(PlaybookOwner owner);
// Parse a stored/canonical owner name back to the enum; false (out untouched) on an
// unknown spelling — the boundary check for the enum.
bool playbook_owner_from_string(std::string_view text, PlaybookOwner& out);

// How urgently to work an account. Derived deterministically from the churn risk
// band (a qualitative label, not a number); a narrator may also choose it.
enum class PlaybookUrgency { Low, Medium, High };

std::string_view to_string(PlaybookUrgency urgency);
bool playbook_urgency_from_string(std::string_view text, PlaybookUrgency& out);

// The urgency band for a churn risk in [0, 1]: High >= 0.7, Medium >= 0.4, else Low.
PlaybookUrgency urgency_for(double risk);

// --- The driver dictionary (the editable policy) -----------------------------

// One driver-dictionary entry: what a churn driver means, the play to run, who owns
// it, and the prior probability that play saves the account.
struct DriverPlay {
    std::string signal;                       // human-readable churn signal
    std::string play;                         // canonical recommended retention play
    PlaybookOwner owner = PlaybookOwner::Csm;  // owning role
    double save_rate = 0.35;                  // P(save | this dominant driver), [0, 1]
    bool involuntary = false;                 // billing/involuntary (cheap, prioritized first)

    bool operator==(const DriverPlay&) const = default;
};

// The editable retention driver dictionary + the default play. Maps a churn driver
// (feature name) to its `DriverPlay`. Carries a validated JSON boundary (mirrors
// schemas/playbook_policy.schema.json; the hand-written non-throwing `from_json`
// pattern of `FeaturizeSpec` / `ScalingPolicy`, NOT the pboettch path — that is for
// top-level replicated entities).
struct PlaybookPolicy {
    std::map<std::string, DriverPlay> drivers;   // feature name -> play (sorted, deterministic)
    DriverPlay default_play;                     // for a driver not in `drivers`

    // The play for a dominant driver: its dictionary entry, or `default_play` when the
    // driver is unmapped (the COALESCE of the driver dictionary).
    const DriverPlay& play_for(std::string_view driver) const;

    // The built-in UC-1 churn driver dictionary (failed_payments ~0.75 down to a 0.35
    // default), keyed by the churn fixture's feature names. The starting palette a team
    // customizes for their own retention motions.
    static PlaybookPolicy churn_default();

    // The JSON value shape mirrors schemas/playbook_policy.schema.json.
    nlohmann::json to_json() const;

    // Parse + validate at the boundary; the non-throwing inverse of to_json
    // (std::optional). std::nullopt unless `value` is an object with a well-formed
    // `default_play` and a `drivers` object whose every value is a well-formed play (a
    // non-empty signal + play, a known owner, a save_rate in [0, 1]).
    static std::optional<PlaybookPolicy> from_json(const nlohmann::json& value);

    bool operator==(const PlaybookPolicy&) const = default;
};

// The PlaybookPolicy JSON Schema (draft-07) — the code-owned source of truth;
// schemas/playbook_policy.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& playbook_policy_schema();

// --- The per-account input (the model's facts) -------------------------------

// One signed driver reason code: a feature's SHAP attribution and which way it
// pushed (positive φ pushes churn up). The auditable "why".
struct DriverContribution {
    std::string feature;        // the driver feature name
    double value = 0.0;         // signed SHAP attribution φ (raw-score space)
    bool pushes_churn = false;  // value > 0 (pushed the prediction toward churn)

    bool operator==(const DriverContribution&) const = default;
};

// The facts the playbook consumes for one at-risk account: its identity, the model's
// calibrated churn risk, its ARR, and its TreeSHAP drivers. Everything the
// quantitative layers and the narrator need — assembled by the caller from the
// [score job](gbdt_score.hpp) + the [explain job](tree_shap.hpp) + the account
// dimension.
struct PlaybookAccount {
    std::int64_t entity = 0;        // the entity key (CRM account index)
    std::string account_id;         // human account id (e.g. "acct_contoso_office"); may be empty
    std::string account_name;       // display name; may be empty
    double risk = 0.0;              // P(churn) — the calibrated risk (predict_proba), [0, 1]
    double arr = 0.0;               // ARR proxy (e.g. mrr x 12)
    std::string dominant_driver;    // the dominant churn driver (top |φ|) feature name
    double dominant_value = 0.0;    // its signed φ (positive = pushed toward churn)
    std::vector<DriverContribution> drivers;  // top-k signed reason codes, strongest |φ| first

    bool operator==(const PlaybookAccount&) const = default;
};

// Fill `account.drivers` (the signed top-k reason codes, strongest |φ| first) plus
// `dominant_driver` / `dominant_value` from a SHAP explanation: `feature_names[i]` is
// the model feature whose attribution is `shap_values[i]`. `top_k` caps the reason
// codes (0 = all). Pure; takes the raw φ vector (no tree_shap.hpp dependency). The
// dominant driver is the strongest |φ|, ties broken by the lower feature index.
void set_drivers_from_shap(PlaybookAccount& account,
                           const std::vector<std::string>& feature_names,
                           const std::vector<double>& shap_values,
                           std::size_t top_k = 5);

// --- The narrator seam -------------------------------------------------------

// What the narrator is handed for one account: the identity, the **read-only**
// computed numbers (which it must not change), and the candidate drivers (each with
// the policy's suggested play/owner/save_rate). The candidates are the top-k signed
// reason codes, strongest |φ| first.
struct NarrationCandidate {
    std::string feature;                      // driver feature name
    double value = 0.0;                       // signed φ
    bool pushes_churn = false;
    std::string suggested_play;               // the policy's play for this driver
    PlaybookOwner suggested_owner = PlaybookOwner::Csm;
    double save_rate = 0.0;                   // the policy's save-rate prior
    bool involuntary = false;
};

struct NarrationRequest {
    std::int64_t entity = 0;
    std::string account_id;
    std::string account_name;
    double risk = 0.0;                        // P(churn) — read-only
    double arr = 0.0;                         // read-only
    double expected_retained_value = 0.0;     // the frozen priority — read-only
    std::string dominant_driver;              // the SHAP dominant driver
    std::vector<NarrationCandidate> candidates;  // top-k, strongest |φ| first
};

// The narrator's output for one account: the strict contract of
// schemas/playbook_entry.schema.json — the narrative + the selected play, never any
// number except its own `confidence`.
struct PlaybookNarration {
    std::string account_id;                   // must echo the request's account_id
    std::string primary_driver;               // selected from the request's candidates
    std::string recommended_play;
    PlaybookOwner owner = PlaybookOwner::Csm;
    PlaybookUrgency urgency = PlaybookUrgency::Low;
    std::string diagnosis;
    std::vector<std::string> talking_points;  // exactly 3
    double confidence = 0.0;                  // [0, 1]

    // Provenance — which narrator actually produced this prose ("claude" /
    // "rules_engine"). NOT part of the strict schema contract (so it is absent from
    // to_json); set in code so an entry honestly records its source even when a
    // constrained narrator fell back internally.
    std::string source;

    // The JSON form mirrors schemas/playbook_entry.schema.json (provenance `source`
    // is deliberately excluded — it is not part of the narrator contract).
    nlohmann::json to_json() const;
};

// The strict narrator-output JSON Schema (draft-07), exposed for tests/introspection.
const nlohmann::json& playbook_entry_schema();

// Validate a candidate narration JSON against the strict schema at the boundary
// (the pboettch validator, like the Run record). Returns true when it matches; on a
// failure sets `error` to a path-qualified reason and returns false. Parses nothing —
// it is the schema gate `parse_playbook_narration` runs first.
bool validate_playbook_narration(const nlohmann::json& value, std::string* error = nullptr);

// Parse + validate a narration from JSON **against the request it answers**: the
// schema gate above, then the semantic rules the schema can't express — `account_id`
// echoes the request, `primary_driver` is one of the request's candidates, and the
// owner/urgency enums parse. std::nullopt (with `error` set) on any failure, so a
// malformed or off-contract LLM response is rejected at the boundary and the caller
// falls back. Non-throwing.
std::optional<PlaybookNarration> parse_playbook_narration(
    const nlohmann::json& value, const NarrationRequest& request,
    std::string* error = nullptr);

// The pluggable narrator seam: write the narrative + select the play for one account.
// Concrete narrators (the deterministic rules engine, the constrained Claude binding)
// implement it and plug in without touching the generator (the `StepExecutor` / `Check`
// pattern). `narrate` returns std::nullopt to signal "cannot narrate" (offline, a
// malformed response) — the generator then synthesizes a deterministic entry; a
// narrator that wraps its own fallback (the `ConstrainedNarrator`) never returns
// nullopt. Non-throwing.
class PlaybookNarrator {
public:
    virtual ~PlaybookNarrator() = default;
    // A short identifier recorded on the entry for provenance ("rules_engine" / "claude").
    virtual std::string name() const = 0;
    virtual std::optional<PlaybookNarration> narrate(const NarrationRequest& request) = 0;
};

// The deterministic rules-engine narrator — the always-available fallback that makes
// the pipeline run offline. Synthesizes the narration straight from the driver
// dictionary: the play/owner of the dominant driver, the urgency band of the risk, a
// templated diagnosis + three talking points, and a save-rate-derived confidence.
// Pure + deterministic; `narrate` never returns nullopt.
class RulesEngineNarrator : public PlaybookNarrator {
public:
    explicit RulesEngineNarrator(PlaybookPolicy policy = PlaybookPolicy::churn_default());
    std::string name() const override { return "rules_engine"; }
    std::optional<PlaybookNarration> narrate(const NarrationRequest& request) override;

private:
    PlaybookPolicy policy_;
};

// The completion seam the constrained narrator calls: given a fully-built prompt,
// return the model's raw JSON response, or std::nullopt when no model is reachable
// (offline, no API key, a transport error). zetta-core carries no HTTP client / LLM
// SDK, so the transport is **injected** (the `CommandRunner` / `FleetSource` pattern);
// the production binding wires this to **Claude (Opus 4.8) with structured outputs**
// constrained to playbook_entry_schema(). Non-throwing.
using NarratorCompletion =
    std::function<std::optional<std::string>(const std::string& prompt)>;

// The **constrained narrator** (default Claude). Builds a strict prompt for an account,
// calls the injected `NarratorCompletion`, parses + validates the model's JSON at the
// boundary (`parse_playbook_narration`), and returns the narration. On **any** failure
// — no completion, non-JSON, a schema or semantic violation — it returns its
// `fallback` narrator's result instead, so it **always** produces an entry and the
// numbers are never at the model's mercy. The fallback defaults to a
// `RulesEngineNarrator`. The prompt it builds is exposed (`build_prompt`) for tests.
class ConstrainedNarrator : public PlaybookNarrator {
public:
    ConstrainedNarrator(NarratorCompletion completion,
                        PlaybookPolicy policy = PlaybookPolicy::churn_default(),
                        std::shared_ptr<PlaybookNarrator> fallback = {});
    std::string name() const override { return "claude"; }
    std::optional<PlaybookNarration> narrate(const NarrationRequest& request) override;

    // The strict prompt handed to the completion seam for one account: the
    // CS-strategist contract, the read-only numbers, the candidate drivers, and the
    // required JSON schema. Pure — exposed so tests can drive a fake completion.
    std::string build_prompt(const NarrationRequest& request) const;

private:
    NarratorCompletion completion_;
    PlaybookPolicy policy_;
    std::shared_ptr<PlaybookNarrator> fallback_;
};

// --- The playbook + outcome log ----------------------------------------------

// One CRM-ready playbook entry for an at-risk account. The frozen, computed numbers
// (risk / arr / save_rate / expected_retained_value / urgency) PLUS the narrative +
// selected play. `narrator` records which narrator produced the prose.
struct PlaybookEntry {
    std::int64_t entity = 0;
    std::string account_id;
    std::string account_name;
    // Computed in code — never from the narrator:
    double risk = 0.0;                         // P(churn)
    double arr = 0.0;                          // ARR proxy
    double save_rate = 0.0;                    // P(save | dominant driver)
    double expected_retained_value = 0.0;      // risk x arr x save_rate (the priority)
    std::string dominant_driver;               // the SHAP dominant driver
    PlaybookUrgency urgency = PlaybookUrgency::Low;
    // Narrative + play (deterministic, or narrator-written within the schema):
    std::string primary_driver;                // the selected driver
    std::string recommended_play;
    PlaybookOwner owner = PlaybookOwner::Csm;
    std::string diagnosis;                     // the "why" narrative
    std::vector<std::string> talking_points;   // three
    double confidence = 0.0;                   // the narrator's confidence
    std::string narrator;                      // provenance ("rules_engine" / "claude")

    // The full CRM-export JSON row (numbers + narrative).
    nlohmann::json to_json() const;
};

// A generated playbook: the at-risk entries ranked by expected retained value
// (descending; ties broken by entity ascending, so the ranking is **deterministic**),
// plus the policy used (provenance).
struct Playbook {
    std::vector<PlaybookEntry> entries;
    PlaybookPolicy policy;

    // The CRM-ready JSON: an array of entry rows.
    nlohmann::json to_json() const;
};

// Generate a playbook from the at-risk accounts. For each account it computes the
// expected-retained-value priority `risk x arr x save_rate(dominant_driver)` (the
// save_rate from `policy`), ranks descending (ties by entity ascending), and runs the
// `narrator` to write the narrative + select the play; a narrator that returns
// nullopt falls back to a deterministic entry from the dictionary. When `top_k > 0`
// only the `top_k` highest-priority accounts are kept (the capacity constraint a team
// works); 0 keeps all. Pure + non-throwing.
Playbook generate_playbook(const std::vector<PlaybookAccount>& accounts,
                           const PlaybookPolicy& policy, PlaybookNarrator& narrator,
                           std::size_t top_k = 0);

// Convenience: generate offline with the deterministic `RulesEngineNarrator` over
// `policy` — the fallback path the acceptance tests and CI use.
Playbook generate_playbook(const std::vector<PlaybookAccount>& accounts,
                           const PlaybookPolicy& policy = PlaybookPolicy::churn_default(),
                           std::size_t top_k = 0);

// One row of the **outcome log** (the feedback loop, 4.7): a recommendation appended
// with the outcome unresolved. A later job fills `renewed` / `action_taken` once the
// churn horizon closes; the accumulated log turns the static save-rate priors into
// learned save-rate-by-driver. Laid down now so the data exists when that layer is
// built.
struct OutcomeLogEntry {
    std::int64_t entity = 0;
    std::string account_id;
    std::string recommended_play;
    PlaybookOwner owner = PlaybookOwner::Csm;
    double risk = 0.0;
    double arr = 0.0;
    double expected_retained_value = 0.0;
    std::string dominant_driver;
    std::string generated_at;            // ISO-8601 stamp (provenance)
    std::optional<bool> renewed;         // outcome: did the account renew? nullopt = pending
    std::string action_taken;            // what the owner did; empty = pending

    nlohmann::json to_json() const;
};

// The append-only outcome log: one pending row per playbook entry.
struct OutcomeLog {
    std::vector<OutcomeLogEntry> rows;

    nlohmann::json to_json() const;
};

// Seed an outcome log from a generated playbook — one pending row per entry (outcome
// and action unresolved). `generated_at` stamps each row; empty defaults to the
// current UTC time (`now_iso8601()`), so tests can pass a fixed stamp for determinism.
OutcomeLog outcome_log_from_playbook(const Playbook& playbook,
                                     std::string generated_at = {});

}  // namespace zetta::ml
