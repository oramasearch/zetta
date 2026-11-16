// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <vector>

#include "zetta/cloud/azure_provisioner.hpp"
#include "zetta/cloud/command_runner.hpp"  // CommandRunner / CommandResult (shared, vendor-neutral)

namespace zetta::cloud {

// AzCliControlPlane — the **reference** AzureControlPlane binding (#376): it
// drives Azure by shelling out to the `az` CLI (no Azure C++ SDK, honoring the
// "no cloud SDK behind the seam" rule). It translates the burst lifecycle into
// `az vmss` commands — create/scale the burst pool, list its instances, delete
// instances — runs them through an injected CommandRunner, and parses the JSON
// the CLI returns at the validated ProvisionedNode boundary.
//
// This is the borrowed-not-copied Azure *shape* (VMSS + Spot): a stable, named
// scale set is the burst pool, scaling it up is the burst, deleting instances (or
// the whole pool) is teardown. A production deployment installs the `az` CLI and
// authenticates it (service principal / managed identity); CI never runs it, so
// the command construction + JSON parsing are what the unit tests cover via a
// fake runner. AWS (#377) / GCP (#378) mirror this with their own CLI bindings
// behind their own control-plane seam.
class AzCliControlPlane : public AzureControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real `az` exec). A test
    // injects a fake that asserts on the argv and returns canned JSON.
    explicit AzCliControlPlane(CommandRunner runner = {});

    // Scale the burst pool to `request.count` nodes (create it if absent — the
    // stable pool name makes this idempotent), then list the instances. The `az`
    // list output is parsed with ProvisionedNode::from_json; malformed entries are
    // skipped. Best-effort: returns what came up with `satisfied` / `detail` set.
    ProvisionResult bring_up(const AzureConfig& config,
                             const ProvisionRequest& request) override;

    // Delete one instance by id (`az vmss delete-instances`). Returns true iff the
    // command succeeded; a non-zero exit (e.g. unknown id) is the idempotent
    // no-op → false.
    bool destroy(const AzureConfig& config,
                 const std::string& node_id) override;

    // List the burst pool's instances (`az vmss list-instances`), parsed at the
    // ProvisionedNode boundary. An empty / failed listing yields an empty vector.
    std::vector<ProvisionedNode> list(const AzureConfig& config) const override;

private:
    CommandRunner runner_;
};

}  // namespace zetta::cloud