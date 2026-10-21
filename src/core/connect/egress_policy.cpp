#include "zetta/connect/egress_policy.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <mutex>
#include <utility>

namespace zetta::connect {
namespace {

std::string lowercase(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

// `[2001:db8::1]` -> `2001:db8::1`, `example.com.` -> `example.com`.
std::string normalize_host(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    while (!host.empty() && host.back() == '.') host.remove_suffix(1);
    return lowercase(host);
}

// One parsed IP address: 4 or 16 bytes.
struct Address {
    std::array<std::uint8_t, 16> bytes{};
    bool ipv6 = false;
};

std::optional<Address> parse_address(std::string_view text) {
    const std::string value(text);
    Address address;
    if (::inet_pton(AF_INET, value.c_str(), address.bytes.data()) == 1) {
        return address;
    }
    if (::inet_pton(AF_INET6, value.c_str(), address.bytes.data()) == 1) {
        address.ipv6 = true;
        return address;
    }
    return std::nullopt;
}

// An IPv4-mapped IPv6 address (::ffff:a.b.c.d) as its IPv4 address.
std::optional<Address> unmapped_ipv4(const Address& address) {
    if (!address.ipv6) return std::nullopt;
    for (int i = 0; i < 10; ++i) {
        if (address.bytes[static_cast<std::size_t>(i)] != 0) return std::nullopt;
    }
    if (address.bytes[10] != 0xff || address.bytes[11] != 0xff) return std::nullopt;
    Address v4;
    std::copy_n(address.bytes.begin() + 12, 4, v4.bytes.begin());
    return v4;
}

std::string canonical(const Address& address) {
    char buffer[INET6_ADDRSTRLEN] = {};
    ::inet_ntop(address.ipv6 ? AF_INET6 : AF_INET, address.bytes.data(), buffer,
                sizeof(buffer));
    return buffer;
}

bool same_address(const Address& a, const Address& b) {
    if (a.ipv6 != b.ipv6) {
        const auto a4 = unmapped_ipv4(a);
        const auto b4 = unmapped_ipv4(b);
        if (a4) return same_address(*a4, b);
        if (b4) return same_address(a, *b4);
        return false;
    }
    return std::equal(a.bytes.begin(), a.bytes.begin() + (a.ipv6 ? 16 : 4),
                      b.bytes.begin());
}

bool in_network(const Address& address, const EgressRule& rule) {
    if (address.ipv6 != rule.ipv6) {
        // An IPv4-mapped IPv6 address is the IPv4 address it carries.
        if (const auto v4 = unmapped_ipv4(address); v4 && !rule.ipv6) {
            return in_network(*v4, rule);
        }
        return false;
    }
    int remaining = rule.prefix_length;
    for (std::size_t i = 0; remaining > 0; ++i, remaining -= 8) {
        const int bits = std::min(remaining, 8);
        const auto mask = static_cast<std::uint8_t>(0xff << (8 - bits));
        if ((address.bytes[i] & mask) != (rule.network[i] & mask)) return false;
    }
    return true;
}

bool is_loopback(const Address& address) {
    if (const auto v4 = unmapped_ipv4(address)) return is_loopback(*v4);
    if (!address.ipv6) return address.bytes[0] == 127;
    for (int i = 0; i < 15; ++i) {
        if (address.bytes[static_cast<std::size_t>(i)] != 0) return false;
    }
    return address.bytes[15] == 1;
}

bool valid_host_name(std::string_view name) {
    if (name.empty() || name.size() > 253) return false;
    if (name.front() == '.' || name.front() == '-') return false;
    return std::ranges::all_of(name, [](unsigned char c) {
        return std::isalnum(c) || c == '-' || c == '.' || c == '_';
    });
}

bool port_matches(const EgressRule& rule, std::uint16_t port) {
    return rule.port == 0 || rule.port == port;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() > suffix.size() &&
           text.substr(text.size() - suffix.size()) == suffix;
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    unsigned value = 0;
    const auto [end, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

// The installed policy and observer. A raw mutex is the lowest cross-thread
// handoff here for the same reason as in http_transport.cpp: written once at
// startup on the main thread, read by the dialler's thread afterwards, and
// invisible in the public API.
struct Installed {
    std::mutex guard;
    EgressPolicy policy;
    EgressDenialObserver observer;
};

Installed& installed() {
    static Installed state;
    return state;
}

}  // namespace

std::optional<EgressPolicy> EgressPolicy::parse(std::string_view list,
                                                std::string* error) {
    EgressPolicy policy;
    std::size_t start = 0;
    while (start < list.size()) {
        const auto end = list.find_first_of(", \t\n", start);
        const auto stop = end == std::string_view::npos ? list.size() : end;
        const std::string_view entry = list.substr(start, stop - start);
        if (!entry.empty() && !policy.add(entry, error)) return std::nullopt;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return policy;
}

bool EgressPolicy::add(std::string_view entry, std::string* error) {
    const auto fail = [&](std::string why) {
        if (error) {
            *error = "egress allowlist entry '" + std::string(entry) + "': " + why;
        }
        return false;
    };

    EgressRule rule;
    rule.text = std::string(entry);

    // Split off the port. A bracketed entry is IPv6 (or an IPv6 range);
    // otherwise one colon separates host and port, and more than one colon is
    // an unbracketed IPv6 literal with no port.
    std::string_view host = entry;
    std::string_view port;
    if (!entry.empty() && entry.front() == '[') {
        const auto close = entry.find(']');
        if (close == std::string_view::npos) return fail("unclosed '['");
        host = entry.substr(1, close - 1);
        const std::string_view rest = entry.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return fail("expected ':port' after ']'");
            port = rest.substr(1);
        }
    } else if (std::ranges::count(entry, ':') == 1) {
        const auto colon = entry.find(':');
        host = entry.substr(0, colon);
        port = entry.substr(colon + 1);
    }
    if (!port.empty() || (entry.size() > host.size() && entry.back() == ':')) {
        if (!parse_port(port, rule.port)) return fail("the port must be 1-65535");
    }
    if (host.empty()) return fail("no destination");

    if (host == "*") {
        rule.kind = EgressRule::Kind::AnyHost;
    } else if (host.starts_with("*.")) {
        const std::string suffix = normalize_host(host.substr(1));  // ".example.com"
        if (suffix.size() < 2 || !valid_host_name(suffix.substr(1))) {
            return fail("not a domain name");
        }
        rule.kind = EgressRule::Kind::HostSuffix;
        rule.host = suffix;
    } else if (const auto slash = host.find('/'); slash != std::string_view::npos) {
        const auto address = parse_address(host.substr(0, slash));
        if (!address) return fail("not an IP address range");
        int length = -1;
        const std::string_view digits = host.substr(slash + 1);
        const auto [end, failure] =
            std::from_chars(digits.data(), digits.data() + digits.size(), length);
        const int maximum = address->ipv6 ? 128 : 32;
        if (failure != std::errc{} || end != digits.data() + digits.size() ||
            length < 0 || length > maximum) {
            return fail("the prefix length must be 0-" + std::to_string(maximum));
        }
        rule.kind = EgressRule::Kind::Network;
        rule.ipv6 = address->ipv6;
        rule.prefix_length = length;
        rule.network = address->bytes;
        // Mask the host bits so 10.1.2.3/8 means 10.0.0.0/8.
        for (int bit = length; bit < maximum; ++bit) {
            rule.network[static_cast<std::size_t>(bit / 8)] &=
                static_cast<std::uint8_t>(~(0x80 >> (bit % 8)));
        }
    } else if (const auto address = parse_address(host)) {
        rule.kind = EgressRule::Kind::Host;
        rule.host = canonical(*address);
    } else {
        const std::string name = normalize_host(host);
        if (!valid_host_name(name)) return fail("not a host name or address");
        rule.kind = EgressRule::Kind::Host;
        rule.host = name;
    }
    rules_.push_back(std::move(rule));
    return true;
}

void EgressPolicy::merge(const EgressPolicy& other) {
    rules_.insert(rules_.end(), other.rules_.begin(), other.rules_.end());
}

bool EgressPolicy::allows_name(std::string_view raw_host, std::uint16_t port) const {
    const std::string host = normalize_host(raw_host);
    if (is_loopback_destination(host)) return true;
    const auto literal = parse_address(host);
    for (const EgressRule& rule : rules_) {
        if (!port_matches(rule, port)) continue;
        switch (rule.kind) {
            case EgressRule::Kind::AnyHost:
                return true;
            case EgressRule::Kind::Host:
                if (literal) {
                    const auto allowed = parse_address(rule.host);
                    if (allowed && same_address(*literal, *allowed)) return true;
                } else if (host == rule.host) {
                    return true;
                }
                break;
            case EgressRule::Kind::HostSuffix:
                if (!literal && ends_with(host, rule.host)) return true;
                break;
            case EgressRule::Kind::Network:
                if (literal && in_network(*literal, rule)) return true;
                break;
        }
    }
    return false;
}

bool EgressPolicy::allows_address(std::string_view text, std::uint16_t port) const {
    const auto address = parse_address(normalize_host(text));
    if (!address) return false;
    if (is_loopback(*address)) return true;
    for (const EgressRule& rule : rules_) {
        if (!port_matches(rule, port)) continue;
        switch (rule.kind) {
            case EgressRule::Kind::AnyHost:
                return true;
            case EgressRule::Kind::Host:
                if (const auto allowed = parse_address(rule.host);
                    allowed && same_address(*address, *allowed)) {
                    return true;
                }
                break;
            case EgressRule::Kind::HostSuffix:
                break;
            case EgressRule::Kind::Network:
                if (in_network(*address, rule)) return true;
                break;
        }
    }
    return false;
}

bool EgressPolicy::has_network_rule_for(std::uint16_t port) const {
    return std::ranges::any_of(rules_, [port](const EgressRule& rule) {
        return rule.kind == EgressRule::Kind::Network && port_matches(rule, port);
    });
}

bool EgressPolicy::allows_everything() const {
    return std::ranges::any_of(rules_, [](const EgressRule& rule) {
        return rule.kind == EgressRule::Kind::AnyHost && rule.port == 0;
    });
}

std::string EgressPolicy::describe() const {
    if (rules_.empty()) return "deny (loopback only)";
    std::string out = "allow ";
    for (std::size_t i = 0; i < rules_.size(); ++i) {
        if (i != 0) out += ",";
        out += rules_[i].text;
    }
    return out;
}

bool is_loopback_destination(std::string_view raw_host) {
    const std::string host = normalize_host(raw_host);
    if (host == "localhost" || ends_with(host, ".localhost")) return true;
    const auto address = parse_address(host);
    return address && is_loopback(*address);
}

void set_egress_policy(EgressPolicy policy) {
    Installed& state = installed();
    const std::lock_guard<std::mutex> lock(state.guard);
    state.policy = std::move(policy);
}

EgressPolicy egress_policy() {
    Installed& state = installed();
    const std::lock_guard<std::mutex> lock(state.guard);
    return state.policy;
}

void set_egress_denial_observer(EgressDenialObserver observer) {
    Installed& state = installed();
    const std::lock_guard<std::mutex> lock(state.guard);
    state.observer = std::move(observer);
}

void report_egress_denial(const EgressDenial& denial) {
    EgressDenialObserver observer;
    {
        Installed& state = installed();
        const std::lock_guard<std::mutex> lock(state.guard);
        observer = state.observer;
    }
    if (observer) observer(denial);
}

std::string egress_denied_reason(std::string_view host, std::uint16_t port) {
    return "egress denied: " + std::string(host) + ":" + std::to_string(port) +
           " is not on this node's egress allowlist (add it with --egress-allow "
           "or ZETTA_EGRESS_ALLOW; see docs/governance/egress-policy.md)";
}

}  // namespace zetta::connect
