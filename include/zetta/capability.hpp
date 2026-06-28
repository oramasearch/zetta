#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {

// The governance domain a right belongs to (ENT-22). A capability grants a
// **specific** right, and ENT-22 groups rights into three coarse domains:
//
//   api            — an API surface / command right (e.g. issue a query, deploy)
//   data           — a data-access right (e.g. read or write rows)
//   orchestration  — a control right over runs/graphs (e.g. start a run)
//
// The domain is one half of a right's identity (domain + action); finer domains
// layer on later by extending this enum without changing the Capability shape.
enum class CapabilityDomain { Api, Data, Orchestration };

// Canonical lowercase name of a domain ("api" / "data" / "orchestration"). This
// is BOTH the display label AND the stored representation (the strings written
// to the CRDT key value + key.json), so it is a stable on-disk identifier.
std::string_view to_string(CapabilityDomain domain);

// Parse a stored/canonical domain name back to the enum; false (out untouched)
// on an unknown spelling — the boundary check for the enum domain.
bool capability_domain_from_string(std::string_view text, CapabilityDomain& out);

// An operation a request wants to perform: a (domain, action) pair. This is the
// thing a capability is matched against — the verb/right half of an access
// decision, orthogonal to the resource a Scope names (see scope.hpp). The action
// is a free-form right name within the domain (e.g. "query", "read", "deploy",
// "start_run"); the taxonomy of actions is intentionally open in V1.
struct Operation {
    CapabilityDomain domain = CapabilityDomain::Api;
    std::string action;

    bool operator==(const Operation&) const = default;
};

// A Capability (ENT-22): a **deny-by-default** grant of a specific right — an
// API / data / orchestration action — optionally **time-bounded** (TTL'd, the
// distinguishing property of a capability vs. a Scope grant). It is the unit of
// authority for orchestration + the sandbox; the audit trail is a separate
// FEAT-9 slice (see docs/access-control.md).
//
// `expires_at` is an optional ISO-8601 UTC stamp in the SAME fixed-width format
// as zetta::now_iso8601() (millisecond precision, trailing 'Z'), so chronology
// is a plain lexicographic compare. Empty means **no TTL** — the capability
// never expires. A capability is considered expired once `now >= expires_at`.
struct Capability {
    CapabilityDomain domain = CapabilityDomain::Api;
    std::string action;
    std::string expires_at;  // optional; empty = never expires (no TTL)

    bool operator==(const Capability&) const = default;

    // The right this capability grants, without its TTL — what it is matched on.
    Operation right() const { return Operation{domain, action}; }

    // Has this capability's TTL elapsed as of `now`? Always false when there is
    // no TTL (`expires_at` empty). `now` is an ISO-8601 UTC stamp.
    bool expired(std::string_view now) const;

    // Does this capability cover `operation` as of `now`? True iff the right
    // matches exactly (same domain AND action) and it has not expired.
    bool covers(const Operation& operation, std::string_view now) const;
};

// A credential's set of capabilities: the rights it may exercise. The realization
// of ENT-22's deny-by-default rule — an operation is permitted only when a
// matching, unexpired capability is present; everything else is denied. There is
// no wildcard/"unrestricted" capability (unlike Scope): capabilities name
// **specific** rights by design.
struct CapabilitySet {
    std::vector<Capability> grants;

    // The access decision: is `operation` permitted as of `now`? Deny-by-default
    // — true iff some capability covers it (matching right, not expired).
    bool permits(const Operation& operation, std::string_view now) const;

    // True iff a capability matches `operation`'s right but is expired — lets the
    // caller distinguish "denied: TTL elapsed" from "denied: never granted" for
    // the audit reason. (A non-expired match makes this false.)
    bool has_expired_match(const Operation& operation, std::string_view now) const;

    // Add a capability to the set (optionally TTL'd). Returns *this so grants can
    // be chained when building a set by hand or in a test.
    CapabilitySet& grant(CapabilityDomain domain, std::string action,
                         std::string expires_at = {});

    bool operator==(const CapabilitySet&) const = default;
};

// JSON serialization. to_json are ADL hooks for nlohmann::json, so
// `nlohmann::json document = capabilities;` works.
//
//   Capability     -> { "domain": "<domain>", "action": "<action>",
//                       "expires_at": "<iso8601>"? }   (expires_at omitted if empty)
//   CapabilitySet  -> [ <Capability>... ]              (a bare array of grants)
void to_json(nlohmann::json& document, const Capability& capability);
void to_json(nlohmann::json& document, const CapabilitySet& capabilities);

// Parse from JSON, validating the shape at the boundary so a malformed capability
// fails fast instead of silently widening or narrowing access (a
// security-sensitive boundary — see docs/access-control.md). Each returns
// std::nullopt on anything malformed: not an object/array, a missing or
// wrong-typed field, or an unknown domain. Mirrors the `capability` $def in
// schemas/access_key.schema.json — keep them in sync.
std::optional<Capability> capability_from_json(const nlohmann::json& document);
std::optional<CapabilitySet> capability_set_from_json(
    const nlohmann::json& document);

}  // namespace zetta
