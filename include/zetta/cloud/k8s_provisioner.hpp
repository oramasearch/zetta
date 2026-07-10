#pragma once

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/burst_provisioner.hpp"
#include "zetta/cloud/provisioner.hpp"

namespace zetta::cloud {

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
    std::string join_token;                        // cluster join token (secret) injected as ZETTA_CLUSTER_TOKEN so a burst pod joins a SECURE (mTLS) cluster (#670); set at wire time from this cluster's own token, never serialized (kept off system.provisioners)

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

// The K8sConfig JSON Schema (draft 2020-12) — the code-owned source of truth;
// schemas/k8s_config.schema.json is generated from it and drift-checked by
// tests/test_schemas.cpp (#646).
const nlohmann::json& k8s_config_schema();

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
// is fully unit-testable with a fake control plane. The shared burst lifecycle
// (provision + mint, teardown + revoke-on-drain, provisioned), the optional
// IdentityHooks, the captured config, and the config() accessor all come from the
// BurstProvisioner<Config, ControlPlane> base — this class only binds the two
// Kubernetes-specific types. The control plane is the source of truth for what is
// up (`provisioned()` and the drain check both read it), so it keeps no parallel
// copy of fleet state.
class K8sProvisioner : public BurstProvisioner<K8sConfig, K8sControlPlane> {
public:
    using BurstProvisioner<K8sConfig, K8sControlPlane>::BurstProvisioner;
};

}  // namespace zetta::cloud