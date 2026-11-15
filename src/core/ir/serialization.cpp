// SPDX-License-Identifier: MIT

#include "zetta/ir/serialization.hpp"

#include <cstddef>
#include <string>
#include <utility>

#include "zetta/ir/step_kind.hpp"

namespace zetta::ir {
namespace {

// Record the reason a document was rejected (when the caller wants it) and
// signal failure with an empty optional — the single bail-out for from_json.
std::optional<ExecutionGraph> reject(std::string* error, std::string message) {
    if (error) *error = std::move(message);
    return std::nullopt;
}

}  // namespace

nlohmann::json to_json(const ExecutionGraph& graph) {
    nlohmann::json nodes = nlohmann::json::array();
    for (const auto& node : graph.nodes()) {
        nlohmann::json element{
            {"id", node->id},
            {"kind", to_string(node->kind)},
            {"name", node->name},
        };
        // Emit the attribute bag only when a node actually carries one — a bare
        // IR node has a null bag, and omitting it keeps the form compact and
        // round-trips back to that same null (an explicit empty object {} is
        // distinct and is preserved verbatim).
        if (!node->attributes.is_null()) {
            element["attributes"] = node->attributes;
        }
        nodes.push_back(std::move(element));
    }
    nlohmann::json edges = nlohmann::json::array();
    for (const Edge& edge : graph.edges()) {
        edges.push_back(nlohmann::json{
            {"from", edge.from},
            {"to", edge.to},
            {"kind", to_string(edge.kind)},
        });
    }
    return nlohmann::json{{"nodes", std::move(nodes)},
                          {"edges", std::move(edges)}};
}

std::optional<ExecutionGraph> from_json(const nlohmann::json& document,
                                        std::string* error) {
    if (!document.is_object()) {
        return reject(error, "graph must be a JSON object");
    }
    if (!document.contains("nodes") || !document["nodes"].is_array()) {
        return reject(error, "graph must have a \"nodes\" array");
    }
    if (!document.contains("edges") || !document["edges"].is_array()) {
        return reject(error, "graph must have an \"edges\" array");
    }

    ExecutionGraph graph;

    std::size_t node_index = 0;
    for (const auto& element : document["nodes"]) {
        const std::string at = "nodes[" + std::to_string(node_index++) + "]";
        if (!element.is_object()) {
            return reject(error, at + " must be an object");
        }
        if (!element.contains("id") || !element["id"].is_string() ||
            element["id"].get<std::string>().empty()) {
            return reject(error, at + " needs a non-empty string \"id\"");
        }
        if (!element.contains("kind") || !element["kind"].is_string()) {
            return reject(error, at + " needs a string \"kind\"");
        }
        if (!element.contains("name") || !element["name"].is_string()) {
            return reject(error, at + " needs a string \"name\"");
        }
        const std::string kind_name = element["kind"].get<std::string>();
        const std::optional<StepKind> kind = step_kind_from_name(kind_name);
        if (!kind) {
            return reject(error, at + ": unknown step kind \"" + kind_name + "\"");
        }
        nlohmann::json attributes;  // null = no bag, matching a bare IR node
        if (element.contains("attributes")) {
            if (!element["attributes"].is_object()) {
                return reject(error, at + " \"attributes\" must be an object");
            }
            attributes = element["attributes"];
        }
        const std::string id = element["id"].get<std::string>();
        if (!graph.add_node(*kind, element["name"].get<std::string>(),
                            std::move(attributes), id)) {
            return reject(error, at + ": duplicate node id \"" + id + "\"");
        }
    }

    std::size_t edge_index = 0;
    for (const auto& element : document["edges"]) {
        const std::string at = "edges[" + std::to_string(edge_index++) + "]";
        if (!element.is_object()) {
            return reject(error, at + " must be an object");
        }
        if (!element.contains("from") || !element["from"].is_string()) {
            return reject(error, at + " needs a string \"from\"");
        }
        if (!element.contains("to") || !element["to"].is_string()) {
            return reject(error, at + " needs a string \"to\"");
        }
        EdgeKind kind = EdgeKind::DataFlow;  // the form's default edge kind
        if (element.contains("kind")) {
            if (!element["kind"].is_string()) {
                return reject(error, at + " \"kind\" must be a string");
            }
            const std::string kind_name = element["kind"].get<std::string>();
            const std::optional<EdgeKind> parsed = edge_kind_from_name(kind_name);
            if (!parsed) {
                return reject(error,
                              at + ": unknown edge kind \"" + kind_name + "\"");
            }
            kind = *parsed;
        }
        const std::string from = element["from"].get<std::string>();
        const std::string to = element["to"].get<std::string>();
        if (!graph.add_edge(from, to, kind)) {
            // add_edge rejects a dangling endpoint or an exact-duplicate edge —
            // report which, so a bad document is diagnosable.
            if (!graph.has_node(from) || !graph.has_node(to)) {
                return reject(error, at + ": edge references an unknown node (\"" +
                                         from + "\" -> \"" + to + "\")");
            }
            return reject(error, at + ": duplicate edge (\"" + from + "\" -> \"" +
                                     to + "\")");
        }
    }

    return graph;
}

}  // namespace zetta::ir
