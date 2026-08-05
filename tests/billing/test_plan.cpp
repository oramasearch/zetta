#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/billing/plan.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

using zetta::billing::PlanValue;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::Node;
using zetta::NodeConfig;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

PlanValue sample_plan(std::string name = "Pro") {
    PlanValue value;
    value.name = std::move(name);
    value.tier = "pro";
    value.quotas = nlohmann::json{{"max_runs", 1000},
                                  {"max_storage_bytes", 10737418240},
                                  {"max_nodes", 8}};
    value.period_start = "2026-07-01T00:00:00Z";
    return value;
}

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

TEST(Plan, CreateFindDropAndEdit) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.plan_count(), 0u);

    auto plan = node.create_plan(sample_plan());
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->name(), "Pro");
    EXPECT_EQ(plan->tier(), "pro");
    EXPECT_EQ(plan->quotas().value("max_runs", 0), 1000);
    EXPECT_FALSE(plan->created().empty());

    EXPECT_FALSE(node.create_plan(PlanValue{}));  // empty name rejected

    EXPECT_EQ(node.plan_count(), 1u);
    EXPECT_EQ(node.find_plan(plan->id()), plan);

    plan->set_content("Enterprise", "enterprise",
                      nlohmann::json{{"max_runs", 100000}}, "2026-08-01T00:00:00Z",
                      "2026-07-24T12:00:00Z");
    EXPECT_EQ(plan->name(), "Enterprise");
    EXPECT_EQ(plan->quotas().value("max_runs", 0), 100000);
    EXPECT_EQ(plan->updated(), "2026-07-24T12:00:00Z");

    EXPECT_TRUE(node.drop_plan(plan->id()));
    EXPECT_EQ(node.plan_count(), 0u);
    node.stop();
}

TEST(Plan, SchemaRejectsMalformedValuesAtTheBoundary) {
    std::string error;
    EXPECT_FALSE(zetta::billing::parse_plan_value(nlohmann::json{{"tier", "pro"}}, &error));  // no name
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(zetta::billing::parse_plan_value(  // quotas must be numeric
        nlohmann::json{{"name", "x"}, {"quotas", {{"max_runs", "lots"}}}}));
    EXPECT_TRUE(zetta::billing::parse_plan_value(nlohmann::json{{"name", "Free"}}));
}

TEST(Plan, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto plan = node.create_plan(sample_plan());
        ASSERT_TRUE(plan);
        id = plan->id();
        node.stop();
    }
    Node reloaded{config_for(tmp)};
    reloaded.start();
    ASSERT_EQ(reloaded.plan_count(), 1u);
    auto plan = reloaded.find_plan(id);
    ASSERT_TRUE(plan);
    EXPECT_EQ(plan->name(), "Pro");
    EXPECT_EQ(plan->quotas().value("max_nodes", 0), 8);
    reloaded.stop();
}

TEST(Plan, ReplicatesToPeer) {
    TempDir dir_a, dir_b;
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
    cfg_a.listen_port = 43660;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43661;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43660"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    auto plan = a.create_plan(sample_plan("Enterprise"));
    ASSERT_TRUE(plan);
    const std::string plan_id = plan->id();

    const bool converged = converge(a, b, [&] {
        auto p = b.find_plan(plan_id);
        return p && p->name() == "Enterprise" && p->tier() == "pro";
    });
    ASSERT_TRUE(converged);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

TEST(Plan, RegistryVisibleViaSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto plan = node.create_plan(sample_plan());
    ASSERT_TRUE(plan);

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.plans;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0]["plan_id"].get<std::string>(), plan->id());
    EXPECT_EQ(sel.rows[0]["name"].get<std::string>(), "Pro");
    EXPECT_EQ(sel.rows[0]["tier"].get<std::string>(), "pro");
    node.stop();
}

// The metered usage view (Conflict D): a single-row live snapshot computed from
// engine state — no stored counters.
TEST(Usage, MeteredSnapshotReflectsLiveState) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    QueryService svc{node};
    auto empty = svc.run("SELECT * FROM system.usage;", "");
    ASSERT_TRUE(empty.ok()) << empty.error;
    ASSERT_EQ(empty.rows.size(), 1u);  // always exactly one snapshot row
    EXPECT_EQ(empty.rows[0]["projects"].get<int>(), 0);
    EXPECT_EQ(empty.rows[0]["workspace_profiles"].get<int>(), 0);

    // Create some entities; the metered snapshot moves with them.
    ASSERT_TRUE(node.create_project("churn"));
    ASSERT_TRUE(node.create_project("renewals"));

    auto used = svc.run("SELECT projects, nodes FROM system.usage;", "");
    ASSERT_TRUE(used.ok()) << used.error;
    ASSERT_EQ(used.rows.size(), 1u);
    EXPECT_EQ(used.rows[0]["projects"].get<int>(), 2);
    node.stop();
}
