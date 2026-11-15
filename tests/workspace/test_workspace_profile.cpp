// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"
#include "zetta/workspace/workspace_profile.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::Node;
using zetta::NodeConfig;
using zetta::query::QueryService;
using zetta::test::TempDir;
using zetta::workspace::WorkspaceProfileValue;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A representative profile value: an owner, a role, a daily-work description, and
// two approved source refs.
WorkspaceProfileValue sample_value(std::string owner = "user-1") {
    WorkspaceProfileValue value;
    value.owner = std::move(owner);
    value.role = "Customer success lead";
    value.daily_work = "Review delivery risk and renewal signals each morning.";
    value.sources = nlohmann::json::array(
        {{{"id", "orders"}, {"name", "Orders database"}, {"kind", "connection"}},
         {{"id", "renewals"}, {"name", "Renewals dataset"}, {"kind", "dataset"}}});
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

TEST(WorkspaceProfile, CreateFindDropAndEdit) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    EXPECT_EQ(node.workspace_profile_count(), 0u);

    auto profile = node.create_workspace_profile(sample_value());
    ASSERT_TRUE(profile);
    EXPECT_FALSE(profile->id().empty());
    EXPECT_EQ(profile->owner(), "user-1");
    EXPECT_EQ(profile->role(), "Customer success lead");
    ASSERT_TRUE(profile->sources().is_array());
    EXPECT_EQ(profile->sources().size(), 2u);
    EXPECT_FALSE(profile->created().empty());
    EXPECT_EQ(profile->created(), profile->updated());  // fresh: created == updated

    // create stamps `created`; owner is required.
    EXPECT_FALSE(node.create_workspace_profile(WorkspaceProfileValue{}));  // empty owner

    EXPECT_EQ(node.workspace_profile_count(), 1u);
    EXPECT_EQ(node.find_workspace_profile(profile->id()), profile);
    EXPECT_FALSE(node.find_workspace_profile("nope"));

    // An edit updates the fields + the updated stamp (owner + created fixed).
    const std::string created = profile->created();
    profile->set_content("VP of engineering", "Ship self-serve AI to the org.",
                         nlohmann::json::array(), "2026-07-24T12:00:00Z");
    EXPECT_EQ(profile->role(), "VP of engineering");
    EXPECT_EQ(profile->sources().size(), 0u);
    EXPECT_EQ(profile->owner(), "user-1");
    EXPECT_EQ(profile->created(), created);
    EXPECT_EQ(profile->updated(), "2026-07-24T12:00:00Z");

    EXPECT_TRUE(node.drop_workspace_profile(profile->id()));
    EXPECT_EQ(node.workspace_profile_count(), 0u);
    EXPECT_FALSE(node.drop_workspace_profile(profile->id()));  // already gone

    node.stop();
}

TEST(WorkspaceProfile, SchemaRejectsMalformedValuesAtTheBoundary) {
    // Missing the required non-empty `owner`.
    std::string error;
    EXPECT_FALSE(zetta::workspace::parse_workspace_profile_value(
        nlohmann::json{{"role", "x"}}, &error));
    EXPECT_FALSE(error.empty());
    // `sources` must be an array of objects.
    EXPECT_FALSE(zetta::workspace::parse_workspace_profile_value(
        nlohmann::json{{"owner", "u"}, {"sources", "not-an-array"}}));
    // A minimal valid value (only owner) parses.
    EXPECT_TRUE(zetta::workspace::parse_workspace_profile_value(
        nlohmann::json{{"owner", "u"}}));
}

TEST(WorkspaceProfile, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto profile = node.create_workspace_profile(sample_value());
        ASSERT_TRUE(profile);
        id = profile->id();
        node.stop();
    }
    // A fresh standalone node rebuilds the profile purely from disk.
    Node reloaded{config_for(tmp)};
    reloaded.start();
    ASSERT_EQ(reloaded.workspace_profile_count(), 1u);
    auto profile = reloaded.find_workspace_profile(id);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->owner(), "user-1");
    EXPECT_EQ(profile->role(), "Customer success lead");
    EXPECT_EQ(profile->sources().size(), 2u);
    reloaded.stop();
}

TEST(WorkspaceProfile, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto profile = node.create_workspace_profile(sample_value());
        ASSERT_TRUE(profile);
        id = profile->id();
        // The create is captured under a profile: key.
        const nlohmann::json snapshot = replication.snapshot();
        ASSERT_TRUE(snapshot.contains("profile:" + id));
        EXPECT_EQ(snapshot["profile:" + id]["owner"], "user-1");
        node.tick();  // persist the CRDT document
        node.stop();
    }
    // A brand-new ReplicationManager rebuilds the projection from the document.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();
    auto profile = node.find_workspace_profile(id);
    ASSERT_TRUE(profile);
    EXPECT_EQ(profile->role(), "Customer success lead");
    node.stop();
}

TEST(WorkspaceProfile, RegistryVisibleViaSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto profile = node.create_workspace_profile(sample_value("vp-eng"));
    ASSERT_TRUE(profile);

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.workspace_profiles;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0]["workspace_profile_id"].get<std::string>(), profile->id());
    EXPECT_EQ(sel.rows[0]["owner"].get<std::string>(), "vp-eng");
    EXPECT_EQ(sel.rows[0]["role"].get<std::string>(), "Customer success lead");
    EXPECT_EQ(sel.rows[0]["source_count"].get<int>(), 2);

    // WHERE + projection over the registry.
    auto by_owner = svc.run(
        "SELECT owner FROM system.workspace_profiles WHERE owner = 'vp-eng';", "");
    ASSERT_TRUE(by_owner.ok()) << by_owner.error;
    EXPECT_EQ(by_owner.rows.size(), 1u);

    node.stop();
}

TEST(WorkspaceProfile, ReplicatesToPeer) {
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
    cfg_a.listen_port = 43640;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43641;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43640"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create the profile on node A only.
    auto profile = a.create_workspace_profile(sample_value("customer-success"));
    ASSERT_TRUE(profile);
    const std::string profile_id = profile->id();

    // Node B converges to the same profile (reconcile_workspace_profiles).
    const bool converged = converge(a, b, [&] {
        auto p = b.find_workspace_profile(profile_id);
        return p && p->owner() == "customer-success" && p->sources().size() == 2u;
    });
    ASSERT_TRUE(converged);

    auto b_profile = b.find_workspace_profile(profile_id);
    ASSERT_TRUE(b_profile);
    EXPECT_EQ(b_profile->role(), "Customer success lead");

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
