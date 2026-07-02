#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "zetta/governance/capability.hpp"
#include "zetta/governance/scope.hpp"

namespace zetta {

// An access key (ENT-21): the login credential whose existing data access
// **scopes** a principal's reach. This is the V1 access model (RULE-3 / PERM-1 /
// ADR-6): a request authenticates with a key and thereby inherits exactly the
// key's scope — no bespoke permission model; team/partner/per-dataset tiers are
// deferred (see docs/governance/access-control.md).
//
// An AccessKey is **replicated state** owned by a Principal, the same way a
// ProjectInput is owned by a Project: the metadata (the owning principal id, the
// scope, the created stamp) is one flat key in the Automerge CRDT document
// (`key:<uuid>`, see src/core/catalog.hpp), and an AccessKey is the in-memory +
// on-disk projection of it. It carries no body — the scope is the whole record.
//
// A key is an **immutable credential**: created once with a fixed scope, never
// edited (to change access, issue a new key and drop the old one). On disk it is
// a folder named by its UUID under the principal's `keys/` directory:
//
//   data_dir/<principal-uuid>/keys/<key-uuid>/key.json
//     { id, principal, scope: { unrestricted, grants: [...] }, created }
//
// The id is the folder name and never changes.
//
// A key bounds access along two orthogonal axes: its **scope** (which resources
// it may touch — see scope.hpp) and its **capabilities** (which rights it may
// exercise — see capability.hpp, ENT-22). Both are deny-by-default.
class AccessKey {
public:
    AccessKey(std::string id, std::string principal_id, Scope scope,
              CapabilitySet capabilities, std::string created,
              std::filesystem::path dir);

    const std::string& id() const { return id_; }
    const std::string& principal_id() const { return principal_id_; }
    const Scope& scope() const { return scope_; }
    const CapabilitySet& capabilities() const { return capabilities_; }
    const std::string& created() const { return created_; }
    const std::filesystem::path& dir() const { return dir_; }

    // Does this key's scope permit `resource`? Deny-by-default — delegates to
    // Scope::permits. This is the per-key half of the RULE-3 decision.
    bool permits(const Resource& resource) const {
        return scope_.permits(resource);
    }

    // Does this key's capabilities permit `operation` as of `now`? Deny-by-default
    // — delegates to CapabilitySet::permits (a matching, unexpired capability).
    // The right/verb half of the access decision (ENT-22).
    bool permits(const Operation& operation, std::string_view now) const {
        return capabilities_.permits(operation, now);
    }

    // The replicated/on-disk value shape: { principal, scope, capabilities,
    // created }. The id is the CRDT key / folder name, so it isn't repeated here.
    nlohmann::json metadata() const;

    // Write this key's metadata (id + fields) to dir/key.json.
    void persist_meta() const;

private:
    std::string id_;
    std::string principal_id_;
    Scope scope_;
    CapabilitySet capabilities_;
    std::string created_;
    std::filesystem::path dir_;
};

// The outcome of an access check: allow or deny, plus a human-readable reason.
// The reason explains *why* — fit for the audit trail FEAT-9 builds toward,
// never a stack trace.
struct AccessDecision {
    bool allowed = false;
    std::string reason;

    explicit operator bool() const { return allowed; }
};

// The V1 enforcement decision (RULE-3): a request that logged in with `key`
// inherits its scope, so `resource` is allowed iff the key's scope permits it.
// Deny-by-default (a scope that grants nothing denies everything). The pure
// decision over a known key; Node::authorize resolves a key id to a key (and
// denies an unknown key) before calling this.
AccessDecision authorize(const AccessKey& key, const Resource& resource);

// The capability enforcement decision (ENT-22): `operation` is allowed iff the
// key holds a matching, unexpired capability as of `now` (an ISO-8601 UTC stamp).
// Deny-by-default — a key with no matching capability denies, and the reason
// distinguishes "never granted" from "TTL expired" for the audit trail. The pure
// decision over a known key; Node::authorize(key_id, operation) resolves the key
// (and supplies the current time) before calling this.
AccessDecision authorize(const AccessKey& key, const Operation& operation,
                         std::string_view now);

}  // namespace zetta
