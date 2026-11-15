// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/observability/trace.hpp"

// The in-app test suite catalog (`zetta::testkit`) — issue #462, building on the
// UC-1 end-to-end (#399). A headless-runnable library of **canned, parameterized
// test cases** that exercise zetta's real primitives end to end: compute / device
// operators, DDF ops, the ML pipeline, distributed / cluster behavior over a real
// multi-node loopback cluster, governance (access + audit + the correctness gate),
// and the whole UC-1 churn pipeline.
//
// This is the model + engine; the **GUI is a thin renderer** over it (the headed
// Test Suite panel, `App::draw_test_suite`), the same rule SDL/ImGui already
// follow. Because the catalog lives in zetta-core it is also runnable headless
// (`tests/test_testkit.cpp`) and carries no SDL dependency.
//
// A `TestCase` declares typed **options** (what the per-test "options modal"
// renders) and a `run(TestOptions, TestContext) -> TestResult` body that calls the
// real engine and reports a structured result (outcome + summary + metrics + log).
// `run_test_case` times the body and converts any thrown exception into a clean
// `Error` result, so a case can never take the app down. See docs/testing/test-suite.md.
namespace zetta::testkit {

// The category a case belongs to — used to group the catalog in the panel and to
// reason about what a case exercises. Declared roughly bottom-up (device → ops →
// model → cluster → governance → the whole pipeline).
enum class TestCategory {
    Compute,      // ComputeDevice operators (CPU always; CUDA when built)
    Ddf,          // DDF DataFrame ops (scan/filter/project/group-by/as-of/sort/join)
    Ml,           // featurize / train (GBDT) / explain (TreeSHAP) / calibrate / score
    Distributed,  // collectives / distributed ops over a real multi-node cluster
    Governance,   // key-scoped access / audit trail / the correctness gate
    Pipeline,     // the UC-1 churn end-to-end (gated + governed)
    Examples,     // sim-Python example scripts run end to end (parse->IR->gate->run)
};

// The canonical lowercase name of a category ("compute", "ddf", …) and a short
// human title ("Compute & device", …) for the panel.
std::string_view to_string(TestCategory category);
std::string_view category_title(TestCategory category);

// The type of a configurable option — picks the widget the panel renders and the
// typed getter a case reads it with.
enum class OptionType { Bool, Int, Double, Choice, Text };

// One configurable option of a case — the unit the options modal renders. `key` is
// the stable id a case reads the value with; the default / bounds / choices are
// interpreted by `type`. For `Choice`, `choices` are the labels and `default_int`
// is the selected index.
struct TestOption {
    std::string key;
    std::string label;
    std::string help;
    OptionType type = OptionType::Int;

    bool default_bool = false;
    long default_int = 0;
    long min_int = 0;
    long max_int = 0;  // min==max==0 => treat as unbounded (free input)

    double default_double = 0.0;
    double min_double = 0.0;
    double max_double = 0.0;

    std::string default_text;
    std::vector<std::string> choices;  // for OptionType::Choice

    // Factory helpers (keep case declarations terse + legible).
    static TestOption boolean(std::string key, std::string label, bool value,
                              std::string help = {});
    static TestOption integer(std::string key, std::string label, long value,
                              long min_value, long max_value, std::string help = {});
    static TestOption real(std::string key, std::string label, double value,
                           double min_value, double max_value, std::string help = {});
    static TestOption choice(std::string key, std::string label,
                             std::vector<std::string> options, int selected,
                             std::string help = {});
    static TestOption text(std::string key, std::string label, std::string value,
                           std::string help = {});
};

// A bag of option values keyed by TestOption::key. Built from a case's defaults
// (`TestCase::defaults`) and then overridden by the panel. Typed getters return a
// caller-supplied fallback when a key is absent or holds another type.
class TestOptions {
public:
    void set_bool(const std::string& key, bool value);
    void set_int(const std::string& key, long value);
    void set_double(const std::string& key, double value);
    void set_choice(const std::string& key, int index);
    void set_text(const std::string& key, std::string value);

    bool get_bool(const std::string& key, bool fallback = false) const;
    long get_int(const std::string& key, long fallback = 0) const;
    double get_double(const std::string& key, double fallback = 0.0) const;
    int get_choice(const std::string& key, int fallback = 0) const;
    std::string get_text(const std::string& key, std::string fallback = {}) const;

    bool contains(const std::string& key) const { return values_.count(key) != 0; }

private:
    struct Value {
        OptionType type = OptionType::Int;
        bool boolean = false;
        long integer = 0;
        double real = 0.0;
        int choice = 0;
        std::string text;
    };
    std::map<std::string, Value> values_;
};

// The outcome of running a case. Pass / Fail are the assertion verdict; Skipped is
// "not applicable here" (e.g. a CUDA case on a CPU-only build); Error is an
// unexpected failure (a thrown exception, a setup failure) distinct from a clean
// assertion Fail.
enum class TestOutcome { Pass, Fail, Skipped, Error };

std::string_view to_string(TestOutcome outcome);

// One labelled metric a case reports (pre-formatted; rendered mono in the panel) —
// e.g. {"AUC", "0.973"}, {"nodes", "3"}.
struct TestMetric {
    std::string label;
    std::string value;
};

// The structured result of one run. `summary` is the one-line verdict; `metrics`
// are key/value rows; `log` is the step-by-step trace; `error` carries the reason
// when Fail / Error. `elapsed_seconds` is filled by run_test_case.
struct TestResult {
    TestOutcome outcome = TestOutcome::Error;
    std::string summary;
    std::vector<TestMetric> metrics;
    std::vector<std::string> log;
    std::string error;
    double elapsed_seconds = 0.0;

    // Convenience builders used inside case bodies.
    static TestResult pass(std::string summary);
    static TestResult fail(std::string summary);
    static TestResult skip(std::string summary);
    static TestResult err(std::string summary);

    TestResult& metric(std::string label, std::string value);
    TestResult& note(std::string line);  // append a log line, returns *this
    bool ok() const { return outcome == TestOutcome::Pass; }
};

// What a case is handed beyond its options: an optional user-supplied custom data
// path (a churn dataset directory, "" = use the built-in synthetic dataset), a
// progress sink the case can stream log lines to as it runs (may be null), and
// an optional span ring + parent trace (#640) — when wired, run_test_case
// records every case as a span (per-case timing + result-shape history — the
// iteration flywheel; docs/architecture/observability.md). Null recorder =
// spans no-op (the ApiContext pattern).
struct TestContext {
    std::string custom_data_path;
    std::function<void(std::string)> log;
    zetta::observability::SpanRecorder* recorder = nullptr;
    zetta::observability::TraceContext trace;
    void emit(std::string line) const {
        if (log) log(std::move(line));
    }
};

// The body that runs one case: a pure-ish function of (options, context) producing
// a result. It may call any zetta-core primitive (including standing up an
// ephemeral loopback cluster); it must not touch the app's live node.
using TestRun = std::function<TestResult(const TestOptions&, TestContext&)>;

// One canned, parameterized test case.
struct TestCase {
    std::string id;  // stable, unique, dotted (e.g. "compute.cpu.elementwise")
    TestCategory category = TestCategory::Compute;
    std::string name;
    std::string description;
    std::vector<TestOption> options;
    TestRun run;

    // Build a TestOptions seeded from this case's option defaults — the panel's
    // starting point before the user edits anything.
    TestOptions defaults() const;
};

// An ordered, read-only catalog of cases. Built once via builtin(); the panel
// renders it and runs selected cases.
class TestSuite {
public:
    void add(TestCase test_case);

    const std::vector<TestCase>& cases() const { return cases_; }
    std::size_t size() const { return cases_.size(); }
    const TestCase* find(const std::string& id) const;
    std::vector<const TestCase*> in_category(TestCategory category) const;

    // The built-in catalog: every canned case across all categories, in a stable
    // display order. Assembled once and cached.
    static const TestSuite& builtin();

private:
    std::vector<TestCase> cases_;
};

// Run a case with the given options + context, timing it. Catches any exception
// thrown by the body and reports it as an Error result (with the message), so a
// misbehaving case can never propagate across the seam / crash the app. A null
// run body yields an Error. Never throws.
TestResult run_test_case(const TestCase& test_case, const TestOptions& options,
                         TestContext& context);

}  // namespace zetta::testkit
