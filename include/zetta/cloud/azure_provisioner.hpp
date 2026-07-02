#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/burst_provisioner.hpp"
#include "zetta/cloud/provisioner.hpp"

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
// fully unit-testable with a fake control plane. The shared burst lifecycle
// (provision + mint, teardown + revoke-on-drain, provisioned), the optional
// IdentityHooks, the captured config, and the config() accessor all come from the
// BurstProvisioner<Config, ControlPlane> base — this class only binds the two
// Azure-specific types. The control plane is the source of truth for what is up
// (`provisioned()` and the drain check both read it), so it keeps no parallel copy
// of fleet state.
class AzureProvisioner
    : public BurstProvisioner<AzureConfig, AzureControlPlane> {
public:
    using BurstProvisioner<AzureConfig, AzureControlPlane>::BurstProvisioner;

    // Steer the next burst's placement: region → Azure `location`, instance type →
    // the `vm_size` SKU (the first candidate the control plane's SKU fallback
    // tries). An empty dimension is left unchanged.
    void set_placement(const std::string& region,
                       const std::string& instance_type) override {
        if (!region.empty()) config_.location = region;
        if (!instance_type.empty()) config_.vm_size = instance_type;
    }
};

}  // namespace zetta
