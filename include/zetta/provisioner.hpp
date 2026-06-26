#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/node_capability.hpp"

namespace zetta {

// A node the provisioner has stood up — or, for the given-hardware (Local)
// backend, an existing cluster node it is reporting. Identity + where to reach
// it + what it can do.
struct ProvisionedNode {
    std::string node_id;            // stable node id (the cluster / CRDT actor id)
    std::string address;            // host:port to reach it; empty if not networked
    NodeCapability capability;      // what this node provides

    bool operator==(const ProvisionedNode&) const = default;

    // The JSON value shape:
    //   { "node_id": "<id>", "address": "<host:port>",
    //     "capability": <NodeCapability> }
    // `address` is omitted when empty. Mirrors schemas/node_capability.schema.json
    // ($defs/provisioned_node) — keep them in sync.
    nlohmann::json to_json() const;

    // Parse from JSON, validating at the boundary (delegates the capability to
    // NodeCapability::from_json). std::nullopt unless `value` is an object with a
    // string `node_id` and a valid `capability`.
    static std::optional<ProvisionedNode> from_json(const nlohmann::json& value);
};

// A request to the provisioner: stand up `count` nodes, each meeting `spec`.
struct ProvisionRequest {
    std::size_t count = 0;     // how many nodes to bring up
    NodeCapability spec;       // the per-node capability floor each must satisfy

    bool operator==(const ProvisionRequest&) const = default;
};

// The outcome of a provision call. A backend is **best-effort**: it may return
// fewer nodes than asked for (e.g. the LocalProvisioner is bounded by the
// existing cluster). `satisfied` says whether the full request was met; `detail`
// is a human-readable note (why it came up short, which target was used, …) the
// autoscaler / cost-cap approval gate can surface.
struct ProvisionResult {
    std::vector<ProvisionedNode> nodes;
    bool satisfied = false;
    std::string detail;

    bool operator==(const ProvisionResult&) const = default;
};

// The provisioning seam (ARC-13 / ENT-32) — the central, distributed compute
// pass's source of nodes. Stand up N nodes against a capability spec, tear them
// down, and list what is currently up. Both the given-hardware Local backend
// (#374) and the cloud-burst backends (AWS #377 / GCP #378) implement it; the
// autoscaler / job coordinator consumes it.
//
// **No cloud SDK lives behind this header.** It is the abstract seam only;
// concrete backends are separate slices. Implementations should be safe to call
// from the control loop and idempotent where noted.
class Provisioner {
public:
    virtual ~Provisioner() = default;

    // Bring up nodes meeting `request.spec`, up to `request.count`. Best-effort:
    // the result lists what actually came up (each satisfying the spec) and
    // whether the requested count was met. A count of 0 is a no-op that returns
    // an empty, satisfied result.
    virtual ProvisionResult provision(const ProvisionRequest& request) = 0;

    // Release a previously-provisioned node by id. **Idempotent**: tearing down
    // an unknown / already-gone id is a no-op that returns false (nothing to do),
    // never an error. Returns true iff a node was actually released.
    virtual bool teardown(const std::string& node_id) = 0;

    // The nodes this provisioner currently has up (those provisioned and not yet
    // torn down).
    virtual std::vector<ProvisionedNode> provisioned() const = 0;
};

}  // namespace zetta
