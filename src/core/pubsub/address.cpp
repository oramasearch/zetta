#include "zetta/pubsub/address.hpp"

#include <algorithm>
#include <cctype>

namespace zetta::pubsub {
namespace {

bool is_kind_character(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-';
}

void set_reason(std::string* reason, std::string text) {
    if (reason != nullptr) *reason = std::move(text);
}

}  // namespace

std::string Address::to_string() const { return kind + ":" + name; }

bool Address::is_wildcard() const {
    return !name.empty() && name.back() == '*';
}

std::optional<Address> parse_address(std::string_view text, std::string* reason) {
    const auto separator = text.find(':');
    if (separator == std::string_view::npos) {
        set_reason(reason, "address '" + std::string(text) + "' has no ':' (want kind:name)");
        return std::nullopt;
    }
    const std::string_view kind = text.substr(0, separator);
    const std::string_view name = text.substr(separator + 1);
    if (kind.empty() || !(kind.front() >= 'a' && kind.front() <= 'z') ||
        !std::ranges::all_of(kind, is_kind_character)) {
        set_reason(reason, "address kind '" + std::string(kind) +
                               "' must match [a-z][a-z0-9_-]*");
        return std::nullopt;
    }
    if (name.empty()) {
        set_reason(reason, "address '" + std::string(text) + "' has an empty name");
        return std::nullopt;
    }
    for (std::size_t index = 0; index < name.size(); ++index) {
        const char character = name[index];
        if (std::isspace(static_cast<unsigned char>(character)) || character == ':') {
            set_reason(reason, "address name '" + std::string(name) +
                                   "' may not contain whitespace or ':'");
            return std::nullopt;
        }
        if (character == '*' && index + 1 != name.size()) {
            set_reason(reason, "address name '" + std::string(name) +
                                   "': '*' is only allowed as the last character");
            return std::nullopt;
        }
    }
    return Address{std::string(kind), std::string(name)};
}

bool address_matches(const Address& pattern, const Address& target) {
    if (pattern.kind != target.kind) return false;
    if (!pattern.is_wildcard()) return pattern.name == target.name;
    const std::string_view prefix(pattern.name.data(), pattern.name.size() - 1);
    return std::string_view(target.name).starts_with(prefix);
}

}  // namespace zetta::pubsub
