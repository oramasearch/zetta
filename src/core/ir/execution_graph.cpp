#include "zetta/ir/execution_graph.hpp"

#include <utility>
#include <vector>

#include "zetta/util/uuid.hpp"

namespace zetta::ir {

std::shared_ptr<Node> ExecutionGraph::add_node(StepKind kind, std::string name,
                                               nlohmann::json attributes,
                                               NodeId id) {
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
    for (const Edge& edge : edges_) {
        if (edge.from == from && edge.to == to && edge.kind == kind) {
            return false;  // identical edge already present
        }
    }
    edges_.push_back(Edge{from, to, kind});
    return true;
}

bool ExecutionGraph::remove_node(const NodeId& id) {
    const auto found = index_.find(id);
    if (found == index_.end()) return false;  // no such node
    index_.erase(found);
    std::erase_if(nodes_, [&](const std::shared_ptr<Node>& node) {
        return node->id == id;
    });
    // Drop every edge touching the removed node so none is left dangling.
    std::erase_if(edges_, [&](const Edge& edge) {
        return edge.from == id || edge.to == id;
    });
    return true;
}

bool ExecutionGraph::remove_edge(const NodeId& from, const NodeId& to,
                                 EdgeKind kind) {
    const std::size_t before = edges_.size();
    std::erase_if(edges_, [&](const Edge& edge) {
        return edge.from == from && edge.to == to && edge.kind == kind;
    });
    return edges_.size() != before;
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
    std::vector<Edge> result;
    for (const Edge& edge : edges_) {
        if (edge.from == id) {
            result.push_back(edge);
        }
    }
    return result;
}

std::vector<Edge> ExecutionGraph::edges_into(const NodeId& id) const {
    std::vector<Edge> result;
    for (const Edge& edge : edges_) {
        if (edge.to == id) {
            result.push_back(edge);
        }
    }
    return result;
}

std::vector<std::shared_ptr<Node>> ExecutionGraph::successors(
    const NodeId& id) const {
    std::vector<std::shared_ptr<Node>> result;
    for (const Edge& edge : edges_) {
        if (edge.from == id) {
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
    for (const Edge& edge : edges_) {
        if (edge.to == id) {
            if (auto node = find_node(edge.from)) {
                result.push_back(std::move(node));
            }
        }
    }
    return result;
}

}  // namespace zetta::ir
