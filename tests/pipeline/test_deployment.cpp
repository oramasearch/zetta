#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/pipeline/deployment.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::pipeline::DeploymentKind;
using zetta::pipeline::DeploymentTarget;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A small connect→clean→model chain: 3 nodes, 2 data-flow edges. Enough to give
// the deployment a non-trivial graph summary (node/edge counts).
ExecutionGraph sample_graph() {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load_orders");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    return graph;
}

bool looks_like_iso8601(const std::string& stamp) {
    return !stamp.empty() && stamp.find('T') != std::string::npos &&
           stamp.back() == 'Z';
}

// Tick both nodes until `done` holds (or we give up). Mirrors the project /
// data replication tests: capture -> cluster gossip -> peer merge -> reconcile.
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

// ---- DeploymentTarget value type + boundary validation ---------------------

TEST(DeploymentTarget, NamesAndJsonRoundTrip) {
    EXPECT_EQ(zetta::pipeline::to_string(DeploymentKind::Local), "local");
    EXPECT_EQ(zetta::pipeline::to_string(DeploymentKind::Kubernetes), "kubernetes");
    EXPECT_EQ(zetta::pipeline::deployment_kind_from_name("cloud"),
              std::optional{DeploymentKind::Cloud});
    EXPECT_FALSE(zetta::pipeline::deployment_kind_from_name("nope").has_value());

    const DeploymentTarget local = DeploymentTarget::local();
    EXPECT_TRUE(local.is_local());
    EXPECT_TRUE(local.binding.empty());

    const nlohmann::json json = local.to_json();
    EXPECT_EQ(json["kind"], "local");
    const auto parsed = DeploymentTarget::from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->kind, DeploymentKind::Local);
}

TEST(DeploymentTarget, FromJsonRejectsMalformedAtTheBoundary) {
    EXPECT_FALSE(DeploymentTarget::from_json(nlohmann::json::array()));  // not obj
    EXPECT_FALSE(DeploymentTarget::from_json(nlohmann::json::object()));  // no kind
    EXPECT_FALSE(
        DeploymentTarget::from_json(nlohmann::json{{"kind", "moon"}}));  // unknown
    EXPECT_FALSE(
        DeploymentTarget::from_json(nlohmann::json{{"kind", 7}}));  // wrong type
}

// ---- placing a graph onto a local target -----------------------------------

TEST(Deployment, DeployGraphToLocalTarget) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_FALSE(node.node_id().empty());  // standalone node has a stable id

    const ExecutionGraph graph = sample_graph();
    auto deployment =
        node.deploy_graph(graph, DeploymentTarget::local(), "churn_pipeline");
    ASSERT_TRUE(deployment);
    EXPECT_EQ(deployment->graph_name(), "churn_pipeline");
    EXPECT_TRUE(deployment->target().is_local());
    EXPECT_EQ(deployment->node_count(), 3u);
    EXPECT_EQ(deployment->edge_count(), 2u);
    EXPECT_EQ(deployment->node_id(), node.node_id());  // placed on this node
    EXPECT_TRUE(looks_like_iso8601(deployment->created()));

    EXPECT_EQ(node.deployment_count(), 1u);
    EXPECT_TRUE(node.find_deployment(deployment->id()));
    EXPECT_FALSE(node.find_deployment("no-such-id"));

    // An empty graph has nothing to place; a non-local target is deferred (R1).
    EXPECT_FALSE(node.deploy_graph(ExecutionGraph{}, DeploymentTarget::local(),
                                   "empty"));
    DeploymentTarget cloud;
    cloud.kind = DeploymentKind::Cloud;
    EXPECT_FALSE(node.deploy_graph(graph, cloud, "to_cloud"));
    EXPECT_EQ(node.deployment_count(), 1u);  // neither was recorded

    node.stop();
}

// RULE-1: deploy_graph runs the correctness gate at the seam, so a programmatic
// deploy can't bypass it. A graph that fails the gate places nothing and records a
// blocked (allowed=false) deploy event.
TEST(Deployment, DeployGraphIsGatedRule1) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    // A graph whose entity-resolution step conflates two distinct entities onto one
    // account — the canonical TC-4 failure the gate must block.
    ExecutionGraph graph;
    nlohmann::json resolution = {
        {"entity_resolution",
         {{"records",
           {{{"mention", "FedEx Freight"}, {"entity", "fedex_freight"}, {"resolved", "acct_fedex"}},
            {{"mention", "FedEx Ground"}, {"entity", "fedex_ground"}, {"resolved", "acct_fedex"}}}}}}};
    graph.add_node(StepKind::Clean, "resolve_accounts", resolution);

    const std::size_t audit_before = node.audit_event_count();
    auto blocked = node.deploy_graph(graph, DeploymentTarget::local(), "churn");
    EXPECT_FALSE(blocked);                       // RULE-1: gate failure blocks deploy
    EXPECT_EQ(node.deployment_count(), 0u);      // nothing was placed
    ASSERT_EQ(node.audit_event_count(), audit_before + 1u);  // the blocked decision is audited
    auto event = node.audit_events().back();
    EXPECT_EQ(event->action(), zetta::governance::AuditAction::GraphDeployed);
    EXPECT_FALSE(event->allowed());
    EXPECT_NE(event->detail().find("entity_resolution"), std::string::npos);

    // A well-formed graph still deploys (the gate passes).
    EXPECT_TRUE(node.deploy_graph(sample_graph(), DeploymentTarget::local(), "ok"));
    EXPECT_EQ(node.deployment_count(), 1u);

    node.stop();
}

// ---- queryable via the system.deployments table ----------------------

TEST(Deployment, QueryableViaSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");

    QueryService svc{node};  // no cluster needed: deployments come from the node

    auto all = svc.run("SELECT * FROM system.deployments;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    ASSERT_EQ(all.rows.size(), 1u);
    EXPECT_EQ(all.rows[0]["graph_name"], "churn");
    EXPECT_EQ(all.rows[0]["target_kind"], "local");
    EXPECT_EQ(all.rows[0]["node_count"], 3);
    EXPECT_EQ(all.rows[0]["node_id"], node.node_id());

    // Projection + WHERE equality work exactly as for a regular table.
    auto projected = svc.run(
        "SELECT graph_name, edge_count FROM system.deployments "
        "WHERE graph_name = 'churn';",
        "");
    ASSERT_TRUE(projected.ok()) << projected.error;
    ASSERT_EQ(projected.rows.size(), 1u);
    EXPECT_EQ(projected.rows[0]["edge_count"], 2);

    // The bare alias is reserved for nodes only; deployments is system.<t>.
    EXPECT_FALSE(svc.run("SELECT * FROM deployments;", "").ok());
    node.stop();
}

// ---- persistence -----------------------------------------------------------

TEST(Deployment, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    std::string created;
    {
        Node node{config_for(tmp)};
        node.start();
        auto deployment =
            node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
        ASSERT_TRUE(deployment);
        id = deployment->id();
        created = deployment->created();
        node.stop();
    }

    // A fresh standalone node reads the deployment purely from deployment.json.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.deployment_count(), 1u);
    auto deployment = reloaded.find_deployment(id);
    ASSERT_TRUE(deployment);
    EXPECT_EQ(deployment->graph_name(), "churn");
    EXPECT_EQ(deployment->node_count(), 3u);
    EXPECT_EQ(deployment->created(), created);
    reloaded.stop();
}

TEST(Deployment, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto deployment =
            node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
        ASSERT_TRUE(deployment);
        id = deployment->id();
        EXPECT_EQ(deployment->node_id(), node_id);  // == replication actor id
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the
    // deployment is in the replicated state, and the projection is rebuilt.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int deployment_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("deployment:", 0) == 0) {
            ++deployment_keys;
            EXPECT_EQ(value.value("graph_name", std::string{}), "churn");
            EXPECT_EQ(value.value("node", std::string{}), node_id);
        }
    }
    EXPECT_EQ(deployment_keys, 1);

    auto deployment = node.find_deployment(id);
    ASSERT_TRUE(deployment);
    EXPECT_EQ(deployment->graph_name(), "churn");
    EXPECT_EQ(deployment->node_id(), node_id);
    node.stop();
}

// ---- replication to a peer -------------------------------------------------

TEST(Deployment, ReplicatesToPeer) {
    TempDir dir_a;
    TempDir dir_b;
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();

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

    // Deploy a graph on node A only.
    auto deployed =
        a.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployed);

    // Node B converges to the same deployment, recording that node A placed it.
    const bool converged = converge(a, b, [&] {
        auto deployment = b.find_deployment(deployed->id());
        return deployment && deployment->graph_name() == "churn" &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto replicated = b.find_deployment(deployed->id());
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->node_id(), id_a);  // placed on A, seen from B
    EXPECT_EQ(replicated->node_count(), 3u);
    EXPECT_TRUE(replicated->target().is_local());

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
