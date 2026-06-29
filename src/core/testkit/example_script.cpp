#include "zetta/testkit/example_script.hpp"

#include <exception>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/executor.hpp"
#include "zetta/gate.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/node.hpp"
#include "zetta/run.hpp"
#include "zetta/testkit/harness.hpp"  // ScratchDir
#include "zetta/validation.hpp"

// The sim-Python example-script harness (issue #523): run a script end to end
// (parse -> IR -> gate -> run) and check its declared expectations, surfacing the
// offending statement on any failure. See docs/example-scripts.md.
namespace zetta::testkit {
namespace {

NodeConfig scratch_config(const ScratchDir& dir) {
    NodeConfig config;
    config.data_dir = dir.path().string();
    config.preflight = false;  // ephemeral, freshly built — nothing to verify
    return config;
}

// Render one captured call argument back to a readable source token. The frontend
// records a literal as its value, a variable reference as `{ref: name}`, and a
// nested call as `{call: name}` (see PythonFrontend::capture_argument).
std::string render_argument(const nlohmann::json& value) {
    if (value.is_string()) return "'" + value.get<std::string>() + "'";
    if (value.is_boolean()) return value.get<bool>() ? "True" : "False";
    if (value.is_null()) return "None";
    if (value.is_object()) {
        if (value.contains("ref") && value["ref"].is_string()) {
            return value["ref"].get<std::string>();
        }
        if (value.contains("call")) {
            if (value["call"].is_string()) return value["call"].get<std::string>() + "(...)";
            return "(...)";
        }
    }
    if (value.is_array()) {
        std::string out = "[";
        for (std::size_t i = 0; i < value.size(); ++i) {
            if (i != 0) out += ", ";
            out += render_argument(value[i]);
        }
        return out + "]";
    }
    return value.dump();
}

// Reconstruct a readable "[target = ]verb(args, kw=val)" statement from a node's
// `python` provenance bag, so a failing step can be named back to its source
// statement. The frontend records call / assigned_to / args / kwargs per node (no
// source-line table), so we rebuild a canonical, deterministic, legible form.
std::string reconstruct_statement(const ir::ExecutionGraph& graph,
                                  const std::string& node_id) {
    const auto node = graph.find_node(node_id);
    if (!node) return node_id;  // no provenance — fall back to the id
    const nlohmann::json bag =
        node->attributes.value("python", nlohmann::json::object());
    const std::string verb = bag.value("call", node->name);

    std::string rendered;
    if (bag.contains("assigned_to") && bag["assigned_to"].is_string()) {
        rendered += bag["assigned_to"].get<std::string>() + " = ";
    }
    rendered += verb + "(";
    bool first = true;
    const auto separate = [&] {
        if (!first) rendered += ", ";
        first = false;
    };
    if (bag.contains("args") && bag["args"].is_array()) {
        for (const auto& arg : bag["args"]) {
            separate();
            rendered += render_argument(arg);
        }
    }
    if (bag.contains("kwargs") && bag["kwargs"].is_object()) {
        for (auto it = bag["kwargs"].begin(); it != bag["kwargs"].end(); ++it) {
            separate();
            rendered += it.key() + "=" + render_argument(it.value());
        }
    }
    rendered += ")";
    return rendered;
}

// Is there a DataFlow edge `from -> to` in the graph?
bool has_dataflow_edge(const ir::ExecutionGraph& graph, const std::string& from,
                       const std::string& to) {
    for (const auto& edge : graph.edges()) {
        if (edge.from == from && edge.to == to &&
            edge.kind == ir::EdgeKind::DataFlow) {
            return true;
        }
    }
    return false;
}

// The first failing check's "name: reason" — the offending-statement locator at
// the gate phase (the gate's CheckResult carries no node id, so its reason, which
// names the step, is the best available locator).
std::string first_gate_failure(const ValidationReport& report) {
    for (const CheckResult& check : report.checks) {
        if (check.status == CheckStatus::Fail) {
            return check.name + ": " + check.reason;
        }
    }
    return "gate blocked deploy";
}

// Set the report into a clean "expectation not met" failure and return it.
ExampleRunReport mismatch(ExampleRunReport report, std::string reason) {
    report.ok = false;
    report.reason = reason;
    report.summary = std::move(reason);
    return report;
}

// Decide whether an *expected* failure at a phase matches what actually happened:
// the actual reason must contain `failure_contains` (empty = any reason).
bool failure_matches(const std::string& actual, const std::string& expected_substr) {
    return expected_substr.empty() || actual.find(expected_substr) != std::string::npos;
}

ExampleRunReport run_example_script_impl(const ExampleScript& script) {
    const ExampleExpectations& expect = script.expect;
    ExampleRunReport report;

    // ---- Phase 1: compile (parse -> ExecutionGraph IR) ----------------------
    report.reached = ExamplePhase::Compile;
    report.log.push_back("compile: lowering sim-Python -> ExecutionGraph IR");
    const input::FrontendResult compiled =
        input::PythonFrontend{}.compile(script.source);

    if (!compiled.ok()) {
        // The frontend is non-throwing; its error string is the parser's own
        // locator at this phase (e.g. "unterminated string", "unexpected ')'").
        report.offending_statement = compiled.error;
        report.metrics.push_back(TestMetric{"phase", "compile"});
        report.log.push_back("compile: FAILED — " + compiled.error);
        if (expect.compiles) {
            return mismatch(std::move(report), "compile failed: " + compiled.error);
        }
        if (!failure_matches(compiled.error, expect.failure_contains)) {
            return mismatch(std::move(report),
                            "compile failed, but the reason did not contain '" +
                                expect.failure_contains + "': " + compiled.error);
        }
        report.ok = true;
        report.summary =
            "compile failed as expected; offending statement surfaced";
        return report;
    }
    if (!expect.compiles) {
        report.metrics.push_back(
            TestMetric{"nodes", std::to_string(compiled.graph.node_count())});
        return mismatch(std::move(report),
                        "expected the script to fail at compile, but it compiled");
    }

    const ir::ExecutionGraph& graph = compiled.graph;
    report.metrics.push_back(
        TestMetric{"steps", std::to_string(graph.node_count())});
    report.metrics.push_back(
        TestMetric{"edges", std::to_string(graph.edge_count())});
    report.log.push_back("compile: ok (" + std::to_string(graph.node_count()) +
                         " steps, " + std::to_string(graph.edge_count()) +
                         " edges)");

    // ---- structural assertions on the lowered IR (attributed to Compile) ----
    if (expect.node_count && graph.node_count() != *expect.node_count) {
        return mismatch(std::move(report),
                        "expected " + std::to_string(*expect.node_count) +
                            " steps, lowered to " +
                            std::to_string(graph.node_count()));
    }
    if (expect.edge_count && graph.edge_count() != *expect.edge_count) {
        return mismatch(std::move(report),
                        "expected " + std::to_string(*expect.edge_count) +
                            " edges, lowered to " +
                            std::to_string(graph.edge_count()));
    }
    for (const ExpectStep& step : expect.steps) {
        const auto node = graph.find_node(step.id);
        if (!node) {
            report.offending_statement = step.id;
            return mismatch(std::move(report),
                            "expected step '" + step.id + "' is missing");
        }
        if (node->kind != step.kind) {
            report.offending_statement = reconstruct_statement(graph, step.id);
            return mismatch(std::move(report),
                            "step '" + step.id + "' lowered to " +
                                std::string(ir::to_string(node->kind)) +
                                ", expected " +
                                std::string(ir::to_string(step.kind)));
        }
    }
    for (const ExpectEdge& edge : expect.edges) {
        if (!has_dataflow_edge(graph, edge.from, edge.to)) {
            report.offending_statement = reconstruct_statement(graph, edge.to);
            return mismatch(std::move(report), "expected data-flow edge " +
                                                   edge.from + " -> " + edge.to +
                                                   " is missing");
        }
    }

    // ---- Phase 2: gate (RULE-1 — no run without a passing gate) -------------
    report.reached = ExamplePhase::Gate;
    report.log.push_back("gate: running default_gate()");
    const ValidationReport gate = default_gate().run(graph);
    report.metrics.push_back(
        TestMetric{"gate checks", std::to_string(gate.checks.size())});

    if (!gate.passed()) {
        const std::string failure = first_gate_failure(gate);
        report.offending_statement = failure;
        report.log.push_back("gate: BLOCKED — " + failure);
        if (expect.gate_passes) {
            return mismatch(std::move(report), "gate blocked the run: " + failure);
        }
        if (!failure_matches(failure, expect.failure_contains)) {
            return mismatch(std::move(report),
                            "gate blocked, but the reason did not contain '" +
                                expect.failure_contains + "': " + failure);
        }
        report.ok = true;
        report.summary = "gate blocked the run as expected (RULE-1)";
        return report;
    }
    if (!expect.gate_passes) {
        return mismatch(std::move(report),
                        "expected the gate to block the script, but it passed");
    }
    report.log.push_back("gate: passed");

    // ---- Phase 3: run (execute the graph into a Run) ------------------------
    report.reached = ExamplePhase::Run;
    report.log.push_back("run: executing the graph on an ephemeral node");
    ScratchDir dir;
    Node node{scratch_config(dir)};
    node.start();
    const ExecutionResult execution = node.run_graph(graph, "examples", script.id);
    const RunStatus status =
        execution.run ? execution.run->status() : RunStatus::Failed;
    const std::vector<RunOutput> outputs =
        execution.run ? execution.run->outputs() : std::vector<RunOutput>{};
    node.stop();

    report.metrics.push_back(TestMetric{"run", std::string(to_string(status))});
    report.metrics.push_back(
        TestMetric{"outputs", std::to_string(outputs.size())});

    if (!execution.ok) {
        report.offending_statement =
            reconstruct_statement(graph, execution.failed_step);
        report.log.push_back("run: FAILED at step '" + execution.failed_step +
                             "' — " + execution.error);
        if (expect.runs) {
            return mismatch(std::move(report),
                            "run failed at step '" + execution.failed_step +
                                "': " + execution.error);
        }
        if (!failure_matches(execution.error, expect.failure_contains)) {
            return mismatch(std::move(report),
                            "run failed, but the reason did not contain '" +
                                expect.failure_contains + "': " + execution.error);
        }
        report.ok = true;
        report.summary = "run failed as expected; offending statement surfaced";
        return report;
    }
    if (!expect.runs) {
        return mismatch(std::move(report),
                        "expected the run to fail, but it succeeded");
    }

    // ---- assert on the produced outputs (attributed to Run) -----------------
    for (const std::string& kind : expect.output_kinds) {
        bool found = false;
        for (const RunOutput& output : outputs) {
            if (output.kind == kind) {
                found = true;
                break;
            }
        }
        if (!found) {
            return mismatch(std::move(report), "expected the run to produce a '" +
                                                   kind + "' output, but none was");
        }
    }

    // ---- Done — every expectation held --------------------------------------
    report.reached = ExamplePhase::Done;
    report.ok = true;
    report.summary = "compiled, gated, and ran to " +
                     std::string(to_string(status)) + " (" +
                     std::to_string(graph.node_count()) + " steps, " +
                     std::to_string(outputs.size()) + " outputs)";
    report.log.push_back("done: " + report.summary);
    return report;
}

}  // namespace

std::string_view to_string(ExamplePhase phase) {
    switch (phase) {
        case ExamplePhase::Compile: return "compile";
        case ExamplePhase::Gate:    return "gate";
        case ExamplePhase::Run:     return "run";
        case ExamplePhase::Done:    return "done";
    }
    return "unknown";
}

ExampleRunReport run_example_script(const ExampleScript& script) {
    // Non-throwing by contract: a misbehaving engine path can never propagate out.
    try {
        return run_example_script_impl(script);
    } catch (const std::exception& error) {
        ExampleRunReport report;
        report.ok = false;
        report.reason = std::string("exception: ") + error.what();
        report.summary = report.reason;
        return report;
    } catch (...) {
        ExampleRunReport report;
        report.ok = false;
        report.reason = "unknown exception";
        report.summary = report.reason;
        return report;
    }
}

}  // namespace zetta::testkit
