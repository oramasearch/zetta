#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
};

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

// Parse the text of an egress allowlist **file** (#1332): the same entries as
// `ZETTA_EGRESS_ALLOW`, separated by commas, spaces or line breaks, where `#`
// starts a comment that runs to the end of its line. Returns nullopt (and fills
// `error`) on the first malformed entry.
std::optional<EgressPolicy> parse_egress_allow_file(std::string_view text,
                                                    std::string* error = nullptr);

// An egress allowlist the node re-reads while it runs (#1332,
// `--egress-allow-file PATH` / `ZETTA_EGRESS_ALLOW_FILE`), so a change to what
// the node may reach does not need a restart. Under Kubernetes the file is a
// projected ConfigMap, which kubelet replaces atomically through a symlink:
// every read opens the path afresh, so it follows the link to the new target.
//
// The policy in force is always `base` (the entries that do not come from the
// file: `--egress-allow` / `ZETTA_EGRESS_ALLOW` and the Clerk issuer) plus the
// file's last good contents. A replacement that is malformed, unreadable or
// missing never changes it: the last good policy stays, and the refusal is
// reported once per distinct state of the file. A file missing at startup
// contributes nothing (deny); one malformed or unreadable at startup stops the
// node, like a malformed `--egress-allow`.
//
// Not thread-safe: one owner calls `load` once, then `check` repeatedly.
// Neither installs the policy; the caller passes it to `set_egress_policy`.
class EgressAllowFile {
public:
    EgressAllowFile(std::string path, EgressPolicy base);

    enum class Outcome {
        Unchanged,  // the file reads as it did last time
        Applied,    // a new, well-formed file: `policy` is the one now in force
        Refused,    // a replacement was refused: `policy` is the last good one
    };

    struct Result {
        Outcome outcome = Outcome::Unchanged;
        EgressPolicy policy;  // the policy in force after this read
        std::string error;    // Refused: why. Applied by `load`: a note when the file is missing
    };

    // The first read, at startup. nullopt (and `error`) when the file exists
    // but is malformed or unreadable. A missing file gives `base` alone
    // (Applied, with a note in `Result::error`).
    std::optional<Result> load(std::string* error = nullptr);

    // A later read: Unchanged, Applied or Refused. Never throws.
    Result check();

    const std::string& path() const { return path_; }
    const EgressPolicy& policy() const { return policy_; }

    // The largest file read; a bigger one is refused.
    static constexpr std::size_t kMaxFileBytes = std::size_t{1} << 20;

private:
    // The file's bytes, or nullopt with `error` set (and `missing` when the
    // path does not exist).
    std::optional<std::string> read(bool& missing, std::string& error) const;

    std::string path_;
    EgressPolicy base_;
    EgressPolicy policy_;
    // What the last read saw: the bytes, or nullopt when the file was missing
    // or unreadable (`last_error_` then says why), so a state is reported once.
    std::optional<std::string> last_text_;
    std::string last_error_;
};

}  // namespace zetta::connect
