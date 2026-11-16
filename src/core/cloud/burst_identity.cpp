// SPDX-License-Identifier: MIT

#include "zetta/cloud/burst_identity.hpp"

#include <utility>

#include "zetta/governance/access_key.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/governance/principal.hpp"

namespace zetta::cloud {

namespace {

// The naming convention that ties a burst identity to its job + node. The service
// principal for a burst node is named `burst/<job-id>/<node-id>`, so the identities
// for a job are exactly the principals whose name carries this prefix — the set is
// derived from replicated state, never tracked in a parallel structure.
std::string burst_prefix(const std::string& job_id) {
    return "burst/" + job_id + "/";
}

std::string burst_principal_name(const std::string& job_id,
                                 const std::string& node_id) {
    return burst_prefix(job_id) + node_id;
}

}  // namespace

std::vector<BurstIdentity> BurstIdentityManager::mint(
    const std::string& job_id, const std::vector<ProvisionedNode>& nodes,
    const zetta::governance::Scope& scope, const zetta::governance::CapabilitySet& capabilities) {
    std::vector<BurstIdentity> minted;
    minted.reserve(nodes.size());
    for (const ProvisionedNode& node : nodes) {
        const std::string name = burst_principal_name(job_id, node.node_id);

        // Idempotent: reuse an existing burst principal for this (job, node) rather
        // than minting a duplicate (a re-provision of the same node).
        auto principal = node_.find_principal(name);
        const bool fresh = !principal;
        if (!principal) {
            principal = node_.create_principal(name, zetta::governance::PrincipalKind::Service);
        }
        if (!principal) continue;  // creation failed (disk error)

        // One short-lived, job-scoped key per identity (reuse the existing one on a
        // re-mint so the credential is stable).
        std::shared_ptr<zetta::governance::AccessKey> key =
            principal->keys().empty() ? principal->create_key(scope, capabilities)
                                      : principal->keys().front();
        if (!key) continue;

        // Audit only a genuinely new mint; a re-mint of an existing identity is not
        // a fresh governed action.
        if (fresh) {
            node_.record_audit_event(
                {zetta::governance::AuditAction::KeyIssued, {}, key->id(), true,
                 "burst identity minted for job " + job_id + " on node " +
                     node.node_id,
                 {}});
        }
        minted.push_back({node.node_id, principal->id(), key->id()});
    }
    return minted;
}

std::size_t BurstIdentityManager::revoke(const std::string& job_id) {
    const std::string prefix = burst_prefix(job_id);
    // Snapshot the names first — drop_principal mutates the node's principal list.
    std::vector<std::string> names;
    for (const std::string& name : node_.principal_names()) {
        if (name.starts_with(prefix)) names.push_back(name);
    }

    std::size_t revoked = 0;
    for (const std::string& name : names) {
        auto principal = node_.find_principal(name);
        if (!principal) continue;
        const std::string node_id = name.substr(prefix.size());
        // The credential revoked is the principal's key (there is exactly one); fall
        // back to the principal id only if a key somehow went missing. Record the
        // audit event *before* the drop, while the key id is still available.
        const std::string target = principal->keys().empty()
                                       ? principal->id()
                                       : principal->keys().front()->id();
        node_.record_audit_event(
            {zetta::governance::AuditAction::KeyRevoked, {}, target, true,
             "burst identity revoked for job " + job_id + " on node " + node_id,
             {}});
        if (node_.drop_principal(name)) ++revoked;
    }
    return revoked;
}

std::vector<BurstIdentity> BurstIdentityManager::minted(
    const std::string& job_id) const {
    const std::string prefix = burst_prefix(job_id);
    std::vector<BurstIdentity> result;
    for (const std::string& name : node_.principal_names()) {
        if (!name.starts_with(prefix)) continue;
        auto principal = node_.find_principal(name);
        if (!principal) continue;
        const std::string node_id = name.substr(prefix.size());
        const std::string key_id =
            principal->keys().empty() ? std::string{}
                                      : principal->keys().front()->id();
        result.push_back({node_id, principal->id(), key_id});
    }
    return result;
}

}  // namespace zetta::cloud