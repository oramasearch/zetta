#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/provisioner.hpp"

namespace zetta {

// How to burst ephemeral worker nodes on the **local host** — the configuration the
// bare-metal cloud-burst backend (#436) is driven by. It mirrors the Azure
// reference (AzureConfig) / GCP / OCI / Kubernetes via the CLI pattern: only the
// worker image is required; everything else has a sensible default. It is
// interpreted by a BareMetalControlPlane (see below) — **no container SDK lives in
// this header** — and carries a validated JSON boundary like the other provisioner
// DTOs.
//
// "Bare metal" is the local host (not a remote cloud / cluster). Each worker runs as
// a **local container** via the host container runtime (`docker` / `podman`) — the
// cross-OS, synchronous-launch mechanism (`docker run -d` returns immediately).
struct BareMetalConfig {
    // Required — a burst has nothing to run without a worker image.
    std::string image;  // container image each worker runs

    // Optional, with defaults filled in below.
    std::string runtime = "docker";        // host container runtime CLI ("docker" / "podman")
    std::string pool_label = "zetta-burst";  // label on every burst worker (stable pool key)
    std::string name_prefix = "zetta-burst"; // container name prefix (a UUID is appended)
    std::string network;                   // container network (e.g. "host"); empty → runtime default
    bool gpus = false;                     // give workers the host GPUs (`--gpus all`)
    std::string seed_peer;                 // cluster seed host:port injected as ZETTA_SEED_PEER to auto-join

    bool operator==(const BareMetalConfig&) const = default;

    // The JSON value shape mirrors schemas/bare_metal_config.schema.json — keep them
    // in sync. Optional string fields are emitted only when set (so a round-trip is
    // stable); `gpus` is always emitted.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a misconfigured
    // burst fails fast instead of bringing up the wrong (or no) fleet. Returns
    // std::nullopt on anything malformed: not an object, a missing/empty required
    // field, or a wrong-typed field.
    static std::optional<BareMetalConfig> from_json(const nlohmann::json& value);
};

// The host control-plane operations the bare-metal backend needs, abstracted behind
// a small **injectable seam** so BareMetalProvisioner stays in zetta-core with no
// container SDK and is unit-testable against a fake. The reference binding is
// BareMetalCliControlPlane (bare_metal_cli_control_plane.hpp), which shells out to
// the host container runtime CLI; a libcontainer / API binding could replace it
// without touching the provisioner's burst / identity / teardown logic. This is the
// exact shape AzureControlPlane established and that GCP / OCI / Kubernetes mirror.
//
// Implementations are best-effort and idempotent where the Provisioner contract
// requires it; they never throw across the seam (zetta-core does not unwind) — a
// failure surfaces as fewer nodes / `satisfied == false` / a `detail` note, or a
// `false` from destroy.
class BareMetalControlPlane {
public:
    virtual ~BareMetalControlPlane() = default;

    // Bring up `request.count` worker nodes meeting `request.spec` on the host.
    // **Best-effort**: may return fewer (resource exhaustion, image pull failure).
    // Each returned ProvisionedNode carries its id (container id/name) + reachable
    // address + the capability it provides. Idempotent on the stable pool label: a
    // second call only launches the shortfall rather than duplicating the fleet.
    virtual ProvisionResult bring_up(const BareMetalConfig& config,
                                     const ProvisionRequest& request) = 0;

    // Release one worker by id. **Idempotent**: an unknown / already-gone id is a
    // no-op returning false, never an error. Returns true iff a worker was released.
    virtual bool destroy(const BareMetalConfig& config,
                         const std::string& node_id) = 0;

    // The workers currently up in the burst pool (those brought up and not yet torn
    // down or exited).
    virtual std::vector<ProvisionedNode> list(const BareMetalConfig& config) const = 0;
};

// BareMetalProvisioner — the bare-metal (local-host) cloud-burst Provisioner backend
// (ARC-13 / ADR-20, #436), a **mirror** of the reference AzureProvisioner (#376) and
// the GCP (#378) / OCI (#432) / Kubernetes (#434) backends. Same burst lifecycle:
// **provision** N ephemeral local worker nodes, **mint** bulk burst identities so
// they can authenticate and join the cluster, run cluster-parallel, then **tear them
// down** and **revoke** those identities — all best-effort and idempotent, matching
// the seam contract.
//
// It owns none of the host mechanics: it delegates them to an injected
// BareMetalControlPlane (above), so it stays in zetta-core with no container SDK and
// is fully unit-testable with a fake control plane. The control plane is the source
// of truth for what is up (`provisioned()` and the "last node?" check both read it),
// so BareMetalProvisioner keeps no parallel copy of fleet state. This class is
// structurally identical to AzureProvisioner / GcpProvisioner / OciProvisioner /
// K8sProvisioner — only the config type it carries differs.
//
// Distinct from LocalProvisioner (#374), which *reports* existing cluster nodes and
// never creates any: BareMetalProvisioner actually **brings up** new local workers.
//
// Bulk burst identity (#375) is wired through two optional **IdentityHooks**
// callables rather than a hard dependency on BurstIdentityManager — the same
// injection style as LocalProvisioner's FleetSource and BurstApprovalGate's
// CostEstimator. Production binds them to BurstIdentityManager::mint / revoke for the
// job; a test passes recording lambdas; both empty means "no identity".
class BareMetalProvisioner : public Provisioner {
public:
    // The bulk burst-identity wiring (optional). `mint` is invoked with the workers a
    // provision call brought up — so each gets a short-lived credential to join the
    // cluster. `revoke` is invoked once, when the burst's **last** worker is torn
    // down, to drop the job's identities in bulk. Production wires these to a
    // BurstIdentityManager for the job (see docs/bare-metal-provisioner.md); leaving
    // both empty disables identity wiring.
    struct IdentityHooks {
        std::function<void(const std::vector<ProvisionedNode>&)> mint;
        std::function<void()> revoke;
    };

    // `control_plane` must outlive the provisioner. `config` is captured by value.
    BareMetalProvisioner(BareMetalControlPlane& control_plane, BareMetalConfig config,
                         IdentityHooks identity = {});

    // Burst up to `request.count` workers meeting `request.spec` via the control
    // plane (best-effort). On any worker coming up, mints bulk identities for the
    // returned workers (so they can join) when an identity mint hook is wired. A
    // count of 0 is a satisfied no-op that touches neither the control plane nor the
    // hooks.
    ProvisionResult provision(const ProvisionRequest& request) override;

    // Tear down one worker by id via the control plane (idempotent — unknown id →
    // false). When a successful teardown leaves the burst pool **empty**, revokes the
    // job's identities in bulk (the revoke hook, if wired). Returns true iff a worker
    // was released.
    bool teardown(const std::string& node_id) override;

    // The workers currently up in the burst pool — delegated to the control plane, so
    // it reflects live state (a worker that exits disappears without a teardown call,
    // like LocalProvisioner reflects self-healing membership).
    std::vector<ProvisionedNode> provisioned() const override;

    const BareMetalConfig& config() const { return config_; }

private:
    BareMetalControlPlane& control_plane_;
    BareMetalConfig config_;
    IdentityHooks identity_;
};

}  // namespace zetta
