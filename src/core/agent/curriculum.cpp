#include "zetta/agent/curriculum.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/agent/end_state.hpp"
#include "zetta/agent/sandbox.hpp"
#include "zetta/agent/task.hpp"
#include "zetta/agent/tool_call.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"

namespace zetta::agent {
namespace {

// The facts a correct churn solution must produce. This is the improvement over
// Charles's n8n (Gemini summarizing communications into qualitative signatures, then an
// LLM matching accounts against them — no trained model, no feature vector): a
// leakage-free, gradient-boosted, SHAP-explained, calibrated pipeline.
EndState churn_required() {
    EndState required;
    required.set("connect", true);           // pulls the account events + communications
    required.set("leakage_free", true);      // point-in-time features, no target leakage
    required.set("model_kind", "gbdt");      // gradient-boosted trees, not an LLM
    required.set("explain", "shap");         // SHAP risk drivers
    required.set("calibrated", true);        // calibrated risk scores
    required.set("calibration_loop", true);  // score-vs-outcome tracking (batting average)
    return required;
}

}  // namespace

ir::ExecutionGraph build_churn_graph(std::string model_kind, std::string explain,
                                     bool leakage_free, bool calibrated,
                                     bool calibration_loop) {
    ir::ExecutionGraph graph;
    const auto connect =
        graph.add_node(ir::StepKind::Connect, "account events + communications");
    const auto clean = graph.add_node(ir::StepKind::Clean, "point-in-time features",
                                      {{"leakage_free", leakage_free}});
    const auto model =
        graph.add_node(ir::StepKind::Model, "churn risk model",
                       {{"kind", std::move(model_kind)}, {"explain", std::move(explain)}});
    const auto serve = graph.add_node(ir::StepKind::Serve, "risk scores",
                                      {{"calibrated", calibrated}});
    const auto act = graph.add_node(ir::StepKind::Act, "write-back + calibration",
                                    {{"calibration_loop", calibration_loop}});
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    graph.add_edge(model->id, serve->id);
    graph.add_edge(serve->id, act->id);
    return graph;
}

EndState churn_graph_facts(const ir::ExecutionGraph& graph) {
    EndState facts;
    facts.set("connect", !graph.nodes_of_kind(ir::StepKind::Connect).empty());

    bool leakage_free = false;
    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Clean)) {
        if (node->attributes.value("leakage_free", false)) leakage_free = true;
    }
    facts.set("leakage_free", leakage_free);

    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Model)) {
        if (node->attributes.contains("kind")) {
            facts.set("model_kind", node->attributes.at("kind"));
        }
        if (node->attributes.contains("explain")) {
            facts.set("explain", node->attributes.at("explain"));
        }
    }

    bool calibrated = false;
    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Serve)) {
        if (node->attributes.value("calibrated", false)) calibrated = true;
    }
    facts.set("calibrated", calibrated);

    bool calibration_loop = false;
    for (const auto& node : graph.nodes_of_kind(ir::StepKind::Act)) {
        if (node->attributes.value("calibration_loop", false)) calibration_loop = true;
    }
    facts.set("calibration_loop", calibration_loop);
    return facts;
}

Sandbox churn_sandbox() {
    // The tools mutate a shared execution graph; the graded end-state is always the
    // facts of that graph (so the sandbox and build_churn_graph agree by construction).
    const auto graph = std::make_shared<ir::ExecutionGraph>();
    Sandbox sandbox;
    sandbox.register_tool("connect", [graph](EndState& world, const nlohmann::json& args) {
        graph->add_node(ir::StepKind::Connect, args.value("name", "source"));
        world = churn_graph_facts(*graph);
        return ToolResult::success();
    });
    sandbox.register_tool("clean", [graph](EndState& world, const nlohmann::json& args) {
        graph->add_node(ir::StepKind::Clean, args.value("name", "clean"),
                        {{"leakage_free", args.value("leakage_free", false)}});
        world = churn_graph_facts(*graph);
        return ToolResult::success();
    });
    sandbox.register_tool("model", [graph](EndState& world, const nlohmann::json& args) {
        graph->add_node(ir::StepKind::Model, args.value("name", "model"),
                        {{"kind", args.value("kind", "")},
                         {"explain", args.value("explain", "none")}});
        world = churn_graph_facts(*graph);
        return ToolResult::success();
    });
    sandbox.register_tool("serve", [graph](EndState& world, const nlohmann::json& args) {
        graph->add_node(ir::StepKind::Serve, args.value("name", "serve"),
                        {{"calibrated", args.value("calibrated", false)}});
        world = churn_graph_facts(*graph);
        return ToolResult::success();
    });
    sandbox.register_tool("act", [graph](EndState& world, const nlohmann::json& args) {
        graph->add_node(ir::StepKind::Act, args.value("name", "act"),
                        {{"calibration_loop", args.value("calibration_loop", false)}});
        world = churn_graph_facts(*graph);
        return ToolResult::success();
    });
    return sandbox;
}

std::vector<ToolCall> churn_reference_trajectory() {
    return {
        ToolCall{"connect", {{"name", "account events + communications"}}},
        ToolCall{"clean", {{"name", "point-in-time features"}, {"leakage_free", true}}},
        ToolCall{"model",
                 {{"name", "churn risk model"}, {"kind", "gbdt"}, {"explain", "shap"}}},
        ToolCall{"serve", {{"name", "risk scores"}, {"calibrated", true}}},
        ToolCall{"act",
                 {{"name", "write-back + calibration"}, {"calibration_loop", true}}},
    };
}

AdmissionCase churn_solution_case() {
    const std::string description =
        "Build a churn-risk pipeline over the account events + communications: connect "
        "the source, engineer leakage-free point-in-time features, train a "
        "gradient-boosted model with SHAP explanations, and serve calibrated risk scores "
        "with a score-vs-outcome calibration loop.";

    EndState initial;
    initial.set("data.churned_events", true);   // labeled churned-account history
    initial.set("data.active_accounts", true);  // the accounts to score

    // Tools in scope: the six execution-graph step kinds the agent mutates
    // (ir::StepKind — connect/clean/model/index/serve/act).
    Task task{"uc1-churn", description, std::move(initial),
              {"connect", "clean", "model", "index", "serve", "act"}, churn_required()};

    // Reference solution: the facts of an actually-built correct churn graph.
    EndState reference = churn_graph_facts(
        build_churn_graph("gbdt", "shap", /*leakage_free=*/true, /*calibrated=*/true,
                          /*calibration_loop=*/true));

    // Negative controls — the facts of degenerate churn graphs, each failing one
    // required fact so the grader must discriminate:
    //  1. his current approach: LLM scoring, not a trained gbdt (+ no SHAP);
    //  2. a leaky pipeline: features peek at the outcome;
    //  3. no calibration loop: scores are never checked against realized churn.
    EndState llm_only = churn_graph_facts(
        build_churn_graph("llm", "none", true, true, true));
    EndState leaky = churn_graph_facts(
        build_churn_graph("gbdt", "shap", /*leakage_free=*/false, true, true));
    EndState no_calibration = churn_graph_facts(
        build_churn_graph("gbdt", "shap", true, true, /*calibration_loop=*/false));

    return AdmissionCase{std::move(task), std::move(reference),
                         {std::move(llm_only), std::move(leaky), std::move(no_calibration)}};
}

}  // namespace zetta::agent
