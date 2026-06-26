#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/project.hpp"
#include "zetta/project_graph.hpp"
#include "zetta/replication.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ReplicationManager;
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

// A small but representative graph: a few stages, an attribute bag, and both
// edge kinds — enough that a faithful round-trip is meaningful.
ExecutionGraph sample_graph() {
    ExecutionGraph graph;
    auto load = graph.add_node(StepKind::Connect, "load_orders",
                               {{"source", "postgres"}, {"table", "orders"}});
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model",
                                {{"algo", "xgboost"}});
    graph.add_edge(load->id, clean->id, EdgeKind::DataFlow);
    graph.add_edge(clean->id, model->id, EdgeKind::DataFlow);
    graph.add_edge(load->id, model->id, EdgeKind::Control);
    return graph;
}

// Tick both nodes until `done` holds (or we give up). Mirrors test_project.cpp:
// capture -> cluster gossip -> peer merge -> reconcile.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    for (int attempt = 0; attempt < 200; ++attempt) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

TEST(ProjectGraph, CreateFindDropUnderAProject) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto project = node.create_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->graph_count(), 0u);

    auto stored = project->create_graph("pipeline", sample_graph());
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->name(), "pipeline");
    EXPECT_EQ(stored->project_id(), project->id());
    EXPECT_FALSE(stored->id().empty());
    EXPECT_EQ(stored->graph().node_count(), 3u);
    EXPECT_EQ(stored->graph().edge_count(), 3u);

    EXPECT_FALSE(project->create_graph("pipeline", sample_graph()));  // duplicate
    EXPECT_FALSE(project->create_graph("", sample_graph()));          // empty name

    EXPECT_EQ(project->graph_count(), 1u);
    EXPECT_TRUE(project->find_graph("pipeline"));
    EXPECT_EQ(project->find_graph_by_id(stored->id()), stored);
    EXPECT_FALSE(project->find_graph("nope"));

    EXPECT_TRUE(project->drop_graph("pipeline"));
    EXPECT_EQ(project->graph_count(), 0u);
    EXPECT_FALSE(project->drop_graph("pipeline"));  // already gone

    node.stop();
}

TEST(ProjectGraph, UpdateGraphGuardsNameAndContents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto project = node.create_project("churn");
    ASSERT_TRUE(project);
    auto stored = project->create_graph("pipeline", sample_graph());
    ASSERT_TRUE(stored);
    const std::string graph_id = stored->id();
    project->create_graph("other", ExecutionGraph{});  // a second graph

    // Updating an unknown graph fails.
    EXPECT_FALSE(project->update_graph("nope", sample_graph()));

    // Edit the contents: drop the model node (and its edges), add an index node.
    ExecutionGraph edited = stored->graph();
    auto model = edited.nodes_of_kind(StepKind::Model).front();
    ASSERT_TRUE(edited.remove_node(model->id));
    edited.add_node(StepKind::Index, "vector_index");
    auto updated = project->update_graph("pipeline", edited);
    ASSERT_TRUE(updated);
    EXPECT_EQ(updated->id(), graph_id);  // same identity, new contents
    EXPECT_EQ(updated->name(), "pipeline");
    EXPECT_EQ(updated->graph().node_count(), 3u);  // -model +index
    EXPECT_TRUE(updated->graph().nodes_of_kind(StepKind::Model).empty());
    EXPECT_EQ(updated->graph().nodes_of_kind(StepKind::Index).size(), 1u);

    // Renaming onto an existing graph's name is rejected.
    EXPECT_FALSE(project->update_graph("pipeline", sample_graph(), "other"));
    EXPECT_TRUE(project->find_graph("pipeline"));

    // A real rename keeps the id and moves the handle to the new name.
    auto renamed = project->update_graph("pipeline", sample_graph(), "flow");
    ASSERT_TRUE(renamed);
    EXPECT_EQ(renamed->id(), graph_id);
    EXPECT_FALSE(project->find_graph("pipeline"));
    EXPECT_TRUE(project->find_graph("flow"));

    node.stop();
}

TEST(ProjectGraph, UpdateGraphReplicatesAndSurvivesRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::new_uuid();
    nlohmann::json edited_json;
    std::string graph_id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto project = node.create_project("churn");
        ASSERT_TRUE(project);
        auto stored = project->create_graph("pipeline", sample_graph());
        ASSERT_TRUE(stored);
        graph_id = stored->id();

        // Edit + persist back to the IR (disk + CRDT).
        ExecutionGraph edited = stored->graph();
        edited.add_node(StepKind::Serve, "predictions_api");
        ASSERT_TRUE(project->update_graph("pipeline", edited));
        edited_json = zetta::ir::to_json(project->find_graph("pipeline")->graph());

        // The edit is captured into the CRDT under the same graph: key.
        const nlohmann::json snapshot = replication.snapshot();
        ASSERT_TRUE(snapshot.contains("graph:" + graph_id));
        EXPECT_EQ(snapshot["graph:" + graph_id]["graph"], edited_json);

        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A fresh ReplicationManager rebuilds the edited graph from the document.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();
    auto project = node.find_project("churn");
    ASSERT_TRUE(project);
    auto stored = project->find_graph("pipeline");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->id(), graph_id);
    EXPECT_EQ(zetta::ir::to_json(stored->graph()), edited_json);
    node.stop();
}

TEST(ProjectGraph, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string graph_id;
    nlohmann::json serialized;
    {
        Node node{config_for(tmp)};
        node.start();
        auto project = node.create_project("churn");
        ASSERT_TRUE(project);
        auto stored = project->create_graph("pipeline", sample_graph());
        ASSERT_TRUE(stored);
        graph_id = stored->id();
        serialized = zetta::ir::to_json(stored->graph());
        node.stop();
    }

    // A fresh standalone node rebuilds the graph purely from disk.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    auto project = reloaded.find_project("churn");
    ASSERT_TRUE(project);
    ASSERT_EQ(project->graph_count(), 1u);
    auto stored = project->find_graph("pipeline");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->id(), graph_id);
    // The reloaded graph serializes identically — a faithful round-trip.
    EXPECT_EQ(zetta::ir::to_json(stored->graph()), serialized);
    reloaded.stop();
}

TEST(ProjectGraph, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::new_uuid();
    std::string graph_id;
    nlohmann::json serialized;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto project = node.create_project("churn");
        ASSERT_TRUE(project);
        auto stored = project->create_graph("pipeline", sample_graph());
        ASSERT_TRUE(stored);
        graph_id = stored->id();
        serialized = zetta::ir::to_json(stored->graph());
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document; the
    // graph rides in it under a graph: key and the projection is rebuilt.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int graph_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("graph:", 0) == 0) {
            ++graph_keys;
            EXPECT_EQ(value.value("name", std::string{}), "pipeline");
            ASSERT_TRUE(value.contains("graph"));
            EXPECT_EQ(value["graph"], serialized);
        }
    }
    EXPECT_EQ(graph_keys, 1);

    auto project = node.find_project("churn");
    ASSERT_TRUE(project);
    auto stored = project->find_graph("pipeline");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->id(), graph_id);
    EXPECT_EQ(zetta::ir::to_json(stored->graph()), serialized);
    node.stop();
}

TEST(ProjectGraph, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();

    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43320;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43321;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43320"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create a project + graph on node A only.
    auto project_a = a.create_project("churn");
    ASSERT_TRUE(project_a);
    auto stored_a = project_a->create_graph("pipeline", sample_graph());
    ASSERT_TRUE(stored_a);
    const nlohmann::json serialized = zetta::ir::to_json(stored_a->graph());

    // Node B converges to the same project + graph, and the CRDT versions match
    // once in sync.
    const bool converged = converge(a, b, [&] {
        auto project_b = b.find_project("churn");
        if (!project_b) return false;
        auto stored_b = project_b->find_graph("pipeline");
        return stored_b && zetta::ir::to_json(stored_b->graph()) == serialized &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto project_b = b.find_project("churn");
    ASSERT_TRUE(project_b);
    auto stored_b = project_b->find_graph("pipeline");
    ASSERT_TRUE(stored_b);
    EXPECT_EQ(stored_b->id(), stored_a->id());
    EXPECT_EQ(stored_b->project_id(), project_b->id());

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
