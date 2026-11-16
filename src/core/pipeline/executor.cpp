// SPDX-License-Identifier: MIT

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
                                       std::string graph_id,
                                       const std::atomic<bool>* cancel,
                                       std::shared_ptr<Run> existing_run) const {
    // Every node-tree write below (the Run creation + the Run's mutators) is
    // gated onto the node's owner thread (#624): inline for the synchronous
    // owner-thread callers, a queued handoff when a background-action worker
    // drives the run. Span recording stays ungated — the recorder is
    // internally mutex-guarded. Run fields read on this thread are captured
    // inside the gated blocks (the tick reconcile may touch the entity on the
    // owner thread while a step computes here).
    ExecutionResult result;
    result.run = std::move(existing_run);
    if (!result.run) {
        node.run_on_owner([&] {
            result.run = node.create_run(std::move(project), std::move(graph_id));
        });
    }
    if (!result.run) return result;  // node could not create the run
    // The graph executed may not be the stored one (a deployed graph, one a
    // caller rewrote): record what its connect steps read before any step runs,
    // so delete_dataset refuses those tables while the run is in flight
    // (zetta#1272).
    node.run_on_owner([&] { result.run->add_reads(node.tables_read(graph)); });
    Run& run = *result.run;

    // One trace per run (#640): the id lives on the replicated run record, so
    // spans recorded anywhere for this run join it. The run root span + the
    // per-step child spans below land in this node's ring (system.spans).
    std::string trace_id;
    std::string run_id;
    std::string run_project;
    std::string run_graph_label;
    node.run_on_owner([&] {
        run.set_trace_id(zetta::observability::make_root_trace().trace_id);
        trace_id = run.value().trace_id;
        run_id = run.id();
        run_project = run.project();
        run_graph_label = run.graph();
        run.add_log({zetta::util::now_iso8601(), "info",
                     "run created: " + std::to_string(graph.node_count()) +
                         " step(s)"});
    });
    step_executor_->bind_run(node, run_id);
    const zetta::observability::TraceContext run_trace{
        trace_id, zetta::observability::make_span_id()};
    zetta::observability::ScopedSpan run_span(&node.span_recorder(), run_trace,
                                              "run:" + run_graph_label);
    run_span.set_attribute("run_id", run_id);
    run_span.set_attribute("project", run_project);
    run_span.set_attribute(
        "plan", nlohmann::json{{"steps", graph.node_count()}});
    const zetta::observability::TraceContext step_parent{
        run_trace.trace_id, run_span.span_id()};

    // Guard the basics the orchestrator needs to run at all. Full semantic
    // validation (legal stage transitions, required inputs) is the correctness
    // gate's job (a separate slice); here we reject only what makes the graph
    // unrunnable, and record the rejection on the run.
    if (graph.empty()) {
        node.run_on_owner([&] {
            run.add_log({zetta::util::now_iso8601(), "error",
                         "execution graph has no steps; nothing to run"});
            run.set_status(RunStatus::Failed);
        });
        result.error = "execution graph has no steps";
        run_span.finish(false, result.error);
        return result;
    }
    const auto order = topological_order(graph);
    if (!order) {
        node.run_on_owner([&] {
            run.add_log({zetta::util::now_iso8601(), "error",
                         "execution graph has a cycle; cannot order steps"});
            run.set_status(RunStatus::Failed);
        });
        result.error = "execution graph has a cycle";
        run_span.finish(false, result.error);
        return result;
    }

    node.run_on_owner([&] {
        run.set_status(RunStatus::Running);
        run.add_log({zetta::util::now_iso8601(), "info", "run started"});
    });

    bool cancelled = false;
    std::vector<RunOutput> outputs;
    RunLineage lineage;
    for (const std::shared_ptr<ir::Node>& step : *order) {
        // Cooperative cancel (#624), checked BETWEEN steps: the in-flight step
        // completes; the remaining steps do not run.
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            cancelled = true;
            result.error = "run cancelled by user";
            node.run_on_owner([&] {
                run.add_log({zetta::util::now_iso8601(), "warn",
                             "run cancelled by user: remaining steps skipped"});
            });
            break;
        }
        node.run_on_owner([&] {
            run.add_log({zetta::util::now_iso8601(), "info",
                         std::string(to_string(step->kind)) + ": " +
                             step_label(*step)});
        });
        zetta::observability::ScopedSpan step_span(
            &node.span_recorder(), step_parent,
            "step:" + std::string(to_string(step->kind)));
        step_span.set_attribute("label", step_label(*step));
        // Step authority (#843): the run's principal must hold the right this
        // step exercises before it executes. A denial fails the step like any
        // other failure (fail-fast), so nothing past it runs.
        zetta::governance::AccessDecision authority{true, {}};
        if (!principal_.local()) {
            node.run_on_owner(
                [&] { authority = authorize_step(node, principal_, *step, run_id); });
        }
        StepOutcome outcome;
        if (authority) {
            outcome = step_executor_->run_step(*step);
        } else {
            outcome.ok = false;
            outcome.error = "not authorized: " + principal_.acting_id + " lacks " +
                            std::string(zetta::governance::to_string(
                                step_operation(step->kind).domain)) +
                            ":" + step_operation(step->kind).action + " (" +
                            authority.reason + ")";
        }
        step_span.set_attribute(
            "result_shape", nlohmann::json{{"outputs", outcome.outputs.size()}});
        step_span.finish(outcome.ok, outcome.error);
        lineage.steps.push_back(step_label(*step));
        if (!outcome.notes.empty()) {
            node.run_on_owner([&] {
                for (const std::string& note : outcome.notes) {
                    run.add_log({zetta::util::now_iso8601(), "info", note});
                }
            });
        }
        for (std::string& source : outcome.sources) {
            lineage.sources.push_back(std::move(source));
        }
        if (!outcome.ok) {
            result.ok = false;
            result.failed_step = step->id;
            result.error = outcome.error;
            node.run_on_owner([&] {
                run.add_log({zetta::util::now_iso8601(), "error",
                             std::string(to_string(step->kind)) + " step '" +
                                 step_label(*step) +
                                 "' failed: " + outcome.error});
            });
            break;  // fail-fast: downstream steps do not run
        }
        if (cancel != nullptr && cancel->load(std::memory_order_acquire)) {
            cancelled = true;
            result.error = "run cancelled by user";
            node.run_on_owner([&] {
                run.add_log({zetta::util::now_iso8601(), "warn",
                             "run cancelled by user: remaining steps skipped"});
            });
            break;
        }
        for (RunOutput& output : outcome.outputs) {
            // A `report` names what the run FOUND, and it is assembled from the
            // run, so the run's own id is its ref. A StepExecutor cannot fill this
            // itself — the Run is created here, after the executor was built — so
            // it leaves the ref empty and this stamps it.
            if (output.kind == "report" && output.ref.empty()) output.ref = run.id();
            outputs.push_back(std::move(output));
        }
    }

    lineage.freshness = zetta::util::now_iso8601();
    // A deterministic local orchestration that completed is fully trusted; a
    // partial (failed / cancelled) run carries no confidence. Real
    // model-derived confidence is later work — see docs/pipeline/execution.md.
    result.ok = result.failed_step.empty() && !cancelled;
    lineage.confidence = result.ok ? 1.0 : 0.0;

    const std::size_t output_count = outputs.size();
    node.run_on_owner([&] {
        run.set_outputs(std::move(outputs));
        run.set_lineage(std::move(lineage));
        if (result.ok) {
            run.add_log({zetta::util::now_iso8601(), "info",
                         "run succeeded: " + std::to_string(output_count) +
                             " output(s)"});
            run.set_status(RunStatus::Succeeded);
        } else {
            run.set_status(RunStatus::Failed);
        }
    });
    run_span.set_attribute(
        "result_shape",
        nlohmann::json{{"status",
                        to_string(result.ok ? RunStatus::Succeeded
                                            : RunStatus::Failed)},
                       {"outputs", output_count},
                       {"failed_step", result.failed_step}});
    run_span.finish(result.ok, result.error);
    return result;
}

}  // namespace zetta::pipeline
