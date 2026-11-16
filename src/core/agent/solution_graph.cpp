// SPDX-License-Identifier: MIT

#include "zetta/agent/solution_graph.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>
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
            // The METHODOLOGY belongs on the step that implements it, not in a bag on the
            // serve node. Until this landed, `model_kind` was swept into the attribute sweep
            // below, so a forecasting graph and a churn graph were structurally IDENTICAL —
            // connect → clean → model → index → serve, differing only by a string nothing
            // read. The corpus was training a distinction the IR could not represent, which
            // is why a neighbouring-solution negative control had nothing to bite on.
            nlohmann::json step_attributes = nlohmann::json::object();
            if (kind == ir::StepKind::Model) {
                if (const auto model_kind = facts.facts().find("model_kind");
                    model_kind != facts.facts().end() && model_kind->second.is_string()) {
                    step_attributes["model_kind"] = model_kind->second;
                }
            }
            const auto node = step_attributes.empty()
                                  ? graph.add_node(kind, name)
                                  : graph.add_node(kind, name, std::move(step_attributes));
            graph.add_edge(last->id, node->id);
            last = node;
        }
    }
    for (const auto& [key, value] : facts.facts()) {
        if (const auto kind = ir::step_kind_from_name(key);
            kind && *kind != ir::StepKind::Connect && value.is_boolean() && value.get<bool>()) {
            continue;  // already a step node above
        }
        // `model_kind` now rides the model step itself (above). Leaving it here too would
        // give the graph two sources of truth for the methodology, free to disagree.
        if (key == "model_kind" && facts.matches("model", true)) continue;
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

// A sentence describing the pipeline these facts produce — the steps in order, then the
// methodology that matters to the person reading it.
//
// This replaces a single constant reply that covered 1,802 action cases. A constant is the
// cheapest thing a model can learn, and it learned it: the v3 checkpoint answered churn, lead
// scoring and collections with the identical sentence, because that is what every one of those
// cases taught. Deriving the message from the facts means a pipeline that differs reads
// differently, which is both better training signal and a better answer.
//
// Deliberately free of digits: an action reply is not grounded in a retrieved measure, so any
// figure here would be one the engine never computed.
std::string describe_pipeline(const EndState& facts) {
    const auto has = [&facts](const char* key) { return facts.matches(key, true); };
    const auto text = [&facts](const char* key) -> std::string {
        const auto found = facts.facts().find(key);
        if (found == facts.facts().end() || !found->second.is_string()) return {};
        return found->second.get<std::string>();
    };

    // How each model kind reads to someone who does not think in model families.
    const std::string kind = text("model_kind");
    static const std::map<std::string, std::string> kModelPhrase = {
        {"gbdt", "train a gradient-boosted model"},
        {"regression", "fit a regression model"},
        {"survival", "fit a time-to-event model"},
        {"forecast", "fit a forecasting model"},
        {"anomaly", "fit an anomaly detector"},
        {"uplift", "fit an uplift model, so we act only where it changes the outcome"},
        {"classifier", "train a classifier"},
        {"graph", "score over the relationship graph"},
        {"vision", "train an image model"},
        {"speech", "transcribe and analyze the audio"},
        {"bandit", "learn online which option works, while still exploring"},
        {"optimization", "solve for the best plan under your constraints"},
        {"bayesian", "fit a model that reports its own uncertainty"},
        {"ensemble", "combine several models"},
        {"online", "keep a model training as data arrives"},
        {"document_intelligence", "extract the fields from the documents"},
    };

    std::vector<std::string> steps;
    if (has("connect")) steps.emplace_back("connect your source");
    if (has("clean")) {
        steps.emplace_back(has("leakage_free")
                               ? "build leakage-free point-in-time features"
                               : "clean and reshape the data");
    }
    if (has("model")) {
        const auto phrase = kModelPhrase.find(kind);
        steps.push_back(phrase != kModelPhrase.end() ? phrase->second : "train a model");
    }
    if (has("index")) {
        steps.emplace_back(facts.matches("index_kind", std::string("semantic"))
                               ? "build a semantic index over them"
                               : "build a search index");
    }
    if (has("serve")) steps.emplace_back("serve the results");
    if (has("act")) steps.emplace_back("act on them");

    std::string message = "I'll ";
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (index > 0) message += (index + 1 == steps.size()) ? ", then " : ", ";
        message += steps[index];
    }
    message += steps.empty() ? "set up the Pipeline for that." : ".";

    // The guarantees worth stating back, in the vocabulary contract's words.
    std::vector<std::string> notes;
    if (has("temporal_validation")) notes.emplace_back("validated on later data than it trains on");
    if (has("backtest")) notes.emplace_back("backtested over rolling origins");
    if (has("calibrated")) notes.emplace_back("with calibrated probabilities");
    if (facts.matches("explain", std::string("shap"))) notes.emplace_back("with per-prediction explanations");
    if (has("holdout")) notes.emplace_back("measured against a held-out group");
    if (has("grounded_in_data")) notes.emplace_back("grounded in your own records");
    // No leading "and" — the joiner below supplies one for the last item, and this note produced
    // "...explanations and and nothing acts without your approval" in every approval-gated case.
    if (has("approval_gated")) notes.emplace_back("gated so nothing acts without your approval");
    if (has("uncertainty")) notes.emplace_back("reporting its own uncertainty");
    if (has("drift_monitoring")) notes.emplace_back("watched for drift");
    if (!notes.empty()) {
        message += " It'll be ";
        for (std::size_t index = 0; index < notes.size(); ++index) {
            if (index > 0) message += (index + 1 == notes.size()) ? " and " : ", ";
            message += notes[index];
        }
        message += ".";
    }
    return message;
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
    action.message = describe_pipeline(facts);
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
                                                   admission_case.task.description(),
                                                   admission_case.history);
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
