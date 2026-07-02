#pragma once

#include <string>
#include <vector>

#include "zetta/cloud/command_runner.hpp"  // CommandRunner / CommandResult (shared, vendor-neutral)
#include "zetta/cloud/oci_provisioner.hpp"

namespace zetta {

// OciCliControlPlane — the **reference** OciControlPlane binding (#432): it drives
// Oracle Cloud Infrastructure by shelling out to the `oci` CLI (no OCI C++ SDK,
// honoring the "no cloud SDK behind the seam" rule), the OCI mirror of the
// reference AzCliControlPlane / GcpCliControlPlane. It translates the burst
// lifecycle into `oci compute-management` commands — ensure an instance
// configuration, create or resize the Instance Pool (the burst pool), list its
// instances, detach (terminate) an instance — runs them through an injected
// CommandRunner, and parses the JSON the CLI returns at the validated
// ProvisionedNode boundary.
//
// This is the borrowed-not-copied OCI *shape* (Instance Pool + preemptible): a
// stable, named Instance Pool (created from an Instance Configuration) is the burst
// pool, resizing it is the burst, detaching+terminating an instance is teardown.
// Unlike Azure/GCP, OCI resources are addressed by OCID, not name — so the binding
// resolves the pool's OCID from its display name (via `--query data[0].id
// --raw-output`) to keep a re-burst idempotent. A production deployment installs
// the `oci` CLI and authenticates it (API key / session token / instance
// principal); CI never runs it, so the command construction + JSON parsing are what
// the unit tests cover via a fake runner.
class OciCliControlPlane : public OciControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real `oci` exec). A test
    // injects a fake that asserts on the argv and returns canned output.
    explicit OciCliControlPlane(CommandRunner runner = {});

    // Resolve the pool by display name; if absent, ensure an instance configuration
    // then create the Instance Pool at `request.count`; if present, resize it.
    // Then list the pool's instances. The `oci` list output is parsed with
    // ProvisionedNode::from_json; malformed entries are skipped. Best-effort:
    // returns what came up with `satisfied` / `detail` set.
    ProvisionResult bring_up(const OciConfig& config,
                             const ProvisionRequest& request) override;

    // Detach + terminate one instance by OCID (`oci compute-management
    // instance-pool-instance detach --is-auto-terminate --is-decrement-size`).
    // Returns true iff the command succeeded; a non-zero exit (e.g. unknown id) is
    // the idempotent no-op → false.
    bool destroy(const OciConfig& config,
                 const std::string& node_id) override;

    // List the Instance Pool's instances (`oci compute-management instance-pool
    // list-instances`), parsed at the ProvisionedNode boundary. An empty / failed
    // listing (or an unresolved pool) yields an empty vector.
    std::vector<ProvisionedNode> list(const OciConfig& config) const override;

private:
    // Resolve the burst pool's OCID from its display name (empty if none / on
    // failure). OCI addresses pools by OCID, so every per-pool call needs this.
    std::string resolve_pool_id(const OciConfig& config) const;

    // List a pool's instances given its already-resolved OCID (empty pool id /
    // failure → empty). bring_up uses the id it just created/resized so it does not
    // re-resolve; list() resolves first then delegates here.
    std::vector<ProvisionedNode> list_instances(const OciConfig& config,
                                                const std::string& pool_id) const;

    CommandRunner runner_;
};

}  // namespace zetta
