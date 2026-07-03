#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>

#include "temp_dir.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/pipeline/n8n_compile.hpp"
#include "zetta/pipeline/n8n_import.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_input.hpp"

namespace fs = std::filesystem;
using zetta::pipeline::compile_n8n_workflow;
using zetta::pipeline::import_n8n_workflow_file;
using zetta::pipeline::n8n_step_kind;
using zetta::Node;
using zetta::NodeConfig;
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

std::string read_raw(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

// The same realistic churn fixture the importer test uses: a schedule →
// Postgres → code → HTTP-score → Slack pipeline with the usual UI-export
// metadata (id/versionId/meta/tags/pinData).
fs::path churn_fixture() {
    return fs::path(ZETTA_TEST_FIXTURES_DIR) / "churn_workflow.json";
}

// Does the graph have an edge `from` → `to` of the given kind?
bool has_edge(const ExecutionGraph& graph, const std::string& from,
              const std::string& to, EdgeKind kind = EdgeKind::DataFlow) {
    for (const auto& edge : graph.edges()) {
        if (edge.from == from && edge.to == to && edge.kind == kind) return true;
    }
    return false;
}

}  // namespace

// Acceptance: the churn fixture compiles into a well-formed ExecutionGraph — the
// node kinds map to the connect→clean→model→…→act spine and the connections
// become the right data-flow edges.
TEST(N8nCompile, CompilesChurnFixtureToWellFormedGraph) {
    std::string error;
    const std::optional<ExecutionGraph> graph =
        compile_n8n_workflow(read_raw(churn_fixture()), &error);
    ASSERT_TRUE(graph.has_value()) << error;

    // Five nodes (one per n8n node), keyed by the n8n node name.
    EXPECT_EQ(graph->node_count(), 5u);
    ASSERT_TRUE(graph->find_node("Daily schedule"));
    ASSERT_TRUE(graph->find_node("Read active accounts"));
    ASSERT_TRUE(graph->find_node("Compute churn features"));
    ASSERT_TRUE(graph->find_node("Score churn risk"));
    ASSERT_TRUE(graph->find_node("Alert on high risk"));

    // Node-type → StepKind mapping for the churn use case.
    EXPECT_EQ(graph->find_node("Daily schedule")->kind, StepKind::Connect);
    EXPECT_EQ(graph->find_node("Read active accounts")->kind, StepKind::Connect);
    EXPECT_EQ(graph->find_node("Compute churn features")->kind, StepKind::Clean);
    EXPECT_EQ(graph->find_node("Score churn risk")->kind, StepKind::Model);
    EXPECT_EQ(graph->find_node("Alert on high risk")->kind, StepKind::Act);
    EXPECT_EQ(graph->nodes_of_kind(StepKind::Connect).size(), 2u);
    EXPECT_EQ(graph->nodes_of_kind(StepKind::Clean).size(), 1u);
    EXPECT_EQ(graph->nodes_of_kind(StepKind::Model).size(), 1u);
    EXPECT_EQ(graph->nodes_of_kind(StepKind::Act).size(), 1u);

    // Connections → the four data-flow edges of the linear pipeline.
    EXPECT_EQ(graph->edge_count(), 4u);
    EXPECT_TRUE(has_edge(*graph, "Daily schedule", "Read active accounts"));
    EXPECT_TRUE(has_edge(*graph, "Read active accounts", "Compute churn features"));
    EXPECT_TRUE(has_edge(*graph, "Compute churn features", "Score churn risk"));
    EXPECT_TRUE(has_edge(*graph, "Score churn risk", "Alert on high risk"));

    // Provenance: each step records its originating n8n node type.
    EXPECT_EQ(graph->find_node("Score churn risk")->attributes["n8n"]["type"],
              "n8n-nodes-base.httpRequest");

    // "Well-formed" = it survives the IR's own JSON trust boundary unchanged.
    std::string round_trip_error;
    const std::optional<ExecutionGraph> reparsed =
        zetta::ir::from_json(zetta::ir::to_json(*graph), &round_trip_error);
    ASSERT_TRUE(reparsed.has_value()) << round_trip_error;
    EXPECT_EQ(reparsed->node_count(), 5u);
    EXPECT_EQ(reparsed->edge_count(), 4u);
}

TEST(N8nCompile, MapsKnownNodeTypesAndMissesUnknown) {
    EXPECT_EQ(n8n_step_kind("n8n-nodes-base.scheduleTrigger"), StepKind::Connect);
    EXPECT_EQ(n8n_step_kind("n8n-nodes-base.postgres"), StepKind::Connect);
    EXPECT_EQ(n8n_step_kind("n8n-nodes-base.code"), StepKind::Clean);
    EXPECT_EQ(n8n_step_kind("n8n-nodes-base.httpRequest"), StepKind::Model);
    EXPECT_EQ(n8n_step_kind("n8n-nodes-base.slack"), StepKind::Act);
    EXPECT_FALSE(n8n_step_kind("n8n-nodes-base.totallyMadeUp").has_value());
}

// The compiled graph can be produced straight from the stored artifact input
// (import → compile), end to end.
TEST(N8nCompile, CompilesFromImportedArtifactInput) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);

    auto imported = import_n8n_workflow_file(*project, churn_fixture());
    ASSERT_TRUE(imported.ok()) << imported.error;

    std::string error;
    const std::optional<ExecutionGraph> graph =
        compile_n8n_workflow(*imported.input, &error);
    ASSERT_TRUE(graph.has_value()) << error;
    EXPECT_EQ(graph->node_count(), 5u);
    EXPECT_EQ(graph->edge_count(), 4u);
    node.stop();
}

TEST(N8nCompile, RejectsInvalidJson) {
    std::string error;
    EXPECT_FALSE(compile_n8n_workflow("{not json", &error).has_value());
    EXPECT_NE(error.find("invalid JSON"), std::string::npos) << error;
}

TEST(N8nCompile, RejectsValidJsonThatIsNotAWorkflow) {
    std::string error;
    EXPECT_FALSE(compile_n8n_workflow(R"({"foo": 1})", &error).has_value());
    EXPECT_NE(error.find("not an n8n workflow"), std::string::npos) << error;
}

TEST(N8nCompile, RejectsUnsupportedNodeType) {
    constexpr const char* workflow = R"({
      "nodes": [
        {"name": "Mystery", "type": "n8n-nodes-base.someFutureNode"}
      ],
      "connections": {}
    })";
    std::string error;
    EXPECT_FALSE(compile_n8n_workflow(workflow, &error).has_value());
    EXPECT_NE(error.find("unsupported n8n node type"), std::string::npos) << error;
    EXPECT_NE(error.find("n8n-nodes-base.someFutureNode"), std::string::npos)
        << error;
}

TEST(N8nCompile, RejectsConnectionToUnknownNode) {
    constexpr const char* workflow = R"({
      "nodes": [
        {"name": "Source", "type": "n8n-nodes-base.postgres"}
      ],
      "connections": {
        "Source": { "main": [ [ {"node": "Ghost", "type": "main", "index": 0} ] ] }
      }
    })";
    std::string error;
    EXPECT_FALSE(compile_n8n_workflow(workflow, &error).has_value());
    EXPECT_NE(error.find("unknown node"), std::string::npos) << error;
    EXPECT_NE(error.find("Ghost"), std::string::npos) << error;
}

TEST(N8nCompile, RejectsDuplicateNodeName) {
    constexpr const char* workflow = R"({
      "nodes": [
        {"name": "Dup", "type": "n8n-nodes-base.postgres"},
        {"name": "Dup", "type": "n8n-nodes-base.code"}
      ],
      "connections": {}
    })";
    std::string error;
    EXPECT_FALSE(compile_n8n_workflow(workflow, &error).has_value());
    EXPECT_NE(error.find("duplicate node name"), std::string::npos) << error;
}
