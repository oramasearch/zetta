// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/ir/step_kind.hpp"
#include "zetta/testkit/test_suite.hpp"  // TestMetric, TestSuite, TestCategory

// The **sim-Python example-script harness** (issue #523) — the piece that makes
// the example programs ARE the integration tests. It runs a sim-Python example
// script through the whole frontend pipeline end to end — parse (the
// `zetta::input::PythonFrontend`) -> ExecutionGraph IR -> the correctness gate
// (`default_gate`) -> execution (`Node::run_graph`) — and asserts on the outputs.
//
// A script that is expected to run green doubles as a canonical usage example; a
// script that is expected to fail proves the harness surfaces the **offending
// statement** (reconstructed from the failing step's `python` provenance bag, or
// the parser's own locator at the compile phase). Each example is registered as a
// `TestCase` under `TestCategory::Examples`, so the catalog runs both headless
// (`tests/test_example_scripts.cpp`, `tests/test_testkit.cpp`) and in the headed
// **Operations -> Test suite** panel — "run all of these simulated python commands
// as examples in the test suite."
//
// This adds **no new JSON format**: the script source is sim-Python text and the
// only JSON it touches is the already-schema-validated IR JSON form (the
// `ir::from_json` trust boundary the frontend lowers through) and the Run record
// (validated at its own boundary). The expectations below are in-code structs, the
// same shape as `TestOption` / `TestCase`. See docs/testing/example-scripts.md.
namespace zetta::testkit {

// One expected data-flow edge in the lowered IR (a `from -> to` DataFlow edge).
struct ExpectEdge {
    std::string from;
    std::string to;
};

// One expected step in the lowered IR: a graph node id and the StepKind it must
// lower to (e.g. {"train", StepKind::Model}).
struct ExpectStep {
    std::string id;
    ir::StepKind kind;
};

// What an example script asserts about its end-to-end run (parse -> IR -> gate ->
// run). Every field is opt-in; a default-constructed Expectations is the happy
// path — the script compiles, passes the gate, and runs to a Succeeded Run.
//
// To author a *negative* example (a script that must FAIL, so the harness can
// prove failures surface with the offending statement) set the phase that must
// fail to false and put a distinctive substring of the expected reason in
// `failure_contains`. A negative example "passes as a test" when it fails exactly
// as expected with the offending statement surfaced.
struct ExampleExpectations {
    bool compiles = true;     // PythonFrontend::compile succeeds
    bool gate_passes = true;  // default_gate().run(graph).passed()
    bool runs = true;         // the graph executes to a Succeeded Run

    // Optional structural assertions on the lowered IR (checked only when set).
    std::optional<std::size_t> node_count;
    std::optional<std::size_t> edge_count;
    std::vector<ExpectStep> steps;  // ids that must exist + the kind they lower to
    std::vector<ExpectEdge> edges;  // DataFlow edges that must exist

    // Optional assertion on the produced Run's outputs: every kind listed here
    // ("dataset" | "model" | "index") must appear at least once.
    std::vector<std::string> output_kinds;

    // For a negative example: the surfaced reason of the failing phase must contain
    // this substring (empty = accept any reason). Consulted only when one of
    // compiles/gate_passes/runs is false.
    std::string failure_contains;
};

// One sim-Python example program that doubles as an integration test.
struct ExampleScript {
    std::string id;           // stable, dotted (e.g. "examples.churn_pipeline")
    std::string title;        // human title for the panel / test name
    std::string description;  // what the script demonstrates
    std::string source;       // the sim-Python source text
    ExampleExpectations expect;
};

// The phase an end-to-end run reached (and, on a mismatch, the phase it failed in).
// A structural-IR assertion failure is attributed to Compile (it asserts on the
// compile output); an output assertion failure to Run.
enum class ExamplePhase { Compile, Gate, Run, Done };

std::string_view to_string(ExamplePhase phase);

// The structured outcome of running an example script through the harness.
struct ExampleRunReport {
    bool ok = false;                  // the actual behavior matched `expect`
    ExamplePhase reached = ExamplePhase::Compile;  // furthest phase entered
    std::string summary;              // one-line description of what happened
    std::string reason;               // why `expect` was NOT met (empty when ok)
    std::string offending_statement;  // the source statement implicated in a phase
                                      // failure (reconstructed from provenance, or
                                      // the parser's locator) — empty when none
    std::vector<std::string> log;     // phase-by-phase trace
    std::vector<TestMetric> metrics;  // node/edge counts, run status, outputs, …
};

// Run one example script end to end: parse (PythonFrontend) -> IR -> gate
// (default_gate) -> run (Node::run_graph on an ephemeral scratch node), checking
// `script.expect` at each phase. **Non-throwing** (any exception is converted to a
// failing report). On a mismatch the report's `ok` is false, `reason` explains it,
// and `offending_statement` names the source statement at fault.
ExampleRunReport run_example_script(const ExampleScript& script);

// The built-in catalog of example scripts — the sim-Python programs that ARE the
// integration tests (issue #523). A green churn-shaped pipeline + the canonical
// minimal sample + method-chaining, plus negative examples that prove a failing
// script surfaces the offending statement. The catalog grows as the cluster/ML
// sim-Python bindings (#522) and the at-scale scripts (TC-9/10/11/13, UC-1-at-scale
// #531) land — see docs/testing/example-scripts.md.
const std::vector<ExampleScript>& example_scripts();

}  // namespace zetta::testkit
