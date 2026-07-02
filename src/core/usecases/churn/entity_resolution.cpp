#include "zetta/usecases/churn/entity_resolution.hpp"

#include <cctype>
#include <set>
#include <sstream>
#include <utility>

namespace zetta {

namespace {

// Split a normalized string (single-space-separated) into its tokens. The input is
// already trimmed + folded by normalize_entity_name, so a plain space split suffices.
std::set<std::string> token_set(const std::string& normalized) {
    std::set<std::string> tokens;
    std::istringstream stream(normalized);
    std::string token;
    while (stream >> token) tokens.insert(token);
    return tokens;
}

// A stable bucket id for a brand: "acct_brand_<normalized-brand>" with the spaces of
// a multi-word brand folded to underscores. Empty brand => empty id.
std::string brand_bucket_id(const std::string& normalized_brand) {
    if (normalized_brand.empty()) return {};
    std::string key = normalized_brand;
    for (char& character : key) {
        if (character == ' ') character = '_';
    }
    return "acct_brand_" + key;
}

}  // namespace

std::string normalize_entity_name(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char raw : text) {
        const unsigned char character = static_cast<unsigned char>(raw);
        if (std::isalnum(character)) {
            if (pending_space && !out.empty()) out.push_back(' ');
            pending_space = false;
            out.push_back(static_cast<char>(std::tolower(character)));
        } else {
            // Any non-alphanumeric run collapses to at most one separating space,
            // and never leads or trails (handled by the !out.empty() guard above and
            // by not emitting a trailing space).
            pending_space = true;
        }
    }
    return out;
}

std::string_view to_string(ResolutionStrategy strategy) {
    switch (strategy) {
        case ResolutionStrategy::FullName: return "full_name";
        case ResolutionStrategy::Brand:    return "brand";
    }
    return "full_name";
}

EntityResolver::EntityResolver(std::vector<ResolutionCandidate> candidates,
                               ResolutionStrategy strategy)
    : candidates_(std::move(candidates)), strategy_(strategy) {
    normalized_names_.reserve(candidates_.size());
    normalized_brands_.reserve(candidates_.size());
    for (const ResolutionCandidate& candidate : candidates_) {
        normalized_names_.push_back(normalize_entity_name(candidate.name));
        normalized_brands_.push_back(normalize_entity_name(candidate.brand));
    }
}

ResolvedMention EntityResolver::resolve(std::string_view mention) const {
    const std::string normalized = normalize_entity_name(mention);

    // The division-correct full-name match: the first candidate whose normalized name
    // equals the normalized mention exactly. Shared by both strategies (Brand falls
    // back to it for a mention whose brand isn't recognized).
    const auto full_name_match = [&]() -> const ResolutionCandidate* {
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            if (normalized_names_[index] == normalized) return &candidates_[index];
        }
        return nullptr;
    };

    if (strategy_ == ResolutionStrategy::Brand) {
        // Coarse: collapse to the brand bucket of the first candidate whose (whole)
        // brand appears among the mention's tokens. This is what conflates the
        // siblings — every "FedEx …" mention lands on acct_brand_fedex.
        const std::set<std::string> tokens = token_set(normalized);
        for (std::size_t index = 0; index < candidates_.size(); ++index) {
            const std::string& brand = normalized_brands_[index];
            if (brand.empty()) continue;
            bool all_present = true;
            for (const std::string& brand_token : token_set(brand)) {
                if (!tokens.contains(brand_token)) {
                    all_present = false;
                    break;
                }
            }
            if (all_present) {
                return {std::string(mention), brand_bucket_id(brand)};
            }
        }
        // No brand matched — resolve to the account by name rather than guess.
        if (const ResolutionCandidate* candidate = full_name_match()) {
            return {std::string(mention), candidate->account_id};
        }
        return {std::string(mention), {}};
    }

    // FullName (default).
    if (const ResolutionCandidate* candidate = full_name_match()) {
        return {std::string(mention), candidate->account_id};
    }
    return {std::string(mention), {}};
}

nlohmann::json entity_resolution_annotation(
    const std::vector<ResolutionCandidate>& candidates,
    const std::vector<LabeledMention>& mentions, ResolutionStrategy strategy) {
    const EntityResolver resolver(candidates, strategy);

    nlohmann::json records = nlohmann::json::array();
    for (const LabeledMention& labeled : mentions) {
        const ResolvedMention decision = resolver.resolve(labeled.mention);
        // Schema requires a non-empty `resolved`. An unresolved mention gets a
        // deterministic, per-mention sentinel so it is visible to the gate (an
        // unresolved mention is not a silent drop) and does not collide with the
        // sentinel of a *different* unresolved mention.
        const std::string resolved =
            decision.resolved()
                ? decision.account_id
                : "unresolved:" + normalize_entity_name(labeled.mention);
        records.push_back({{"mention", labeled.mention},
                           {"entity", labeled.entity},
                           {"resolved", resolved}});
    }
    return nlohmann::json{{"entity_resolution", {{"records", std::move(records)}}}};
}

}  // namespace zetta
