// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <unordered_map>
#include <vector>

#include "zetta/pipeline/graph_plan.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"

using zetta::pipeline::ClusterPlan;
using zetta::pipeline::placement_label;
using zetta::pipeline::project_cluster_plan;
using zetta::pipeline::StepPlacement;
using zetta::pipeline::StepPlan;
using zetta::pipeline::to_string;
using zetta::ir::EdgeKind;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;

namespace {

// The plan entry for `node_id`, or nullptr if the graph had no such step.
const StepPlan* find_step(const ClusterPlan& plan, const std::string& node_id) {
    for (const StepPlan& step : plan.steps) {
        if (step.node_id == node_id) return &step;
    }
    return nullptr;
}

// A churn-shaped graph: two connect sources → clean → model → index → serve → act,
// with stable ids so a test can assert per-step placement.
ExecutionGraph churn_graph() {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "accounts", {}, "load_accounts");
    graph.add_node(StepKind::Connect, "events", {}, "load_events");
    graph.add_node(StepKind::Clean, "featurize", {}, "featurize");
    graph.add_node(StepKind::Model, "train", {}, "train");
    graph.add_node(StepKind::Index, "index", {}, "index");
    graph.add_node(StepKind::Serve, "explain", {}, "explain");
    graph.add_node(StepKind::Act, "notify", {}, "notify");
    graph.add_edge("load_accounts", "featurize");
    graph.add_edge("load_events", "featurize");
    graph.add_edge("featurize", "train");
    graph.add_edge("train", "index");
    graph.add_edge("index", "explain");
    graph.add_edge("explain", "notify");
    return graph;
}

}  // namespace

// With no partition catalog at all (no near-data locality), connect loads
// replicate to all nodes, the compute stages fan out SPMD, and the action funnels
// to the coordinator (the smallest node id).
TEST(GraphPlan, NoPartitionsFanOut) {
    const ExecutionGraph graph = churn_graph();
    const std::vector<std::string> members{"node-c", "node-a", "node-b"};
    const ClusterPlan plan = project_cluster_plan(graph, members, {});

    // Members are returned sorted + unique.
    ASSERT_EQ(plan.members, (std::vector<std::string>{"node-a", "node-b", "node-c"}));

    EXPECT_EQ(find_step(plan, "load_accounts")->placement, StepPlacement::AllNodes);
    EXPECT_EQ(find_step(plan, "load_events")->placement, StepPlacement::AllNodes);
    EXPECT_EQ(find_step(plan, "featurize")->placement, StepPlacement::FanOut);
    EXPECT_EQ(find_step(plan, "train")->placement, StepPlacement::FanOut);
    EXPECT_EQ(find_step(plan, "index")->placement, StepPlacement::FanOut);
    EXPECT_EQ(find_step(plan, "explain")->placement, StepPlacement::FanOut);

    const StepPlan* notify = find_step(plan, "notify");
    EXPECT_EQ(notify->placement, StepPlacement::Coordinator);
    // The coordinator is the lexicographically-smallest member.
    ASSERT_EQ(notify->target_nodes.size(), 1u);
    EXPECT_EQ(notify->target_nodes.front(), "node-a");

    // Fan-out / all-nodes steps target the whole cluster.
    EXPECT_EQ(find_step(plan, "train")->target_nodes, plan.members);
    EXPECT_EQ(find_step(plan, "load_accounts")->target_nodes, plan.members);
}

// A connect step that reads a partitioned table runs near-data on the partition
// owners, and that locality PROPAGATES forward along DataFlow edges so the
// downstream compute steps also run on the owner nodes.
TEST(GraphPlan, NearDataPropagatesDownstream) {
    const ExecutionGraph graph = churn_graph();
    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    // The accounts source is partitioned, owned by a + b (a 2-of-3 placement).
    std::unordered_map<std::string, std::vector<std::string>> direct_owners;
    direct_owners["load_accounts"] = {"node-b", "node-a"};

    const ClusterPlan plan = project_cluster_plan(graph, members, direct_owners);

    const StepPlan* accounts = find_step(plan, "load_accounts");
    EXPECT_EQ(accounts->placement, StepPlacement::NearData);
    EXPECT_EQ(accounts->target_nodes, (std::vector<std::string>{"node-a", "node-b"}));

    // The other source has no partition data → still a replicated load.
    EXPECT_EQ(find_step(plan, "load_events")->placement, StepPlacement::AllNodes);

    // Locality reaches every downstream compute step (near-data on the owners).
    for (const char* id : {"featurize", "train", "index", "explain"}) {
        const StepPlan* step = find_step(plan, id);
        ASSERT_NE(step, nullptr) << id;
        EXPECT_EQ(step->placement, StepPlacement::NearData) << id;
        EXPECT_EQ(step->target_nodes,
                  (std::vector<std::string>{"node-a", "node-b"}))
            << id;
    }

    // The action still funnels to the coordinator, not the owners.
    EXPECT_EQ(find_step(plan, "notify")->placement, StepPlacement::Coordinator);
}

// Two partitioned sources with different owners merge into one downstream step:
// it runs near-data on the UNION of its sources' owners.
TEST(GraphPlan, MergesOwnersFromMultipleSources) {
    const ExecutionGraph graph = churn_graph();
    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    std::unordered_map<std::string, std::vector<std::string>> direct_owners;
    direct_owners["load_accounts"] = {"node-a"};
    direct_owners["load_events"] = {"node-c"};

    const ClusterPlan plan = project_cluster_plan(graph, members, direct_owners);

    // featurize joins both sources → union of {a} and {c}.
    EXPECT_EQ(find_step(plan, "featurize")->target_nodes,
              (std::vector<std::string>{"node-a", "node-c"}));
    EXPECT_EQ(find_step(plan, "train")->target_nodes,
              (std::vector<std::string>{"node-a", "node-c"}));
}

// A catalog owner that is no longer a live cluster member is dropped — a stale
// owner never appears as a target.
TEST(GraphPlan, DropsStaleOwnersNotInCluster) {
    const ExecutionGraph graph = churn_graph();
    const std::vector<std::string> members{"node-a", "node-b"};
    std::unordered_map<std::string, std::vector<std::string>> direct_owners;
    // node-z left the cluster but still appears in the catalog.
    direct_owners["load_accounts"] = {"node-a", "node-z"};

    const ClusterPlan plan = project_cluster_plan(graph, members, direct_owners);

    const StepPlan* accounts = find_step(plan, "load_accounts");
    EXPECT_EQ(accounts->placement, StepPlacement::NearData);
    EXPECT_EQ(accounts->target_nodes, (std::vector<std::string>{"node-a"}));
}

// If every owner of a source has left the cluster, the step degrades to its
// no-locality default (a replicated load), never an empty near-data placement.
TEST(GraphPlan, AllOwnersGoneDegradesToDefault) {
    const ExecutionGraph graph = churn_graph();
    const std::vector<std::string> members{"node-a", "node-b"};
    std::unordered_map<std::string, std::vector<std::string>> direct_owners;
    direct_owners["load_accounts"] = {"node-y", "node-z"};

    const ClusterPlan plan = project_cluster_plan(graph, members, direct_owners);

    EXPECT_EQ(find_step(plan, "load_accounts")->placement, StepPlacement::AllNodes);
    EXPECT_EQ(find_step(plan, "featurize")->placement, StepPlacement::FanOut);
}

// One step per graph node, in graph insertion order — the renderer relies on this.
TEST(GraphPlan, OneStepPerNodeInOrder) {
    const ExecutionGraph graph = churn_graph();
    const ClusterPlan plan =
        project_cluster_plan(graph, {"node-a"}, {});
    ASSERT_EQ(plan.steps.size(), graph.node_count());
    for (std::size_t i = 0; i < graph.nodes().size(); ++i) {
        EXPECT_EQ(plan.steps[i].node_id, graph.nodes()[i]->id);
        EXPECT_EQ(plan.steps[i].kind, graph.nodes()[i]->kind);
    }
}

// An empty graph projects an empty step list (but still reports the members).
TEST(GraphPlan, EmptyGraph) {
    const ExecutionGraph graph;
    const ClusterPlan plan = project_cluster_plan(graph, {"node-a", "node-b"}, {});
    EXPECT_TRUE(plan.steps.empty());
    EXPECT_EQ(plan.members, (std::vector<std::string>{"node-a", "node-b"}));
}

// Control edges do not carry data, so near-data locality does NOT propagate along
// them — only DataFlow edges move data.
TEST(GraphPlan, ControlEdgesDoNotPropagateLocality) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "src", {}, "src");
    graph.add_node(StepKind::Model, "downstream", {}, "downstream");
    graph.add_edge("src", "downstream", EdgeKind::Control);  // ordering only

    std::unordered_map<std::string, std::vector<std::string>> direct_owners;
    direct_owners["src"] = {"node-a"};
    const ClusterPlan plan =
        project_cluster_plan(graph, {"node-a", "node-b"}, direct_owners);

    EXPECT_EQ(find_step(plan, "src")->placement, StepPlacement::NearData);
    // No data flows over the control edge → downstream stays a fan-out.
    EXPECT_EQ(find_step(plan, "downstream")->placement, StepPlacement::FanOut);
}

// The placement token + label forms are stable.
TEST(GraphPlan, PlacementNames) {
    EXPECT_EQ(to_string(StepPlacement::NearData), "near_data");
    EXPECT_EQ(to_string(StepPlacement::FanOut), "fan_out");
    EXPECT_EQ(to_string(StepPlacement::AllNodes), "all_nodes");
    EXPECT_EQ(to_string(StepPlacement::Coordinator), "coordinator");
    EXPECT_EQ(placement_label(StepPlacement::NearData), "near-data");
    EXPECT_EQ(placement_label(StepPlacement::Coordinator), "coordinator");
}
