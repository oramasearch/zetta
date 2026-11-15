#include "zetta/ir/execution_graph.hpp"

#include <string>
#include <utility>
#include <vector>

#include "zetta/util/uuid.hpp"

namespace zetta::ir {

namespace {

// One edge's identity for the duplicate check: the endpoints and the kind,
// separated by a byte no id contains in practice (and the lengths, so no two
// distinct edges can collide).
std::string edge_key(const NodeId& from, const NodeId& to, EdgeKind kind) {
    std::string key = std::to_string(from.size());
    key += ':';
    key += from;
    key += '\x1f';
    key += to;
    key += '\x1f';
    key += std::to_string(static_cast<int>(kind));
    return key;
}

bool same_edge(const Edge& edge, const NodeId& from, const NodeId& to, EdgeKind kind) {
    return edge.from == from && edge.to == to && edge.kind == kind;
}

}  // namespace

std::shared_ptr<Node> ExecutionGraph::add_node(StepKind kind, std::string name,
                                               nlohmann::json attributes,
                                               NodeId id) {
    if (nodes_.size() >= kMaxGraphNodes) {
        over_limit_ = true;  // the graph is full (#1426)
        return {};
    }
    if (id.empty()) {
        id = zetta::util::new_uuid();
    } else if (index_.contains(id)) {
        return {};  // id already taken — never overwrite an existing node
    }
    auto node = std::make_shared<Node>(
        Node{std::move(id), kind, std::move(name), std::move(attributes)});
    index_.emplace(node->id, node);
    nodes_.push_back(node);
    return node;
}

bool ExecutionGraph::add_edge(const NodeId& from, const NodeId& to,
                              EdgeKind kind) {
    if (!index_.contains(from) || !index_.contains(to)) {
        return false;  // dangling endpoint — both nodes must already exist
    }
    std::string key = edge_key(from, to, kind);
    if (edge_keys_.contains(key)) {
        return false;  // identical edge already present
    }
    if (edges_.size() >= kMaxGraphEdges) {
        over_limit_ = true;  // the graph is full (#1426)
        return false;
    }
    edge_keys_.insert(std::move(key));
    const Edge edge{from, to, kind};
    edges_.push_back(edge);
    outgoing_[from].push_back(edge);
    incoming_[to].push_back(edge);
    return true;
}

bool ExecutionGraph::remove_node(const NodeId& id) {
    const auto found = index_.find(id);
    if (found == index_.end()) return false;  // no such node
    index_.erase(found);
    std::erase_if(nodes_, [&](const std::shared_ptr<Node>& node) {
        return node->id == id;
    });
    // Drop every edge touching the removed node so none is left dangling, from
    // the edge list and from the neighbours' adjacency lists.
    std::erase_if(edges_, [&](const Edge& edge) {
        if (edge.from != id && edge.to != id) return false;
        edge_keys_.erase(edge_key(edge.from, edge.to, edge.kind));
        if (edge.from != id) {
            if (auto list = outgoing_.find(edge.from); list != outgoing_.end()) {
                std::erase_if(list->second, [&](const Edge& other) {
                    return same_edge(other, edge.from, edge.to, edge.kind);
                });
            }
        }
        if (edge.to != id) {
            if (auto list = incoming_.find(edge.to); list != incoming_.end()) {
                std::erase_if(list->second, [&](const Edge& other) {
                    return same_edge(other, edge.from, edge.to, edge.kind);
                });
            }
        }
        return true;
    });
    outgoing_.erase(id);
    incoming_.erase(id);
    return true;
}

bool ExecutionGraph::remove_edge(const NodeId& from, const NodeId& to,
                                 EdgeKind kind) {
    if (edge_keys_.erase(edge_key(from, to, kind)) == 0) return false;
    std::erase_if(edges_, [&](const Edge& edge) { return same_edge(edge, from, to, kind); });
    if (auto list = outgoing_.find(from); list != outgoing_.end()) {
        std::erase_if(list->second,
                      [&](const Edge& edge) { return same_edge(edge, from, to, kind); });
    }
    if (auto list = incoming_.find(to); list != incoming_.end()) {
        std::erase_if(list->second,
                      [&](const Edge& edge) { return same_edge(edge, from, to, kind); });
    }
    return true;
}

std::shared_ptr<Node> ExecutionGraph::find_node(const NodeId& id) const {
    const auto found = index_.find(id);
    return found == index_.end() ? std::shared_ptr<Node>{} : found->second;
}

std::vector<std::shared_ptr<Node>> ExecutionGraph::nodes_of_kind(
    StepKind kind) const {
    std::vector<std::shared_ptr<Node>> result;
    for (const auto& node : nodes_) {
        if (node->kind == kind) {
            result.push_back(node);
        }
    }
    return result;
}

std::vector<Edge> ExecutionGraph::edges_of_kind(EdgeKind kind) const {
    std::vector<Edge> result;
    for (const Edge& edge : edges_) {
        if (edge.kind == kind) {
            result.push_back(edge);
        }
    }
    return result;
}

std::vector<Edge> ExecutionGraph::edges_from(const NodeId& id) const {
    const auto found = outgoing_.find(id);
    return found == outgoing_.end() ? std::vector<Edge>{} : found->second;
}

std::vector<Edge> ExecutionGraph::edges_into(const NodeId& id) const {
    const auto found = incoming_.find(id);
    return found == incoming_.end() ? std::vector<Edge>{} : found->second;
}

std::vector<std::shared_ptr<Node>> ExecutionGraph::successors(
    const NodeId& id) const {
    std::vector<std::shared_ptr<Node>> result;
    if (const auto found = outgoing_.find(id); found != outgoing_.end()) {
        for (const Edge& edge : found->second) {
            if (auto node = find_node(edge.to)) {
                result.push_back(std::move(node));
            }
        }
    }
    return result;
}

std::vector<std::shared_ptr<Node>> ExecutionGraph::predecessors(
    const NodeId& id) const {
    std::vector<std::shared_ptr<Node>> result;
    if (const auto found = incoming_.find(id); found != incoming_.end()) {
        for (const Edge& edge : found->second) {
            if (auto node = find_node(edge.from)) {
                result.push_back(std::move(node));
            }
        }
    }
    return result;
}

}  // namespace zetta::ir
