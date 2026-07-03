#include "zetta/pipeline/executor.hpp"

#include <deque>
#include <string>
#include <unordered_map>
#include <utility>

#include "zetta/node.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::pipeline {
namespace {

// A short, human-readable label for a step in logs + lineage: its name when it
// has one, else its (UUID) id so the step is always identifiable.
std::string step_label(const ir::Node& step) {
    return step.name.empty() ? step.id : step.name;
}

// The source a connect step pulls from, for the run's lineage. Read from the
// step's free-form attribute bag (the same place the future dual-form payload
// anchors): a "source"+"table" pair reads as "source:table", else the first of
// "source" / "uri" / "path", else the step's label as a last resort.
std::string connect_source(const ir::Node& step) {
    if (step.attributes.is_object()) {
        const std::string source = step.attributes.value("source", std::string{});
        const std::string table = step.attributes.value("table", std::string{});
        if (!source.empty() && !table.empty()) return source + ":" + table;
        if (!source.empty()) return source;
        if (const std::string uri = step.attributes.value("uri", std::string{});
            !uri.empty()) {
            return uri;
        }
        if (const std::string path = step.attributes.value("path", std::string{});
            !path.empty()) {
            return path;
        }
    }
    return step_label(step);
}

// If a step is marked to fail (a truthy `"fail"` attribute), the reason. A
// boolean true yields a default reason; a non-empty string is used verbatim.
// This drives the Failed path deterministically without a custom StepExecutor.
std::optional<std::string> requested_failure(const ir::Node& step) {
    if (!step.attributes.is_object()) return std::nullopt;
    const auto it = step.attributes.find("fail");
    if (it == step.attributes.end()) return std::nullopt;
    if (it->is_boolean() && it->get<bool>()) {
        return "step '" + step_label(step) + "' marked as failing";
    }
    if (it->is_string()) {
        const std::string reason = it->get<std::string>();
        if (!reason.empty()) return reason;
    }
    return std::nullopt;
}

// The artifact kind a materializing step produces, or nullopt when the step kind
// produces no artifact (serve exposes existing ones; act acts on the world). The
// returned string is always one of the Run schema's closed output kinds.
std::optional<std::string> output_kind_for(ir::StepKind kind) {
    switch (kind) {
        case ir::StepKind::Connect: return "dataset";
        case ir::StepKind::Clean:   return "dataset";
        case ir::StepKind::Model:   return "model";
        case ir::StepKind::Index:   return "index";
        case ir::StepKind::Serve:   return std::nullopt;
        case ir::StepKind::Act:     return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

StepOutcome DefaultStepExecutor::run_step(const ir::Node& step) const {
    if (const auto reason = requested_failure(step)) {
        return StepOutcome{/*ok=*/false, *reason, {}, {}};
    }

    StepOutcome outcome;
    if (step.kind == ir::StepKind::Connect) {
        outcome.sources.push_back(connect_source(step));
    }
    if (const auto kind = output_kind_for(step.kind)) {
        // ref ties the artifact back to the graph step that produced it; it is a
        // logical handle (no real table/model/index is materialized in R1).
        outcome.outputs.push_back(
            RunOutput{*kind, step_label(step),
                      std::string(to_string(step.kind)) + ":" + step.id});
    }
    return outcome;
}

std::optional<std::vector<std::shared_ptr<ir::Node>>> topological_order(
    const ir::ExecutionGraph& graph) {
    // Kahn's algorithm over all edges (DataFlow and Control both constrain
    // order). Ready steps are taken in graph insertion order, so the result is
    // deterministic; a remaining step means a cycle (a self-edge counts).
    std::unordered_map<ir::NodeId, std::size_t> in_degree;
    in_degree.reserve(graph.node_count());
    for (const auto& node : graph.nodes()) in_degree.emplace(node->id, 0);
    for (const ir::Edge& edge : graph.edges()) ++in_degree[edge.to];

    std::deque<std::shared_ptr<ir::Node>> ready;
    for (const auto& node : graph.nodes()) {
        if (in_degree[node->id] == 0) ready.push_back(node);
    }

    std::vector<std::shared_ptr<ir::Node>> order;
    order.reserve(graph.node_count());
    while (!ready.empty()) {
        const std::shared_ptr<ir::Node> node = ready.front();
        ready.pop_front();
        order.push_back(node);
        for (const ir::Edge& edge : graph.edges_from(node->id)) {
            if (--in_degree[edge.to] == 0) {
                ready.push_back(graph.find_node(edge.to));
            }
        }
    }

    if (order.size() != graph.node_count()) return std::nullopt;  // cycle
    return order;
}

GraphExecutor::GraphExecutor(std::shared_ptr<StepExecutor> step_executor)
    : step_executor_(step_executor ? std::move(step_executor)
                                   : std::make_shared<DefaultStepExecutor>()) {}

ExecutionResult GraphExecutor::execute(Node& node,
                                       const ir::ExecutionGraph& graph,
                                       std::string project,
                                       std::string graph_id) const {
    ExecutionResult result;
    result.run = node.create_run(std::move(project), std::move(graph_id));
    if (!result.run) return result;  // node could not create the run
    Run& run = *result.run;

    // One trace per run (#640): the id lives on the replicated run record, so
    // spans recorded anywhere for this run join it. The run root span + the
    // per-step child spans below land in this node's ring (system.spans).
    run.set_trace_id(zetta::observability::make_root_trace().trace_id);
    const zetta::observability::TraceContext run_trace{
        run.value().trace_id, zetta::observability::make_span_id()};
    zetta::observability::ScopedSpan run_span(&node.span_recorder(), run_trace,
                                              "run:" + run.graph());
    run_span.set_attribute("run_id", run.id());
    run_span.set_attribute("project", run.project());
    run_span.set_attribute(
        "plan", nlohmann::json{{"steps", graph.node_count()}});
    const zetta::observability::TraceContext step_parent{
        run_trace.trace_id, run_span.span_id()};

    run.add_log({zetta::util::now_iso8601(), "info",
                 "run created: " + std::to_string(graph.node_count()) +
                     " step(s)"});

    // Guard the basics the orchestrator needs to run at all. Full semantic
    // validation (legal stage transitions, required inputs) is the correctness
    // gate's job (a separate slice); here we reject only what makes the graph
    // unrunnable, and record the rejection on the run.
    if (graph.empty()) {
        run.add_log({zetta::util::now_iso8601(), "error",
                     "execution graph has no steps; nothing to run"});
        run.set_status(RunStatus::Failed);
        result.error = "execution graph has no steps";
        run_span.finish(false, result.error);
        return result;
    }
    const auto order = topological_order(graph);
    if (!order) {
        run.add_log({zetta::util::now_iso8601(), "error",
                     "execution graph has a cycle; cannot order steps"});
        run.set_status(RunStatus::Failed);
        result.error = "execution graph has a cycle";
        run_span.finish(false, result.error);
        return result;
    }

    run.set_status(RunStatus::Running);
    run.add_log({zetta::util::now_iso8601(), "info", "run started"});

    std::vector<RunOutput> outputs;
    RunLineage lineage;
    for (const std::shared_ptr<ir::Node>& step : *order) {
        run.add_log({zetta::util::now_iso8601(), "info",
                     std::string(to_string(step->kind)) + ": " +
                         step_label(*step)});
        zetta::observability::ScopedSpan step_span(
            &node.span_recorder(), step_parent,
            "step:" + std::string(to_string(step->kind)));
        step_span.set_attribute("label", step_label(*step));
        StepOutcome outcome = step_executor_->run_step(*step);
        step_span.set_attribute(
            "result_shape", nlohmann::json{{"outputs", outcome.outputs.size()}});
        step_span.finish(outcome.ok, outcome.error);
        lineage.steps.push_back(step_label(*step));
        for (std::string& source : outcome.sources) {
            lineage.sources.push_back(std::move(source));
        }
        if (!outcome.ok) {
            result.ok = false;
            result.failed_step = step->id;
            result.error = outcome.error;
            run.add_log({zetta::util::now_iso8601(), "error",
                         std::string(to_string(step->kind)) + " step '" +
                             step_label(*step) + "' failed: " + outcome.error});
            break;  // fail-fast: downstream steps do not run
        }
        for (const RunOutput& output : outcome.outputs) outputs.push_back(output);
    }

    lineage.freshness = zetta::util::now_iso8601();
    // A deterministic local orchestration that completed is fully trusted; a
    // partial (failed) run carries no confidence. Real model-derived confidence
    // is later work — see docs/pipeline/execution.md.
    result.ok = result.failed_step.empty();
    lineage.confidence = result.ok ? 1.0 : 0.0;

    run.set_outputs(std::move(outputs));
    run.set_lineage(std::move(lineage));
    if (result.ok) {
        run.add_log({zetta::util::now_iso8601(), "info",
                     "run succeeded: " +
                         std::to_string(run.outputs().size()) + " output(s)"});
        run.set_status(RunStatus::Succeeded);
    } else {
        run.set_status(RunStatus::Failed);
    }
    run_span.set_attribute(
        "result_shape",
        nlohmann::json{{"status", to_string(run.status())},
                       {"outputs", run.outputs().size()},
                       {"failed_step", result.failed_step}});
    run_span.finish(result.ok, result.error);
    return result;
}

}  // namespace zetta::pipeline