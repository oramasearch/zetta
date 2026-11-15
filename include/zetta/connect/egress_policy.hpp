#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <map>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

// The node's **outbound egress policy** (CTL-4,
// [#75](https://github.com/oramasearch/zetta/issues/75)): outbound network from
// the node is **denied by default**. Only destinations on an explicit allowlist,
// set by whoever runs the node, may be dialled.
//
// Every connection the node opens to anything outside its own cluster goes
// through one dialler: the native HTTP client behind `connect::HttpTransport`
// (src/core/api/native_http_transport.cpp). Connector queries, distributed
// ingest, solution action integrations, model-weight downloads and the Clerk
// JWKS fetch all use it, and so does every graph step that reaches the
// network, because a graph step has no other way to open a socket (zetta runs no
// foreign code; `runtime/net.hpp` names only that dialler and the cluster's own
// mutual-TLS peer links). The dialler asks this policy before every connection,
// including each redirect hop, and **before any DNS lookup**, so a denied name
// is never even resolved (a DNS query can carry data out on its own).
//
// The allowlist is a comma- or space-separated list of entries (`--egress-allow`
// / `ZETTA_EGRESS_ALLOW`). Each entry is one destination, optionally `:port`:
//
//   * `api.example.com`      — exactly that host name (case-insensitive)
//   * `*.example.com`        — any name under that domain (not the apex)
//   * `203.0.113.7`, `[2001:db8::1]` — exactly that address
//   * `10.0.0.0/8`, `[2001:db8::/32]` — any resolved address in that range
//   * `*`                    — any destination (turns the control off)
//
// with `:443` (`[...]:443` for an IPv6 entry, which must then be bracketed)
// limiting it to one TCP port; no port means any port.
//
// **Loopback is not egress**: `localhost`, 127.0.0.0/8 and ::1 stay reachable
// under every policy, because traffic to them never leaves the machine (or, in
// Kubernetes, the pod) the node runs on. A sidecar on loopback is the node's own
// deployment, not an outside destination.
//
// Each refused connection is reported to the installed denial observer, which
// the binary wires to the node's audit trail as an `egress_denied` event. The
// policy is a property of the running node, not of how it was built.
//
// Full statement, configuration and limits: docs/governance/egress-policy.md.
namespace zetta::connect {

// One allowlist entry, parsed.
struct EgressRule {
    enum class Kind {
        AnyHost,     // `*`
        Host,        // an exact host name or IP literal
        HostSuffix,  // `*.example.com`: any name ending in `.example.com`
        Network,     // an address range in CIDR form
    };

    Kind kind = Kind::Host;
    // Host: the lowercase name or the canonical IP literal (no brackets).
    // HostSuffix: the lowercase suffix including its leading dot.
    std::string host;
    // Network: the masked network address and its prefix length.
    std::array<std::uint8_t, 16> network{};
    bool ipv6 = false;
    int prefix_length = 0;
    // 0 means any port.
    std::uint16_t port = 0;
    // The entry exactly as it was written, for diagnostics.
    std::string text;
};

class EgressPolicy {
public:
    // The default: an empty allowlist. Only loopback is reachable.
    EgressPolicy() = default;

    // Parse a comma- or space-separated allowlist. Returns nullopt (and fills
    // `error`) on the first malformed entry, so a typo is refused at startup
    // rather than silently widening or narrowing what the node may reach.
    static std::optional<EgressPolicy> parse(std::string_view list,
                                             std::string* error = nullptr);

    // Add one entry. Returns false (and fills `error`) when it is malformed.
    bool add(std::string_view entry, std::string* error = nullptr);

    // Append every rule of `other`.
    void merge(const EgressPolicy& other);

    // The decision on a destination **name** before any DNS lookup: loopback,
    // `*`, an exact host or IP literal, a suffix, or (for an IP literal) a
    // network rule.
    bool allows_name(std::string_view host, std::uint16_t port) const;

    // The decision on one **resolved address** (an IPv4 or IPv6 literal):
    // loopback or a network rule. A name that `allows_name` refused may still be
    // dialled when one of its resolved addresses is inside an allowed range.
    bool allows_address(std::string_view address, std::uint16_t port) const;

    // True when some rule is an address range that could admit `port`, so a
    // name refused by `allows_name` is worth resolving.
    bool has_network_rule_for(std::uint16_t port) const;

    // True when `*` is on the allowlist (the control is off).
    bool allows_everything() const;

    const std::vector<EgressRule>& rules() const { return rules_; }

    // A one-line description for the startup log: the entries as written, or
    // "deny (loopback only)".
    std::string describe() const;

private:
    std::vector<EgressRule> rules_;
};

// True for `localhost`, `*.localhost`, 127.0.0.0/8 and ::1 (and IPv4-mapped
// loopback) — destinations that never leave the machine.
bool is_loopback_destination(std::string_view host);

// The process-wide policy the native HTTP client consults. Starts as the
// default (deny, loopback only). `src/main.cpp` installs the configured one at
// startup. Safe to call from any thread.
void set_egress_policy(EgressPolicy policy);
EgressPolicy egress_policy();

// A refused connection, as reported to the observer.
struct EgressDenial {
    std::string host;
    std::uint16_t port = 0;
    std::string reason;
    // Who the connection was for (#1323): the run's principal (or the
    // capability caller), "local" for the trusted session, empty when no entry
    // point declared one (the node's own background work); and the run, when a
    // graph step made it.
    std::string principal;
    std::string run_id;
    // The surface the caller came through (#1328); empty when none was declared.
    std::string surface;
    // Which allowlist refused it (#1323): "node" (the node's allowlist) or
    // "scope" (the principal's egress scope).
    std::string refused_by = "node";
};

// ---- per-principal egress scopes (#1323) ------------------------------------
//
// An egress scope narrows what named principals may reach, inside the node's
// allowlist: a destination is allowed only when the node allowlist admits it
// and, when the connection's principal has a scope, the scope admits it too. A
// principal with no scope gets the node allowlist alone. Loopback stays exempt
// under every scope. The record is replicated as `egress_scope:<id>`, sealed by
// the identity root like identity records (identity_seal.hpp), and managed by
// `cluster:admin` through `create_egress_scope` / `drop_egress_scope`.
// Docs: docs/governance/egress-policy.md.

// The replicated value of one scope. `principals` are user, role or access-key
// ids; a role names every user that holds it. `allow` is an allowlist in the
// EgressPolicy syntax (an empty list admits loopback only).
struct EgressScopeValue {
    std::string name;
    std::vector<std::string> principals;
    std::string allow;
    friend bool operator==(const EgressScopeValue&, const EgressScopeValue&) = default;
};

nlohmann::json to_json(const EgressScopeValue& value);

// Boundary validation: nullopt (and `error`) unless `value` is an object with
// exactly `name` (a valid connection-style name), `principals` (1 to 256
// non-empty strings) and `allow` (a string EgressPolicy::parse accepts).
std::optional<EgressScopeValue> parse_egress_scope_value(const nlohmann::json& value,
                                                         std::string* error = nullptr);

// What one principal may reach: the union of every scope that names it (or one
// of its roles), and those scopes' names for the refusal.
struct PrincipalEgress {
    EgressPolicy policy;
    std::vector<std::string> scopes;
};

// The process-wide snapshot the dialler consults, keyed by principal id. The
// owner thread compiles it (Node::publish_egress_scopes) and installs it here,
// so the dialler never touches the Node. Safe to call from any thread.
void set_egress_scopes(std::map<std::string, PrincipalEgress> scopes);

// The scope of `principal`, or nullopt when it has none (or is empty or
// "local", the trusted session).
std::optional<PrincipalEgress> egress_scope_for(std::string_view principal);

// The reason a connection the principal's scope refused carries. Starts with
// "egress denied:" like every egress refusal.
std::string egress_scope_denied_reason(std::string_view host, std::uint16_t port,
                                       std::string_view principal,
                                       const std::vector<std::string>& scopes);

// Who an outbound connection is made on behalf of (#1323): a graph step runs
// inside one of these, naming the run's principal and the run, so the dialler
// can attribute an egress denial to them rather than to `node`. Thread-local
// like governance::AuditActorScope (which the dialler falls back to for a
// capability call); scopes nest, the innermost wins.
struct OutboundCaller {
    std::string principal;
    std::string run_id;
};

class OutboundCallerScope {
public:
    explicit OutboundCallerScope(OutboundCaller caller);
    ~OutboundCallerScope();
    OutboundCallerScope(const OutboundCallerScope&) = delete;
    OutboundCallerScope& operator=(const OutboundCallerScope&) = delete;

private:
    std::optional<OutboundCaller> previous_;
};

// The innermost open OutboundCallerScope on this thread, or nullopt.
std::optional<OutboundCaller> current_outbound_caller();

// Called once for every refused connection, on the dialler's thread. The
// binary forwards it to `Node::record_audit_event`. Empty means nothing is
// reported beyond the error the caller receives.
using EgressDenialObserver = std::function<void(const EgressDenial&)>;
void set_egress_denial_observer(EgressDenialObserver observer);

// Report a refusal to the installed observer (no-op when none is installed).
void report_egress_denial(const EgressDenial& denial);

// The reason a refused connection carries: names the destination and how to
// allow it. Every refusal starts with "egress denied:" so callers and tests
// can tell it from a network failure.
std::string egress_denied_reason(std::string_view host, std::uint16_t port);

}  // namespace zetta::connect
