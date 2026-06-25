#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"

using zetta::ir::Edge;
using zetta::ir::EdgeKind;
using zetta::ir::ExecutionGraph;
using zetta::ir::Node;
using zetta::ir::StepKind;

namespace {

// Build the canonical six-stage pipeline as a small chain:
//   connect → clean → model → index → serve → act
// Returns the graph plus the ids in pipeline order, so traversal tests can
// follow it without re-deriving the ids.
struct Pipeline {
    ExecutionGraph graph;
    std::vector<std::string> ids;
};

Pipeline build_pipeline() {
    Pipeline pipeline;
    auto& graph = pipeline.graph;
    auto connect = graph.add_node(StepKind::Connect, "load_orders");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    auto index = graph.add_node(StepKind::Index, "vector_index");
    auto serve = graph.add_node(StepKind::Serve, "predictions_api");
    auto act = graph.add_node(StepKind::Act, "notify_sales");
    for (const auto& node : {connect, clean, model, index, serve, act}) {
        pipeline.ids.push_back(node->id);
    }
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    graph.add_edge(model->id, index->id);
    graph.add_edge(index->id, serve->id);
    graph.add_edge(serve->id, act->id);
    return pipeline;
}

}  // namespace

// ---- step-kind taxonomy ----------------------------------------------------

TEST(StepKind, EnumeratesAllStagesInCanonicalOrder) {
    const std::array<StepKind, 6> expected{StepKind::Connect, StepKind::Clean,
                                           StepKind::Model,   StepKind::Index,
                                           StepKind::Serve,   StepKind::Act};
    EXPECT_EQ(zetta::ir::all_step_kinds(), expected);
}

TEST(StepKind, UnderlyingValuesIncreaseAlongThePipeline) {
    // Declaration order is the canonical pipeline order, so connect precedes act.
    EXPECT_LT(static_cast<int>(StepKind::Connect), static_cast<int>(StepKind::Act));
    EXPECT_LT(static_cast<int>(StepKind::Clean), static_cast<int>(StepKind::Model));
}

TEST(StepKind, NamesRoundTrip) {
    for (const StepKind kind : zetta::ir::all_step_kinds()) {
        const auto name = zetta::ir::to_string(kind);
        const auto parsed = zetta::ir::step_kind_from_name(name);
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, kind);
    }
    EXPECT_EQ(zetta::ir::to_string(StepKind::Model), "model");
    EXPECT_FALSE(zetta::ir::step_kind_from_name("nope").has_value());
}

TEST(EdgeKind, HasCanonicalNames) {
    EXPECT_EQ(zetta::ir::to_string(EdgeKind::DataFlow), "data_flow");
    EXPECT_EQ(zetta::ir::to_string(EdgeKind::Control), "control");
}

// ---- building a graph ------------------------------------------------------

TEST(ExecutionGraph, StartsEmpty) {
    ExecutionGraph graph;
    EXPECT_TRUE(graph.empty());
    EXPECT_EQ(graph.node_count(), 0u);
    EXPECT_EQ(graph.edge_count(), 0u);
}

TEST(ExecutionGraph, AddNodeAssignsAUuidWhenNoIdGiven) {
    ExecutionGraph graph;
    auto first = graph.add_node(StepKind::Connect, "a");
    auto second = graph.add_node(StepKind::Connect, "b");
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    EXPECT_FALSE(first->id.empty());
    EXPECT_NE(first->id, second->id);  // distinct ids
    EXPECT_EQ(first->id.size(), 36u);  // canonical 8-4-4-4-12 UUID
    EXPECT_EQ(graph.node_count(), 2u);
}

TEST(ExecutionGraph, AddNodeHonoursAnExplicitId) {
    ExecutionGraph graph;
    auto node = graph.add_node(StepKind::Model, "m", {}, "my-id");
    ASSERT_TRUE(node);
    EXPECT_EQ(node->id, "my-id");
    EXPECT_TRUE(graph.has_node("my-id"));
}

TEST(ExecutionGraph, AddNodeRejectsADuplicateId) {
    ExecutionGraph graph;
    ASSERT_TRUE(graph.add_node(StepKind::Connect, "first", {}, "dup"));
    auto clash = graph.add_node(StepKind::Clean, "second", {}, "dup");
    EXPECT_FALSE(clash);                 // empty handle — no overwrite
    EXPECT_EQ(graph.node_count(), 1u);
    EXPECT_EQ(graph.find_node("dup")->name, "first");
}

TEST(ExecutionGraph, NodesCarryAFreeFormAttributeBag) {
    ExecutionGraph graph;
    auto node = graph.add_node(StepKind::Connect, "load",
                               {{"source", "postgres"}, {"table", "orders"}});
    ASSERT_TRUE(node);
    EXPECT_EQ(node->attributes.at("source"), "postgres");
}

TEST(ExecutionGraph, FindNodeReturnsAnEmptyHandleOnMiss) {
    ExecutionGraph graph;
    EXPECT_FALSE(graph.find_node("absent"));
    EXPECT_FALSE(graph.has_node("absent"));
}

// ---- edges: structural integrity -------------------------------------------

TEST(ExecutionGraph, AddEdgeConnectsExistingNodes) {
    ExecutionGraph graph;
    auto from = graph.add_node(StepKind::Connect, "a");
    auto to = graph.add_node(StepKind::Clean, "b");
    EXPECT_TRUE(graph.add_edge(from->id, to->id));
    EXPECT_EQ(graph.edge_count(), 1u);
}

TEST(ExecutionGraph, AddEdgeRejectsDanglingEndpoints) {
    ExecutionGraph graph;
    auto real = graph.add_node(StepKind::Connect, "a");
    EXPECT_FALSE(graph.add_edge(real->id, "ghost"));
    EXPECT_FALSE(graph.add_edge("ghost", real->id));
    EXPECT_EQ(graph.edge_count(), 0u);
}

TEST(ExecutionGraph, AddEdgeRejectsExactDuplicatesButAllowsDistinctKinds) {
    ExecutionGraph graph;
    auto from = graph.add_node(StepKind::Connect, "a");
    auto to = graph.add_node(StepKind::Clean, "b");
    EXPECT_TRUE(graph.add_edge(from->id, to->id, EdgeKind::DataFlow));
    EXPECT_FALSE(graph.add_edge(from->id, to->id, EdgeKind::DataFlow));  // dup
    EXPECT_TRUE(graph.add_edge(from->id, to->id, EdgeKind::Control));    // kind differs
    EXPECT_EQ(graph.edge_count(), 2u);
}

// ---- traversal by step kind / edge kind ------------------------------------

TEST(ExecutionGraph, TraversesNodesByStepKind) {
    ExecutionGraph graph;
    graph.add_node(StepKind::Connect, "src1");
    graph.add_node(StepKind::Connect, "src2");
    graph.add_node(StepKind::Model, "m");

    const auto connects = graph.nodes_of_kind(StepKind::Connect);
    ASSERT_EQ(connects.size(), 2u);
    EXPECT_EQ(connects[0]->name, "src1");  // insertion order preserved
    EXPECT_EQ(connects[1]->name, "src2");

    EXPECT_EQ(graph.nodes_of_kind(StepKind::Model).size(), 1u);
    EXPECT_TRUE(graph.nodes_of_kind(StepKind::Act).empty());
}

TEST(ExecutionGraph, TraversesEdgesByEdgeKind) {
    ExecutionGraph graph;
    auto a = graph.add_node(StepKind::Connect, "a");
    auto b = graph.add_node(StepKind::Clean, "b");
    auto c = graph.add_node(StepKind::Model, "c");
    graph.add_edge(a->id, b->id, EdgeKind::DataFlow);
    graph.add_edge(b->id, c->id, EdgeKind::Control);

    EXPECT_EQ(graph.edges_of_kind(EdgeKind::DataFlow).size(), 1u);
    EXPECT_EQ(graph.edges_of_kind(EdgeKind::Control).size(), 1u);
}

TEST(ExecutionGraph, ReportsEdgesAndNeighboursOfANode) {
    ExecutionGraph graph;
    auto a = graph.add_node(StepKind::Connect, "a");
    auto b = graph.add_node(StepKind::Clean, "b");
    auto c = graph.add_node(StepKind::Clean, "c");
    graph.add_edge(a->id, b->id);
    graph.add_edge(a->id, c->id);
    graph.add_edge(b->id, c->id);

    EXPECT_EQ(graph.edges_from(a->id).size(), 2u);
    EXPECT_EQ(graph.edges_into(c->id).size(), 2u);

    const auto succ = graph.successors(a->id);
    ASSERT_EQ(succ.size(), 2u);
    EXPECT_EQ(succ[0]->name, "b");
    EXPECT_EQ(succ[1]->name, "c");

    const auto preds = graph.predecessors(c->id);
    ASSERT_EQ(preds.size(), 2u);
    EXPECT_EQ(preds[0]->name, "a");
    EXPECT_EQ(preds[1]->name, "b");
}

// ---- end to end: the canonical connect→…→act pipeline ----------------------

TEST(ExecutionGraph, WalksTheCanonicalPipelineStageByStage) {
    auto pipeline = build_pipeline();
    auto& graph = pipeline.graph;

    EXPECT_EQ(graph.node_count(), 6u);
    EXPECT_EQ(graph.edge_count(), 5u);

    // Exactly one node per stage, and following the data-flow edges visits the
    // stages in canonical order.
    std::string current = pipeline.ids.front();
    for (const StepKind kind : zetta::ir::all_step_kinds()) {
        const auto here = graph.nodes_of_kind(kind);
        ASSERT_EQ(here.size(), 1u) << "stage " << zetta::ir::to_string(kind);
        EXPECT_EQ(here.front()->id, current);
        const auto next = graph.successors(current);
        if (kind == StepKind::Act) {
            EXPECT_TRUE(next.empty());  // the pipeline terminates at act
        } else {
            ASSERT_EQ(next.size(), 1u);
            current = next.front()->id;
        }
    }
}
