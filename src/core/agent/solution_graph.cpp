#include "zetta/agent/solution_graph.hpp"

#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "zetta/agent/base_prompt.hpp"
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

StudioAction reference_action_from_facts(const EndState& facts) {
    StudioAction action;
    action.execution_graph = build_solution_graph(facts);
    // Out of scope (abstain): decline and build NOTHING — no tool calls, a decline message. The
    // graph still carries the abstain facts so the gate scores the abstain end-state (the agent
    // must learn to refuse, not fabricate a pipeline — the ABSTAIN discrimination).
    if (facts.matches("abstain", true)) {
        action.message =
            "That isn't something Orama Zetta builds \xE2\x80\x94 it's out of scope, so I won't "
            "set up a pipeline for it.";
        return action;
    }
    // One tool call per pipeline step — what the agent does to build it (mirrors the shape of
    // churn_reference_trajectory), carrying the node's config as arguments.
    for (const auto& node : action.execution_graph.nodes()) {
        nlohmann::json arguments =
            node->attributes.is_object() ? node->attributes : nlohmann::json::object();
        arguments["name"] = node->name;
        action.tool_calls.emplace_back(std::string(ir::to_string(node->kind)),
                                       std::move(arguments));
    }
    action.message = "Here's the pipeline I'll build and run for your request.";
    return action;
}

void attach_reference_action(AdmissionCase& admission_case) {
    admission_case.reference_action = reference_action_from_facts(admission_case.reference);
}

std::pair<std::string, std::string> render_action_example(const AdmissionCase& admission_case) {
    std::string prompt = agent_prompt_format(base_agent_system_prompt(),
                                             admission_case.task.description());
    const StudioAction action = admission_case.reference_action
                                    ? *admission_case.reference_action
                                    : reference_action_from_facts(admission_case.reference);
    return {std::move(prompt), to_json(action).dump()};
}

}  // namespace zetta::agent
