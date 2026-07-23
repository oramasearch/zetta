#include "zetta/agent/solution_graph.hpp"

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/ir/step_kind.hpp"

namespace zetta::agent {

ir::ExecutionGraph build_solution_graph(const EndState& facts) {
    ir::ExecutionGraph graph;
    const auto connect = graph.add_node(ir::StepKind::Connect, "source");
    std::shared_ptr<ir::Node> last = connect;

    // Facts whose key names a (non-connect) step and are true become real step nodes, chained in
    // step order; everything else is carried as an attribute on the solution node.
    nlohmann::json attributes = nlohmann::json::object();
    for (const ir::StepKind kind : ir::all_step_kinds()) {
        if (kind == ir::StepKind::Connect) continue;
        const std::string name(ir::to_string(kind));
        const auto fact = facts.facts().find(name);
        if (fact != facts.facts().end() && fact->second.is_boolean() &&
            fact->second.get<bool>()) {
            const auto node = graph.add_node(kind, name);
            graph.add_edge(last->id, node->id);
            last = node;
        }
    }
    for (const auto& [key, value] : facts.facts()) {
        if (const auto kind = ir::step_kind_from_name(key);
            kind && *kind != ir::StepKind::Connect && value.is_boolean() && value.get<bool>()) {
            continue;  // already a step node above
        }
        attributes[key] = value;  // an attribute fact (incl. a false step fact, e.g. "act":false)
    }
    if (!attributes.empty()) {
        const auto solution =
            graph.add_node(ir::StepKind::Serve, "solution", std::move(attributes));
        graph.add_edge(last->id, solution->id);
    }
    return graph;
}

EndState graph_solution_facts(const ir::ExecutionGraph& graph) {
    EndState facts;
    for (const auto& node : graph.nodes()) {
        facts.set(std::string(ir::to_string(node->kind)), true);  // step-kind presence
        if (node->attributes.is_object()) {
            for (const auto& [key, value] : node->attributes.items()) {
                facts.set(key, value);  // the config the node carries
            }
        }
    }
    return facts;
}

}  // namespace zetta::agent
