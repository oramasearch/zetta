#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A plausible ISO-8601 UTC stamp (e.g. "2026-06-25T14:03:21.123Z"): non-empty,
// date/time separated by 'T', and zulu-suffixed. Fixed-width, so the strings
// also order chronologically as plain text.
bool looks_like_iso8601(const std::string& stamp) {
    return !stamp.empty() && stamp.find('T') != std::string::npos &&
           stamp.back() == 'Z';
}

// Tick both nodes until `done` holds (or we give up). Mirrors the data
// replication test: capture -> cluster gossip -> peer merge -> reconcile.
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

TEST(Project, CreateFindDropRename) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    auto churn = node.create_project("churn");
    ASSERT_TRUE(churn);
    EXPECT_EQ(churn->name(), "churn");
    EXPECT_TRUE(churn->owner().empty());  // default owner
    EXPECT_TRUE(looks_like_iso8601(churn->created()));
    EXPECT_EQ(churn->created(), churn->updated());  // equal at creation

    EXPECT_FALSE(node.create_project("churn"));  // duplicate
    EXPECT_FALSE(node.create_project(""));        // empty

    auto fraud = node.create_project("fraud", "team:risk");
    ASSERT_TRUE(fraud);
    EXPECT_EQ(fraud->owner(), "team:risk");  // owner key recorded

    EXPECT_EQ(node.project_count(), 2u);
    EXPECT_TRUE(node.find_project("churn"));
    EXPECT_TRUE(node.find_project("fraud"));
    EXPECT_FALSE(node.find_project("nope"));

    // Rename bumps `updated` (>= created), keeps the id, frees the old name.
    EXPECT_TRUE(node.rename_project("churn", "retention"));
    auto retention = node.find_project("retention");
    ASSERT_TRUE(retention);
    EXPECT_EQ(retention->id(), churn->id());
    EXPECT_GE(retention->updated(), retention->created());
    EXPECT_FALSE(node.find_project("churn"));
    EXPECT_FALSE(node.rename_project("retention", "fraud"));  // target taken
    EXPECT_FALSE(node.rename_project("nope", "x"));           // missing source

    EXPECT_TRUE(node.drop_project("retention"));
    EXPECT_EQ(node.project_count(), 1u);
    EXPECT_FALSE(node.drop_project("retention"));  // already gone

    node.stop();
}

TEST(Project, PersistsAcrossRestartViaDisk) {
    TempDir tmp;
    std::string id;
    std::string created;
    {
        Node node{config_for(tmp)};
        node.start();
        auto project = node.create_project("churn", "team:growth");
        ASSERT_TRUE(project);
        id = project->id();
        created = project->created();
        node.stop();
    }

    // A fresh standalone node reads the project purely from disk (project.json).
    Node reloaded{config_for(tmp)};
    reloaded.start();
    EXPECT_EQ(reloaded.project_count(), 1u);
    auto project = reloaded.find_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->id(), id);
    EXPECT_EQ(project->owner(), "team:growth");
    EXPECT_EQ(project->created(), created);
    reloaded.stop();
}

TEST(Project, PersistsAcrossRestartViaCRDT) {
    TempDir tmp;
    const std::string node_id = zetta::util::new_uuid();
    std::string id;
    {
        ReplicationManager replication{node_id};
        Node node{config_for(tmp), &replication};
        node.start();
        auto project = node.create_project("churn", "team:growth");
        ASSERT_TRUE(project);
        id = project->id();
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loads only the persisted document (via a
    // fresh Node's start): the project is in the replicated document, and the
    // projection is rebuilt from it.
    ReplicationManager replication{node_id};
    Node node{config_for(tmp), &replication};
    node.start();

    int project_keys = 0;
    const nlohmann::json snapshot = replication.snapshot();
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("project:", 0) == 0) {
            ++project_keys;
            EXPECT_EQ(value.value("name", std::string{}), "churn");
            EXPECT_EQ(value.value("owner", std::string{}), "team:growth");
        }
    }
    EXPECT_EQ(project_keys, 1);

    auto project = node.find_project("churn");
    ASSERT_TRUE(project);
    EXPECT_EQ(project->id(), id);
    EXPECT_EQ(project->owner(), "team:growth");
    node.stop();
}

TEST(Project, ReplicatesToPeer) {
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
    cfg_a.listen_port = 43310;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43311;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43310"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create a project on node A only.
    auto created = a.create_project("churn", "team:risk");
    ASSERT_TRUE(created);

    // Node B converges to the same project (name + owner), and the CRDT
    // versions match once in sync.
    const bool converged = converge(a, b, [&] {
        auto project = b.find_project("churn");
        return project && project->owner() == "team:risk" &&
               repl_a.version() == repl_b.version();
    });
    ASSERT_TRUE(converged);

    auto replicated = b.find_project("churn");
    ASSERT_TRUE(replicated);
    EXPECT_EQ(replicated->id(), created->id());

    // A rename on A also replicates to B.
    ASSERT_TRUE(a.rename_project("churn", "retention"));
    const bool renamed = converge(a, b, [&] {
        return b.find_project("retention") && !b.find_project("churn");
    });
    EXPECT_TRUE(renamed);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
