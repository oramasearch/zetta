#include <gtest/gtest.h>

#include <optional>
#include <string>

#include "temp_dir.hpp"
#include "zetta/executor.hpp"
#include "zetta/gate.hpp"
#include "zetta/input/python_frontend.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::input::FrontendResult;
using zetta::input::python_step_kind;
using zetta::input::PythonFrontend;
using zetta::ir::EdgeKind;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Is there an edge `from` → `to` of the given kind?
bool has_edge(const ExecutionGraph& graph, const std::string& from,
              const std::string& to, EdgeKind kind = EdgeKind::DataFlow) {
    for (const auto& edge : graph.edges()) {
        if (edge.from == from && edge.to == to && edge.kind == kind) return true;
    }
    return false;
}

FrontendResult compile_python(const std::string& source) {
    return PythonFrontend{}.compile(source);
}

}  // namespace

// Acceptance (issue #520): a sample script parses to a valid IR graph.
TEST(PythonFrontend, CompilesAcceptanceScriptToValidGraph) {
    const FrontendResult result = compile_python(
        "ds = load_dataset('saas-churn')\n"
        "m = train(ds, target='outcome')\n");
    ASSERT_TRUE(result.ok()) << result.error;

    // One step per call, keyed by the verb.
    EXPECT_EQ(result.graph.node_count(), 2u);
    ASSERT_TRUE(result.graph.find_node("load_dataset"));
    ASSERT_TRUE(result.graph.find_node("train"));
    EXPECT_EQ(result.graph.find_node("load_dataset")->kind, StepKind::Connect);
    EXPECT_EQ(result.graph.find_node("train")->kind, StepKind::Model);

    // `m = train(ds, …)` references `ds` → a data-flow edge load_dataset → train.
    EXPECT_EQ(result.graph.edge_count(), 1u);
    EXPECT_TRUE(has_edge(result.graph, "load_dataset", "train"));

    // "Valid IR graph" = it survives the IR's own JSON trust boundary unchanged…
    std::string error;
    const std::optional<ExecutionGraph> reparsed =
        zetta::ir::from_json(zetta::ir::to_json(result.graph), &error);
    ASSERT_TRUE(reparsed.has_value()) << error;
    EXPECT_EQ(reparsed->node_count(), 2u);
    EXPECT_EQ(reparsed->edge_count(), 1u);

    // …and passes the baseline correctness gate.
    EXPECT_TRUE(zetta::default_gate().run(result.graph).passed());
}

// The lowered graph executes through Node::run_graph like any other frontend's.
TEST(PythonFrontend, ExecutesThroughNodeRunGraph) {
    const FrontendResult result = compile_python(
        "ds = load_dataset('saas-churn')\n"
        "m = train(ds, target='outcome')\n");
    ASSERT_TRUE(result.ok()) << result.error;

    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    const auto execution = node.run_graph(result.graph);
    EXPECT_TRUE(execution.ok) << execution.error;
    node.stop();
}

// Attribute / method chaining: each method call is a step; the receiver feeds it,
// so a chain lowers to a linear data-flow path.
TEST(PythonFrontend, MethodChainingProducesLinearEdges) {
    const FrontendResult result = compile_python(
        "df = read_csv('accounts.csv').dropna().fillna(0)\n"
        "m = fit(df)\n");
    ASSERT_TRUE(result.ok()) << result.error;

    EXPECT_EQ(result.graph.node_count(), 4u);
    EXPECT_EQ(result.graph.find_node("read_csv")->kind, StepKind::Connect);
    EXPECT_EQ(result.graph.find_node("dropna")->kind, StepKind::Clean);
    EXPECT_EQ(result.graph.find_node("fillna")->kind, StepKind::Clean);
    EXPECT_EQ(result.graph.find_node("fit")->kind, StepKind::Model);

    EXPECT_TRUE(has_edge(result.graph, "read_csv", "dropna"));
    EXPECT_TRUE(has_edge(result.graph, "dropna", "fillna"));
    EXPECT_TRUE(has_edge(result.graph, "fillna", "fit"));
    EXPECT_EQ(result.graph.edge_count(), 3u);
}

// A list argument passes each element's producer through as a data-flow input,
// and a repeated verb gets a deterministic, unique id (`load`, `load_2`).
TEST(PythonFrontend, ListArgumentEdgesAndRepeatedVerbIds) {
    const FrontendResult result = compile_python(
        "a = load('customers')\n"
        "b = load('events')\n"
        "c = merge([a, b])\n");
    ASSERT_TRUE(result.ok()) << result.error;

    ASSERT_TRUE(result.graph.find_node("load"));
    ASSERT_TRUE(result.graph.find_node("load_2"));
    ASSERT_TRUE(result.graph.find_node("merge"));
    EXPECT_EQ(result.graph.find_node("merge")->kind, StepKind::Clean);
    EXPECT_TRUE(has_edge(result.graph, "load", "merge"));
    EXPECT_TRUE(has_edge(result.graph, "load_2", "merge"));
    EXPECT_EQ(result.graph.edge_count(), 2u);
}

// The verb table maps known pipeline verbs and honestly misses unknown ones.
TEST(PythonFrontend, MapsKnownVerbsAndMissesUnknown) {
    EXPECT_EQ(python_step_kind("load_dataset"), StepKind::Connect);
    EXPECT_EQ(python_step_kind("read_csv"), StepKind::Connect);
    EXPECT_EQ(python_step_kind("dropna"), StepKind::Clean);
    EXPECT_EQ(python_step_kind("train"), StepKind::Model);
    EXPECT_EQ(python_step_kind("embed"), StepKind::Index);
    EXPECT_EQ(python_step_kind("serve"), StepKind::Serve);
    EXPECT_EQ(python_step_kind("notify"), StepKind::Act);
    EXPECT_FALSE(python_step_kind("totally_made_up").has_value());
}

// The cluster + ML bindings (#522) lower to the stage whose engine op they name:
// `train` trains a model (model), inference ranks entities (index), `explain` is
// TreeSHAP (serve), and `add_nodes` is a cluster control action (act).
TEST(PythonFrontend, BindsClusterAndMlVerbsToTheRightStages) {
    EXPECT_EQ(python_step_kind("train"), StepKind::Model);    // native GBDT
    EXPECT_EQ(python_step_kind("rank"), StepKind::Index);     // batch scoring / rank
    EXPECT_EQ(python_step_kind("score"), StepKind::Index);    // inference, not training
    EXPECT_EQ(python_step_kind("predict"), StepKind::Index);  // inference, not training
    EXPECT_EQ(python_step_kind("explain"), StepKind::Serve);  // TreeSHAP
    EXPECT_EQ(python_step_kind("add_nodes"), StepKind::Act);  // permanent-add control
}

// `cluster.add_nodes(n, provider=)` lowers to a single `act`-stage step keyed by
// the method name — the receiver `cluster` (an unbound name) creates no spurious
// node — carrying the count + provider as `python` provenance.
TEST(PythonFrontend, ClusterAddNodesLowersToASingleActStep) {
    const FrontendResult result =
        compile_python("cluster.add_nodes(3, provider='azure')\n");
    ASSERT_TRUE(result.ok()) << result.error;

    EXPECT_EQ(result.graph.node_count(), 1u);
    EXPECT_EQ(result.graph.edge_count(), 0u);
    const auto add = result.graph.find_node("add_nodes");
    ASSERT_TRUE(add);
    EXPECT_EQ(add->kind, StepKind::Act);
    const auto& python = add->attributes["python"];
    EXPECT_EQ(python["call"], "add_nodes");
    EXPECT_EQ(python["args"][0], 3);
    EXPECT_EQ(python["kwargs"]["provider"], "azure");
}

// An unmapped verb still becomes a step, defaulting to Clean — so an arbitrary
// script lowers to a well-formed graph rather than failing.
TEST(PythonFrontend, UnknownVerbDefaultsToClean) {
    const FrontendResult result = compile_python(
        "ds = load('d')\n"
        "x = some_helper(ds)\n");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_TRUE(result.graph.find_node("some_helper"));
    EXPECT_EQ(result.graph.find_node("some_helper")->kind, StepKind::Clean);
    EXPECT_TRUE(has_edge(result.graph, "load", "some_helper"));
}

// Parsed call arguments are recorded under a namespaced `python` provenance bag.
TEST(PythonFrontend, CapturesCallArgumentsAsProvenance) {
    const FrontendResult result = compile_python(
        "m = train(ds, target='outcome', max_depth=6, shuffle=True)\n");
    ASSERT_TRUE(result.ok()) << result.error;
    const auto train = result.graph.find_node("train");
    ASSERT_TRUE(train);
    const auto& python = train->attributes["python"];
    EXPECT_EQ(python["call"], "train");
    EXPECT_EQ(python["assigned_to"], "m");
    EXPECT_EQ(python["kwargs"]["target"], "outcome");
    EXPECT_EQ(python["kwargs"]["max_depth"], 6);
    EXPECT_EQ(python["kwargs"]["shuffle"], true);
    // `ds` is an unbound name here, captured as a reference marker.
    ASSERT_TRUE(python.contains("args"));
    EXPECT_EQ(python["args"][0]["ref"], "ds");
}

// Every literal form parses: string (incl. triple/f-prefix), int, float, bool,
// None, list, dict.
TEST(PythonFrontend, ParsesEveryLiteralForm) {
    const FrontendResult result = compile_python(
        "x = configure(name='a', count=3, ratio=0.5, on=True, off=False, "
        "missing=None, tags=['x', 'y'], opts={'k': 1}, doc=\"\"\"hi\"\"\", "
        "label=f'churn')\n");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_TRUE(result.graph.find_node("configure"));
    const auto& kwargs =
        result.graph.find_node("configure")->attributes["python"]["kwargs"];
    EXPECT_EQ(kwargs["count"], 3);
    EXPECT_EQ(kwargs["ratio"], 0.5);
    EXPECT_EQ(kwargs["on"], true);
    EXPECT_TRUE(kwargs["missing"].is_null());
    EXPECT_EQ(kwargs["doc"], "hi");
    EXPECT_EQ(kwargs["label"], "churn");
}

// Statements may be separated by newlines or ';'; comments and imports are
// dropped (no step produced for them).
TEST(PythonFrontend, NewlinesSemicolonsCommentsAndImports) {
    const FrontendResult result = compile_python(
        "from datasets import load_dataset  # a comment\n"
        "import numpy as np\n"
        "\n"
        "ds = load_dataset('d'); m = train(ds)  # inline comment\n");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.graph.node_count(), 2u);
    EXPECT_TRUE(has_edge(result.graph, "load_dataset", "train"));
}

// A multi-line call (implicit line joining inside parentheses) parses.
TEST(PythonFrontend, ImplicitLineJoiningInsideParentheses) {
    const FrontendResult result = compile_python(
        "m = train(\n"
        "    ds,\n"
        "    target='outcome',\n"
        ")\n");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_TRUE(result.graph.find_node("train"));
    EXPECT_EQ(result.graph.find_node("train")->kind, StepKind::Model);
}

// A script with no calls has nothing to lower — rejected with a clear reason.
TEST(PythonFrontend, RejectsScriptWithNoCalls) {
    FrontendResult result = compile_python("x = 5\n");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("no pipeline steps"), std::string::npos)
        << result.error;

    result = compile_python("   \n\n");  // empty / whitespace
    EXPECT_FALSE(result.ok());
}

// Unsupported Python constructs are rejected explicitly, not silently mis-parsed.
TEST(PythonFrontend, RejectsUnsupportedConstructs) {
    FrontendResult result = compile_python("for x in items:\n    train(x)\n");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("unsupported Python construct"),
              std::string::npos)
        << result.error;

    result = compile_python("def f():\n    pass\n");
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("unsupported"), std::string::npos) << result.error;
}

// Syntax errors are caught at the boundary (non-throwing) with a reason.
TEST(PythonFrontend, RejectsSyntaxErrors) {
    FrontendResult result = compile_python("m = train(ds\n");  // unclosed call
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());

    result = compile_python("ds = load('unterminated\n");  // unterminated string
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("unterminated string"), std::string::npos)
        << result.error;

    result = compile_python("ds = load(1.2.3)\n");  // malformed number literal
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("invalid number"), std::string::npos)
        << result.error;
}
