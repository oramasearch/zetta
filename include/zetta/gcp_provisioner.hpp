#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/provisioner.hpp"

namespace zetta {

// Where on Google Cloud to burst ephemeral compute nodes, how to size and price
// them, and how each node joins the cluster — the configuration the GCP cloud-burst
// backend (#378) is driven by. It mirrors the Azure reference (AzureConfig) via the
// CLI pattern: required are the project + zone; everything else has a sensible
// default. It is interpreted by a GcpControlPlane (see below) — **no cloud SDK
// lives in this header** — and carries a validated JSON boundary like the other
// provisioner DTOs (NodeCapability / ProvisionedNode / BurstCap / AzureConfig).
struct GcpConfig {
    // Required — without these there is no place to create the pool.
    std::string project_id;  // the GCP project
    std::string zone;        // the Compute Engine zone (e.g. "us-central1-a")

    // Optional, with defaults filled in below.
    std::string pool_name = "zetta-burst";  // Managed Instance Group name (stable → idempotent)
    std::string instance_template = "zetta-burst-template";  // the template the MIG instantiates
    std::string machine_type;     // Compute Engine machine type; empty → the control plane maps the spec to one
    std::string image;            // image family; empty → the control plane's default
    std::string network;          // VPC network; empty → the project default
    std::string subnetwork;       // subnetwork within the region; empty → auto subnet
    std::string service_account;  // the service account granted to burst nodes
    bool spot = false;            // burst Spot / preemptible (cheaper, evictable) capacity
    std::string seed_peer;        // cluster seed host:port injected into node bootstrap to auto-join

    bool operator==(const GcpConfig&) const = default;

    // The JSON value shape mirrors schemas/gcp_config.schema.json — keep them in
    // sync. Optional fields are emitted only when set (so a round-trip is stable);
    // `spot` is always emitted.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a misconfigured
    // burst fails fast instead of bringing up the wrong (or no) fleet. Returns
    // std::nullopt on anything malformed: not an object, a missing/empty required
    // field, or a wrong-typed field.
    static std::optional<GcpConfig> from_json(const nlohmann::json& value);
};

// The cloud control-plane operations the GCP backend needs, abstracted behind a
// small **injectable seam** so GcpProvisioner stays in zetta-core with no GCP SDK
// and is unit-testable against a fake. The reference binding is GcpCliControlPlane
// (gcp_cli_control_plane.hpp), which shells out to the `gcloud` CLI; a REST or SDK
// binding could replace it without touching the provisioner's burst / identity /
// teardown logic. This is the exact shape AzureControlPlane established and that
// AWS (#377) mirrors with its own provisioner + control-plane pair.
//
// Implementations are best-effort and idempotent where the Provisioner contract
// requires it; they never throw across the seam (zetta-core does not unwind) — a
// failure surfaces as fewer nodes / `satisfied == false` / a `detail` note, or a
// `false` from destroy.
class GcpControlPlane {
public:
    virtual ~GcpControlPlane() = default;

    // Bring up `request.count` nodes meeting `request.spec` in the burst pool
    // described by `config`. **Best-effort**: may return fewer (Spot capacity,
    // quota). Each returned ProvisionedNode carries its id + reachable address +
    // the capability it actually provides. Idempotent on the stable pool name: a
    // second call scales the existing MIG rather than creating a duplicate.
    virtual ProvisionResult bring_up(const GcpConfig& config,
                                     const ProvisionRequest& request) = 0;

    // Release one node by id. **Idempotent**: an unknown / already-gone id is a
    // no-op returning false, never an error. Returns true iff a node was released.
    virtual bool destroy(const GcpConfig& config,
                         const std::string& node_id) = 0;

    // The nodes currently up in the burst pool (those brought up and not yet torn
    // down or preempted).
    virtual std::vector<ProvisionedNode> list(const GcpConfig& config) const = 0;
};

// GcpProvisioner — the GCP cloud-burst Provisioner backend (ARC-13 / ADR-20,
// #378), the first **mirror** of the reference AzureProvisioner (#376). Same burst
// lifecycle: **provision** N ephemeral GCP nodes, **mint** bulk burst identities so
// they can authenticate and join the cluster, run cluster-parallel, then **tear
// them down** and **revoke** those identities — all best-effort and idempotent,
// matching the seam contract.
//
// It owns none of the GCP mechanics: it delegates them to an injected
// GcpControlPlane (above), so it stays in zetta-core with no cloud SDK and is fully
// unit-testable with a fake control plane. The control plane is the source of truth
// for what is up (`provisioned()` and the "last node?" check both read it), so
// GcpProvisioner keeps no parallel copy of fleet state. This class is structurally
// identical to AzureProvisioner — only the config type it carries differs.
//
// Bulk burst identity (#375) is wired through two optional **IdentityHooks**
// callables rather than a hard dependency on BurstIdentityManager — the same
// injection style as LocalProvisioner's FleetSource and BurstApprovalGate's
// CostEstimator. Production binds them to BurstIdentityManager::mint / revoke for
// the job; a test passes recording lambdas; both empty means "no identity".
class GcpProvisioner : public Provisioner {
public:
    // The bulk burst-identity wiring (optional). `mint` is invoked with the nodes a
    // provision call brought up — so each gets a short-lived credential to join the
    // cluster. `revoke` is invoked once, when the burst's **last** node is torn
    // down, to drop the job's identities in bulk. Production wires these to a
    // BurstIdentityManager for the job (see docs/gcp-provisioner.md); leaving both
    // empty disables identity wiring.
    struct IdentityHooks {
        std::function<void(const std::vector<ProvisionedNode>&)> mint;
        std::function<void()> revoke;
    };

    // `control_plane` must outlive the provisioner. `config` is captured by value.
    GcpProvisioner(GcpControlPlane& control_plane, GcpConfig config,
                   IdentityHooks identity = {});

    // Burst up to `request.count` nodes meeting `request.spec` via the control plane
    // (best-effort). On any node coming up, mints bulk identities for the returned
    // nodes (so they can join) when an identity mint hook is wired. A count of 0 is
    // a satisfied no-op that touches neither the control plane nor the hooks.
    ProvisionResult provision(const ProvisionRequest& request) override;

    // Tear down one node by id via the control plane (idempotent — unknown id →
    // false). When a successful teardown leaves the burst pool **empty**, revokes
    // the job's identities in bulk (the revoke hook, if wired). Returns true iff a
    // node was released.
    bool teardown(const std::string& node_id) override;

    // The nodes currently up in the burst pool — delegated to the control plane, so
    // it reflects live capacity (Spot preemptions disappear without a teardown call,
    // like LocalProvisioner reflects self-healing membership).
    std::vector<ProvisionedNode> provisioned() const override;

    const GcpConfig& config() const { return config_; }

private:
    GcpControlPlane& control_plane_;
    GcpConfig config_;
    IdentityHooks identity_;
};

}  // namespace zetta
