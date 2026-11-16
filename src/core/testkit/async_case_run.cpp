// SPDX-License-Identifier: MIT

#include "zetta/testkit/async_case_run.hpp"

#include <mutex>
#include <utility>
#include <vector>

#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::testkit {

namespace {

// The Run project label every testkit-driven run carries — what an agent (or
// the Runs panel) filters system.runs by to find case executions.
constexpr const char* kTestkitProject = "testkit";

zetta::pipeline::RunStatus terminal_status(TestOutcome outcome) {
    switch (outcome) {
        case TestOutcome::Pass:
        case TestOutcome::Skipped:
            return zetta::pipeline::RunStatus::Succeeded;
        case TestOutcome::Fail:
        case TestOutcome::Error:
            return zetta::pipeline::RunStatus::Failed;
    }
    return zetta::pipeline::RunStatus::Failed;
}

}  // namespace

std::shared_ptr<AsyncCaseRun> AsyncCaseRun::start(
    Node& node, const TestCase& test_case, TestOptions options,
    std::string custom_data_path, zetta::observability::SpanRecorder* recorder,
    zetta::observability::TraceContext trace) {
    std::shared_ptr<AsyncCaseRun> handle(
        new AsyncCaseRun(node, test_case, std::move(options),
                         std::move(custom_data_path), recorder, trace));
    if (handle->run_id_.empty()) return nullptr;  // create_run failed
    return handle;
}

AsyncCaseRun::AsyncCaseRun(Node& node, const TestCase& test_case,
                           TestOptions options, std::string custom_data_path,
                           zetta::observability::SpanRecorder* recorder,
                           zetta::observability::TraceContext trace)
    : node_(node),
      test_case_(test_case),
      options_(std::move(options)),
      custom_data_path_(std::move(custom_data_path)),
      recorder_(recorder),
      trace_(trace) {
    // On the owner thread by contract, so the durable handle exists — and is
    // Running, trace-linked, and readable via get_run — before start() returns.
    run_ = node_.create_run(kTestkitProject, test_case_.id);
    if (!run_) return;
    run_id_ = run_->id();
    if (!trace_.trace_id.empty()) run_->set_trace_id(trace_.trace_id);
    run_->add_log({zetta::util::now_iso8601(), "info",
                   "testkit case '" + test_case_.id + "' started"});
    run_->set_status(zetta::pipeline::RunStatus::Running);

    worker_ = std::thread([this] {
        // The case body stands up its own ephemeral nodes/clusters and never
        // touches the live node (test_suite.hpp), so it runs freely here; the
        // log lines buffer locally and flush with the one gated write below.
        std::vector<std::string> lines;
        TestContext context;
        context.custom_data_path = custom_data_path_;
        context.recorder = recorder_;
        context.trace = trace_;
        context.log = [&lines](std::string line) {
            lines.push_back(std::move(line));
        };
        TestResult outcome = run_test_case(test_case_, options_, context);

        node_.run_on_owner([this, &lines, &outcome] {
            for (std::string& line : lines) {
                run_->add_log(
                    {zetta::util::now_iso8601(), "info", std::move(line)});
            }
            for (const TestMetric& metric : outcome.metrics) {
                run_->add_log({zetta::util::now_iso8601(), "info",
                               metric.label + ": " + metric.value});
            }
            const bool failed = outcome.outcome == TestOutcome::Fail ||
                                outcome.outcome == TestOutcome::Error;
            run_->add_log({zetta::util::now_iso8601(),
                           failed ? "error" : "info",
                           std::string(to_string(outcome.outcome)) + ": " +
                               (failed && !outcome.error.empty()
                                    ? outcome.error
                                    : outcome.summary)});
            run_->set_status(terminal_status(outcome.outcome));
        });
        result_ = std::move(outcome);
        done_.store(true, std::memory_order_release);
    });
}

AsyncCaseRun::~AsyncCaseRun() {
    if (!worker_.joinable()) return;  // create_run failed; no worker launched
    // The worker's completion flush gates through the owner loop; pump the
    // gate while waiting so joining from the owner thread cannot deadlock
    // (the AsyncChurnRun teardown discipline).
    while (!done_.load(std::memory_order_acquire)) {
        node_.drain_owner_work();
        std::this_thread::yield();
    }
    worker_.join();
}

}  // namespace zetta::testkit
