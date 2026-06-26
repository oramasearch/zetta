#pragma once

#include <string>
#include <vector>

#include "zetta/command_runner.hpp"  // CommandRunner / CommandResult (shared, vendor-neutral)
#include "zetta/k8s_provisioner.hpp"

namespace zetta {

// K8sCliControlPlane — the **reference** K8sControlPlane binding (#434): it drives
// a Kubernetes cluster by shelling out to the `kubectl` CLI (no Kubernetes client
// library, honoring the "no client lib behind the seam" rule), the Kubernetes
// mirror of the reference AzCliControlPlane / GcpCliControlPlane / OciCliControlPlane.
// It translates the burst lifecycle into `kubectl` commands — ensure a Deployment,
// set its env / service account / node selector, scale it, list its pods, delete a
// pod — runs them through an injected CommandRunner, and parses the JSON the CLI
// returns at the validated ProvisionedNode boundary.
//
// This is the borrowed-not-copied Kubernetes *shape* (Deployment + scale): a
// stable, named Deployment is the burst pool, scaling it is the burst, deleting a
// pod is teardown. Unlike the cloud backends, the cluster already exists — `kubectl`
// targets it through the kubeconfig context (configured out-of-band). A production
// deployment installs `kubectl` and authenticates a context; CI never runs it, so
// the command construction + JSON parsing are what the unit tests cover via a fake
// runner.
class K8sCliControlPlane : public K8sControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real `kubectl` exec). A test
    // injects a fake that asserts on the argv and returns canned JSON.
    explicit K8sCliControlPlane(CommandRunner runner = {});

    // Ensure the Deployment (create if absent — the stable name makes this
    // idempotent), apply its per-pod shape (env seed / service account / spot node
    // selector), scale it to `request.count`, then list its pods. The `kubectl get`
    // output is parsed with ProvisionedNode::from_json; malformed entries are
    // skipped. Best-effort: returns what came up with `satisfied` / `detail` set.
    ProvisionResult bring_up(const K8sConfig& config,
                             const ProvisionRequest& request) override;

    // Delete one pod by name (`kubectl delete pod <name>`). Returns true iff the
    // command succeeded; a non-zero exit (e.g. unknown name) is the idempotent
    // no-op → false.
    bool destroy(const K8sConfig& config,
                 const std::string& node_id) override;

    // List the Deployment's pods (`kubectl get pods --selector app=<name> --output
    // json`), parsed at the ProvisionedNode boundary. An empty / failed listing
    // yields an empty vector.
    std::vector<ProvisionedNode> list(const K8sConfig& config) const override;

private:
    CommandRunner runner_;
};

}  // namespace zetta
