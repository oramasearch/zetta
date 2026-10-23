#include "zetta/connect/destination_policy.hpp"

#include <array>
#include <cctype>
#include <cstdint>
#include <mutex>
#include <string>

#include <arpa/inet.h>

// The destination policy (LSR-161, #1394): see
// include/zetta/connect/destination_policy.hpp for the rules. This file only
// classifies addresses and holds the node's setting; the native HTTP client
// (src/core/api/native_http_transport.cpp) applies it to each resolved address.

namespace zetta::connect {
namespace {

using Ipv4 = std::array<std::uint8_t, 4>;
using Ipv6 = std::array<std::uint8_t, 16>;

// True when the first `bits` bits of `address` equal those of `network`.
template <std::size_t N>
bool in_range(const std::array<std::uint8_t, N>& address,
              const std::array<std::uint8_t, N>& network, int bits) {
    for (std::size_t index = 0; index < N && bits > 0; ++index, bits -= 8) {
        const std::uint8_t mask =
            bits >= 8 ? 0xff : static_cast<std::uint8_t>(0xff << (8 - bits));
        if ((address[index] & mask) != (network[index] & mask)) return false;
    }
    return true;
}

AddressClass classify_v4(const Ipv4& a) {
    const auto range = [&a](Ipv4 network, int bits) { return in_range(a, network, bits); };
    // Metadata first: these sit inside the link-local and shared ranges below.
    if (a == Ipv4{169, 254, 169, 254} || a == Ipv4{169, 254, 170, 2} ||
        a == Ipv4{100, 100, 100, 200}) {
        return AddressClass::Metadata;
    }
    if (range({127, 0, 0, 0}, 8)) return AddressClass::Loopback;
    if (range({10, 0, 0, 0}, 8) || range({172, 16, 0, 0}, 12) ||
        range({192, 168, 0, 0}, 16) || range({100, 64, 0, 0}, 10)) {
        return AddressClass::Private;
    }
    if (range({169, 254, 0, 0}, 16)) return AddressClass::LinkLocal;
    if (range({0, 0, 0, 0}, 8) ||          // "this network", including 0.0.0.0
        range({192, 0, 0, 0}, 24) ||       // IETF protocol assignments
        range({192, 0, 2, 0}, 24) ||       // documentation
        range({198, 51, 100, 0}, 24) ||    // documentation
        range({203, 0, 113, 0}, 24) ||     // documentation
        range({198, 18, 0, 0}, 15) ||      // benchmarking
        range({192, 88, 99, 0}, 24) ||     // retired 6to4 relay anycast
        range({224, 0, 0, 0}, 4) ||        // multicast
        range({240, 0, 0, 0}, 4)) {        // reserved, and 255.255.255.255
        return AddressClass::Reserved;
    }
    return AddressClass::Public;
}

AddressClass classify_v6(const Ipv6& a) {
    const auto range = [&a](Ipv6 network, int bits) { return in_range(a, network, bits); };
    // An IPv6 address that carries an IPv4 one is that IPv4 address:
    // ::ffff:a.b.c.d (mapped), ::a.b.c.d (compatible), 64:ff9b::a.b.c.d (NAT64).
    const Ipv4 tail{a[12], a[13], a[14], a[15]};
    if (range({0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff}, 96) ||
        range({0x00, 0x64, 0xff, 0x9b}, 96)) {
        return classify_v4(tail);
    }
    if (range({}, 96)) {
        const Ipv6 loopback{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
        if (a == loopback) return AddressClass::Loopback;
        if (a == Ipv6{}) return AddressClass::Reserved;  // ::
        return classify_v4(tail);                         // ::a.b.c.d
    }
    if (a == Ipv6{0xfd, 0x00, 0x0e, 0xc2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x02, 0x54}) {
        return AddressClass::Metadata;  // fd00:ec2::254, the AWS IPv6 metadata endpoint
    }
    if (range({0xfe, 0x80}, 10)) return AddressClass::LinkLocal;
    if (range({0xfc, 0x00}, 7) || range({0xfe, 0xc0}, 10)) return AddressClass::Private;
    if (range({0xff, 0x00}, 8) ||                     // multicast
        !range({0x20}, 3) ||                          // outside global unicast 2000::/3
        range({0x3f, 0xff}, 20) ||                    // documentation
        range({0x20, 0x01, 0x00, 0x10}, 28) ||        // ORCHID
        range({0x20, 0x01, 0x00, 0x20}, 28) ||        // ORCHIDv2
        range({0x20, 0x01, 0x00, 0x02, 0, 0}, 48) ||  // benchmarking
        range({0x20, 0x01, 0x0d, 0xb8}, 32) ||        // documentation
        range({0x01, 0x00}, 64) ||                    // discard-only
        range({0x20, 0x02}, 16) ||                    // 6to4: carries an arbitrary IPv4
        range({0x20, 0x01, 0x00, 0x00}, 32)) {        // Teredo: likewise
        return AddressClass::Reserved;
    }
    return AddressClass::Public;
}

std::string lowercase(std::string_view text) {
    std::string out(text);
    for (char& character : out) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return out;
}

std::mutex& setting_guard() {
    static std::mutex guard;
    return guard;
}

AllowedDestinations& setting() {
    static AllowedDestinations allowed;
    return allowed;
}

}  // namespace

AddressClass classify_address(std::string_view address) {
    const std::string text(address);
    Ipv4 v4{};
    if (::inet_pton(AF_INET, text.c_str(), v4.data()) == 1) return classify_v4(v4);
    Ipv6 v6{};
    if (::inet_pton(AF_INET6, text.c_str(), v6.data()) == 1) return classify_v6(v6);
    return AddressClass::Reserved;
}

std::string_view to_string(AddressClass value) {
    switch (value) {
        case AddressClass::Public: return "public";
        case AddressClass::Loopback: return "loopback";
        case AddressClass::Private: return "private";
        case AddressClass::LinkLocal: return "link-local";
        case AddressClass::Metadata: return "metadata";
        case AddressClass::Reserved: return "reserved";
    }
    return "reserved";
}

std::optional<AllowedDestinations> AllowedDestinations::parse(std::string_view list,
                                                              std::string* error) {
    AllowedDestinations allowed;
    std::size_t position = 0;
    while (position < list.size()) {
        const std::size_t end = list.find_first_of(", \t", position);
        const std::string_view token =
            list.substr(position, end == std::string_view::npos ? std::string_view::npos
                                                                : end - position);
        position = end == std::string_view::npos ? list.size() : end + 1;
        if (token.empty()) continue;
        const std::string name = lowercase(token);
        if (name == "loopback") {
            allowed.loopback = true;
        } else if (name == "private") {
            allowed.private_networks = true;
        } else if (name == "link-local") {
            allowed.link_local = true;
        } else if (name == "metadata") {
            allowed.metadata = true;
        } else {
            if (error != nullptr) {
                *error = "unknown destination class '" + std::string(token) +
                         "' (expected loopback, private, link-local or metadata)";
            }
            return std::nullopt;
        }
    }
    return allowed;
}

std::string AllowedDestinations::describe() const {
    std::string out;
    const auto add = [&out](bool on, std::string_view name) {
        if (!on) return;
        if (!out.empty()) out += ",";
        out += name;
    };
    add(loopback, "loopback");
    add(private_networks, "private");
    add(link_local, "link-local");
    add(metadata, "metadata");
    return out.empty() ? std::string("public only") : "public + " + out;
}

bool destination_allowed(AddressClass value, const AllowedDestinations& allowed) {
    switch (value) {
        case AddressClass::Public: return true;
        case AddressClass::Loopback: return allowed.loopback;
        case AddressClass::Private: return allowed.private_networks;
        case AddressClass::LinkLocal: return allowed.link_local;
        case AddressClass::Metadata: return allowed.metadata;
        case AddressClass::Reserved: return false;
    }
    return false;
}

void set_allowed_destinations(AllowedDestinations allowed) {
    const std::lock_guard<std::mutex> lock(setting_guard());
    setting() = allowed;
}

AllowedDestinations allowed_destinations() {
    const std::lock_guard<std::mutex> lock(setting_guard());
    return setting();
}

std::string destination_refused_reason(std::string_view host, std::string_view address,
                                       AddressClass value) {
    std::string reason = "destination refused: " + std::string(host);
    if (!address.empty() && address != host) reason += " resolves to " + std::string(address);
    reason += ", a " + std::string(to_string(value)) +
              " address; only public addresses are dialled for this request";
    if (value != AddressClass::Reserved && value != AddressClass::Public) {
        reason += " unless the node allows '" + std::string(to_string(value)) +
                  "' (ZETTA_ACTION_ALLOW_DESTINATIONS)";
    }
    return reason;
}

}  // namespace zetta::connect
