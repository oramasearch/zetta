#include "zetta/connect/connector_destinations.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <mutex>

#include "zetta/connect/destination_policy.hpp"

namespace zetta::connect {
namespace {

bool dns_character(char character) {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
}

std::string canonical_host(std::string_view host) {
    std::array<unsigned char, 16> address{};
    const std::string text(host);
    for (const int family : {AF_INET, AF_INET6}) {
        if (::inet_pton(family, text.c_str(), address.data()) == 1) {
            std::array<char, INET6_ADDRSTRLEN> buffer{};
            if (::inet_ntop(family, address.data(), buffer.data(), buffer.size())) {
                return buffer.data();
            }
        }
    }
    std::string result(host);
    if (result.ends_with('.')) result.pop_back();
    for (char& character : result) {
        if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
    }
    return result;
}

// This setting adapter is the owner/network/offload thread handoff. Readers
// receive a value snapshot, like the action destination setting adapter.
std::mutex setting_guard;
ConnectorDestinations setting;

}  // namespace

bool valid_connector_host(std::string_view host) {
    if (host.empty() || host.size() > 253 || host.find('\0') != host.npos) return false;
    if (host.find(':') != host.npos) {
        // Some platform parsers accept scope IDs; admit only literal characters
        // before asking the platform to validate the IPv6 structure.
        if (!std::ranges::all_of(host, [](char character) {
                return (character >= '0' && character <= '9') ||
                       (character >= 'a' && character <= 'f') ||
                       (character >= 'A' && character <= 'F') || character == ':' || character == '.';
            })) return false;
        std::array<unsigned char, 16> address{};
        return ::inet_pton(AF_INET6, std::string(host).c_str(), address.data()) == 1;
    }
    if (host.ends_with('.')) host.remove_suffix(1);
    while (!host.empty()) {
        const auto separator = host.find('.');
        const auto label = host.substr(0, separator);
        if (label.empty() || label.size() > 63 || !dns_character(label.front()) ||
            !dns_character(label.back()) || !std::ranges::all_of(label, [](char character) {
                return dns_character(character) || character == '-';
            }))
            return false;
        if (separator == host.npos) return true;
        host.remove_prefix(separator + 1);
    }
    return false;
}

std::optional<std::uint16_t> connector_port(std::string_view port) {
    if (port.empty()) return std::nullopt;
    unsigned number = 0;
    const auto [end, error] = std::from_chars(port.data(), port.data() + port.size(), number);
    if (error != std::errc{} || end != port.data() + port.size() || number == 0 || number > 65535)
        return std::nullopt;
    return static_cast<std::uint16_t>(number);
}

std::optional<ConnectorDestinations> ConnectorDestinations::parse(std::string_view list,
                                                                  std::string* error) {
    ConnectorDestinations result;
    while (!list.empty()) {
        const auto end = list.find_first_of(", \t\r\n");
        const auto entry = list.substr(0, end);
        list = end == list.npos ? std::string_view{} : list.substr(end + 1);
        if (entry.empty()) continue;
        const auto colon = entry.rfind(':');
        std::string_view host = entry.substr(0, colon);
        const auto port =
            colon == entry.npos ? std::nullopt : connector_port(entry.substr(colon + 1));
        const bool bracketed = host.starts_with('[') && host.ends_with(']');
        if (bracketed) {
            host.remove_prefix(1);
            host.remove_suffix(1);
        }
        if (!port || !valid_connector_host(host) || (host.find(':') != host.npos && !bracketed)) {
            if (error) *error = "expected exact host:port entries (IPv6: [address]:port)";
            return std::nullopt;
        }
        result.authorities_.emplace_back(canonical_host(host), *port);
    }
    return result;
}

bool ConnectorDestinations::allows(std::string_view host, std::string_view port,
                                   std::string_view resolved_address) const {
    if (!valid_connector_host(host) || resolved_address.find('\0') != resolved_address.npos)
        return false;
    const auto number = connector_port(port);
    if (!number) return false;
    switch (classify_address(resolved_address)) {
        case AddressClass::Public:
            return true;
        case AddressClass::Private:
        case AddressClass::Loopback:
            return std::ranges::find(authorities_, std::pair{canonical_host(host), *number}) !=
                   authorities_.end();
        case AddressClass::LinkLocal:
        case AddressClass::Metadata:
        case AddressClass::Reserved:
            return false;
    }
    return false;
}

void set_connector_destinations(ConnectorDestinations destinations) {
    const std::lock_guard lock(setting_guard);
    setting = std::move(destinations);
}

ConnectorDestinations connector_destinations() {
    const std::lock_guard lock(setting_guard);
    return setting;
}

}  // namespace zetta::connect
