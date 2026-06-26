#include "zetta/local_provisioner.hpp"

#include <string>
#include <utility>

namespace zetta {

LocalProvisioner::LocalProvisioner(FleetSource fleet)
    : fleet_(std::move(fleet)) {}

ProvisionResult LocalProvisioner::provision(const ProvisionRequest& request) {
    ProvisionResult result;

    // A count of 0 is a satisfied no-op (the seam contract) — and avoids
    // consulting the source when nothing was asked for.
    if (request.count == 0) {
        result.satisfied = true;
        result.detail = "no nodes requested";
        return result;
    }

    // Best-effort selection over the live fleet: take existing cluster nodes
    // that meet the spec, up to the requested count, in fleet order.
    const std::vector<ProvisionedNode> fleet = current_fleet();
    for (const ProvisionedNode& node : fleet) {
        if (result.nodes.size() == request.count) break;
        if (node.capability.satisfies(request.spec)) {
            result.nodes.push_back(node);
        }
    }

    result.satisfied = result.nodes.size() == request.count;
    if (result.satisfied) {
        result.detail = "using existing cluster nodes";
    } else {
        // Given hardware: we can't add capacity to make up the shortfall.
        result.detail = "best-effort: " + std::to_string(result.nodes.size()) +
                        " of " + std::to_string(request.count) +
                        " requested nodes available in the cluster "
                        "(given hardware: cannot add capacity)";
    }
    return result;
}

bool LocalProvisioner::teardown(const std::string& /*node_id*/) {
    // Given hardware is not ours to release: nothing is ever torn down.
    return false;
}

std::vector<ProvisionedNode> LocalProvisioner::provisioned() const {
    return current_fleet();
}

std::vector<ProvisionedNode> LocalProvisioner::current_fleet() const {
    return fleet_ ? fleet_() : std::vector<ProvisionedNode>{};
}

}  // namespace zetta
