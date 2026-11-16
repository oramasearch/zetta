// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>
#include <string_view>

// **Which addresses an outbound request may reach**
// ([#1394](https://github.com/oramasearch/zetta/issues/1394)).
//
// Some outbound requests go to a URL a caller chose: an action connector's
// `endpoint` (a webhook, a CRM task API) and a solution's report delivery
// endpoint are stored by a workspace writer and dialled later by the node. Left
// alone, such a URL can name the node's own loopback services, a pod or service
// on the cluster network, or a cloud metadata endpoint, and the request (with
// the delivery's bearer token) goes there. A network policy around the node may
// stop that, or may not: a Lake's Zettas run with `ZETTA_EGRESS_ALLOW=*`.
//
// So such a request is marked `HttpRequest::destinations =
// DestinationPolicy::PublicOnly`, and the native HTTP client then:
//
//   1. resolves the host once;
//   2. classifies every resolved address (`classify_address`) and drops each one
//      that is not **public**, unless its class is explicitly allowed on this
//      node (`set_allowed_destinations`);
//   3. connects only to an address that passed, so the address it checked is the
//      address it dials (no second lookup a DNS answer could change);
//   4. does the same on every redirect hop.
//
// A request whose host resolves to no allowed address is refused before any
// connection, with a reason starting `destination refused:`.
//
// Warehouse sources use `DestinationPolicy::Connector` and their separate exact
// authority allowance (#1414). Model-weights fetches and Clerk JWKS keep `Any`.
//
// Full statement and configuration: docs/governance/destination-policy.md.
namespace zetta::connect {

// Whether a request may reach any resolved address, or only public ones.
enum class DestinationPolicy {
    Any,         // the default: every resolved address may be dialled
    PublicOnly,  // only public addresses, plus the classes allowed on this node
    Connector,   // public, or an exact connector host:port approved by the operator
};

// What kind of address one IP address is.
enum class AddressClass {
    Public,     // a globally routable unicast address
    Loopback,   // 127.0.0.0/8, ::1
    Private,    // 10/8, 172.16/12, 192.168/16, 100.64/10 (shared), fc00::/7, fec0::/10
    LinkLocal,  // 169.254/16, fe80::/10 (except the metadata addresses)
    Metadata,   // cloud instance metadata: 169.254.169.254, 169.254.170.2,
                // 100.100.100.200, fd00:ec2::254
    Reserved,   // unspecified, multicast, broadcast, documentation,
                // benchmarking, 240/4 and other non-routable ranges; never allowed
};

// The class of an IPv4 or IPv6 literal (no brackets, no port). An IPv4-mapped
// (`::ffff:a.b.c.d`), IPv4-compatible or NAT64 (`64:ff9b::/96`) IPv6 address is
// classified by the IPv4 address it carries. Something that is not an address
// literal is `Reserved`, so it is never dialled under `PublicOnly`.
AddressClass classify_address(std::string_view address);

// The name of a class, as written in `ZETTA_ACTION_ALLOW_DESTINATIONS`
// (`public`, `loopback`, `private`, `link-local`, `metadata`, `reserved`).
std::string_view to_string(AddressClass value);

// The non-public classes a `PublicOnly` request may still reach on this node.
// All off by default. `Reserved` can never be allowed.
struct AllowedDestinations {
    bool loopback = false;
    bool private_networks = false;
    bool link_local = false;
    bool metadata = false;

    // Parse a comma- or space-separated list of class names: `loopback`,
    // `private`, `link-local`, `metadata`. An empty list allows nothing. Returns
    // nullopt (and fills `error`) on an unknown name.
    static std::optional<AllowedDestinations> parse(std::string_view list,
                                                    std::string* error = nullptr);

    // A one-line description for the startup log ("public only" or the list).
    std::string describe() const;
};

// True when a `PublicOnly` request may dial an address of class `value`.
bool destination_allowed(AddressClass value, const AllowedDestinations& allowed);

// The process-wide setting the native HTTP client consults. `src/main.cpp`
// installs it from `--action-allow-destinations` /
// `ZETTA_ACTION_ALLOW_DESTINATIONS`; tests that deliver to a loopback fixture
// allow `loopback`. Safe to call from any thread.
void set_allowed_destinations(AllowedDestinations allowed);
AllowedDestinations allowed_destinations();

// The reason a refused request carries. Starts with "destination refused:" and
// names the host, the address and its class, never a header or a body.
std::string destination_refused_reason(std::string_view host, std::string_view address,
                                       AddressClass value);

}  // namespace zetta::connect
