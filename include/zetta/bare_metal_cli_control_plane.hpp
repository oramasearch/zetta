#pragma once

#include <string>
#include <vector>

#include "zetta/bare_metal_provisioner.hpp"
#include "zetta/command_runner.hpp"  // CommandRunner / CommandResult (shared, vendor-neutral)

namespace zetta {

// BareMetalCliControlPlane — the **reference** BareMetalControlPlane binding (#436):
// it brings up worker nodes on the local host by shelling out to the host container
// runtime CLI (`docker` / `podman`), no container SDK — the bare-metal mirror of the
// reference AzCliControlPlane / GcpCliControlPlane / OciCliControlPlane /
// K8sCliControlPlane. It translates the burst lifecycle into runtime commands — list
// the labelled burst workers, `run -d` the shortfall, `rm -f` one — runs them through
// an injected CommandRunner, and parses the JSON the CLI returns at the validated
// ProvisionedNode boundary.
//
// This is the borrowed-not-copied bare-metal *shape* (local containers): a stable
// **pool label** marks the burst workers, launching `count` labelled containers is
// the burst, force-removing one is teardown. `docker run -d` starts a detached
// worker and returns immediately, which is what makes a long-lived worker launchable
// through the synchronous CommandRunner. A production deployment installs the runtime
// (Docker Desktop / Docker Engine / Podman); CI never runs it, so the command
// construction + JSON parsing are what the unit tests cover via a fake runner.
class BareMetalCliControlPlane : public BareMetalControlPlane {
public:
    // `runner` defaults to default_command_runner() (a real runtime exec). A test
    // injects a fake that asserts on the argv and returns canned output.
    explicit BareMetalCliControlPlane(CommandRunner runner = {});

    // Launch the shortfall (`request.count` minus what is already up under the pool
    // label) of worker containers, then list the pool. Idempotent on the pool label:
    // a re-burst to the same count launches nothing. The runtime list output is
    // parsed with ProvisionedNode::from_json; malformed entries are skipped.
    // Best-effort: returns what came up with `satisfied` / `detail` set.
    ProvisionResult bring_up(const BareMetalConfig& config,
                             const ProvisionRequest& request) override;

    // Force-remove one worker container by id/name (`<runtime> rm -f <id>`). Returns
    // true iff the command succeeded; a non-zero exit (e.g. unknown id) is the
    // idempotent no-op → false.
    bool destroy(const BareMetalConfig& config,
                 const std::string& node_id) override;

    // List the burst pool's workers (`<runtime> ps --filter label=<pool> --format
    // '{{json .}}'`), parsed at the ProvisionedNode boundary. An empty / failed
    // listing yields an empty vector.
    std::vector<ProvisionedNode> list(const BareMetalConfig& config) const override;

private:
    CommandRunner runner_;
};

}  // namespace zetta
