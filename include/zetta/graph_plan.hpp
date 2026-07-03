#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "zetta/ir/step_kind.hpp"

// The graph-over-cluster plan projection (#591, part of #575 area 6) — given an
// ExecutionGraph IR and the current cluster, project HOW each step would run over
// it: which steps run near their data, which fan out across the cluster, and which
// nodes each lands on.
//
// This is a PROJECTION of zetta's documented execution model (SPMD +
// compute-near-data), NOT a committed schedule. There is no general per-step
// scheduler yet — R1 deploy places onto the local node, and the compute pass runs
// SPMD (every node runs the same graph against its shard, partials combine over
// collectives, near-data over each node's owned partitions). This function answers
// "where would each step run if this graph ran over the cluster as it stands right
// now" so the Graph Workspace can render a cluster-aware plan view.
//
// Pure + deterministic + non-throwing. It reads only already-validated inputs (the
// IR — validated by ir::from_json — and the partition-owner sets, derived from the
// schema-validated dataset_partition catalog), so it adds NO new JSON boundary. See
// docs/pipeline/graph-plan.md.
namespace zetta {

namespace ir {
class ExecutionGraph;
}

// How one graph step is projected to run over the current cluster.
enum class StepPlacement {
    NearData,     // runs on the partition owner nodes, near the data it reads
    FanOut,       // SPMD across every node; partials combine over collectives
    AllNodes,     // a load whose result is replicated to every node
    Coordinator,  // runs once on the coordinator (a single-node side effect)
};

// The lowercase canonical name ("near_data" / "fan_out" / "all_nodes" /
// "coordinator"). Stable token form (labels, tests).
std::string_view to_string(StepPlacement placement);

// A short human label for a badge ("near-data" / "fan-out" / "all nodes" /
// "coordinator"). The display form the GUI shows.
std::string_view placement_label(StepPlacement placement);

// The projected placement of one graph step.
struct StepPlan {
    std::string node_id;                    // the ir::Node id
    ir::StepKind kind;                      // its stage in the pipeline taxonomy
    StepPlacement placement;                // where it is projected to run
    std::vector<std::string> target_nodes;  // the cluster node ids it lands on (sorted)
    std::string rationale;                  // one-line reason for the projection
};

// The projection of a whole graph over a cluster.
struct ClusterPlan {
    std::vector<std::string> members;  // the cluster node ids projected over (sorted, unique)
    std::vector<StepPlan> steps;       // one per graph node, in graph insertion order
};

// Project how `graph` would run over a cluster of `members` (node ids; sorted +
// de-duplicated internally). `direct_owners` maps a node id to the owner nodes of
// the partitioned table it DIRECTLY reads — present only for nodes whose source
// table is in the partition catalog with at least one live owner. Near-data
// locality then propagates forward along DataFlow edges (the union of every
// partitioned source upstream of a step), so a downstream compute step inherits the
// owners of the data flowing into it.
//
// Placement rules (zetta's SPMD + compute-near-data model):
//   * connect            → near-data when it reads a partitioned source (its
//                          owners), else all-nodes (a replicated load).
//   * clean/model/index/serve → near-data when a partitioned source flows in (its
//                          owners; partials still combine cluster-wide), else
//                          fan-out (SPMD across every node).
//   * act                → coordinator (a single-node side effect, on the
//                          lexicographically-smallest node — the collectives root).
ClusterPlan project_cluster_plan(
    const ir::ExecutionGraph& graph, const std::vector<std::string>& members,
    const std::unordered_map<std::string, std::vector<std::string>>& direct_owners);

}  // namespace zetta
