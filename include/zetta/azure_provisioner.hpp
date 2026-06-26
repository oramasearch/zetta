#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/provisioner.hpp"

namespace zetta {

// Where on Azure to burst ephemeral compute nodes, how to size and price them,
// and how each node joins the cluster — the configuration the reference
// cloud-burst backend (#376) is driven by. Required: the subscription, resource
// group, and region; everything else has a sensible default. It is interpreted
// by an AzureControlPlane (see below) — **no cloud SDK lives in this header** —
// and carries a validated JSON boundary like the other provisioner DTOs
// (NodeCapability / ProvisionedNode / BurstCap).
struct AzureConfig {
    // Required — without these there is no place to create the pool.
    std::string subscription_id;  // the Azure subscription
    std::string resource_group;   // owns the burst pool; the unit deleted on full teardown
    std::string location;         // the Azure region (e.g. "eastus")

    // Optional, with defaults filled in below.
    std::string pool_name = "zetta-burst";  // VMSS / Batch pool name (stable → idempotent)
    std::string vm_size;          // VM SKU; empty → the control plane maps the spec to one
    std::string image;            // VM image ref; empty → the control plane's default
    bool spot = false;            // burst Spot / low-priority (cheaper, evictable) capacity
    double max_price = -1.0;      // Spot max $/node-hour; -1 = up to on-demand (never price-evicted)
    std::string seed_peer;        // cluster seed host:port injected into node bootstrap to auto-join
    std::string managed_identity; // optional user-assigned managed identity granted to burst nodes
    std::string tenant_id;        // optional Azure AD tenant id

    bool operator==(const AzureConfig&) const = default;

    // The JSON value shape mirrors schemas/azure_config.schema.json — keep them in
    // sync. Optional fields are emitted only when set (so a round-trip is stable);
    // `spot` and `max_price` are always emitted.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a misconfigured
    // burst fails fast instead of bringing up the wrong (or unbounded) fleet.
    // Returns std::nullopt on anything malformed: not an object, a missing/empty
    // required field, a wrong-typed field, or a max_price below -1.
    static std::optional<AzureConfig> from_json(const nlohmann::json& value);
};

// The cloud control-plane operations the Azure backend needs, abstracted behind
// a small **injectable seam** so AzureProvisioner stays in zetta-core with no
// Azure SDK and is unit-testable against a fake. The reference binding is
// AzCliControlPlane (azure_cli_control_plane.hpp), which shells out to the `az`
// CLI; a REST or SDK binding could replace it without touching the provisioner's
// burst / identity / teardown logic. AWS (#377) and GCP (#378) mirror this shape
// with their own provisioner + control-plane pair.
//
// Implementations are best-effort and idempotent where the Provisioner contract
// requires it; they never throw across the seam (zetta-core does not unwind) —
// a failure surfaces as fewer nodes / `satisfied == false` / a `detail` note, or
// a `false` from destroy.
class AzureControlPlane {
public:
    virtual ~AzureControlPlane() = default;

    // Bring up `request.count` nodes meeting `request.spec` in the burst pool
    // described by `config`. **Best-effort**: may return fewer (Spot capacity,
    // quota). Each returned ProvisionedNode carries its id + reachable address +
    // the capability it actually provides. Idempotent on the stable pool name: a
    // second call scales the existing pool rather than creating a duplicate.
    virtual ProvisionResult bring_up(const AzureConfig& config,
                                     const ProvisionRequest& request) = 0;

    // Release one node by id. **Idempotent**: an unknown / already-gone id is a
    // no-op returning false, never an error. Returns true iff a node was released.
    virtual bool destroy(const AzureConfig& config,
                         const std::string& node_id) = 0;

    // The nodes currently up in the burst pool (those brought up and not yet torn
    // down or evicted).
    virtual std::vector<ProvisionedNode> list(const AzureConfig& config) const = 0;
};

// AzureProvisioner — the **reference** cloud-burst Provisioner backend
// (ARC-13 / ADR-20, #376), the first concrete cloud backend behind the
// Provisioner seam (the given-hardware LocalProvisioner, #374, is the other). It
// realizes the burst lifecycle: **provision** N ephemeral Azure nodes, **mint**
// bulk burst identities so they can authenticate and join the cluster, run
// cluster-parallel, then **tear them down** and **revoke** those identities — all
// best-effort and idempotent, matching the seam contract.
//
// It owns none of the Azure mechanics: it delegates them to an injected
// AzureControlPlane (above), so it stays in zetta-core with no cloud SDK and is
// fully unit-testable with a fake control plane. The control plane is the source
// of truth for what is up (`provisioned()` and the "last node?" check both read
// it), so AzureProvisioner keeps no parallel copy of fleet state.
//
// Bulk burst identity (#375) is wired through two optional **IdentityHooks**
// callables rather than a hard dependency on BurstIdentityManager — the same
// injection style as LocalProvisioner's FleetSource and BurstApprovalGate's
// CostEstimator. Production binds them to BurstIdentityManager::mint / revoke for
// the job; a test passes recording lambdas; both empty means "no identity".
class AzureProvisioner : public Provisioner {
public:
    // The bulk burst-identity wiring (optional). `mint` is invoked with the nodes
    // a provision call brought up — so each gets a short-lived credential to join
    // the cluster. `revoke` is invoked once, when the burst's **last** node is
    // torn down, to drop the job's identities in bulk. Production wires these to a
    // BurstIdentityManager for the job (see docs/azure-provisioner.md); leaving
    // both empty disables identity wiring.
    struct IdentityHooks {
        std::function<void(const std::vector<ProvisionedNode>&)> mint;
        std::function<void()> revoke;
    };

    // `control_plane` must outlive the provisioner. `config` is captured by value.
    AzureProvisioner(AzureControlPlane& control_plane, AzureConfig config,
                     IdentityHooks identity = {});

    // Burst up to `request.count` nodes meeting `request.spec` via the control
    // plane (best-effort). On any node coming up, mints bulk identities for the
    // returned nodes (so they can join) when an identity mint hook is wired. A
    // count of 0 is a satisfied no-op that touches neither the control plane nor
    // the identity hooks.
    ProvisionResult provision(const ProvisionRequest& request) override;

    // Tear down one node by id via the control plane (idempotent — unknown id →
    // false). When a successful teardown leaves the burst pool **empty**, revokes
    // the job's identities in bulk (the revoke hook, if wired). Returns true iff a
    // node was released.
    bool teardown(const std::string& node_id) override;

    // The nodes currently up in the burst pool — delegated to the control plane,
    // so it reflects live capacity (Spot evictions disappear without a teardown
    // call, like LocalProvisioner reflects self-healing membership).
    std::vector<ProvisionedNode> provisioned() const override;

    const AzureConfig& config() const { return config_; }

private:
    AzureControlPlane& control_plane_;
    AzureConfig config_;
    IdentityHooks identity_;
};

}  // namespace zetta
