#pragma once

#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/cloud/provisioner.hpp"

namespace zetta::cloud {

// BurstProvisioner — the shared orchestration every cloud-burst Provisioner
// backend (Azure #376, AWS #377, GCP #378, OCI #432, Kubernetes #434, bare-metal
// #436) is built on. The burst lifecycle is identical on every target — only the
// Config DTO and the ControlPlane seam type differ — so it lives here once,
// parameterized over those two types, instead of being copy-pasted into each
// backend:
//
//   - **provision**: burst N nodes via the control plane, then mint bulk burst
//     identities for whatever came up so they can join the cluster.
//   - **teardown**: release one node via the control plane, and when that drains
//     the pool, revoke the job's identities in bulk.
//   - **provisioned**: report the live fleet straight from the control plane (the
//     source of truth — no parallel copy of fleet state).
//
// A concrete backend derives from BurstProvisioner<XConfig, XControlPlane>,
// inherits this constructor (`using BurstProvisioner<…>::BurstProvisioner;`), and
// adds only its XConfig-specific surface. The control plane is the source of truth
// for what is up (`provisioned()` and the drain check both read it), so a backend
// keeps no parallel copy of fleet state.
//
// `ControlPlane` is the small injectable seam each backend declares
// (bring_up / destroy / list against its Config) — so the provisioner stays in
// zetta-core with no cloud SDK and is unit-testable against a fake. `Config` is the
// validated JSON DTO describing where/how to burst.
template <class Config, class ControlPlane>
class BurstProvisioner : public Provisioner {
public:
    // The bulk burst-identity wiring (optional, #375). `mint` is invoked with the
    // nodes a provision call brought up — so each gets a short-lived credential to
    // join the cluster. `revoke` is invoked once, when the burst's **last** node is
    // torn down, to drop the job's identities in bulk. Production binds these to a
    // BurstIdentityManager for the job; a test passes recording lambdas; both empty
    // means "no identity". Same injection style as LocalProvisioner's FleetSource
    // and BurstApprovalGate's CostEstimator.
    struct IdentityHooks {
        std::function<void(const std::vector<ProvisionedNode>&)> mint;
        std::function<void()> revoke;
    };

    // `control_plane` must outlive the provisioner. `config` is captured by value.
    BurstProvisioner(ControlPlane& control_plane, Config config,
                     IdentityHooks identity = {})
        : control_plane_(control_plane),
          config_(std::move(config)),
          identity_(std::move(identity)) {}

    // Burst up to `request.count` nodes meeting `request.spec` via the control plane
    // (best-effort). On any node coming up, mints bulk identities for the returned
    // nodes (so they can join) when an identity mint hook is wired. A count of 0 is
    // a satisfied no-op that touches neither the control plane nor the hooks.
    ProvisionResult provision(const ProvisionRequest& request) override {
        // A count of 0 is a satisfied no-op (the seam contract): touch neither the
        // control plane nor the identity hooks.
        if (request.count == 0) {
            ProvisionResult empty;
            empty.satisfied = true;
            empty.detail = "no nodes requested";
            return empty;
        }

        ProvisionResult result = control_plane_.bring_up(config_, request);

        // Mint bulk burst identities for the nodes that came up so they can
        // authenticate and join the cluster (#375). Best-effort backends may return
        // zero nodes (no capacity) — only mint when something actually came up.
        if (!result.nodes.empty() && identity_.mint) {
            identity_.mint(result.nodes);
        }
        return result;
    }

    // Tear down one node by id via the control plane (idempotent — unknown id →
    // false). When a successful teardown leaves the burst pool **empty**, revokes
    // the job's identities in bulk (the revoke hook, if wired). Returns true iff a
    // node was released.
    bool teardown(const std::string& node_id) override {
        const bool released = control_plane_.destroy(config_, node_id);

        // Bulk-revoke the job's identities when this teardown drains the pool — the
        // "bulk revoke on teardown" half of the burst-identity lifecycle. Reading
        // the control plane (the source of truth) avoids a parallel copy of fleet
        // state; revoke is idempotent, so firing it exactly on the transition to
        // empty is safe even if a later teardown finds the pool already empty.
        if (released && identity_.revoke && control_plane_.list(config_).empty()) {
            identity_.revoke();
        }
        return released;
    }

    // The nodes currently up in the burst pool — delegated to the control plane, so
    // it reflects live capacity (Spot evictions / preemptions / exits disappear
    // without a teardown call, like LocalProvisioner reflects self-healing
    // membership).
    std::vector<ProvisionedNode> provisioned() const override {
        return control_plane_.list(config_);
    }

    const Config& config() const { return config_; }

protected:
    ControlPlane& control_plane_;
    Config config_;
    IdentityHooks identity_;
};

}  // namespace zetta::cloud