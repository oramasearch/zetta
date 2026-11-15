#include "zetta/connect/egress_gateway.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iterator>
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

namespace {

// Drop `#` comments (to the end of their line) and join the lines with commas,
// so a file may hold one entry per line.
std::string strip_comments(std::string_view text) {
    std::string out;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = text.find('\n', start);
        std::string_view line =
            text.substr(start, (end == std::string_view::npos ? text.size() : end) - start);
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) {
            line = line.substr(0, hash);
        }
        if (!out.empty()) out += ',';
        out += line;
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    // Trim surrounding separators and blanks so a lone address parses.
    const auto is_blank = [](char character) {
        return character == ',' || std::isspace(static_cast<unsigned char>(character));
    };
    while (!out.empty() && is_blank(out.back())) out.pop_back();
    std::size_t first = 0;
    while (first < out.size() && is_blank(out[first])) ++first;
    return out.substr(first);
}

}  // namespace

EgressGatewayFiles::EgressGatewayFiles(std::string directory, EgressGateway fallback)
    : directory_(std::move(directory)), fallback_(std::move(fallback)), gateway_(fallback_) {}

std::string EgressGatewayFiles::directory_of(std::string_view allow_file) {
    const std::filesystem::path parent = std::filesystem::path(allow_file).parent_path();
    return parent.empty() ? std::string(".") : parent.string();
}

EgressGatewayFiles::FileState EgressGatewayFiles::read(std::string_view name) const {
    FileState state;
    const std::filesystem::path path = std::filesystem::path(directory_) / std::string(name);
    std::error_code error_code;
    if (!std::filesystem::exists(path, error_code)) {
        if (error_code) state.error = path.string() + ": " + error_code.message();
        return state;  // missing
    }
    state.present = true;
    const auto size = std::filesystem::file_size(path, error_code);
    if (error_code) {
        state.error = path.string() + ": " + error_code.message();
        return state;
    }
    if (size > kMaxFileBytes) {
        state.error = path.string() + " is larger than 1 MiB";
        return state;
    }
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        state.error = path.string() + ": cannot be read";
        return state;
    }
    state.text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    return state;
}

std::optional<EgressGatewayFiles::Result> EgressGatewayFiles::decide(
    const FileState& gateway, const FileState& routes, std::string& error) const {
    if (!gateway.error.empty() || !routes.error.empty()) {
        error = gateway.error.empty() ? routes.error : gateway.error;
        return std::nullopt;
    }
    Result result;
    result.outcome = Outcome::Applied;
    if (!gateway.present && !routes.present) {
        result.gateway = fallback_;  // neither file: the environment applies
        return result;
    }
    std::string parse_error;
    auto parsed = EgressGateway::parse(strip_comments(gateway.text), strip_comments(routes.text),
                                       &parse_error);
    if (!parsed) {
        error = directory_ + ": " + parse_error;
        return std::nullopt;
    }
    result.gateway = std::move(*parsed);
    result.from_files = true;
    return result;
}

std::optional<EgressGatewayFiles::Result> EgressGatewayFiles::load(std::string* error) {
    const FileState gateway = read(kGatewayFile);
    const FileState routes = read(kRoutesFile);
    last_seen_ = std::make_pair(gateway, routes);
    std::string reason;
    std::optional<Result> result = decide(gateway, routes, reason);
    if (!result) {
        if (error) *error = "egress gateway files: " + reason;
        return std::nullopt;
    }
    gateway_ = result->gateway;
    from_files_ = result->from_files;
    return result;
}

EgressGatewayFiles::Result EgressGatewayFiles::check() {
    const FileState gateway = read(kGatewayFile);
    const FileState routes = read(kRoutesFile);
    Result result;
    result.gateway = gateway_;
    result.from_files = from_files_;
    if (last_seen_ && last_seen_->first == gateway && last_seen_->second == routes) {
        return result;  // Unchanged, including a refused state already reported
    }
    last_seen_ = std::make_pair(gateway, routes);
    std::string reason;
    std::optional<Result> decided = decide(gateway, routes, reason);
    if (!decided) {
        result.outcome = Outcome::Refused;
        result.error = reason;
        return result;
    }
    gateway_ = decided->gateway;
    from_files_ = decided->from_files;
    return *decided;
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
