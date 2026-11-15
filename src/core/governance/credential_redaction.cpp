// SPDX-License-Identifier: MIT

#include "zetta/governance/credential_redaction.hpp"

#include <algorithm>
#include <array>

namespace zetta::governance {

bool is_credential_field(std::string_view key) {
    constexpr std::array<std::string_view, 4> names{"token", "secret", "password",
                                                    "api_key"};
    constexpr std::array<std::string_view, 3> suffixes{"_token", "_secret",
                                                       "_password"};
    return std::ranges::find(names, key) != names.end() ||
           std::ranges::any_of(suffixes, [key](std::string_view suffix) {
               return key.ends_with(suffix);
           });
}

nlohmann::json redact_credentials(const nlohmann::json& value) {
    if (value.is_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const nlohmann::json& item : value) out.push_back(redact_credentials(item));
        return out;
    }
    if (!value.is_object()) return value;
    nlohmann::json out = nlohmann::json::object();
    for (const auto& [key, field] : value.items()) {
        if (is_credential_field(key) && !field.is_null()) {
            out[key] = std::string(kRedactedCredential);
        } else {
            out[key] = redact_credentials(field);
        }
    }
    return out;
}

}  // namespace zetta::governance
