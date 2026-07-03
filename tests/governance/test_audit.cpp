#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/ir/execution_graph.hpp"
#include "zetta/ir/step_kind.hpp"
#include "zetta/pipeline/deployment_target.hpp"
#include "zetta/node.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/util/uuid.hpp"

using zetta::governance::AuditAction;
using zetta::governance::AuditEventValue;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::pipeline::DeploymentTarget;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::governance::Resource;
using zetta::governance::ResourceKind;
using zetta::governance::Scope;
using zetta::ir::ExecutionGraph;
using zetta::ir::StepKind;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A small connect→clean→model chain — enough to deploy / store as a named graph.
ExecutionGraph sample_graph() {
    ExecutionGraph graph;
    auto connect = graph.add_node(StepKind::Connect, "load_orders");
    auto clean = graph.add_node(StepKind::Clean, "dedupe");
    auto model = graph.add_node(StepKind::Model, "churn_model");
    graph.add_edge(connect->id, clean->id);
    graph.add_edge(clean->id, model->id);
    return graph;
}

Resource database(std::string name) {
    return Resource{ResourceKind::Database, std::move(name)};
}

bool looks_like_iso8601(const std::string& stamp) {
    return !stamp.empty() && stamp.find('T') != std::string::npos &&
           stamp.back() == 'Z';
}

// Tick both nodes until `done` holds (or we give up). Mirrors the run / project
// replication tests: capture -> cluster gossip -> peer merge -> reconcile.
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

// ----- emission at the three governed seams ---------------------------------

TEST(AuditTrail, GraphChangesEmitEvents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.audit_event_count(), 0u);

    auto project = node.create_project("churn", "owner-key");
    ASSERT_TRUE(project);

    // A graph create + drop each append one immutable event.
    ASSERT_TRUE(project->create_graph("pipeline", sample_graph()));
    ASSERT_TRUE(project->drop_graph("pipeline"));

    ASSERT_EQ(node.audit_event_count(), 2u);
    auto events = node.audit_events();
    EXPECT_EQ(events[0]->action(), AuditAction::GraphCreated);
    EXPECT_EQ(events[0]->target(), "pipeline");
    EXPECT_EQ(events[0]->actor(), "owner-key");  // the project's owner key
    EXPECT_TRUE(events[0]->allowed());
    EXPECT_TRUE(looks_like_iso8601(events[0]->time()));
    EXPECT_EQ(events[1]->action(), AuditAction::GraphDropped);
    EXPECT_EQ(events[1]->target(), "pipeline");

    // A no-op drop (graph already gone) records nothing.
    EXPECT_FALSE(project->drop_graph("pipeline"));
    EXPECT_EQ(node.audit_event_count(), 2u);

    node.stop();
}

TEST(AuditTrail, DeployEmitsEvent) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto deployment =
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    ASSERT_EQ(node.audit_event_count(), 1u);
    auto event = node.audit_events().front();
    EXPECT_EQ(event->action(), AuditAction::GraphDeployed);
    EXPECT_EQ(event->target(), "churn");
    EXPECT_TRUE(event->allowed());

    // A rejected deploy (empty graph) records nothing.
    EXPECT_FALSE(node.deploy_graph(ExecutionGraph{}, DeploymentTarget::local(),
                                   "empty"));
    EXPECT_EQ(node.audit_event_count(), 1u);

    node.stop();
}

TEST(AuditTrail, AuthorizeEmitsDataAccessEvents) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto principal = node.create_principal("analyst");
    ASSERT_TRUE(principal);
    Scope scope;
    scope.grant(ResourceKind::Database, "churn");
    auto key = principal->create_key(scope);
    ASSERT_TRUE(key);

    EXPECT_TRUE(node.authorize(key->id(), database("churn")).allowed);   // allow
    EXPECT_FALSE(node.authorize(key->id(), database("fraud")).allowed);  // deny
    EXPECT_FALSE(node.authorize("no-such-key", database("churn")).allowed);

    ASSERT_EQ(node.audit_event_count(), 3u);
    auto events = node.audit_events();
    for (const auto& event : events) {
        EXPECT_EQ(event->action(), AuditAction::DataAccess);
    }
    // The allow/deny outcome and the targeted resource are recorded.
    EXPECT_TRUE(events[0]->allowed());
    EXPECT_EQ(events[0]->target(), "database:churn");
    EXPECT_EQ(events[0]->actor(), key->id());
    EXPECT_FALSE(events[1]->allowed());
    EXPECT_EQ(events[1]->target(), "database:fraud");
    EXPECT_FALSE(events[2]->allowed());
    EXPECT_EQ(events[2]->actor(), "no-such-key");
    EXPECT_FALSE(events[2]->detail().empty());  // the deny reason

    node.stop();
}

// ----- persistence + replication --------------------------------------------

TEST(AuditTrail, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    {
        Node node{config_for(tmp)};
        node.start();
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
        EXPECT_EQ(node.audit_event_count(), 1u);
        node.stop();
    }

    // A fresh standalone node reads the trail purely from disk (audit/*.json).
    Node reloaded{config_for(tmp)};
    reloaded.start();
    ASSERT_EQ(reloaded.audit_event_count(), 1u);
    auto event = reloaded.audit_events().front();
    EXPECT_EQ(event->action(), AuditAction::GraphDeployed);
    EXPECT_EQ(event->target(), "churn");
    reloaded.stop();
}

TEST(AuditTrail, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        node.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document: the audit
    // event is in the replicated document, and the projection is rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int audit_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("audit:", 0) == 0) {
            ++audit_keys;
            EXPECT_EQ(value.value("action", std::string{}), "graph_deployed");
        }
    }
    EXPECT_EQ(audit_keys, 1);
    EXPECT_EQ(node.audit_event_count(), 1u);
    node.stop();
}

TEST(AuditTrail, ReplicatesToPeer) {
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
    cfg_a.listen_port = 43330;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43331;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43330"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Record an audit event on node A only (by deploying a graph).
    auto deployment =
        a.deploy_graph(sample_graph(), DeploymentTarget::local(), "churn");
    ASSERT_TRUE(deployment);

    // Node B converges to the same audit event (the immutable trail replicates).
    const bool converged = converge(a, b, [&] {
        auto events = b.audit_events();
        return events.size() == 1u &&
               events.front()->action() == AuditAction::GraphDeployed &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);
    EXPECT_EQ(b.audit_events().front()->target(), "churn");

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

// ----- boundary validation ---------------------------------------------------

TEST(AuditTrail, SchemaRejectsMalformedValuesAtTheBoundary) {
    using nlohmann::json;
    std::string error;

    // A well-formed value passes.
    AuditEventValue value;
    value.action = AuditAction::GraphDeployed;
    value.target = "churn";
    value.time = "2026-06-25T14:00:00.000Z";
    EXPECT_TRUE(
        zetta::governance::validate_audit_event_value(zetta::governance::to_json(value), &error))
        << error;

    // Unknown action enum.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "exploded"}, {"time", "t"}}, &error));
    // Missing the required action field.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(json{{"time", "t"}}, &error));
    // Missing the required time field.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "data_access"}}, &error));
    // Wrong type for a field.
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "data_access"}, {"time", "t"}, {"allowed", "yes"}},
        &error));
    // A stray top-level field (additionalProperties: false).
    EXPECT_FALSE(zetta::governance::validate_audit_event_value(
        json{{"action", "data_access"}, {"time", "t"}, {"surprise", 1}},
        &error));

    // parse_audit_event_value rejects the same and returns nullopt with a reason.
    EXPECT_FALSE(zetta::governance::parse_audit_event_value(json{{"action", "exploded"}},
                                                &error));
    EXPECT_FALSE(error.empty());
}

TEST(AuditTrail, InvalidAuditJsonOnDiskIsSkipped) {
    TempDir tmp;
    // Plant an audit/ folder with a malformed event file before start().
    const std::filesystem::path audit = tmp.path() / "audit";
    std::filesystem::create_directories(audit);
    {
        std::ofstream out(audit / (zetta::util::new_uuid() + ".json"));
        out << R"({"id":"x","action":"exploded","time":"t"})" << '\n';
    }

    Node node{config_for(tmp)};
    node.start();
    // The malformed event is skipped at the boundary, not loaded.
    EXPECT_EQ(node.audit_event_count(), 0u);
    node.stop();
}

TEST(AuditTrail, BackgroundActionSpellingsRoundTrip) {
    // #624's two new auditable actions: wire spellings + schema acceptance.
    using zetta::governance::AuditAction;
    using zetta::governance::audit_action_from;
    EXPECT_EQ(to_string(AuditAction::CloudInspected), "cloud_inspected");
    EXPECT_EQ(to_string(AuditAction::DatasetAdded), "dataset_added");
    EXPECT_EQ(audit_action_from("cloud_inspected"), AuditAction::CloudInspected);
    EXPECT_EQ(audit_action_from("dataset_added"), AuditAction::DatasetAdded);

    zetta::governance::AuditEventValue value;
    value.action = AuditAction::DatasetAdded;
    value.time = "2026-07-03T00:00:00Z";
    EXPECT_TRUE(zetta::governance::validate_audit_event_value(
        zetta::governance::to_json(value)));
}
