// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>

#include "zetta/observability/trace.hpp"
#include "zetta/testkit/test_suite.hpp"

namespace zetta {
class Node;
}
namespace zetta::pipeline {
class Run;
}

// Run one testkit case WITHOUT blocking the caller's thread, with the node's
// own Run record as the durable, replicated progress handle (#617 — the MCP
// `testsuite_run` capability's engine entry point, mirroring AsyncChurnRun's
// worker discipline).
//
// start() runs on the node's owner thread: it creates a Run (project label
// "testkit", graph label = the case id) so the durable `run:<uuid>` handle is
// known at submit, marks it Running, then launches a worker that executes
// `run_test_case` — a case body never touches the live node (test_suite.hpp),
// so the only gated write is the completion flush: the case's log lines, its
// summary/metrics, and the terminal status (Pass/Skipped → Succeeded,
// Fail/Error → Failed) land on the Run via Node::run_on_owner. Progress is
// observable through the SAME live record every surface reads (system.runs /
// get_run) — the #614 job-handle semantics; log lines arrive with the
// completion flush, not streamed (a bounded, single gated write).
//
// The destructor joins the worker, PUMPING the node's owner gate while it
// waits (the worker may be blocked handing the completion flush to the owner
// loop). Cases carry no cancel seam, so dropping an unfinished handle waits
// for the case to finish — the same join-in-teardown the headed Test Suite
// panel performs. Destroy the handle on the node's owner thread; `node` must
// outlive the handle.
namespace zetta::testkit {

class AsyncCaseRun {
public:
    // Launch `test_case` with `options`. Must be called on the node's owner
    // thread (the API handlers run there via the bridge). Returns null only
    // if the Run record could not be created on the node. `recorder`/`trace`
    // flow into the case's TestContext so its spans join the caller's trace
    // (#640); null recorder = spans no-op.
    static std::shared_ptr<AsyncCaseRun> start(
        Node& node, const TestCase& test_case, TestOptions options,
        std::string custom_data_path = {},
        zetta::observability::SpanRecorder* recorder = nullptr,
        zetta::observability::TraceContext trace = {});
    ~AsyncCaseRun();

    AsyncCaseRun(const AsyncCaseRun&) = delete;
    AsyncCaseRun& operator=(const AsyncCaseRun&) = delete;

    // True once the worker has flushed the result to the Run and result() is
    // valid.
    bool done() const { return done_.load(std::memory_order_acquire); }

    // The durable engine handle — known from start(), before the case runs.
    const std::string& run_id() const { return run_id_; }

    // The case's structured outcome. Valid only once done() is true.
    const TestResult& result() const { return result_; }

private:
    AsyncCaseRun(Node& node, const TestCase& test_case, TestOptions options,
                 std::string custom_data_path,
                 zetta::observability::SpanRecorder* recorder,
                 zetta::observability::TraceContext trace);

    Node& node_;
    TestCase test_case_;   // by value: the worker owns its inputs
    TestOptions options_;
    std::string custom_data_path_;
    zetta::observability::SpanRecorder* recorder_;
    zetta::observability::TraceContext trace_;
    std::string run_id_;
    std::shared_ptr<zetta::pipeline::Run> run_;
    std::atomic<bool> done_{false};
    TestResult result_;  // written by the worker before done_ flips
    std::thread worker_;
};

}  // namespace zetta::testkit
