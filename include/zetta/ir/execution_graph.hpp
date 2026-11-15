#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/ir/step_kind.hpp"

// The ExecutionGraph IR — the in-memory intermediate representation of a data/AI
// system as a directed graph of typed steps. This is the keystone the compilers,
// the gate, deploy, and run all build on (ENT-3 / FEAT-2).
//
// This header is the data model: build a graph in code and traverse it. The JSON
// form (to_json / from_json, used to persist + replicate a graph under a Project)
// lives next door in ir/serialization.hpp; the dual-form code/columnar
// (Lua/Orly/Parquet) representation is still a later slice, as is semantic
// validation (acyclicity, legal stage transitions — the gate's job).
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

// The lowercase canonical name of an edge kind. Used both as a display helper
// and as the on-the-wire token in the JSON serialization (see serialization.hpp).
constexpr std::string_view to_string(EdgeKind kind) {
    switch (kind) {
        case EdgeKind::DataFlow: return "data_flow";
        case EdgeKind::Control:  return "control";
    }
    return "unknown";
}

// Parse a canonical edge-kind name back to the enum; std::nullopt if unknown.
// The inverse of to_string(EdgeKind), used when deserializing a graph.
constexpr std::optional<EdgeKind> edge_kind_from_name(std::string_view name) {
    if (name == to_string(EdgeKind::DataFlow)) return EdgeKind::DataFlow;
    if (name == to_string(EdgeKind::Control)) return EdgeKind::Control;
    return std::nullopt;
}

// A directed, typed edge between two nodes of the same graph.
struct Edge {
    NodeId from;
    NodeId to;
    EdgeKind kind = EdgeKind::DataFlow;
};

// The most nodes and edges one graph may hold (#1426, LSR-253). A graph is
// caller-controlled (save_graph, the compilers, a deploy), and every check,
// compile and run walks it on the node's owner loop, so its size is bounded at
// construction: add_node / add_edge refuse past these, from_json rejects a
// document that names more, and the compilers report a graph that hit them.
// A real pipeline is tens of steps; these leave orders of magnitude of room.
inline constexpr std::size_t kMaxGraphNodes = 4096;
inline constexpr std::size_t kMaxGraphEdges = 16384;

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
    // assigned. Returns an empty handle if `id` is already taken (no overwrite),
    // or when the graph already holds kMaxGraphNodes (then over_limit() is set).
    std::shared_ptr<Node> add_node(StepKind kind, std::string name,
                                   nlohmann::json attributes = {}, NodeId id = {});

    // Connect `from` → `to` with the given edge kind. Returns false if either
    // endpoint is unknown (no dangling edges) or the identical edge already
    // exists (no duplicates), or when the graph already holds kMaxGraphEdges
    // (then over_limit() is set). Self-edges are permitted by the data model.
    // The duplicate check is a hash lookup, not a scan of every edge.
    bool add_edge(const NodeId& from, const NodeId& to,
                  EdgeKind kind = EdgeKind::DataFlow);

    // --- edit (remove) -----------------------------------------------------

    // Remove the node with this id and every edge incident to it (so removing a
    // node never leaves a dangling edge — the inverse guarantee of add_edge).
    // Returns false if there is no such node. Insertion order is preserved for
    // the survivors.
    bool remove_node(const NodeId& id);

    // Remove the exact edge `from`→`to` of this kind. Returns false if no such
    // edge exists. The endpoints are left untouched (the inverse of add_edge).
    bool remove_edge(const NodeId& from, const NodeId& to,
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
    // True once an add_node / add_edge was refused because the graph was full
    // (kMaxGraphNodes / kMaxGraphEdges): the graph is a truncated copy of what
    // its builder asked for, and must be rejected rather than used.
    bool over_limit() const { return over_limit_; }

    // --- traversal ---------------------------------------------------------

    // All nodes tagged with `kind`, in insertion order (traverse by step kind).
    std::vector<std::shared_ptr<Node>> nodes_of_kind(StepKind kind) const;
    // All edges of a given kind (traverse by edge kind).
    std::vector<Edge> edges_of_kind(EdgeKind kind) const;

    // Edges leaving / entering a node, in insertion order. Each reads the node's
    // own adjacency list (O(degree)), never every edge of the graph.
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
    // Adjacency indexes (#1426): each node's outgoing and incoming edges in
    // insertion order, and every edge's identity for the duplicate check. Kept
    // in step with edges_ by add_edge / remove_edge / remove_node.
    std::unordered_map<NodeId, std::vector<Edge>> outgoing_;
    std::unordered_map<NodeId, std::vector<Edge>> incoming_;
    std::unordered_set<std::string> edge_keys_;
    bool over_limit_ = false;
};

}  // namespace zetta::ir
