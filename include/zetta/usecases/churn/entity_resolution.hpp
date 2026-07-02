#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// Entity resolution for featurization (FEAT-6 / TC-4, #394; part of the native
// distributed accelerated-compute pass for UC-1 churn, #264). The **make-or-break**
// data step: before churn featurization can aggregate a customer's events against
// the right account, the raw account-name strings on comms events (the `mention`s)
// must be **resolved** to a canonical CRM account. The non-negotiable property is
// *division-correctness* — "FedEx Freight" resolves to its own account **without
// conflating** the 5–6 sibling FedEx subsidiaries (FedEx Ground, Express, Office,
// Logistics, Services), which are separate CRM accounts. Conflating siblings is the
// canonical failure the [entity-resolution gate](checks/entity_resolution_check.hpp)
// exists to catch.
//
// This is the *producer* the existing `zetta::EntityResolutionCheck` was built to
// guard: the resolver runs over the mentions, emits the schema-shaped
// `entity_resolution` annotation, and a featurize/clean step carries that annotation
// so the gate verifies the resolution before deploy (RULE-1). The two compose —
// resolver produces, gate verifies — so a buggy resolver cannot ship.
//
// **Transparent + deterministic**, the same heuristic spirit as `compile_intent`:
// pure normalized string matching, no ML/LLM. Real fuzzy/embedding resolution plugs
// in later behind the same `resolve` front door without changing the contract.
//
// Non-JSON-parsing: the resolver's inputs are typed C++ structs (built from
// already-validated `ChurnAccount`s, or in tests), so there is no new JSON *parse*
// boundary here. Its *output* rides the existing `entity_resolution` format
// (schemas/entity_resolution.schema.json), schema-validated where it enters the gate
// (`EntityResolutionCheck`). See docs/use-cases/entity-resolution.md.
namespace zetta {

// A candidate CRM account a raw mention can resolve to. `name` is the match key for
// the division-correct `FullName` strategy; `brand` is the (coarser) match key for
// `Brand`. `account_id` is the resolution target. `entity` is the ground-truth
// canonical division — used only to *label* the resolver's output for the gate, never
// to make the resolution decision (the resolver sees the mention string alone).
struct ResolutionCandidate {
    std::string account_id;
    std::string name;
    std::string brand;
    std::string entity;
};

// How a mention is matched to a candidate.
//
//   * `FullName` — the **division-correct** default: match the normalized full
//     account name (ASCII-lowercased, punctuation/whitespace folded), so "FedEx
//     Freight" and the variant "Fedex Freight" both land on `acct_fedex_freight`
//     while "FedEx Ground" stays a separate account. A mention that does not exactly
//     match a division name is left **unresolved** rather than force-fit to a wrong
//     (possibly conflating) guess — the conservative choice.
//   * `Brand` — a deliberately **coarse** strategy that collapses a mention to a
//     brand bucket (`acct_brand_<brand>`), so every "FedEx …" mention merges onto one
//     account. This is the *wrong* way: it conflates the siblings. It exists to make
//     the conflation the gate catches a real resolver behavior (the TC-4 trap), not a
//     hand-mutated payload.
enum class ResolutionStrategy { FullName, Brand };

// The lowercase canonical name of a strategy ("full_name", "brand"). For labels,
// log lines, and test output.
std::string_view to_string(ResolutionStrategy strategy);

// One resolution decision: the raw mention and the account it resolved to (empty
// `account_id` when nothing matched).
struct ResolvedMention {
    std::string mention;
    std::string account_id;  // resolver's choice; empty => unresolved
    bool resolved() const noexcept { return !account_id.empty(); }
};

// A transparent, deterministic entity resolver over a candidate account directory.
// Move-friendly value type; resolution is a pure function of the candidates + the
// mention (same input => same output), independent of candidate iteration cost.
class EntityResolver {
public:
    explicit EntityResolver(std::vector<ResolutionCandidate> candidates,
                            ResolutionStrategy strategy = ResolutionStrategy::FullName);

    // Resolve one raw mention to a candidate account_id. Never throws.
    ResolvedMention resolve(std::string_view mention) const;

    ResolutionStrategy strategy() const noexcept { return strategy_; }
    const std::vector<ResolutionCandidate>& candidates() const noexcept {
        return candidates_;
    }

private:
    std::vector<ResolutionCandidate> candidates_;
    std::vector<std::string> normalized_names_;   // candidates_[i].name, normalized
    std::vector<std::string> normalized_brands_;  // candidates_[i].brand, normalized
    ResolutionStrategy strategy_;
};

// A raw mention paired with the ground-truth `entity` it truly belongs to. The
// resolver is handed only `mention`; `entity` is **held out** so the gate can check
// the resolver's choice against the truth.
struct LabeledMention {
    std::string mention;
    std::string entity;
};

// Build the `entity_resolution` annotation by resolving each labeled mention against
// `candidates` with `strategy`:
//
//   { "entity_resolution": { "records": [ { mention, entity, resolved }, … ] } }
//
// `entity` is the held-out ground truth; `resolved` is the resolver's choice. An
// unresolved mention is recorded with a deterministic, per-mention
// `unresolved:<normalized-mention>` account so it is neither silently dropped nor
// falsely merged with other unresolved mentions. Attach the result to an
// ExecutionGraph node's `attributes` to drive `zetta::EntityResolutionCheck`. The
// output rides the existing `entity_resolution` format (schema-validated at the gate
// boundary) — it introduces no new JSON format.
nlohmann::json entity_resolution_annotation(
    const std::vector<ResolutionCandidate>& candidates,
    const std::vector<LabeledMention>& mentions,
    ResolutionStrategy strategy = ResolutionStrategy::FullName);

// Normalize a name/mention for matching: ASCII-lowercase, fold every run of
// non-alphanumeric characters to a single space, trim the ends. "FedEx Freight" and
// "Fedex  Freight," both normalize to "fedex freight". Exposed for tests + reuse.
std::string normalize_entity_name(std::string_view text);

}  // namespace zetta
