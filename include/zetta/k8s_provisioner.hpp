#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/provisioner.hpp"

namespace zetta {

// Where in a Kubernetes cluster to burst ephemeral worker pods, what image they
// run, and how each joins the cluster — the configuration the Kubernetes
// cloud-burst backend (#434) is driven by. It mirrors the Azure reference
// (AzureConfig) / GCP / OCI via the CLI pattern: only the container image is
// required; everything else has a sensible default. It is interpreted by a
// K8sControlPlane (see below) — **no Kubernetes client library lives in this
// header** — and carries a validated JSON boundary like the other provisioner DTOs.
//
// Unlike the cloud backends, the cluster already exists: it is the kubeconfig
// **context** (configured out-of-band, e.g. eks/gke/oke get-credentials), so this
// config names *where in* the cluster (namespace + Deployment) to burst, not how to
// create the cluster.
struct K8sConfig {
    // Required — a burst has nothing to run without an image.
    std::string image;  // container image the worker pods run

    // Optional, with defaults filled in below.
    std::string namespace_name = "default";        // the namespace the Deployment + pods live in
    std::string deployment_name = "zetta-burst";   // the burst Deployment (pool) name (stable → idempotent)
    std::string context;                           // kubeconfig context; empty → the current context
    std::string service_account;                   // the pods' ServiceAccount; empty → the namespace default
    bool spot = false;                             // schedule onto spot/preemptible nodes (a nodeSelector)
    std::string seed_peer;                         // cluster seed host:port injected as ZETTA_SEED_PEER to auto-join

    bool operator==(const K8sConfig&) const = default;

    // The JSON value shape mirrors schemas/k8s_config.schema.json — keep them in
    // sync. Optional string fields are emitted only when set (so a round-trip is
    // stable); `spot` is always emitted. The `namespace` JSON key maps to
    // `namespace_name` (C++ keyword).
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a misconfigured
    // burst fails fast instead of bringing up the wrong (or no) fleet. Returns
    // std::nullopt on anything malformed: not an object, a missing/empty required
    // field, or a wrong-typed field.
    static std::optional<K8sConfig> from_json(const nlohmann::json& value);
};

// The cluster control-plane operations the Kubernetes backend needs, abstracted
// behind a small **injectable seam** so K8sProvisioner stays in zetta-core with no
// Kubernetes client and is unit-testable against a fake. The reference binding is
// K8sCliControlPlane (k8s_cli_control_plane.hpp), which shells out to the `kubectl`
// CLI; a client-go / REST binding could replace it without touching the
// provisioner's burst / identity / teardown logic. This is the exact shape
// AzureControlPlane established and that GCP / OCI mirror.
//
// Implementations are best-effort and idempotent where the Provisioner contract
// requires it; they never throw across the seam (zetta-core does not unwind) — a
// failure surfaces as fewer nodes / `satisfied == false` / a `detail` note, or a
// `false` from destroy.
class K8sControlPlane {
public:
    virtual ~K8sControlPlane() = default;

    // Bring up `request.count` pods meeting `request.spec` in the burst Deployment
    // described by `config`. **Best-effort**: may return fewer (node capacity,
    // quota). Each returned ProvisionedNode carries its id (pod name) + reachable
    // address (pod IP) + the capability it provides. Idempotent on the stable
    // Deployment name: a second call scales the existing Deployment rather than
    // creating a duplicate.
    virtual ProvisionResult bring_up(const K8sConfig& config,
                                     const ProvisionRequest& request) = 0;

    // Release one pod by id (pod name). **Idempotent**: an unknown / already-gone id
    // is a no-op returning false, never an error. Returns true iff a pod was
    // released.
    virtual bool destroy(const K8sConfig& config,
                         const std::string& node_id) = 0;

    // The pods currently up in the burst Deployment (those brought up and not yet
    // torn down or evicted).
    virtual std::vector<ProvisionedNode> list(const K8sConfig& config) const = 0;
};

// K8sProvisioner — the Kubernetes cloud-burst Provisioner backend (ARC-13 /
// ADR-20, #434), a **mirror** of the reference AzureProvisioner (#376) and the GCP
// (#378) / OCI (#432) backends. Same burst lifecycle: **provision** N ephemeral
// worker pods, **mint** bulk burst identities so they can authenticate and join the
// cluster, run cluster-parallel, then **tear them down** and **revoke** those
// identities — all best-effort and idempotent, matching the seam contract.
//
// It owns none of the Kubernetes mechanics: it delegates them to an injected
// K8sControlPlane (above), so it stays in zetta-core with no Kubernetes client and
// is fully unit-testable with a fake control plane. The control plane is the source
// of truth for what is up (`provisioned()` and the "last node?" check both read
// it), so K8sProvisioner keeps no parallel copy of fleet state. This class is
// structurally identical to AzureProvisioner / GcpProvisioner / OciProvisioner —
// only the config type it carries differs.
//
// Bulk burst identity (#375) is wired through two optional **IdentityHooks**
// callables rather than a hard dependency on BurstIdentityManager — the same
// injection style as LocalProvisioner's FleetSource and BurstApprovalGate's
// CostEstimator. Production binds them to BurstIdentityManager::mint / revoke for
// the job; a test passes recording lambdas; both empty means "no identity".
class K8sProvisioner : public Provisioner {
public:
    // The bulk burst-identity wiring (optional). `mint` is invoked with the pods a
    // provision call brought up — so each gets a short-lived credential to join the
    // cluster. `revoke` is invoked once, when the burst's **last** pod is torn down,
    // to drop the job's identities in bulk. Production wires these to a
    // BurstIdentityManager for the job (see docs/k8s-provisioner.md); leaving both
    // empty disables identity wiring.
    struct IdentityHooks {
        std::function<void(const std::vector<ProvisionedNode>&)> mint;
        std::function<void()> revoke;
    };

    // `control_plane` must outlive the provisioner. `config` is captured by value.
    K8sProvisioner(K8sControlPlane& control_plane, K8sConfig config,
                   IdentityHooks identity = {});

    // Burst up to `request.count` pods meeting `request.spec` via the control plane
    // (best-effort). On any pod coming up, mints bulk identities for the returned
    // pods (so they can join) when an identity mint hook is wired. A count of 0 is a
    // satisfied no-op that touches neither the control plane nor the hooks.
    ProvisionResult provision(const ProvisionRequest& request) override;

    // Tear down one pod by id via the control plane (idempotent — unknown id →
    // false). When a successful teardown leaves the burst pool **empty**, revokes
    // the job's identities in bulk (the revoke hook, if wired). Returns true iff a
    // pod was released.
    bool teardown(const std::string& node_id) override;

    // The pods currently up in the burst Deployment — delegated to the control
    // plane, so it reflects live capacity (an evicted pod disappears without a
    // teardown call, like LocalProvisioner reflects self-healing membership).
    std::vector<ProvisionedNode> provisioned() const override;

    const K8sConfig& config() const { return config_; }

private:
    K8sControlPlane& control_plane_;
    K8sConfig config_;
    IdentityHooks identity_;
};

}  // namespace zetta
