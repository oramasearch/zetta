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
        action.kind = InteractionKind::Abstain;
        action.message =
            "That isn't something Orama Zetta builds \xE2\x80\x94 it's out of scope, so I won't "
            "set up a pipeline for it.";
        return action;
    }
    // One tool call per pipeline step — what the agent DOES to build it. Derived from the graph
    // rather than written alongside it, so the two encodings cannot drift apart.
    action.tool_calls = tool_calls_from_graph(action.execution_graph);
    action.message = "Here's the pipeline I'll build and run for your request.";
    return action;
}

std::vector<ToolCall> tool_calls_from_graph(const ir::ExecutionGraph& graph) {
    std::vector<ToolCall> calls;
    calls.reserve(graph.nodes().size());
    for (const auto& node : graph.nodes()) {
        nlohmann::json arguments =
            node->attributes.is_object() ? node->attributes : nlohmann::json::object();
        arguments["name"] = node->name;
        calls.emplace_back(std::string(ir::to_string(node->kind)), std::move(arguments));
    }
    return calls;
}

ir::ExecutionGraph build_pipeline_from_tool_calls(const std::vector<ToolCall>& calls) {
    ir::ExecutionGraph graph;
    for (const ToolCall& call : calls) {
        const std::optional<ir::StepKind> kind = ir::step_kind_from_name(call.tool);
        if (!kind) continue;  // not a pipeline step (a read tool); it builds nothing
        nlohmann::json attributes =
            call.arguments.is_object() ? call.arguments : nlohmann::json::object();
        std::string name = call.tool;
        if (const auto named = attributes.find("name");
            named != attributes.end() && named->is_string()) {
            name = named->get<std::string>();
            attributes.erase("name");  // the node's name, not a config value
        }
        const bool first = graph.nodes().empty();
        const ir::NodeId previous = first ? ir::NodeId{} : graph.nodes().back()->id;
        // A step with no configuration gets NO attributes, not an empty object — so a pipeline
        // built from tool calls is identical to the same pipeline built directly, and the
        // graph <-> tool-calls round trip is exact rather than merely equivalent.
        const std::shared_ptr<ir::Node> node =
            attributes.empty() ? graph.add_node(*kind, std::move(name))
                               : graph.add_node(*kind, std::move(name), std::move(attributes));
        if (node == nullptr) continue;
        // The calls arrive in pipeline order, and that order IS the data flow.
        if (!first) graph.add_edge(previous, node->id, ir::EdgeKind::DataFlow);
    }
    return graph;
}

void attach_reference_action(AdmissionCase& admission_case) {
    admission_case.reference_action = reference_action_from_facts(admission_case.reference);
}

std::pair<std::string, std::string> render_action_example(const AdmissionCase& admission_case) {
    // THE shared renderer (#918) — the identical call the serving harness makes in
    // agent::respond. Going through it (rather than framing the prompt here) is what makes
    // train/serve skew structurally impossible; test_interaction_contract asserts the two
    // agree byte for byte.
    std::string prompt = render_interaction_prompt(base_agent_system_prompt(),
                                                   admission_case.grounding,
                                                   admission_case.task.description());
    const StudioAction action = admission_case.reference_action
                                    ? *admission_case.reference_action
                                    : reference_action_from_facts(admission_case.reference);

    // The target is the DECISION — mode, message, tool calls — and deliberately NOT the
    // execution_graph.
    //
    // The graph's node ids are random UUIDs. Training a model to reproduce them token by token is
    // unlearnable: there is no signal in a random id, and they dominate the target, so the only
    // thing a model can extract is what a UUID looks like. That is exactly what happened — the
    // trained checkpoint emits `11111111-1111-4111-8888-…` for every node, so two steps collide on
    // one id and the pipeline will not compile.
    //
    // The agent is a tool-calling harness: the model decides which steps to call and how to
    // configure them, and the harness mints ids and wires edges when it runs those calls. The
    // graph stays on the AdmissionCase as the gate's reference for scoring; it is simply not
    // something the model should be asked to write.
    nlohmann::json target = to_json(action);
    target.erase("execution_graph");
    return {std::move(prompt), target.dump()};
}

}  // namespace zetta::agent
