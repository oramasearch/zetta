#include "zetta/governance/capability.hpp"

#include <utility>

namespace zetta::governance {

std::string_view to_string(CapabilityDomain domain) {
    switch (domain) {
        case CapabilityDomain::Api:           return "api";
        case CapabilityDomain::Data:          return "data";
        case CapabilityDomain::Orchestration: return "orchestration";
    }
    return "unknown";
}

bool capability_domain_from_string(std::string_view text, CapabilityDomain& out) {
    if (text == "api") {
        out = CapabilityDomain::Api;
        return true;
    }
    if (text == "data") {
        out = CapabilityDomain::Data;
        return true;
    }
    if (text == "orchestration") {
        out = CapabilityDomain::Orchestration;
        return true;
    }
    return false;
}

bool Capability::expired(std::string_view now) const {
    // No TTL => never expires. Otherwise compare ISO-8601 stamps lexicographically
    // (same fixed-width UTC format => chronological); expired once now >= expiry.
    if (expires_at.empty()) return false;
    return now >= expires_at;
}

bool Capability::covers(const Operation& operation, std::string_view now) const {
    return domain == operation.domain && action == operation.action &&
           !expired(now);
}

bool CapabilitySet::permits(const Operation& operation,
                            std::string_view now) const {
    for (const auto& capability : grants) {
        if (capability.covers(operation, now)) return true;
    }
    return false;
}

bool CapabilitySet::has_expired_match(const Operation& operation,
                                      std::string_view now) const {
    bool expired_match = false;
    for (const auto& capability : grants) {
        if (capability.right() != operation) continue;
        if (!capability.expired(now)) return false;  // a live match wins
        expired_match = true;
    }
    return expired_match;
}

CapabilitySet& CapabilitySet::grant(CapabilityDomain domain, std::string action,
                                    std::string expires_at) {
    grants.push_back(
        Capability{domain, std::move(action), std::move(expires_at)});
    return *this;
}

void to_json(nlohmann::json& document, const Capability& capability) {
    document = nlohmann::json{{"domain", to_string(capability.domain)},
                              {"action", capability.action}};
    // Omit the TTL entirely when there is none, so a non-expiring capability is
    // not mistaken for one with an empty/zero expiry.
    if (!capability.expires_at.empty()) {
        document["expires_at"] = capability.expires_at;
    }
}

void to_json(nlohmann::json& document, const CapabilitySet& capabilities) {
    document = nlohmann::json::array();
    for (const auto& capability : capabilities.grants) {
        document.push_back(capability);
    }
}

std::optional<Capability> capability_from_json(const nlohmann::json& document) {
    if (!document.is_object()) return std::nullopt;
    if (!document.contains("domain") || !document["domain"].is_string()) {
        return std::nullopt;
    }
    if (!document.contains("action") || !document["action"].is_string()) {
        return std::nullopt;
    }
    CapabilityDomain domain{};
    if (!capability_domain_from_string(document["domain"].get<std::string>(),
                                       domain)) {
        return std::nullopt;
    }
    Capability parsed;
    parsed.domain = domain;
    parsed.action = document["action"].get<std::string>();
    if (document.contains("expires_at")) {
        if (!document["expires_at"].is_string()) return std::nullopt;
        parsed.expires_at = document["expires_at"].get<std::string>();
    }
    return parsed;
}

std::optional<CapabilitySet> capability_set_from_json(
    const nlohmann::json& document) {
    if (!document.is_array()) return std::nullopt;
    CapabilitySet parsed;
    parsed.grants.reserve(document.size());
    for (const auto& entry : document) {
        const auto capability = capability_from_json(entry);
        if (!capability) return std::nullopt;
        parsed.grants.push_back(*capability);
    }
    return parsed;
}

}  // namespace zetta::governance