#include <gtest/gtest.h>

#include <string>

#include "zetta/ir/step_kind.hpp"
#include "zetta/testkit/example_script.hpp"
#include "zetta/testkit/test_suite.hpp"

// The sim-Python example-script harness (issue #523): the example programs ARE the
// integration tests. Each runs end to end — parse (PythonFrontend) -> IR -> gate
// (default_gate) -> run (Node::run_graph) — and asserts on its outputs; a failure
// surfaces with the offending statement. See docs/example-scripts.md.
namespace {

using zetta::ir::StepKind;
using zetta::testkit::ExampleExpectations;
using zetta::testkit::ExamplePhase;
using zetta::testkit::ExampleRunReport;
using zetta::testkit::ExampleScript;
using zetta::testkit::example_scripts;
using zetta::testkit::run_example_script;

}  // namespace

// Acceptance: the canonical sample example script runs green end to end.
TEST(ExampleScripts, MinimalSampleRunsGreen) {
    ExampleScript script;
    script.id = "test.minimal";
    script.source =
        "ds = load_dataset('saas-churn')\n"
        "model = train(ds, target='outcome')\n";
    script.expect.node_count = 2;
    script.expect.edge_count = 1;
    script.expect.steps = {{"load_dataset", StepKind::Connect},
                           {"train", StepKind::Model}};
    script.expect.edges = {{"load_dataset", "train"}};
    script.expect.output_kinds = {"dataset", "model"};

    const ExampleRunReport report = run_example_script(script);
    EXPECT_TRUE(report.ok) << report.reason;
    EXPECT_EQ(report.reached, ExamplePhase::Done);
    EXPECT_TRUE(report.offending_statement.empty());
}

// The full churn-shaped pipeline spans every stage and runs green.
TEST(ExampleScripts, ChurnPipelineRunsGreen) {
    ExampleScript script;
    script.id = "test.churn";
    script.source =
        "accounts = load_dataset('a')\n"
        "events = load_dataset('e')\n"
        "features = featurize(accounts, events)\n"
        "model = train(features, target='churned')\n"
        "idx = build_index(model)\n"
        "api = serve(idx)\n"
        "notify(api)\n";
    script.expect.output_kinds = {"dataset", "model", "index"};

    const ExampleRunReport report = run_example_script(script);
    EXPECT_TRUE(report.ok) << report.reason;
    EXPECT_EQ(report.reached, ExamplePhase::Done);
}

// A failing script surfaces as a failure naming the offending statement — at the
// compile phase, the parser's own locator.
TEST(ExampleScripts, CompileFailureSurfacesOffendingStatement) {
    ExampleScript script;
    script.id = "test.broken";
    // Expected to run green (default), but the source has a syntax error.
    script.source =
        "ds = load('saas')\n"
        "model = train(ds\n";  // unclosed call
    const ExampleRunReport report = run_example_script(script);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.reached, ExamplePhase::Compile);
    EXPECT_FALSE(report.offending_statement.empty());
    EXPECT_NE(report.reason.find("compile failed"), std::string::npos)
        << report.reason;
}

// An unexpected structural shape names the offending statement, reconstructed from
// the failing step's provenance (verb + assignment + args/kwargs).
TEST(ExampleScripts, StructuralMismatchNamesTheStatement) {
    ExampleScript script;
    script.id = "test.mismatch";
    script.source = "m = train(ds, target='outcome')\n";
    // Wrong on purpose: `train` lowers to Model, not Connect.
    script.expect.steps = {{"train", StepKind::Connect}};

    const ExampleRunReport report = run_example_script(script);
    EXPECT_FALSE(report.ok);
    EXPECT_EQ(report.reached, ExamplePhase::Compile);
    // The offending statement is the reconstructed `m = train(ds, target='outcome')`.
    EXPECT_NE(report.offending_statement.find("train"), std::string::npos)
        << report.offending_statement;
    EXPECT_NE(report.offending_statement.find("target"), std::string::npos)
        << report.offending_statement;
    EXPECT_NE(report.offending_statement.find("m ="), std::string::npos)
        << report.offending_statement;
}

// A negative example (expected compile failure) "passes" when it fails exactly as
// declared, with the offending statement surfaced.
TEST(ExampleScripts, NegativeExamplePassesWhenItFailsAsExpected) {
    ExampleScript script;
    script.id = "test.negative";
    script.source = "ds = load('unterminated\n";
    script.expect.compiles = false;
    script.expect.failure_contains = "unterminated string";

    const ExampleRunReport report = run_example_script(script);
    EXPECT_TRUE(report.ok) << report.reason;
    EXPECT_EQ(report.reached, ExamplePhase::Compile);
    EXPECT_FALSE(report.offending_statement.empty());
}

// A negative example whose actual failure reason does NOT match is itself a failure.
TEST(ExampleScripts, NegativeExampleFailsOnWrongReason) {
    ExampleScript script;
    script.id = "test.negative.wrong";
    script.source = "ds = load('unterminated\n";
    script.expect.compiles = false;
    script.expect.failure_contains = "no pipeline steps";  // wrong reason

    const ExampleRunReport report = run_example_script(script);
    EXPECT_FALSE(report.ok);
}

// The built-in catalog: every example runs (positives green, negatives fail as
// expected) — so the example programs ARE the integration tests.
TEST(ExampleScripts, EveryBuiltinExampleRuns) {
    EXPECT_FALSE(example_scripts().empty());
    for (const ExampleScript& script : example_scripts()) {
        const ExampleRunReport report = run_example_script(script);
        EXPECT_TRUE(report.ok) << script.id << ": " << report.reason;
    }
}

// The examples are registered as TestCases in the testkit catalog and run there.
TEST(ExampleScripts, RegisteredInTheTestkitCatalog) {
    using zetta::testkit::TestCategory;
    using zetta::testkit::TestContext;
    using zetta::testkit::TestOptions;
    using zetta::testkit::TestOutcome;
    using zetta::testkit::TestSuite;
    using zetta::testkit::run_test_case;

    const TestSuite& suite = TestSuite::builtin();
    EXPECT_FALSE(suite.in_category(TestCategory::Examples).empty());

    const auto* sample = suite.find("examples.minimal_train");
    ASSERT_NE(sample, nullptr);
    EXPECT_EQ(sample->category, TestCategory::Examples);

    TestOptions options = sample->defaults();
    TestContext context;
    const auto result = run_test_case(*sample, options, context);
    EXPECT_EQ(result.outcome, TestOutcome::Pass) << result.error;
}
