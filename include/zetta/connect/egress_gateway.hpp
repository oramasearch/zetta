#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "zetta/connect/egress_policy.hpp"

// The **workspace egress gateway** route
// ([#1331](https://github.com/oramasearch/zetta/issues/1331)).
//
// Inside a Lake workspace a DNS-name egress rule never opens a direct path to
// the destination: the operator's network policy lets the workspace's pods
// reach only the workspace's egress gateway (zetta-tunnel's `egress` role), which
// authorizes the source address, the exact name and the port, resolves the name
// itself, refuses any non-public answer and forwards the bytes. A node that
// dials such a name directly is refused by the network.
//
// This is the node's side of that contract. When a gateway is configured
// (`--egress-gateway HOST:PORT` / `ZETTA_EGRESS_GATEWAY`), a destination that
// matches one of the gateway routes (`--egress-gateway-route LIST` /
// `ZETTA_EGRESS_GATEWAY_ROUTES`) is reached through the gateway by the native
// HTTP client instead of directly:
//
//   * HTTPS on port 443 connects to the gateway and sends its ordinary TLS
//     ClientHello with the destination as SNI. TLS stays end to end and the
//     certificate is checked against the destination name; the gateway never
//     sees the application bytes in clear.
//   * Any other destination port first sends the explicit preface
//     `ZTEG1 <name> <port>\n`, then TLS (https) or plaintext (http).
//
// What does not change:
//
//   * The egress allowlist (`egress_policy.hpp`) is checked on the
//     **destination** name before anything is dialled. A route the allowlist
//     does not admit is refused exactly as before; the gateway address itself is
//     never an allowlist entry, so configuring it widens nothing.
//   * Redirect hops are new connections and go through the same decision.
//   * Only names are routed. IP literals, loopback and names that match no route
//     (in-cluster Services, address-range rules, `*:443`) are dialled directly,
//     because the network policy allows those directly.
//
// A routed destination is not resolved by the node: the gateway resolves it and
// refuses the whole lookup when any answer is not a public unicast address, which
// is at least as strict as the node's destination policy for caller-chosen URLs
// and connectors (`destination_policy.hpp`, `connector_destinations.hpp`).
//
// Full statement: docs/governance/egress-policy.md ("The workspace egress
// gateway").
namespace zetta::connect {

// The destination port the gateway infers from a TLS ClientHello's SNI: the
// operator runs it with `--sni-port 443`. Every other port uses the preface.
inline constexpr std::uint16_t kEgressGatewaySniPort = 443;

// The gateway's default listener when the address names no port.
inline constexpr std::uint16_t kEgressGatewayDefaultPort = 7445;

class EgressGateway {
public:
    // No gateway: every destination is dialled directly.
    EgressGateway() = default;

    // Parse the gateway address (`host`, `host:port`, `[v6]:port`; the port
    // defaults to 7445) and the route list (allowlist syntax restricted to names:
    // `api.example.com`, `*.example.com`, each optionally `:port`). An empty
    // address with an empty route list is "no gateway". Returns nullopt (and fills
    // `error`) on a malformed address or route, on routes without an address, on
    // an address without routes, and on a route that is not a name (`*`, an IP
    // literal or an address range): those are reached directly, never through
    // the gateway.
    static std::optional<EgressGateway> parse(std::string_view address,
                                              std::string_view routes,
                                              std::string* error = nullptr);

    bool configured() const { return !host_.empty(); }
    const std::string& host() const { return host_; }
    std::uint16_t port() const { return port_; }
    const EgressPolicy& routes() const { return routes_; }

    // True when `host:port` is reached through the gateway: a gateway is
    // configured, `host` is a DNS name (not an IP literal, not loopback) and a
    // route matches it. The caller still checks the egress allowlist first.
    bool routes_destination(std::string_view host, std::uint16_t port) const;

    // For the startup log: "none", or "HOST:PORT for ROUTES".
    std::string describe() const;

private:
    std::string host_;
    std::uint16_t port_ = 0;
    EgressPolicy routes_;
};

// The explicit-routing preface for a non-SNI destination:
// `ZTEG1 <name> <port>\n`, the name lowercased with one final dot removed.
std::string egress_gateway_preface(std::string_view host, std::uint16_t port);

// True when `host:port` over `secure` is announced by SNI alone (HTTPS on the
// gateway's SNI port); false when the preface is sent first.
bool egress_gateway_uses_sni(bool secure, std::uint16_t port);

// The process-wide gateway the native HTTP client consults. Starts as "no
// gateway". `src/main.cpp` installs the configured one at startup. Safe to call
// from any thread.
void set_egress_gateway(EgressGateway gateway);
EgressGateway egress_gateway();

}  // namespace zetta::connect
