#include <gtest/gtest.h>

#include <functional>
#include <string>

#include "zetta/testkit/test_suite.hpp"

// The in-app test-suite catalog (zetta::testkit) runs headless here: every
// representative case actually exercises the real engine and reports a structured
// result. This proves the catalog the GUI renders is correct (the GUI is only a
// thin renderer over it). Distributed cases stand up a real loopback cluster, so
// they are run at the smallest multi-node size to keep this fast.
namespace {

using zetta::testkit::TestCategory;
using zetta::testkit::TestContext;
using zetta::testkit::TestOptions;
using zetta::testkit::TestOutcome;
using zetta::testkit::TestResult;
using zetta::testkit::TestSuite;
using zetta::testkit::run_test_case;

// Run a case by id with optional option overrides applied to its defaults.
TestResult run(const std::string& id,
               const std::function<void(TestOptions&)>& overrides = {}) {
    const TestSuite& suite = TestSuite::builtin();
    const auto* test_case = suite.find(id);
    EXPECT_NE(test_case, nullptr) << "no such case: " << id;
    if (!test_case) return TestResult::err("missing case");
    TestOptions options = test_case->defaults();
    if (overrides) overrides(options);
    TestContext context;
    return run_test_case(*test_case, options, context);
}

}  // namespace

TEST(Testkit, BuiltinCatalogSpansEveryCategory) {
    const TestSuite& suite = TestSuite::builtin();
    EXPECT_GT(suite.size(), 10u);
    for (const TestCategory category :
         {TestCategory::Compute, TestCategory::Ddf, TestCategory::Ml,
          TestCategory::Distributed, TestCategory::Governance,
          TestCategory::Pipeline, TestCategory::Examples}) {
        EXPECT_FALSE(suite.in_category(category).empty())
            << "category " << zetta::testkit::to_string(category) << " has no cases";
    }
    // Every case has a stable id, a name, and a run body.
    for (const auto& test_case : suite.cases()) {
        EXPECT_FALSE(test_case.id.empty());
        EXPECT_FALSE(test_case.name.empty());
        EXPECT_TRUE(static_cast<bool>(test_case.run)) << test_case.id;
    }
}

TEST(Testkit, ComputeCasesPass) {
    const TestResult elementwise =
        run("compute.cpu.elementwise", [](TestOptions& o) { o.set_int("count", 256); });
    EXPECT_EQ(elementwise.outcome, TestOutcome::Pass) << elementwise.error;
    const TestResult reduce_sort =
        run("compute.cpu.reduce_sort", [](TestOptions& o) { o.set_int("count", 256); });
    EXPECT_EQ(reduce_sort.outcome, TestOutcome::Pass) << reduce_sort.error;
    // CUDA conformance is Pass when built, Skipped on a CPU-only build — both fine.
    const TestResult cuda = run("compute.cuda.conformance");
    EXPECT_TRUE(cuda.outcome == TestOutcome::Pass || cuda.outcome == TestOutcome::Skipped);
}

TEST(Testkit, DdfCasesPass) {
    const TestResult scan_filter =
        run("ddf.scan_filter", [](TestOptions& o) { o.set_int("rows", 500); });
    EXPECT_EQ(scan_filter.outcome, TestOutcome::Pass) << scan_filter.error;
    const TestResult group_by =
        run("ddf.group_by", [](TestOptions& o) { o.set_int("rows", 500); });
    EXPECT_EQ(group_by.outcome, TestOutcome::Pass) << group_by.error;
}

// The TC-9 (#499) Parquet reader parity case runs over the embedded pyarrow
// fixture + golden — both at the default chunk size and at a tiny one (so each
// column splits across chunks while still matching the golden exactly).
TEST(Testkit, ParquetReaderParityCasePasses) {
    const TestResult whole = run("ddf.parquet_reader_parity");
    EXPECT_EQ(whole.outcome, TestOutcome::Pass) << whole.error;
    const TestResult chunked =
        run("ddf.parquet_reader_parity", [](TestOptions& o) { o.set_int("chunk_rows", 5); });
    EXPECT_EQ(chunked.outcome, TestOutcome::Pass) << chunked.error;
}

TEST(Testkit, MlCasesPass) {
    EXPECT_EQ(run("ml.gbdt_train").outcome, TestOutcome::Pass);
    EXPECT_EQ(run("ml.tree_shap").outcome, TestOutcome::Pass);
    EXPECT_EQ(run("ml.calibration").outcome, TestOutcome::Pass);
}

TEST(Testkit, GovernanceCasesPass) {
    EXPECT_EQ(run("governance.access_control").outcome, TestOutcome::Pass);
    EXPECT_EQ(run("governance.audit").outcome, TestOutcome::Pass);
    // Division-correct resolution passes the gate.
    EXPECT_EQ(run("governance.entity_resolution_gate",
                  [](TestOptions& o) { o.set_choice("resolver", 0); })
                  .outcome,
              TestOutcome::Pass);
    // The coarse Brand resolver conflates the siblings; the case passes by
    // asserting the gate BLOCKS it.
    EXPECT_EQ(run("governance.entity_resolution_gate",
                  [](TestOptions& o) { o.set_choice("resolver", 1); })
                  .outcome,
              TestOutcome::Pass);
    // TC-6: burst N nodes -> N distinct identities -> teardown revokes, no orphans.
    const TestResult burst =
        run("governance.burst_identity", [](TestOptions& o) { o.set_int("nodes", 4); });
    EXPECT_EQ(burst.outcome, TestOutcome::Pass) << burst.error;
}

TEST(Testkit, DistributedCasesRunOnARealCluster) {
    EXPECT_EQ(run("cluster.membership", [](TestOptions& o) { o.set_int("nodes", 2); })
                  .outcome,
              TestOutcome::Pass);
    EXPECT_EQ(run("cluster.allreduce", [](TestOptions& o) { o.set_int("nodes", 2); })
                  .outcome,
              TestOutcome::Pass);
    const TestResult group_by = run("cluster.group_by_consistency", [](TestOptions& o) {
        o.set_int("nodes", 2);
        o.set_int("rows", 400);
    });
    EXPECT_EQ(group_by.outcome, TestOutcome::Pass) << group_by.error;
    // TC-11: a DDF op runs over each node's locally-owned partitions; only the
    // partials cross the wire and the result matches the single-node oracle.
    const TestResult near_data = run("cluster.compute_near_data", [](TestOptions& o) {
        o.set_int("nodes", 2);
        o.set_int("rows", 600);
    });
    EXPECT_EQ(near_data.outcome, TestOutcome::Pass) << near_data.error;
    // TC-10: a partitioned table rebalances on a node join and the catalog converges
    // on every node (run with replicas to exercise the rf-preserved path).
    const TestResult rebalance = run("cluster.partition_rebalance", [](TestOptions& o) {
        o.set_int("nodes", 3);
        o.set_int("rf", 2);
        o.set_int("rows", 120);
    });
    EXPECT_EQ(rebalance.outcome, TestOutcome::Pass) << rebalance.error;
    const TestResult featurize = run("cluster.featurize_partitions", [](TestOptions& o) {
        o.set_int("nodes", 2);
        o.set_int("rows", 140);
    });
    EXPECT_EQ(featurize.outcome, TestOutcome::Pass) << featurize.error;
    // TC-13: build a full-text index over each node's owned partitions and assert the
    // scatter-gather ranking matches a consolidated-index oracle on every node (the
    // oramacore-cpp backend when compiled in, else the reference scorer).
    const TestResult fulltext = run("cluster.fulltext_index", [](TestOptions& o) {
        o.set_int("nodes", 2);
        o.set_int("copies", 3);
    });
    EXPECT_EQ(fulltext.outcome, TestOutcome::Pass) << fulltext.error;
    // TC-12: the capstone UC-1-at-scale E2E — one sim-Python program loads the saas
    // parquet as one partitioned table, bursts within its cost_cap= (over-cap
    // blocked), rebalances partitions onto the burst node, trains a calibrated
    // `outcome` model compute-near-data across the grown replicated cluster, then
    // tears the burst down with the model still serving — gated + governed,
    // identical on every node.
    const TestResult uc1 = run("cluster.uc1_at_scale", [](TestOptions& o) {
        o.set_int("nodes", 2);
        o.set_int("burst", 1);
        o.set_int("rows", 96);
    });
    EXPECT_EQ(uc1.outcome, TestOutcome::Pass) << uc1.error;
}

// The source-connector cases (#827, #833). Both stand up a real cluster and run
// against an injected fake transport — no network, no credentials, no warehouse.
TEST(Testkit, ConnectorCasesRunOnARealCluster) {
    // #827: a saved connection reaches every node with its credential, a write
    // statement never reaches the transport, no read surface carries the token,
    // and the cells come back typed.
    const TestResult connections =
        run("cluster.connections", [](TestOptions& o) { o.set_int("nodes", 2); });
    EXPECT_EQ(connections.outcome, TestOutcome::Pass) << connections.error;

    // #833 section 7: the co-launched ingest — fetched exactly once cluster-wide,
    // landed where it was pulled, no row on the replication plane, read back whole.
    const TestResult import = run("cluster.distributed_import", [](TestOptions& o) {
        o.set_int("nodes", 2);
        o.set_int("chunks", 6);
        o.set_int("rows", 4);
    });
    EXPECT_EQ(import.outcome, TestOutcome::Pass) << import.error;

    // #840: the all-or-nothing property, in-app. A node that stays in the roster
    // but never drains the launch must fail the whole ingest and leave nothing
    // behind — and the case must do that without wedging anything, which is why
    // "dead" here is the absence of a tick loop on a node the case owns.
    const TestResult lost = run("cluster.import_participant_loss",
                                [](TestOptions& o) { o.set_int("nodes", 2); });
    EXPECT_EQ(lost.outcome, TestOutcome::Pass) << lost.error;
}

TEST(Testkit, ExampleScriptCasesRunGreen) {
    // Every sim-Python example script registered in the catalog runs end to end
    // (parse -> IR -> gate -> run) and passes — the example programs ARE the tests.
    const TestSuite& suite = TestSuite::builtin();
    const auto examples = suite.in_category(TestCategory::Examples);
    EXPECT_FALSE(examples.empty());
    for (const auto* test_case : examples) {
        TestContext context;
        TestOptions options = test_case->defaults();
        const TestResult result = run_test_case(*test_case, options, context);
        EXPECT_EQ(result.outcome, TestOutcome::Pass)
            << test_case->id << ": " << result.error;
    }
}

TEST(Testkit, Uc1PipelineGatedAndGoverned) {
    // FullName: the gate passes and the whole pipeline runs end to end.
    const TestResult full = run("pipeline.uc1_end_to_end", [](TestOptions& o) {
        o.set_choice("resolver", 0);
    });
    EXPECT_EQ(full.outcome, TestOutcome::Pass) << full.error;

    // Brand: the conflating resolver blocks the gate; the case passes by asserting
    // the run never happens (RULE-1).
    const TestResult blocked = run("pipeline.uc1_end_to_end", [](TestOptions& o) {
        o.set_choice("resolver", 1);
    });
    EXPECT_EQ(blocked.outcome, TestOutcome::Pass) << blocked.error;
}

// --- Traced testkit (#640) ------------------------------------------------------

// Every case runs inside a trace: with a recorder wired on the context,
// run_test_case records one span per case — timing, category, and the result
// shape (outcome / summary / metrics) — the per-case history the iteration
// flywheel reads back through system.spans / get_trace.
TEST(Testkit, EveryCaseRunsInsideATrace) {
    const TestSuite& suite = TestSuite::builtin();
    const auto* test_case = suite.find("compute.cpu.elementwise");
    ASSERT_NE(test_case, nullptr);
    TestOptions options = test_case->defaults();
    options.set_int("count", 64);

    zetta::observability::SpanRecorder recorder;
    TestContext context;
    context.recorder = &recorder;
    const TestResult result = run_test_case(*test_case, options, context);
    ASSERT_EQ(result.outcome, TestOutcome::Pass) << result.error;

    // The runner minted a trace and reported it back on the context.
    ASSERT_TRUE(context.trace.valid());
    const auto spans = recorder.trace(context.trace.trace_id);
    ASSERT_EQ(spans.size(), 1u);
    const auto& span = spans[0];
    EXPECT_EQ(span.name, "testkit:compute.cpu.elementwise");
    EXPECT_TRUE(span.ok);
    EXPECT_GT(span.duration_ms, 0.0);
    EXPECT_EQ(span.attributes.at("result_shape").at("outcome"), "pass");
    EXPECT_FALSE(span.attributes.at("result_shape")
                     .at("summary")
                     .get<std::string>()
                     .empty());

    // A second case with the same context joins the same trace — a suite
    // sweep reads back as ONE trace with a span per case.
    const auto* second = suite.find("compute.cpu.reduce_sort");
    ASSERT_NE(second, nullptr);
    TestOptions second_options = second->defaults();
    run_test_case(*second, second_options, context);
    EXPECT_EQ(recorder.trace(context.trace.trace_id).size(), 2u);
}
