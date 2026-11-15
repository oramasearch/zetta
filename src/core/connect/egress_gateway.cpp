#include "zetta/connect/egress_gateway.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <mutex>
#include <utility>

// The workspace egress gateway route (#1331). See
// include/zetta/connect/egress_gateway.hpp and docs/governance/egress-policy.md.
namespace zetta::connect {
namespace {

std::string normalized_name(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    if (!host.empty() && host.back() == '.') host.remove_suffix(1);
    std::string out(host);
    std::ranges::transform(out, out.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return out;
}

bool is_ip_literal(std::string_view host) {
    const std::string value = normalized_name(host);
    unsigned char bytes[16] = {};
    return ::inet_pton(AF_INET, value.c_str(), bytes) == 1 ||
           ::inet_pton(AF_INET6, value.c_str(), bytes) == 1;
}

bool valid_gateway_host(std::string_view host) {
    if (host.empty() || host.size() > 253) return false;
    if (is_ip_literal(host)) return true;
    if (host.front() == '.' || host.front() == '-') return false;
    return std::ranges::all_of(host, [](unsigned char character) {
        return std::isalnum(character) || character == '-' || character == '.';
    });
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    unsigned value = 0;
    const auto [end, failure] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (failure != std::errc{} || end != text.data() + text.size() || value == 0 ||
        value > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(value);
    return true;
}

struct Installed {
    std::mutex guard;
    EgressGateway gateway;
};

Installed& installed() {
    static Installed state;
    return state;
}

}  // namespace

std::optional<EgressGateway> EgressGateway::parse(std::string_view address,
                                                  std::string_view routes,
                                                  std::string* error) {
    const auto fail = [&](std::string why) -> std::optional<EgressGateway> {
        if (error) *error = "egress gateway: " + std::move(why);
        return std::nullopt;
    };
    const auto blank = [](std::string_view text) {
        return std::ranges::all_of(text, [](unsigned char character) {
            return character == ',' || std::isspace(character);
        });
    };

    EgressGateway gateway;
    if (blank(address)) {
        if (!blank(routes)) return fail("routes were given without a gateway address");
        return gateway;
    }

    // HOST, HOST:PORT or [V6]:PORT.
    std::string_view host = address;
    std::string_view port;
    if (address.front() == '[') {
        const auto close = address.find(']');
        if (close == std::string_view::npos) return fail("unclosed '[' in the address");
        host = address.substr(1, close - 1);
        const std::string_view rest = address.substr(close + 1);
        if (!rest.empty()) {
            if (rest.front() != ':') return fail("expected ':port' after ']'");
            port = rest.substr(1);
            if (port.empty()) return fail("the port must be 1-65535");
        }
    } else if (std::ranges::count(address, ':') == 1) {
        const auto colon = address.find(':');
        host = address.substr(0, colon);
        port = address.substr(colon + 1);
        if (port.empty()) return fail("the port must be 1-65535");
    } else if (std::ranges::count(address, ':') > 1) {
        return fail("bracket an IPv6 address: [address]:port");
    }
    if (!valid_gateway_host(host)) {
        return fail("'" + std::string(address) + "' is not a host name or address");
    }
    gateway.host_ = normalized_name(host);
    gateway.port_ = kEgressGatewayDefaultPort;
    if (!port.empty() && !parse_port(port, gateway.port_)) {
        return fail("the port must be 1-65535");
    }

    std::string route_error;
    auto parsed = EgressPolicy::parse(routes, &route_error);
    if (!parsed) return fail("route " + route_error);
    for (const EgressRule& rule : parsed->rules()) {
        const bool name = (rule.kind == EgressRule::Kind::Host && !is_ip_literal(rule.host)) ||
                          rule.kind == EgressRule::Kind::HostSuffix;
        if (!name) {
            return fail("route '" + rule.text +
                        "' is not a DNS name: only names are reached through the gateway "
                        "(addresses, ranges and '*' are dialled directly)");
        }
        if (is_loopback_destination(rule.host)) {
            return fail("route '" + rule.text + "' is loopback, which is never egress");
        }
    }
    if (parsed->rules().empty()) {
        return fail("a gateway address was given without routes");
    }
    gateway.routes_ = std::move(*parsed);
    return gateway;
}

bool EgressGateway::routes_destination(std::string_view host, std::uint16_t port) const {
    if (!configured()) return false;
    if (is_ip_literal(host) || is_loopback_destination(host)) return false;
    // The routes hold names only (parse() refuses anything else), so
    // allows_name answers "does a route name this destination".
    return routes_.allows_name(host, port);
}

std::string EgressGateway::describe() const {
    if (!configured()) return "none";
    const bool bracket = host_.find(':') != std::string::npos;
    std::string text = (bracket ? "[" + host_ + "]" : host_) + ":" + std::to_string(port_);
    const std::string routes = routes_.describe();  // "allow a,b"
    constexpr std::string_view prefix = "allow ";
    return text + " for " +
           (routes.starts_with(prefix) ? routes.substr(prefix.size()) : routes);
}

std::string egress_gateway_preface(std::string_view host, std::uint16_t port) {
    return "ZTEG1 " + normalized_name(host) + " " + std::to_string(port) + "\n";
}

bool egress_gateway_uses_sni(bool secure, std::uint16_t port) {
    return secure && port == kEgressGatewaySniPort;
}

void set_egress_gateway(EgressGateway gateway) {
    Installed& state = installed();
    const std::lock_guard<std::mutex> lock(state.guard);
    state.gateway = std::move(gateway);
}

EgressGateway egress_gateway() {
    Installed& state = installed();
    const std::lock_guard<std::mutex> lock(state.guard);
    return state.gateway;
}

}  // namespace zetta::connect
