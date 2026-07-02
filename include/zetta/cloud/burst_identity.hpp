#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "zetta/governance/capability.hpp"
#include "zetta/cloud/provisioner.hpp"  // ProvisionedNode
#include "zetta/governance/scope.hpp"

namespace zetta { class Node; }

namespace zetta::cloud {

// A short-lived service identity minted for one burst compute node: the service
// Principal and its single AccessKey. This is the credential a burst node
// authenticates with for the life of the job; both the key and the principal are
// dropped on teardown.
struct BurstIdentity {
    std::string node_id;       // the provisioned burst node this identity serves
    std::string principal_id;  // the minted service Principal's id
    std::string key_id;        // the minted AccessKey's id (scoped to the job, TTL'd)

    bool operator==(const BurstIdentity&) const = default;
};

// Bulk-mint short-lived service identities for burst compute nodes on provision,
// and bulk-revoke them on teardown — **reusing** the existing Principal/AccessKey
// identity model (#304), no new identity system (ARC-13; mitigates RSK-9).
//
// Each burst node gets one **service** `Principal` + one `AccessKey` scoped to the
// job (pass a `Scope` naming the job's project) and — to be *short-lived* — TTL'd
// `Capability` grants (see capability.hpp). Principals + keys are ordinary
// replicated state, so a mint replicates + persists like any other identity, and a
// revoke removes it from the document + disk on every node.
//
// The job→identity association is carried in the service principal's **name**
// (`burst/<job-id>/<node-id>`), so the set to revoke is **derived** from the node's
// replicated principals — never a parallel, separately-maintained copy of state
// (the CRDT stays the source of truth; see docs/cluster/replication.md). Because of this,
// `minted()` and `revoke()` keep working after a restart: they read the identities
// straight back off the reconstructed principal tree.
//
// The manager pairs with the `Provisioner` seam (provisioner.hpp): provision the
// nodes, `mint()` identities for the returned `ProvisionedNode`s, then `revoke()`
// them as the job's nodes are torn down. Every mint records a `key_issued` audit
// event and every revoke a `key_revoked` one (FEAT-9), via the Node's AuditSink.
class BurstIdentityManager {
public:
    explicit BurstIdentityManager(Node& node) : node_(node) {}

    // Bulk-mint one service Principal + one scoped AccessKey per node. `scope` is
    // what each key may reach (scope it to the job's project); `capabilities` the
    // rights it may exercise (TTL them so the identity is short-lived). Records a
    // `key_issued` audit event per *newly* minted identity. Returns the identities
    // in node order, skipping any node whose principal/key could not be created.
    // **Idempotent**: a node already minted for this job is returned as-is (no
    // duplicate principal, no second audit event).
    std::vector<BurstIdentity> mint(const std::string& job_id,
                                    const std::vector<ProvisionedNode>& nodes,
                                    const zetta::governance::Scope& scope,
                                    const zetta::governance::CapabilitySet& capabilities = {});

    // Bulk-revoke every identity minted for `job_id`: drop each service principal
    // (which drops its key), recording a `key_revoked` audit event per identity.
    // **Idempotent** — revoking an unknown / already-revoked job is a no-op that
    // returns 0. Returns the number of identities revoked.
    std::size_t revoke(const std::string& job_id);

    // The identities currently minted (not yet revoked) for `job_id`, derived from
    // the node's principals. Empty for an unknown job; order is principal order.
    std::vector<BurstIdentity> minted(const std::string& job_id) const;

private:
    Node& node_;
};

}  // namespace zetta::cloud