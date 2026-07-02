#include "zetta/add_nodes.hpp"

#include <cstdint>
#include <set>
#include <utility>
#include <vector>

#include "zetta/node.hpp"
#include "zetta/partition_rebalance.hpp"

namespace zetta {

namespace {

// Accept a JSON integer that is non-negative, whether nlohmann stored it signed
// (a plain `4` literal) or unsigned (what to_json writes). Returns false if it is
// not an integer or is negative — the boundary check for the `count` field. Mirrors
// the helper in node_capability.cpp / job_autoscaler.cpp.
bool as_nonneg_u64(const nlohmann::json& value, std::uint64_t& out) {
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {  // signed-but-non-negative
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value < 0) return false;
        out = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    return false;
}

}  // namespace

nlohmann::json AddNodesRequest::to_json() const {
    return nlohmann::json{{"count", count}, {"spec", spec.to_json()}};
}

std::optional<AddNodesRequest> AddNodesRequest::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    std::uint64_t count = 0;
    if (!value.contains("count") || !as_nonneg_u64(value["count"], count)) {
        return std::nullopt;
    }

    AddNodesRequest request;
    request.count = static_cast<std::size_t>(count);
    // `spec` is optional; absent means the no-floor capability (every node
    // satisfies it). When present it must be a valid NodeCapability.
    if (value.contains("spec")) {
        std::optional<NodeCapability> spec = NodeCapability::from_json(value["spec"]);
        if (!spec) return std::nullopt;
        request.spec = std::move(*spec);
    }
    return request;
}

AddNodesResult add_nodes(Provisioner& provider, const AddNodesRequest& request) {
    // Drive the provisioner to bring up `count` permanent nodes meeting `spec`.
    // The seam handles a count of 0 as a satisfied no-op, so this stays a pure
    // translation of the burst-flavoured ProvisionResult into the permanent-add
    // result. Crucially, add_nodes never calls teardown() / revoke — the members
    // it brings up are permanent.
    ProvisionResult provisioned =
        provider.provision(ProvisionRequest{request.count, request.spec});

    AddNodesResult result;
    result.members = std::move(provisioned.nodes);
    result.satisfied = provisioned.satisfied;
    result.detail = std::move(provisioned.detail);
    return result;
}

AddNodesResult add_nodes(Provisioner& provider, std::size_t count,
                         const NodeCapability& spec) {
    return add_nodes(provider, AddNodesRequest{count, spec});
}

void apply_added_nodes(Node& node, const AddNodesResult& result) {
    // The previous membership is this node's current replicated node:<id> set; the
    // grown membership adds the just-provisioned members (their node_id IS the
    // cluster/CRDT id). Dedup into a sorted set so a re-add of an already-present
    // member doesn't double-count, and so we can tell whether anything new came up.
    const std::vector<std::string> previous = node.cluster_members();
    std::set<std::string> grown(previous.begin(), previous.end());
    bool added_member = false;
    for (const ProvisionedNode& member : result.members) {
        if (!member.node_id.empty() && grown.insert(member.node_id).second) {
            added_member = true;
        }
    }

    // Nothing new joined (a zero / best-effort-short add, or an idempotent re-add)
    // — there is no membership delta, so leave placement untouched.
    if (added_member) {
        const std::vector<std::string> members(grown.begin(), grown.end());
        rebalance_partitions(node, members, previous);
    }
}

AddNodesResult add_nodes(Node& node, Provisioner& provider,
                         const AddNodesRequest& request) {
    // Provision first (the Provisioner-only path — the slow, Node-free half a GUI
    // caller can run off the UI thread), then apply the membership change + the
    // between-jobs rebalance (#519) on the thread that owns the node.
    AddNodesResult result = add_nodes(provider, request);
    apply_added_nodes(node, result);
    return result;
}

AddNodesResult add_nodes(Node& node, Provisioner& provider, std::size_t count,
                         const NodeCapability& spec) {
    return add_nodes(node, provider, AddNodesRequest{count, spec});
}

}  // namespace zetta
