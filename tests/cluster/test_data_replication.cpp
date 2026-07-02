#include <gtest/gtest.h>

#include <chrono>
#include <set>
#include <string>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/node.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/table.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ReplicationManager;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Tick both nodes and wait until `done` holds (or we give up). Data created on
// one node reaches the other via: capture → cluster gossip (background thread)
// → peer merge → reconcile (on tick).
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

std::set<std::string> skus(const zetta::Table& table) {
    std::set<std::string> result;
    const zetta::StorageEngine& store = table.store();
    for (std::size_t i = 0; i < store.size(); ++i) {
        result.insert(store.at(i).at("sku").get<std::string>());
    }
    return result;
}

}  // namespace

TEST(DataReplication, DataCreatedOnOneNodeAppearsOnAnothersDisk) {
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
    cfg_a.listen_port = 43300;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43301;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43300"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // Create a database, table, and rows on node A only.
    auto shop = a.create_database("shop");
    ASSERT_TRUE(shop);
    auto items = shop->create_table("items");
    ASSERT_TRUE(items);
    items->add_row(nlohmann::json{{"sku", "A1"}, {"qty", 5}});
    items->add_row(nlohmann::json{{"sku", "B2"}, {"qty", 3}});

    // Node B should converge to the same database/table/rows.
    const bool converged = converge(a, b, [&] {
        auto db = b.find_database("shop");
        if (!db) return false;
        auto table = db->find_table("items");
        return table && table->row_count() == 2;
    });
    ASSERT_TRUE(converged);

    auto b_items = b.find_database("shop")->find_table("items");
    EXPECT_EQ(skus(*b_items), (std::set<std::string>{"A1", "B2"}));

    cluster_b.stop();
    cluster_a.stop();

    // ...and it's on B's DISK: a fresh, non-replicated node reading B's data dir
    // (purely from disk) sees the same data.
    Node b_reloaded{config_for(dir_b)};
    b_reloaded.start();
    auto reloaded_db = b_reloaded.find_database("shop");
    ASSERT_TRUE(reloaded_db);
    auto reloaded_items = reloaded_db->find_table("items");
    ASSERT_TRUE(reloaded_items);
    EXPECT_EQ(reloaded_items->row_count(), 2u);
    EXPECT_EQ(skus(*reloaded_items), (std::set<std::string>{"A1", "B2"}));

    a.stop();
    b.stop();
}

TEST(DataReplication, ReplicationStrategyTravelsToAPeerAndDisk) {
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
    cfg_a.listen_port = 43302;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43303;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43302"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    // A partitioned-with-replicas database (ENT-35) created on node A only, plus a
    // table that inherits its strategy.
    using zetta::ReplicationStrategy;
    auto shop = a.create_database(
        "shop", ReplicationStrategy::partitioned_with_replicas(2));
    ASSERT_TRUE(shop);
    auto items = shop->create_table("items");  // inherits the db's strategy
    ASSERT_TRUE(items);

    // Node B converges to the same strategy on both the database and the table.
    const bool converged = converge(a, b, [&] {
        auto db = b.find_database("shop");
        if (!db) return false;
        auto table = db->find_table("items");
        return table && db->replication_strategy() ==
                            ReplicationStrategy::partitioned_with_replicas(2) &&
               table->replication_strategy() ==
                   ReplicationStrategy::partitioned_with_replicas(2);
    });
    ASSERT_TRUE(converged);

    cluster_b.stop();
    cluster_a.stop();

    // ...and the strategy is on B's DISK: a fresh, non-replicated node reading B's
    // data dir sees the same strategy on the database and the table.
    Node b_reloaded{config_for(dir_b)};
    b_reloaded.start();
    auto reloaded_db = b_reloaded.find_database("shop");
    ASSERT_TRUE(reloaded_db);
    EXPECT_EQ(reloaded_db->replication_strategy(),
              ReplicationStrategy::partitioned_with_replicas(2));
    auto reloaded_items = reloaded_db->find_table("items");
    ASSERT_TRUE(reloaded_items);
    EXPECT_EQ(reloaded_items->replication_strategy(),
              ReplicationStrategy::partitioned_with_replicas(2));

    a.stop();
    b.stop();
}

TEST(DataReplication, ReplicatedDocumentPersistsAcrossRestart) {
    TempDir dir;
    const std::string node_id = zetta::new_uuid();
    {
        ReplicationManager replication{node_id};
        Node node{config_for(dir), &replication};
        node.start();
        auto db = node.create_database("logs");
        ASSERT_TRUE(db);
        auto table = db->create_table("events");
        ASSERT_TRUE(table);
        table->add_row(nlohmann::json{{"sku", "E1"}});
        node.tick();  // persist the CRDT document
        node.stop();
    }

    // A brand-new ReplicationManager loading only the persisted document (via a
    // fresh Node's start) holds the captured catalog.
    ReplicationManager replication{node_id};
    Node node{config_for(dir), &replication};
    node.start();
    const nlohmann::json snapshot = replication.snapshot();

    int databases = 0, tables = 0, rows = 0;
    for (const auto& [key, value] : snapshot.items()) {
        if (key.rfind("db:", 0) == 0) ++databases;
        else if (key.rfind("tbl:", 0) == 0) ++tables;
        else if (key.rfind("row:", 0) == 0) ++rows;
    }
    EXPECT_EQ(databases, 1);
    EXPECT_EQ(tables, 1);
    EXPECT_EQ(rows, 1);

    node.stop();
}
