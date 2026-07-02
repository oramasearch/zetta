#pragma once

#include <string>
#include <vector>

#include "zetta/cloud/command_runner.hpp"  // CommandRunner / CommandResult (shared, vendor-neutral)
#include "zetta/cloud/gcp_provisioner.hpp"

namespace zetta {

// GcpCliControlPlane — the **reference** GcpControlPlane binding (#378): it drives
// Google Cloud by shelling out to the `gcloud` CLI (no GCP C++ SDK, honoring the
// "no cloud SDK behind the seam" rule), the GCP mirror of the reference
// AzCliControlPlane. It translates the burst lifecycle into `gcloud compute`
// commands — ensure an instance template, create + resize the Managed Instance
// Group (the burst pool), list its instances, delete instances — runs them through
// an injected CommandRunner, and parses the JSON the CLI returns at the validated
// ProvisionedNode boundary.
//
// This is the borrowed-not-copied GCP *shape* (Managed Instance Group + Spot): a
// stable, named MIG is the burst pool, scaling it up is the burst, deleting
// instances (or the whole MIG) is teardown. A production deployment installs the
// `gcloud` CLI and authenticates it (`gcloud auth` / a service account); CI never
// runs it, so the command construction + JSON parsing are what the unit tests cover
// via a fake runner. AWS (#377) mirrors this with its own `aws` CLI binding behind
// its own control-plane seam.
class GcpCliControlPlane : public GcpControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real `gcloud` exec). A test
    // injects a fake that asserts on the argv and returns canned JSON.
    explicit GcpCliControlPlane(CommandRunner runner = {});

    // Ensure the instance template + the MIG, scale it to `request.count` nodes
    // (create-or-resize, idempotent on the stable pool name), then list the
    // instances. The `gcloud` list output is parsed with ProvisionedNode::from_json;
    // malformed entries are skipped. Best-effort: returns what came up with
    // `satisfied` / `detail` set.
    ProvisionResult bring_up(const GcpConfig& config,
                             const ProvisionRequest& request) override;

    // Delete one instance by name (`gcloud compute instance-groups managed
    // delete-instances`). Returns true iff the command succeeded; a non-zero exit
    // (e.g. unknown name) is the idempotent no-op → false.
    bool destroy(const GcpConfig& config,
                 const std::string& node_id) override;

    // List the MIG's instances (`gcloud compute instance-groups managed
    // list-instances --format json`), parsed at the ProvisionedNode boundary. An
    // empty / failed listing yields an empty vector.
    std::vector<ProvisionedNode> list(const GcpConfig& config) const override;

private:
    CommandRunner runner_;
};

}  // namespace zetta
