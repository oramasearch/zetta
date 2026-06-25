#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/step_kind.hpp"

// The ExecutionGraph IR — the in-memory intermediate representation of a data/AI
// system as a directed graph of typed steps. This is the keystone the compilers,
// the gate, deploy, and run all build on (ENT-3 / FEAT-2).
//
// This slice is the data model only: build a graph in code and traverse it.
// There is deliberately NO serialization here (the dual-form code/tabular
// Lua/Orly/Parquet representation is a later slice) and NO semantic validation
// (acyclicity, legal stage transitions — the gate's job, also later).
namespace zetta::ir {

// A node's identity, unique within one ExecutionGraph. A fresh UUID by default
// (see ExecutionGraph::add_node), but any caller-chosen stable string works.
using NodeId = std::string;

// One step in an execution graph: a typed unit of work tagged with its stage in
// the connect→clean→model→index→serve→act taxonomy. `attributes` is a free-form
// in-memory configuration bag (the future dual-form payload anchors here); it is
// empty for a bare IR node and carries no schema yet at this slice.
struct Node {
    NodeId id;
    StepKind kind;
    std::string name;
    nlohmann::json attributes;
};

// How one node depends on another. Edges are directed (from → to) and typed so
// downstream layers can tell a data dependency from a pure ordering constraint.
enum class EdgeKind {
    DataFlow,  // `from`'s output feeds `to`'s input (the common case)
    Control,   // ordering only: `to` runs after `from`, no data passes
};

// The lowercase canonical name of an edge kind. A display helper, not IR
// serialization.
constexpr std::string_view to_string(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::DataFlow: return "data_flow";
        case EdgeKind::Control:  return "control";
    }
    return "unknown";
}

// A directed, typed edge between two nodes of the same graph.
struct Edge {
    NodeId from;
    NodeId to;
    EdgeKind kind = EdgeKind::DataFlow;
};

// An in-memory directed graph of typed step nodes and typed edges.
//
// Nodes are handed back as shared_ptr handles (empty on miss), matching the rest
// of zetta; the graph co-owns each node with its id index. Edges are small value
// types returned by copy. Insertion order is preserved for nodes and edges so
// traversals are deterministic.
class ExecutionGraph {
public:
    // --- build -------------------------------------------------------------

    // Add a step node and return its handle. When `id` is empty a fresh UUID is
    // assigned. Returns an empty handle if `id` is already taken (no overwrite).
    std::shared_ptr<Node> add_node(StepKind kind, std::string name,
                                   nlohmann::json attributes = {}, NodeId id = {});

    // Connect `from` → `to` with the given edge kind. Returns false if either
    // endpoint is unknown (no dangling edges) or the identical edge already
    // exists (no duplicates). Self-edges are permitted by the data model.
    bool add_edge(const NodeId& from, const NodeId& to,
                  EdgeKind kind = EdgeKind::DataFlow);

    // --- lookup ------------------------------------------------------------

    // The node with this id, or an empty handle if there is none.
    std::shared_ptr<Node> find_node(const NodeId& id) const;
    bool has_node(const NodeId& id) const { return index_.contains(id); }

    const std::vector<std::shared_ptr<Node>>& nodes() const { return nodes_; }
    const std::vector<Edge>& edges() const { return edges_; }
    std::size_t node_count() const { return nodes_.size(); }
    std::size_t edge_count() const { return edges_.size(); }
    bool empty() const { return nodes_.empty(); }

    // --- traversal ---------------------------------------------------------

    // All nodes tagged with `kind`, in insertion order (traverse by step kind).
    std::vector<std::shared_ptr<Node>> nodes_of_kind(StepKind kind) const;
    // All edges of a given kind (traverse by edge kind).
    std::vector<Edge> edges_of_kind(EdgeKind kind) const;

    // Edges leaving / entering a node, in insertion order.
    std::vector<Edge> edges_from(const NodeId& id) const;
    std::vector<Edge> edges_into(const NodeId& id) const;

    // Direct neighbours of a node, following / against edge direction. Empty
    // handles are never returned (edges always reference existing nodes).
    std::vector<std::shared_ptr<Node>> successors(const NodeId& id) const;
    std::vector<std::shared_ptr<Node>> predecessors(const NodeId& id) const;

private:
    std::vector<std::shared_ptr<Node>> nodes_;                 // insertion order
    std::unordered_map<NodeId, std::shared_ptr<Node>> index_;  // id → node
    std::vector<Edge> edges_;                                  // insertion order
};

}  // namespace zetta::ir
