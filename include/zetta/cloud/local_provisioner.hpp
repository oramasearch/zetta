#pragma once

#include <functional>
#include <string>
#include <vector>

#include "zetta/cloud/provisioner.hpp"

namespace zetta::cloud {

// LocalProvisioner — the **given-hardware** Provisioner backend (ARC-13, #374),
// the first concrete implementation of the Provisioner seam. It does NOT stand
// up VMs: it reports and uses the nodes **already in the cluster**, best-effort.
// This is the path for when we don't control the infrastructure — the fleet is
// fixed, so `provision` selects from it and `teardown` has nothing to release.
//
// It reads the live fleet through an injected **fleet source** — a callable that
// returns the current cluster nodes as ProvisionedNodes (id + address +
// capability) — rather than owning a Cluster directly. That keeps it in
// zetta-core with no network coupling and makes it unit-testable without a live
// cluster. The source is consulted on every call, so the provisioner always
// reflects live, self-healing membership (a node that left the cluster simply
// stops appearing). Production wiring binds the source to Cluster::nodes() with
// each node's reported NodeCapability once capability reporting lands (a later
// slice); until then a caller supplies the inventory.
//
// The class holds no mutable state of its own beyond the source, so it is as
// thread-safe as the source it is given.
class LocalProvisioner : public Provisioner {
public:
    // The source of the current cluster fleet. Consulted on each provision() /
    // provisioned() call so the provisioner reflects live membership. An empty
    // (null) source behaves as an empty cluster.
    using FleetSource = std::function<std::vector<ProvisionedNode>()>;

    explicit LocalProvisioner(FleetSource fleet);

    // Select up to `request.count` existing cluster nodes that satisfy
    // `request.spec`, in fleet order — **best-effort**: it returns the matching
    // nodes, sets `satisfied` true iff it found the full count, and leaves a
    // `detail` note when it came up short (it cannot add hardware it does not
    // control). A count of 0 is a satisfied no-op. A spec with no floor (a
    // default NodeCapability) is satisfied by every node, so the call returns the
    // current cluster nodes up to the count.
    ProvisionResult provision(const ProvisionRequest& request) override;

    // A no-op. The Local backend uses given hardware it does not own, so it can
    // never release a node — always returns false (nothing was torn down). This
    // is the idempotent contract of the seam, taken to its limit for given
    // hardware.
    bool teardown(const std::string& node_id) override;

    // The current cluster fleet — everything this given-hardware provisioner
    // "has up", which is the whole cluster, since it never creates or destroys
    // nodes.
    std::vector<ProvisionedNode> provisioned() const override;

private:
    // Snapshot the fleet from the source (empty when no source is set).
    std::vector<ProvisionedNode> current_fleet() const;

    FleetSource fleet_;
};

}  // namespace zetta::cloud