// SPDX-License-Identifier: MIT

#include "zetta/ml/playbook.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/util/timestamp.hpp"

namespace zetta::ml {

// --- enum names --------------------------------------------------------------

std::string_view to_string(PlaybookOwner owner) {
    switch (owner) {
        case PlaybookOwner::Csm:            return "csm";
        case PlaybookOwner::AccountManager: return "account_manager";
        case PlaybookOwner::Billing:        return "billing";
        case PlaybookOwner::Support:        return "support";
        case PlaybookOwner::Marketing:      return "marketing";
    }
    return "csm";
}

bool playbook_owner_from_string(std::string_view text, PlaybookOwner& out) {
    constexpr std::array<PlaybookOwner, 5> owners{
        PlaybookOwner::Csm, PlaybookOwner::AccountManager, PlaybookOwner::Billing,
        PlaybookOwner::Support, PlaybookOwner::Marketing};
    for (const PlaybookOwner owner : owners) {
        if (to_string(owner) == text) {
            out = owner;
            return true;
        }
    }
    return false;
}

std::string_view to_string(PlaybookUrgency urgency) {
    switch (urgency) {
        case PlaybookUrgency::Low:    return "low";
        case PlaybookUrgency::Medium: return "medium";
        case PlaybookUrgency::High:   return "high";
    }
    return "low";
}

bool playbook_urgency_from_string(std::string_view text, PlaybookUrgency& out) {
    constexpr std::array<PlaybookUrgency, 3> urgencies{
        PlaybookUrgency::Low, PlaybookUrgency::Medium, PlaybookUrgency::High};
    for (const PlaybookUrgency urgency : urgencies) {
        if (to_string(urgency) == text) {
            out = urgency;
            return true;
        }
    }
    return false;
}

PlaybookUrgency urgency_for(double risk) {
    if (risk >= 0.7) return PlaybookUrgency::High;
    if (risk >= 0.4) return PlaybookUrgency::Medium;
    return PlaybookUrgency::Low;
}

namespace {

// Compact, noise-free rendering of a double for prompts / narratives ("0.82" not
// "0.820000"). Default ostream precision is 6 significant digits — plenty.
std::string num(double value) {
    std::ostringstream out;
    out << value;
    return out.str();
}

// A money figure for narratives: "$12,000". Rounds to whole units and groups
// thousands; ARR is an estimate, so cents add only noise.
std::string money(double value) {
    const long long whole = static_cast<long long>(std::llround(value));
    std::string digits = std::to_string(whole < 0 ? -whole : whole);
    std::string grouped;
    int count = 0;
    for (auto it = digits.rbegin(); it != digits.rend(); ++it) {
        if (count != 0 && count % 3 == 0) grouped.push_back(',');
        grouped.push_back(*it);
        ++count;
    }
    std::reverse(grouped.begin(), grouped.end());
    return (whole < 0 ? "-$" : "$") + grouped;
}

// A percentage for narratives: "82%".
std::string percent(double fraction) {
    return num(std::llround(fraction * 100.0)) + "%";
}

}  // namespace

// --- DriverPlay / PlaybookPolicy ---------------------------------------------

const DriverPlay& PlaybookPolicy::play_for(std::string_view driver) const {
    const auto found = drivers.find(std::string(driver));
    return found == drivers.end() ? default_play : found->second;
}

PlaybookPolicy PlaybookPolicy::churn_default() {
    PlaybookPolicy policy;
    policy.default_play =
        DriverPlay{.signal = "General churn risk",
                   .play = "CSM check-in and account health review",
                   .owner = PlaybookOwner::Csm,
                   .save_rate = 0.35,
                   .involuntary = false};
    policy.drivers = {
        {"failed_payments",
         DriverPlay{.signal = "Repeated payment failures (involuntary churn risk)",
                    .play = "Billing recovery / card update outreach",
                    .owner = PlaybookOwner::Billing,
                    .save_rate = 0.75,
                    .involuntary = true}},
        {"support_severity",
         DriverPlay{.signal = "Unresolved or severe support load",
                    .play = "Escalate and resolve open tickets",
                    .owner = PlaybookOwner::Support,
                    .save_rate = 0.45,
                    .involuntary = false}},
        {"total_events",
         DriverPlay{.signal = "Falling product engagement",
                    .play = "Value review / re-enablement",
                    .owner = PlaybookOwner::Csm,
                    .save_rate = 0.40,
                    .involuntary = false}},
        {"recent_downgrade",
         DriverPlay{.signal = "Recent contraction / downgrade",
                    .play = "Win-back / contraction save offer",
                    .owner = PlaybookOwner::AccountManager,
                    .save_rate = 0.30,
                    .involuntary = false}},
        {"seats",
         DriverPlay{.signal = "Low seat utilization",
                    .play = "Adoption / onboarding push",
                    .owner = PlaybookOwner::Csm,
                    .save_rate = 0.40,
                    .involuntary = false}},
        {"mrr",
         DriverPlay{.signal = "Revenue concentration",
                    .play = "Executive business review",
                    .owner = PlaybookOwner::AccountManager,
                    .save_rate = 0.35,
                    .involuntary = false}},
        {"tenure_days",
         DriverPlay{.signal = "Renewal cliff approaching",
                    .play = "Early renewal motion / term offer",
                    .owner = PlaybookOwner::AccountManager,
                    .save_rate = 0.50,
                    .involuntary = false}},
    };
    return policy;
}

namespace {

nlohmann::json play_to_json(const DriverPlay& play) {
    nlohmann::json value{{"signal", play.signal},
                         {"play", play.play},
                         {"owner", to_string(play.owner)},
                         {"save_rate", play.save_rate}};
    if (play.involuntary) value["involuntary"] = true;
    return value;
}

// Parse one play object at the boundary: a non-empty signal + play, a known owner, a
// save_rate in [0, 1], and an optional boolean `involuntary`. std::nullopt on any
// violation (the hand-written FeaturizeSpec / ScalingPolicy pattern).
std::optional<DriverPlay> play_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    if (!value.contains("signal") || !value["signal"].is_string()) return std::nullopt;
    if (!value.contains("play") || !value["play"].is_string()) return std::nullopt;
    DriverPlay play;
    play.signal = value["signal"].get<std::string>();
    play.play = value["play"].get<std::string>();
    if (play.signal.empty() || play.play.empty()) return std::nullopt;

    if (!value.contains("owner") || !value["owner"].is_string()) return std::nullopt;
    if (!playbook_owner_from_string(value["owner"].get<std::string>(), play.owner)) {
        return std::nullopt;
    }

    if (!value.contains("save_rate") || !value["save_rate"].is_number()) {
        return std::nullopt;
    }
    play.save_rate = value["save_rate"].get<double>();
    if (play.save_rate < 0.0 || play.save_rate > 1.0) return std::nullopt;

    if (value.contains("involuntary")) {
        if (!value["involuntary"].is_boolean()) return std::nullopt;
        play.involuntary = value["involuntary"].get<bool>();
    }
    return play;
}

}  // namespace

nlohmann::json PlaybookPolicy::to_json() const {
    nlohmann::json drivers_json = nlohmann::json::object();
    for (const auto& [feature, play] : drivers) {
        drivers_json[feature] = play_to_json(play);
    }
    return nlohmann::json{{"default_play", play_to_json(default_play)},
                          {"drivers", std::move(drivers_json)}};
}

std::optional<PlaybookPolicy> PlaybookPolicy::from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("default_play")) return std::nullopt;
    if (!value.contains("drivers") || !value["drivers"].is_object()) {
        return std::nullopt;
    }

    std::optional<DriverPlay> default_play = play_from_json(value["default_play"]);
    if (!default_play) return std::nullopt;

    PlaybookPolicy policy;
    policy.default_play = std::move(*default_play);
    for (const auto& [feature, play_json] : value["drivers"].items()) {
        std::optional<DriverPlay> play = play_from_json(play_json);
        if (!play) return std::nullopt;
        policy.drivers.emplace(feature, std::move(*play));
    }
    return policy;
}

// The PlaybookPolicy JSON Schema (draft-07) — the code-owned source of truth;
// schemas/playbook_policy.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& playbook_policy_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "$id": "https://oramasearch.github.io/zetta/schemas/playbook_policy.schema.json",
  "title": "PlaybookPolicy",
  "description": "The editable churn retention driver dictionary (FEAT-6, #397; part of the native distributed accelerated compute pass for UC-1 churn — #264). Maps each churn driver (a feature name, the dominant TreeSHAP driver of an at-risk account) to a deterministic recommended play, an owning role, and a save-rate prior P(save | this driver) used in the expected-retained-value priority risk x ARR x save_rate. This is the use-case skin a team customizes per their retention motions; the featurize -> train -> explain -> rank core is reused unchanged. Mirrored by zetta::ml::PlaybookPolicy in include/zetta/ml/playbook.hpp and enforced by PlaybookPolicy::from_json() in src/core/ml/playbook.cpp (the hand-written non-throwing boundary, like FeaturizeSpec / ScalingPolicy) — keep them in sync. The narrator's JSON output contract is the separate playbook_entry.schema.json.",
  "type": "object",
  "additionalProperties": false,
  "required": ["default_play", "drivers"],
  "properties": {
    "drivers": {
      "description": "The driver dictionary: feature name -> the play for an account whose dominant churn driver is that feature.",
      "type": "object",
      "additionalProperties": { "$ref": "#/$defs/play" }
    },
    "default_play": {
      "description": "The fallback play for a dominant driver not present in `drivers` (the SQL COALESCE of the driver dictionary; save_rate defaults to ~0.35).",
      "$ref": "#/$defs/play"
    }
  },
  "$defs": {
    "play": {
      "type": "object",
      "additionalProperties": false,
      "required": ["signal", "play", "owner", "save_rate"],
      "properties": {
        "signal": {
          "description": "The human-readable churn signal this driver represents (e.g. 'Repeated payment failures').",
          "type": "string",
          "minLength": 1
        },
        "play": {
          "description": "The canonical recommended retention play (e.g. 'Billing recovery / card update outreach').",
          "type": "string",
          "minLength": 1
        },
        "owner": {
          "description": "The owning role for the play.",
          "type": "string",
          "enum": ["csm", "account_manager", "billing", "support", "marketing"]
        },
        "save_rate": {
          "description": "The save-rate prior P(save | this is the dominant driver), in [0, 1] — a billing/involuntary issue is cheap and high-save (~0.75); a competitive/contraction issue is hard to save (~0.30). Replaced by learned uplift once the outcome log has data.",
          "type": "number",
          "minimum": 0,
          "maximum": 1
        },
        "involuntary": {
          "description": "Whether this is an involuntary/billing churn driver (cheap to fix, prioritized first when drivers conflict). Optional; defaults to false.",
          "type": "boolean"
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

// --- SHAP -> driver reason codes ---------------------------------------------

void set_drivers_from_shap(PlaybookAccount& account,
                           const std::vector<std::string>& feature_names,
                           const std::vector<double>& shap_values,
                           std::size_t top_k) {
    account.drivers.clear();
    account.dominant_driver.clear();
    account.dominant_value = 0.0;

    const std::size_t count = std::min(feature_names.size(), shap_values.size());
    if (count == 0) return;

    // Rank feature indices by |φ| descending, ties broken by the lower index so the
    // result is deterministic.
    std::vector<std::size_t> order(count);
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        const double magnitude_a = std::abs(shap_values[a]);
        const double magnitude_b = std::abs(shap_values[b]);
        if (magnitude_a != magnitude_b) return magnitude_a > magnitude_b;
        return a < b;
    });

    account.dominant_driver = feature_names[order.front()];
    account.dominant_value = shap_values[order.front()];

    const std::size_t keep = top_k == 0 ? count : std::min(top_k, count);
    account.drivers.reserve(keep);
    for (std::size_t rank = 0; rank < keep; ++rank) {
        const std::size_t index = order[rank];
        account.drivers.push_back(
            DriverContribution{.feature = feature_names[index],
                               .value = shap_values[index],
                               .pushes_churn = shap_values[index] > 0.0});
    }
}

// --- PlaybookNarration: JSON + boundary --------------------------------------

nlohmann::json PlaybookNarration::to_json() const {
    return nlohmann::json{{"account_id", account_id},
                          {"primary_driver", primary_driver},
                          {"recommended_play", recommended_play},
                          {"owner", to_string(owner)},
                          {"urgency", to_string(urgency)},
                          {"diagnosis", diagnosis},
                          {"talking_points", talking_points},
                          {"confidence", confidence}};
}

namespace {

// The embedded draft-07 schema for a narrator's output — mirrors
// schemas/playbook_entry.schema.json (kept in sync). The narrator's JSON enters here
// (the LLM/API response boundary), so it is validated with the pboettch validator
// like the Run record.
const nlohmann::json& narration_schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "type": "object",
      "additionalProperties": false,
      "required": ["account_id", "primary_driver", "recommended_play", "owner",
                   "urgency", "diagnosis", "talking_points", "confidence"],
      "properties": {
        "account_id": { "type": "string" },
        "primary_driver": { "type": "string", "minLength": 1 },
        "recommended_play": { "type": "string", "minLength": 1 },
        "owner": { "type": "string",
                   "enum": ["csm", "account_manager", "billing", "support", "marketing"] },
        "urgency": { "type": "string", "enum": ["low", "medium", "high"] },
        "diagnosis": { "type": "string", "minLength": 1 },
        "talking_points": { "type": "array", "minItems": 3, "maxItems": 3,
                            "items": { "type": "string", "minLength": 1 } },
        "confidence": { "type": "number", "minimum": 0, "maximum": 1 }
      }
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& narration_validator() {
    static nlohmann::json_schema::json_validator validator(narration_schema_document());
    return validator;
}

// Collects the first validation failure without letting the validator throw across the
// library boundary (the run.cpp / gbdt.cpp pattern).
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

const nlohmann::json& playbook_entry_schema() { return narration_schema_document(); }

bool validate_playbook_narration(const nlohmann::json& value, std::string* error) {
    collecting_error_handler handler;
    narration_validator().validate(value, handler);
    if (handler.failed) {
        if (error) *error = handler.reason;
        return false;
    }
    return true;
}

std::optional<PlaybookNarration> parse_playbook_narration(
    const nlohmann::json& value, const NarrationRequest& request, std::string* error) {
    if (!validate_playbook_narration(value, error)) return std::nullopt;

    PlaybookNarration narration;
    narration.account_id = value.at("account_id").get<std::string>();
    // The narrator must not cross-wire accounts (a batched narrator returns one object
    // per account): the echoed id must match the requested one.
    if (narration.account_id != request.account_id) {
        if (error) {
            *error = "account_id: expected '" + request.account_id + "', got '" +
                     narration.account_id + "'";
        }
        return std::nullopt;
    }

    narration.primary_driver = value.at("primary_driver").get<std::string>();
    // The selected driver must be one the model was actually given — it cannot
    // hallucinate a driver outside the provided reason codes.
    const bool known_driver =
        std::any_of(request.candidates.begin(), request.candidates.end(),
                    [&](const NarrationCandidate& candidate) {
                        return candidate.feature == narration.primary_driver;
                    });
    if (!known_driver) {
        if (error) {
            *error = "primary_driver: '" + narration.primary_driver +
                     "' is not one of the provided candidate drivers";
        }
        return std::nullopt;
    }

    narration.recommended_play = value.at("recommended_play").get<std::string>();
    if (!playbook_owner_from_string(value.at("owner").get<std::string>(),
                                    narration.owner)) {
        if (error) *error = "owner: unknown role";
        return std::nullopt;
    }
    if (!playbook_urgency_from_string(value.at("urgency").get<std::string>(),
                                      narration.urgency)) {
        if (error) *error = "urgency: unknown band";
        return std::nullopt;
    }
    narration.diagnosis = value.at("diagnosis").get<std::string>();
    narration.talking_points =
        value.at("talking_points").get<std::vector<std::string>>();
    narration.confidence = value.at("confidence").get<double>();
    return narration;
}

// --- RulesEngineNarrator (the deterministic fallback) ------------------------

RulesEngineNarrator::RulesEngineNarrator(PlaybookPolicy policy)
    : policy_(std::move(policy)) {}

std::optional<PlaybookNarration> RulesEngineNarrator::narrate(
    const NarrationRequest& request) {
    const DriverPlay& play = policy_.play_for(request.dominant_driver);

    PlaybookNarration narration;
    narration.account_id = request.account_id;
    narration.primary_driver = request.dominant_driver;
    narration.recommended_play = play.play;
    narration.owner = play.owner;
    narration.urgency = urgency_for(request.risk);

    const std::string who =
        request.account_name.empty()
            ? (request.account_id.empty() ? "This account" : request.account_id)
            : request.account_name;
    narration.diagnosis =
        who + " is at " + percent(request.risk) +
        " churn risk; the dominant driver is " + play.signal +
        " (" + request.dominant_driver + "). " + play.play +
        " — expected retained value " + money(request.expected_retained_value) + ".";

    narration.talking_points = {
        "Lead the conversation on " + play.signal + ".",
        "Run the play: " + play.play + ".",
        "Recoverable value at stake: " + money(request.expected_retained_value) +
            " (ARR " + money(request.arr) + " x " + percent(request.risk) +
            " churn risk x " + percent(play.save_rate) + " save rate)."};

    // A deterministic, non-fabricated confidence: the save-rate prior is exactly the
    // model's belief that this play works.
    narration.confidence = play.save_rate;
    narration.source = name();
    return narration;
}

// --- ConstrainedNarrator (default Claude, behind the seam) -------------------

ConstrainedNarrator::ConstrainedNarrator(NarratorCompletion completion,
                                         PlaybookPolicy policy,
                                         std::shared_ptr<PlaybookNarrator> fallback)
    : completion_(std::move(completion)),
      policy_(std::move(policy)),
      fallback_(fallback ? std::move(fallback)
                         : std::make_shared<RulesEngineNarrator>(policy_)) {}

std::string ConstrainedNarrator::build_prompt(const NarrationRequest& request) const {
    std::ostringstream prompt;
    prompt << "You are a Customer-Success retention strategist. For the one at-risk "
              "account below, write a concise diagnosis and pick exactly one primary "
              "retention play and the right owner.\n\n"
              "Hard rules:\n"
              "- Use ONLY the drivers and numbers provided. Never invent a metric.\n"
              "- Never change the computed churn probability, ARR, or "
              "expected-retained-value priority.\n"
              "- Choose primary_driver from the candidate drivers listed below.\n"
              "- When drivers conflict, prioritize involuntary/billing issues first.\n"
              "- Reply with ONLY a JSON object matching this schema (no prose):\n";
    prompt << playbook_entry_schema().dump(2) << "\n\n";

    prompt << "Account:\n";
    prompt << "  account_id: " << request.account_id << "\n";
    if (!request.account_name.empty()) {
        prompt << "  name: " << request.account_name << "\n";
    }
    prompt << "  churn_probability: " << num(request.risk) << "\n";
    prompt << "  arr: " << num(request.arr) << "\n";
    prompt << "  expected_retained_value: " << num(request.expected_retained_value)
           << "\n";
    prompt << "  dominant_driver: " << request.dominant_driver << "\n";

    prompt << "Candidate drivers (strongest first; phi > 0 pushes toward churn):\n";
    for (const NarrationCandidate& candidate : request.candidates) {
        prompt << "  - feature: " << candidate.feature
               << ", phi: " << num(candidate.value)
               << ", pushes_churn: " << (candidate.pushes_churn ? "true" : "false")
               << ", suggested_play: " << candidate.suggested_play
               << ", owner: " << to_string(candidate.suggested_owner)
               << ", save_rate: " << num(candidate.save_rate)
               << (candidate.involuntary ? ", involuntary: true" : "") << "\n";
    }
    return prompt.str();
}

std::optional<PlaybookNarration> ConstrainedNarrator::narrate(
    const NarrationRequest& request) {
    // The injected transport — absent (offline / no key) means fall straight back.
    if (completion_) {
        if (std::optional<std::string> reply = completion_(build_prompt(request))) {
            // Parse + schema/semantic-validate the model's JSON at the boundary; any
            // failure (non-JSON, off-schema, off-contract) falls back to the rules
            // engine rather than trusting the response.
            nlohmann::json value =
                nlohmann::json::parse(*reply, nullptr, /*allow_exceptions=*/false);
            if (!value.is_discarded()) {
                std::string error;
                if (std::optional<PlaybookNarration> narration =
                        parse_playbook_narration(value, request, &error)) {
                    narration->source = name();  // the model produced this prose
                    return narration;
                }
            }
        }
    }
    return fallback_ ? fallback_->narrate(request) : std::nullopt;
}

// --- generate_playbook -------------------------------------------------------

nlohmann::json PlaybookEntry::to_json() const {
    return nlohmann::json{{"entity", entity},
                          {"account_id", account_id},
                          {"account_name", account_name},
                          {"risk", risk},
                          {"arr", arr},
                          {"save_rate", save_rate},
                          {"expected_retained_value", expected_retained_value},
                          {"dominant_driver", dominant_driver},
                          {"urgency", to_string(urgency)},
                          {"primary_driver", primary_driver},
                          {"recommended_play", recommended_play},
                          {"owner", to_string(owner)},
                          {"diagnosis", diagnosis},
                          {"talking_points", talking_points},
                          {"confidence", confidence},
                          {"narrator", narrator}};
}

nlohmann::json Playbook::to_json() const {
    nlohmann::json rows = nlohmann::json::array();
    for (const PlaybookEntry& entry : entries) rows.push_back(entry.to_json());
    return rows;
}

namespace {

// Build the narrator's request for one account: the frozen numbers + the candidate
// drivers enriched with the policy's suggested play/owner/save_rate.
NarrationRequest make_request(const PlaybookAccount& account,
                              const PlaybookPolicy& policy,
                              double expected_retained_value) {
    NarrationRequest request;
    request.entity = account.entity;
    request.account_id = account.account_id;
    request.account_name = account.account_name;
    request.risk = account.risk;
    request.arr = account.arr;
    request.expected_retained_value = expected_retained_value;
    request.dominant_driver = account.dominant_driver;

    // Candidates default to the signed driver reason codes; if none were attached, fall
    // back to the dominant driver alone so the narrator always has at least one choice.
    std::vector<DriverContribution> drivers = account.drivers;
    if (drivers.empty() && !account.dominant_driver.empty()) {
        drivers.push_back(DriverContribution{.feature = account.dominant_driver,
                                             .value = account.dominant_value,
                                             .pushes_churn = account.dominant_value > 0.0});
    }
    request.candidates.reserve(drivers.size());
    for (const DriverContribution& driver : drivers) {
        const DriverPlay& play = policy.play_for(driver.feature);
        request.candidates.push_back(
            NarrationCandidate{.feature = driver.feature,
                               .value = driver.value,
                               .pushes_churn = driver.pushes_churn,
                               .suggested_play = play.play,
                               .suggested_owner = play.owner,
                               .save_rate = play.save_rate,
                               .involuntary = play.involuntary});
    }
    return request;
}

}  // namespace

Playbook generate_playbook(const std::vector<PlaybookAccount>& accounts,
                           const PlaybookPolicy& policy, PlaybookNarrator& narrator,
                           std::size_t top_k) {
    Playbook playbook;
    playbook.policy = policy;
    playbook.entries.reserve(accounts.size());

    for (const PlaybookAccount& account : accounts) {
        const DriverPlay& dominant_play = policy.play_for(account.dominant_driver);
        // The priority is computed in code and FROZEN before the narrator runs: it
        // uses the deterministic dominant driver's save-rate prior, so a narrator can
        // never change the ranking. (4.5)
        const double expected_retained_value =
            account.risk * account.arr * dominant_play.save_rate;

        PlaybookEntry entry;
        entry.entity = account.entity;
        entry.account_id = account.account_id;
        entry.account_name = account.account_name;
        entry.risk = account.risk;
        entry.arr = account.arr;
        entry.save_rate = dominant_play.save_rate;
        entry.expected_retained_value = expected_retained_value;
        entry.dominant_driver = account.dominant_driver;
        entry.urgency = urgency_for(account.risk);

        const NarrationRequest request =
            make_request(account, policy, expected_retained_value);
        std::optional<PlaybookNarration> narration = narrator.narrate(request);
        if (narration) {
            entry.primary_driver = narration->primary_driver;
            entry.recommended_play = narration->recommended_play;
            entry.owner = narration->owner;
            entry.urgency = narration->urgency;
            entry.diagnosis = narration->diagnosis;
            entry.talking_points = narration->talking_points;
            entry.confidence = narration->confidence;
            // Honest provenance: the narration carries who actually wrote it (so a
            // constrained narrator that fell back internally records "rules_engine",
            // not "claude"). Fall back to the seam's name only if unset.
            entry.narrator =
                narration->source.empty() ? narrator.name() : narration->source;
        } else {
            // A narrator that declined and carries no fallback: synthesize a
            // deterministic entry straight from the dictionary so the playbook is never
            // left without a play.
            entry.primary_driver = account.dominant_driver;
            entry.recommended_play = dominant_play.play;
            entry.owner = dominant_play.owner;
            entry.diagnosis = dominant_play.signal + " (" + account.dominant_driver +
                              "). " + dominant_play.play + ".";
            entry.talking_points = {"Run the play: " + dominant_play.play + ".",
                                    "Lead on " + dominant_play.signal + ".",
                                    "Recoverable value: " +
                                        money(expected_retained_value) + "."};
            entry.confidence = dominant_play.save_rate;
            entry.narrator = "rules_engine";
        }
        playbook.entries.push_back(std::move(entry));
    }

    // Rank by expected retained value descending; ties broken by entity ascending so
    // the at-risk list is deterministic (input-order-independent).
    std::stable_sort(playbook.entries.begin(), playbook.entries.end(),
                     [](const PlaybookEntry& a, const PlaybookEntry& b) {
                         if (a.expected_retained_value != b.expected_retained_value) {
                             return a.expected_retained_value > b.expected_retained_value;
                         }
                         return a.entity < b.entity;
                     });

    if (top_k > 0 && playbook.entries.size() > top_k) {
        playbook.entries.resize(top_k);
    }
    return playbook;
}

Playbook generate_playbook(const std::vector<PlaybookAccount>& accounts,
                           const PlaybookPolicy& policy, std::size_t top_k) {
    RulesEngineNarrator narrator(policy);
    return generate_playbook(accounts, policy, narrator, top_k);
}

// --- OutcomeLog --------------------------------------------------------------

nlohmann::json OutcomeLogEntry::to_json() const {
    nlohmann::json value{{"entity", entity},
                         {"account_id", account_id},
                         {"recommended_play", recommended_play},
                         {"owner", to_string(owner)},
                         {"risk", risk},
                         {"arr", arr},
                         {"expected_retained_value", expected_retained_value},
                         {"dominant_driver", dominant_driver},
                         {"generated_at", generated_at},
                         {"renewed", renewed ? nlohmann::json(*renewed) : nlohmann::json()},
                         {"action_taken", action_taken.empty()
                                              ? nlohmann::json()
                                              : nlohmann::json(action_taken)}};
    return value;
}

nlohmann::json OutcomeLog::to_json() const {
    nlohmann::json values = nlohmann::json::array();
    for (const OutcomeLogEntry& row : rows) values.push_back(row.to_json());
    return values;
}

OutcomeLog outcome_log_from_playbook(const Playbook& playbook,
                                     std::string generated_at) {
    if (generated_at.empty()) generated_at = zetta::util::now_iso8601();
    OutcomeLog log;
    log.rows.reserve(playbook.entries.size());
    for (const PlaybookEntry& entry : playbook.entries) {
        log.rows.push_back(
            OutcomeLogEntry{.entity = entry.entity,
                            .account_id = entry.account_id,
                            .recommended_play = entry.recommended_play,
                            .owner = entry.owner,
                            .risk = entry.risk,
                            .arr = entry.arr,
                            .expected_retained_value = entry.expected_retained_value,
                            .dominant_driver = entry.dominant_driver,
                            .generated_at = generated_at,
                            .renewed = std::nullopt,
                            .action_taken = {}});
    }
    return log;
}

}  // namespace zetta::ml
