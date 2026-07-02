#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::DatasetPartition;
using zetta::DatasetPartitionValue;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ReplicationManager;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

DatasetPartitionValue sample_value(std::string table = "tbl-1") {
    DatasetPartitionValue value;
    value.table = std::move(table);
    value.blob_ref = "lo-7";
    value.row_range = {0, 10000};
    value.stats = nlohmann::json{{"qty", {{"min", 1}, {"max", 99}}}};
    value.owner_nodes = {"node-a", "node-b"};
    return value;
}

// Tick both nodes and wait until `done` holds (or we give up).
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

// ---- value type + JSON-Schema boundary -------------------------------------

TEST(DatasetPartitionValue, RoundTripsThroughJson) {
    const DatasetPartitionValue value = sample_value();
    const nlohmann::json json = zetta::to_json(value);
    const auto parsed = zetta::parse_dataset_partition_value(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->table, "tbl-1");
    EXPECT_EQ(parsed->blob_ref, "lo-7");
    EXPECT_EQ(parsed->row_range.start, 0u);
    EXPECT_EQ(parsed->row_range.end, 10000u);
    EXPECT_EQ(parsed->owner_nodes, (std::vector<std::string>{"node-a", "node-b"}));
    EXPECT_EQ(parsed->stats.at("qty").at("max").get<int>(), 99);
}

TEST(DatasetPartitionValue, RejectsMalformedAtTheBoundary) {
    std::string error;
    // Missing the required `table`.
    EXPECT_FALSE(zetta::parse_dataset_partition_value(
        nlohmann::json{{"blob_ref", "lo-1"}}, &error));
    EXPECT_FALSE(error.empty());
    // Unknown property (sealed shape).
    EXPECT_FALSE(zetta::parse_dataset_partition_value(
        nlohmann::json{{"table", "t"}, {"bogus", 1}}));
    // Wrong type for owner_nodes.
    EXPECT_FALSE(zetta::parse_dataset_partition_value(
        nlohmann::json{{"table", "t"}, {"owner_nodes", "not-an-array"}}));
}

// ---- Node CRUD + persistence (standalone) ----------------------------------

TEST(DatasetPartition, CreateFindDropStandalone) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    EXPECT_EQ(node.dataset_partition_count(), 0u);
    // A partition with no table ref is rejected (the catalog's anchor).
    EXPECT_FALSE(node.create_dataset_partition(DatasetPartitionValue{}));

    auto partition = node.create_dataset_partition(sample_value());
    ASSERT_TRUE(partition);
    EXPECT_FALSE(partition->created().empty());  // stamped
    EXPECT_EQ(node.dataset_partition_count(), 1u);
    EXPECT_EQ(node.find_dataset_partition(partition->id()), partition);

    EXPECT_TRUE(node.drop_dataset_partition(partition->id()));
    EXPECT_EQ(node.dataset_partition_count(), 0u);
    EXPECT_FALSE(node.drop_dataset_partition(partition->id()));

    node.stop();
}

TEST(DatasetPartition, PersistsAcrossRestart) {
    TempDir tmp;
    std::string id;
    {
        Node node{config_for(tmp)};
        node.start();
        auto partition = node.create_dataset_partition(sample_value());
        ASSERT_TRUE(partition);
        id = partition->id();
        node.stop();
    }
    // A fresh node reading the same data dir rebuilds the partition from disk.
    Node node{config_for(tmp)};
    node.start();
    auto partition = node.find_dataset_partition(id);
    ASSERT_TRUE(partition);
    EXPECT_EQ(partition->table(), "tbl-1");
    EXPECT_EQ(partition->row_range().end, 10000u);
    node.stop();
}

// ---- the system.dataset_partitions computed view ---------------------------

TEST(DatasetPartition, VisibleInSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    auto partition = node.create_dataset_partition(sample_value());
    ASSERT_TRUE(partition);

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.dataset_partitions;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0]["partition_id"].get<std::string>(), partition->id());
    EXPECT_EQ(sel.rows[0]["table"].get<std::string>(), "tbl-1");
    EXPECT_EQ(sel.rows[0]["blob_ref"].get<std::string>(), "lo-7");
    EXPECT_EQ(sel.rows[0]["row_count"].get<int>(), 10000);
    EXPECT_EQ(sel.rows[0]["owner_count"].get<int>(), 2);

    // Projection + WHERE work over the computed rows like any system table.
    auto filtered = svc.run(
        "SELECT partition_id FROM system.dataset_partitions WHERE table = 'tbl-1';",
        "");
    ASSERT_TRUE(filtered.ok()) << filtered.error;
    EXPECT_EQ(filtered.rows.size(), 1u);

    node.stop();
}

// ---- the acceptance: replicate-to-all, visible on every node ---------------

TEST(DatasetPartition, ReplicatesToEveryNodeAndIsQueryable) {
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

    // Record a partition on node A only.
    auto partition = a.create_dataset_partition(sample_value());
    ASSERT_TRUE(partition);
    const std::string part_id = partition->id();

    // Node B converges to the same partition (capture → gossip → reconcile).
    const bool converged = converge(a, b, [&] {
        return b.find_dataset_partition(part_id) != nullptr;
    });
    ASSERT_TRUE(converged);

    // ...and it is queryable on B via system.dataset_partitions (the acceptance).
    QueryService svc_b{b};
    auto sel = svc_b.run(
        "SELECT table, blob_ref, owner_count FROM system.dataset_partitions;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);
    EXPECT_EQ(sel.rows[0]["table"].get<std::string>(), "tbl-1");
    EXPECT_EQ(sel.rows[0]["blob_ref"].get<std::string>(), "lo-7");
    EXPECT_EQ(sel.rows[0]["owner_count"].get<int>(), 2);

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
