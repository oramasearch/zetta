#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// **Which addresses an outbound request may reach** (LSR-161,
// [#1394](https://github.com/oramasearch/zetta/issues/1394)).
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

// ---- Delivery endpoints (#1453, follows LSR-161) ---------------------------
//
// An action or report delivery may carry a bearer the caller supplies. Two
// rules decide where it may go, before anything is sent:
//
//   1. **HTTPS.** Every delivery endpoint is `https://`, so neither the bearer
//      nor the payload crosses a network in plaintext. The one exception is a
//      loopback host (`127.0.0.0/8`, `::1`, `localhost`): nothing leaves the
//      machine, and the destination policy above still decides whether
//      loopback may be dialled at all.
//   2. **An approved endpoint for a credential.** A delivery that carries a
//      bearer goes only to an origin an administrator approved for the node
//      (`--action-approved-endpoints` / ZETTA_ACTION_APPROVED_ENDPOINTS), never
//      to whatever endpoint a solution or action author stored. A delivery
//      without a bearer needs only rule 1 and the destination policy.

// The origin of an endpoint URL: lowercase scheme and host, and the port
// (the scheme's default when absent). nullopt when it does not parse.
struct EndpointOrigin {
    std::string scheme;
    std::string host;  // without IPv6 brackets
    std::uint16_t port = 0;
    bool operator==(const EndpointOrigin&) const = default;
};
std::optional<EndpointOrigin> endpoint_origin(std::string_view endpoint);

// Empty when `endpoint` satisfies rule 1, else the reason, which starts with
// "delivery refused:".
std::string delivery_endpoint_rejection(std::string_view endpoint);

// The origins approved for delivery credentials. Each entry is
// `https://host[:port]`; `http://` is accepted only for a loopback host, and
// `:*` for any port of that host (a test fixture's ephemeral server).
struct ApprovedDeliveryEndpoints {
    struct Entry {
        EndpointOrigin origin;
        bool any_port = false;
    };
    std::vector<Entry> entries;

    // Parse a comma- or space-separated list. An empty list approves nothing.
    // nullopt (and `error`) on an entry that is not a valid origin, or a
    // plaintext non-loopback one.
    static std::optional<ApprovedDeliveryEndpoints> parse(std::string_view list,
                                                          std::string* error = nullptr);
    bool approves(std::string_view endpoint) const;
    std::string describe() const;
};

// The process-wide setting the action runtime consults. `src/main.cpp`
// installs it; empty (the default) approves no endpoint for a credential.
// Safe to call from any thread.
void set_approved_delivery_endpoints(ApprovedDeliveryEndpoints approved);
ApprovedDeliveryEndpoints approved_delivery_endpoints();

// Empty when a delivery to `endpoint` may carry a bearer (`has_credential`),
// else the reason, starting "delivery refused:". Applies rule 1 always and
// rule 2 when there is a credential.
std::string delivery_credential_rejection(std::string_view endpoint, bool has_credential);

}  // namespace zetta::connect
