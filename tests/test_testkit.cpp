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
