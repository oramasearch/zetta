#include <string>
#include <vector>

#include "cases.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/testkit/example_script.hpp"
#include "zetta/testkit/test_suite.hpp"

// Example-script cases (issue #523): each sim-Python example program is registered
// as a TestCase under TestCategory::Examples and run end to end through the harness
// (parse -> IR -> gate -> run) by run_example_script. The example programs ARE the
// integration tests — a green script doubles as a canonical usage example; a
// negative script proves a failure surfaces with the offending statement. See
// docs/testing/example-scripts.md.
namespace zetta::testkit {
namespace {

using ir::StepKind;

// The full churn-shaped pipeline, exercising every stage of the
// connect -> clean -> model -> index -> serve -> act taxonomy. Lowers to a
// well-formed graph, passes the baseline gate, and runs to Succeeded.
ExampleScript churn_pipeline() {
    ExampleScript script;
    script.id = "examples.churn_pipeline";
    script.title = "Churn pipeline (full taxonomy)";
    script.description =
        "A churn-shaped sim-Python pipeline spanning every stage "
        "(connect -> clean -> model -> index -> serve -> act): load two sources, "
        "featurize, train, build an index, serve, and notify. Compiles, passes the "
        "gate, and runs to Succeeded — the canonical end-to-end example.";
    script.source =
        "accounts = load_dataset('saas-churn-accounts')\n"
        "events = load_dataset('saas-churn-events')\n"
        "features = featurize(accounts, events)\n"
        "model = train(features, target='churned')\n"
        "risk_index = build_index(model)\n"
        "api = serve(risk_index)\n"
        "notify(api)\n";
    script.expect.node_count = 7;
    script.expect.edge_count = 6;
    script.expect.steps = {
        {"load_dataset", StepKind::Connect}, {"load_dataset_2", StepKind::Connect},
        {"featurize", StepKind::Clean},      {"train", StepKind::Model},
        {"build_index", StepKind::Index},    {"serve", StepKind::Serve},
        {"notify", StepKind::Act},
    };
    script.expect.edges = {
        {"load_dataset", "featurize"}, {"load_dataset_2", "featurize"},
        {"featurize", "train"},        {"train", "build_index"},
        {"build_index", "serve"},      {"serve", "notify"},
    };
    script.expect.output_kinds = {"dataset", "model", "index"};
    return script;
}

// The minimal load -> train sample (the #520 frontend acceptance script): two
// statements, one data-flow edge. The smallest script that "runs green in the
// suite".
ExampleScript minimal_train() {
    ExampleScript script;
    script.id = "examples.minimal_train";
    script.title = "Minimal load + train";
    script.description =
        "The smallest green example: load a dataset and train on it. Two steps, one "
        "data-flow edge; compiles, gates, and runs to Succeeded.";
    script.source =
        "ds = load_dataset('saas-churn')\n"
        "model = train(ds, target='outcome')\n";
    script.expect.node_count = 2;
    script.expect.edge_count = 1;
    script.expect.steps = {{"load_dataset", StepKind::Connect},
                           {"train", StepKind::Model}};
    script.expect.edges = {{"load_dataset", "train"}};
    script.expect.output_kinds = {"dataset", "model"};
    return script;
}

// Method chaining: a chained transform lowers to a linear data-flow path, each
// method its own step.
ExampleScript method_chain() {
    ExampleScript script;
    script.id = "examples.method_chain";
    script.title = "Method chaining";
    script.description =
        "A chained transform — read_csv(...).dropna().fillna(0) — lowers to a linear "
        "data-flow path (each method a step), then fit. Compiles, gates, and runs.";
    script.source =
        "df = read_csv('accounts.csv').dropna().fillna(0)\n"
        "model = fit(df)\n";
    script.expect.node_count = 4;
    script.expect.edge_count = 3;
    script.expect.steps = {{"read_csv", StepKind::Connect},
                           {"dropna", StepKind::Clean},
                           {"fillna", StepKind::Clean},
                           {"fit", StepKind::Model}};
    script.expect.edges = {
        {"read_csv", "dropna"}, {"dropna", "fillna"}, {"fillna", "fit"}};
    script.expect.output_kinds = {"dataset", "model"};
    return script;
}

// Negative example: an unterminated string literal must fail at compile, and the
// harness must surface the parser's offending-statement locator.
ExampleScript unterminated_string() {
    ExampleScript script;
    script.id = "examples.unterminated_string";
    script.title = "Syntax error: unterminated string";
    script.description =
        "A negative example: an unterminated string literal fails at the compile "
        "phase. The harness passes by proving the failure surfaces with the parser's "
        "offending-statement locator.";
    script.source = "ds = load('unterminated\n";
    script.expect.compiles = false;
    script.expect.failure_contains = "unterminated string";
    return script;
}

// Negative example: a script with no pipeline steps (only a literal assignment)
// must be rejected — there is nothing to lower / run.
ExampleScript no_pipeline_steps() {
    ExampleScript script;
    script.id = "examples.no_pipeline_steps";
    script.title = "Rejected: no pipeline steps";
    script.description =
        "A negative example: a script with no calls has nothing to lower, so it is "
        "rejected at compile. The harness passes by proving the failure surfaces with "
        "the reason.";
    script.source = "threshold = 0.5\n";
    script.expect.compiles = false;
    script.expect.failure_contains = "no pipeline steps";
    return script;
}

// The UC-1-at-scale program (TC-12 / #531): add permanent nodes, load the
// saas-sales-conversations parquet, train on `outcome`, explain, and rank. Shares
// its source with the real cluster.uc1_at_scale case (one canonical program); here
// the standard example harness compiles + gates it and runs it single-node, so this
// asserts the lowered shape + that it runs to Succeeded. The at-scale, real-cluster,
// near-data, governed run is the cluster.uc1_at_scale case. See docs/use-cases/uc1-at-scale-e2e.md.
ExampleScript uc1_at_scale() {
    ExampleScript script;
    script.id = "examples.uc1_at_scale";
    script.title = "UC-1 at scale (add nodes, load, train, explain, rank)";
    script.description =
        "The capstone UC-1-at-scale program: add permanent nodes, load the "
        "saas-sales-conversations parquet, train on `outcome`, explain (drivers), and "
        "rank (the at-risk list). Lowers across the connect -> model -> index -> serve "
        "taxonomy plus an act control step, passes the gate, and runs to Succeeded. The "
        "real distributed near-data run over a cluster is the cluster.uc1_at_scale case.";
    script.source = uc1_at_scale_script();
    script.expect.node_count = 5;
    script.expect.edge_count = 3;
    script.expect.steps = {{"add_nodes", StepKind::Act},
                           {"load_dataset", StepKind::Connect},
                           {"train", StepKind::Model},
                           {"explain", StepKind::Serve},
                           {"rank", StepKind::Index}};
    script.expect.edges = {
        {"load_dataset", "train"}, {"train", "explain"}, {"train", "rank"}};
    script.expect.output_kinds = {"dataset", "model", "index"};
    return script;
}

}  // namespace

const std::vector<ExampleScript>& example_scripts() {
    static const std::vector<ExampleScript> scripts = {
        churn_pipeline(),  minimal_train(),     method_chain(),
        uc1_at_scale(),
        unterminated_string(), no_pipeline_steps(),
    };
    return scripts;
}

void register_example_cases(TestSuite& suite) {
    for (const ExampleScript& script : example_scripts()) {
        suite.add(TestCase{
            .id = script.id,
            .category = TestCategory::Examples,
            .name = script.title,
            .description = script.description,
            .options = {},
            .run = [script](const TestOptions&, TestContext& context) -> TestResult {
                context.emit("running sim-Python example: " + script.id);
                const ExampleRunReport report = run_example_script(script);

                TestResult result = report.ok ? TestResult::pass(report.summary)
                                              : TestResult::fail(report.reason);
                for (const std::string& line : report.log) result.note(line);
                for (const TestMetric& metric : report.metrics) {
                    result.metric(metric.label, metric.value);
                }
                result.metric("reached", std::string(to_string(report.reached)));
                if (!report.offending_statement.empty()) {
                    // The offending statement is the headline of a failure — surface
                    // it as a metric always, and fold it into the error on a fail so
                    // the test failure names the statement at fault.
                    result.metric("offending statement", report.offending_statement);
                    if (!report.ok) {
                        result.error += "  [at: " + report.offending_statement + "]";
                    }
                }
                return result;
            },
        });
    }
}

}  // namespace zetta::testkit
