#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/node_capability.hpp"
#include "zetta/provisioner.hpp"  // Provisioner, ProvisionedNode

namespace zetta {

// AddNodesRequest — the control-command payload for "add N permanent nodes to
// this cluster" (#518). It is the *permanent* counterpart to a burst
// ProvisionRequest: bring up `count` nodes, each meeting the per-node capability
// floor `spec`, and keep them as ordinary cluster members.
//
// It carries a **validated JSON boundary** (`to_json` / non-throwing `from_json`)
// like the other provisioner DTOs (NodeCapability / AzureConfig / AutoscalerConfig)
// so a malformed control command fails fast instead of bringing up the wrong (or
// an unbounded) fleet. The shape mirrors schemas/add_nodes_request.schema.json —
// keep them in sync.
struct AddNodesRequest {
    std::size_t count = 0;   // how many permanent nodes to add
    NodeCapability spec;     // per-node capability floor (default = no floor: any node)

    bool operator==(const AddNodesRequest&) const = default;

    // The JSON value shape:
    //   { "count": <integer >= 0>, "spec": <NodeCapability> }
    nlohmann::json to_json() const;

    // Parse from JSON, validating the shape at the boundary. Returns std::nullopt
    // on anything malformed: not an object, a missing / non-integer / negative
    // `count`, or an invalid `spec`. `spec` is optional — omitted means the
    // no-floor capability (every node satisfies it).
    static std::optional<AddNodesRequest> from_json(const nlohmann::json& value);
};

// AddNodesResult — the outcome of an add. The `members` are the new permanent
// cluster members that came up (best-effort: a backend may return fewer than the
// requested `count`). `satisfied` says whether the full count was added; `detail`
// is a human-readable note (why it came up short, which target was used, …).
struct AddNodesResult {
    std::vector<ProvisionedNode> members;  // the new permanent members that came up
    bool satisfied = false;                // were all `count` nodes added?
    std::string detail;

    bool operator==(const AddNodesResult&) const = default;
};

// add_nodes — the **permanent-add control entry point** (#518). The goal UX is
// "add 10 nodes to this cluster" and it just adds them: it drives a Provisioner
// (the cloud-burst AzureProvisioner per the issue, or any backend) to bring up
// `count` nodes, each of which auto-joins the cluster via the seed-peer handshake
// and **persists as an ordinary member** (a replicated `node:<id>` key) — the same
// membership path a node added by hand takes, now automatic with the cloud
// provisioner.
//
// It is the *permanent* sibling of the [JobAutoscaler](job_autoscaler.hpp), which
// is the *elastic/burst* consumer of the same seam: this entry point **only**
// provisions and reports — it **never** tears a node down or revokes its identity,
// so the members it adds stay. Elastic / autoscaler / cost-cap burst is
// deliberately out of scope here (those drive the same Provisioner through their
// own loop).
//
// Vendor-neutral: it programs against the abstract `Provisioner&` (like the
// autoscaler), so it works with the cloud backends, the given-hardware
// LocalProvisioner, or a cost-cap-decorated provisioner unchanged. Best-effort —
// the result lists what actually came up. `provider` must outlive the call.
AddNodesResult add_nodes(Provisioner& provider, const AddNodesRequest& request);

// Convenience overload matching the issue's `add_nodes(azure, N)` UX: add `count`
// permanent nodes meeting `spec` (default no-floor). Equivalent to building an
// AddNodesRequest and calling the overload above.
AddNodesResult add_nodes(Provisioner& provider, std::size_t count,
                         const NodeCapability& spec = {});

}  // namespace zetta
