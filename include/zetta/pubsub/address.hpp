#pragma once

#include <compare>
#include <optional>
#include <string>
#include <string_view>

// Pub/sub addresses (#1025, decisions D26/D27 in docs/plans/pubsub/decisions.md).
//
// Everything a pub/sub event can be sent to has a unique typed name
// `kind:name` — `topic:orders`, `agent:405`, `table:logs`. An address whose
// name ends in `*` is a **wildcard**: `agent:*` matches every agent,
// `agent:security-*` every agent whose name starts with `security-` (a prefix
// glob inside the name, not a token wildcard). See docs/pubsub/addressing.md.
namespace zetta::pubsub {

struct Address {
    std::string kind;  // lowercase: [a-z][a-z0-9_-]*
    std::string name;  // non-empty, no whitespace or ':'; may end in '*'

    // "kind:name".
    std::string to_string() const;

    // True when the name ends in '*' (a prefix glob).
    bool is_wildcard() const;

    auto operator<=>(const Address&) const = default;
    bool operator==(const Address&) const = default;
};

// Parse "kind:name" at the boundary. Non-throwing: nullopt on anything
// malformed, with the reason written to `reason` when it is non-null.
// A '*' is accepted only as the last character of the name.
std::optional<Address> parse_address(std::string_view text,
                                     std::string* reason = nullptr);

// Does `pattern` (a concrete address or a wildcard) match the concrete
// `target`? Kinds must be equal; a wildcard name matches every target name
// that starts with the pattern's name minus its trailing '*'.
bool address_matches(const Address& pattern, const Address& target);

}  // namespace zetta::pubsub
