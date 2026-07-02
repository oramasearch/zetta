#include "zetta/pipeline/graph_plan.hpp"

#include <algorithm>
#include <utility>

#include "zetta/ir/execution_graph.hpp"

namespace zetta {

std::string_view to_string(StepPlacement placement) {
    switch (placement) {
        case StepPlacement::NearData:    return "near_data";
        case StepPlacement::FanOut:      return "fan_out";
        case StepPlacement::AllNodes:    return "all_nodes";
        case StepPlacement::Coordinator: return "coordinator";
    }
    return "unknown";
}

std::string_view placement_label(StepPlacement placement) {
    switch (placement) {
        case StepPlacement::NearData:    return "near-data";
        case StepPlacement::FanOut:      return "fan-out";
        case StepPlacement::AllNodes:    return "all nodes";
        case StepPlacement::Coordinator: return "coordinator";
    }
    return "unknown";
}

namespace {

// Merge `source` ids into the sorted, unique `into`, preserving the invariant.
void merge_owners(std::vector<std::string>& into,
                  const std::vector<std::string>& source) {
    for (const std::string& id : source) {
        const auto pos = std::lower_bound(into.begin(), into.end(), id);
        if (pos == into.end() || *pos != id) into.insert(pos, id);
    }
}

// A plural-aware node count phrase ("1 node" / "3 nodes").
std::string node_count_phrase(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " node" : " nodes");
}

std::string owner_count_phrase(std::size_t count) {
    return std::to_string(count) + (count == 1 ? " owner" : " owners");
}

}  // namespace

ClusterPlan project_cluster_plan(
    const ir::ExecutionGraph& graph, const std::vector<std::string>& members,
    const std::unordered_map<std::string, std::vector<std::string>>& direct_owners) {
    ClusterPlan plan;
    plan.members = members;
    std::sort(plan.members.begin(), plan.members.end());
    plan.members.erase(std::unique(plan.members.begin(), plan.members.end()),
                       plan.members.end());

    // 1) Effective near-data owners per node. Seed from each node's direct
    //    partitioned source, keeping only owners that are live members of the
    //    projected cluster (a stale catalog owner that has left the cluster never
    //    appears as a target).
    std::unordered_map<std::string, std::vector<std::string>> effective;
    for (const auto& [node_id, owners] : direct_owners) {
        std::vector<std::string> live;
        for (const std::string& id : owners) {
            if (std::binary_search(plan.members.begin(), plan.members.end(), id)) {
                const auto pos = std::lower_bound(live.begin(), live.end(), id);
                if (pos == live.end() || *pos != id) live.insert(pos, id);
            }
        }
        if (!live.empty()) effective[node_id] = std::move(live);
    }

    // 2) Propagate locality forward along DataFlow edges: a step inherits the
    //    union of its partitioned sources' owners, so a downstream compute step
    //    runs near the data flowing into it. A bounded fixpoint over the DAG (the
    //    union is order-independent, so the result is deterministic). The source
    //    is copied before touching effective[edge.to] because that insert may
    //    rehash the map and invalidate a reference into it.
    const std::size_t passes = graph.node_count();
    for (std::size_t pass = 0; pass < passes; ++pass) {
        bool changed = false;
        for (const ir::Edge& edge : graph.edges()) {
            if (edge.kind != ir::EdgeKind::DataFlow) continue;
            const auto from = effective.find(edge.from);
            if (from == effective.end() || from->second.empty()) continue;
            const std::vector<std::string> source = from->second;
            std::vector<std::string>& into = effective[edge.to];
            const std::size_t before = into.size();
            merge_owners(into, source);
            if (into.size() != before) changed = true;
        }
        if (!changed) break;
    }

    // 3) Assign a placement per step from its kind + whether near-data locality
    //    reaches it.
    for (const auto& node : graph.nodes()) {
        StepPlan step;
        step.node_id = node->id;
        step.kind = node->kind;

        const auto owners_it = effective.find(node->id);
        const bool near_data =
            owners_it != effective.end() && !owners_it->second.empty();
        const std::vector<std::string>& owners =
            near_data ? owners_it->second : plan.members;

        switch (node->kind) {
            case ir::StepKind::Connect:
                if (near_data) {
                    step.placement = StepPlacement::NearData;
                    step.target_nodes = owners;
                    step.rationale =
                        "reads a partitioned source, so it runs on its " +
                        owner_count_phrase(owners.size()) +
                        ", near the data (no full-dataset pull)";
                } else {
                    step.placement = StepPlacement::AllNodes;
                    step.target_nodes = plan.members;
                    step.rationale = "loads data replicated to all " +
                                     node_count_phrase(plan.members.size());
                }
                break;
            case ir::StepKind::Clean:
            case ir::StepKind::Model:
            case ir::StepKind::Index:
            case ir::StepKind::Serve:
                if (near_data) {
                    step.placement = StepPlacement::NearData;
                    step.target_nodes = owners;
                    step.rationale =
                        "runs near its data on " + owner_count_phrase(owners.size()) +
                        "; partials combine over the cluster";
                } else {
                    step.placement = StepPlacement::FanOut;
                    step.target_nodes = plan.members;
                    step.rationale = "fans out SPMD across all " +
                                     node_count_phrase(plan.members.size()) +
                                     "; partials combine over collectives";
                }
                break;
            case ir::StepKind::Act:
                step.placement = StepPlacement::Coordinator;
                if (!plan.members.empty()) {
                    step.target_nodes = {plan.members.front()};
                }
                step.rationale =
                    "runs once on the coordinator (the lexicographically-smallest "
                    "node), a single-node side effect";
                break;
        }
        plan.steps.push_back(std::move(step));
    }
    return plan;
}

}  // namespace zetta
