#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/promote_dataset.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

// #833 section 9 — **promote a cached result set to durable**. A connector
// result set lands at RF=1 because it is a cache; promotion is the deliberate
// act that raises it to the durable factor. Adding an owner to the replicated
// partition catalog IS the copy: the new owner sees the body in missing_blobs()
// and pulls it over the existing large-object transport.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::data::AddDatasetOptions;
using zetta::data::AddDatasetResult;
using zetta::data::add_dataset_rows;
using zetta::data::promote_dataset;
using zetta::data::PromoteDatasetResult;
using zetta::data::ReplicatedBlobStore;
using zetta::test::TempDir;

constexpr const char* kNodeA = "33333333-3333-4333-8333-333333333333";
constexpr const char* kNodeB = "44444444-4444-4444-8444-444444444444";

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

std::vector<nlohmann::json> sample_rows(std::size_t count = 32) {
    std::vector<nlohmann::json> rows;
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(nlohmann::json{{"id", static_cast<int>(i)},
                                      {"amount", static_cast<int>(i * 3)}});
    }
    return rows;
}

template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

}  // namespace

// The headline: a cache at RF=1 becomes durable, and every partition keeps the
// owner it already had (promotion adds copies, it never moves the only one).
TEST(PromoteDataset, RaisesACachedResultSetToTheDurableFactor) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.replication_factor = 1;  // the connector path's default: a cache
    options.partition_only = true;
    options.members = {"node-1", "node-2", "node-3", "node-4"};
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_FALSE(added.partitions.empty());

    std::map<std::string, std::string> original_owner;  // catalog id -> owner
    for (const auto& partition : added.partitions) {
        ASSERT_EQ(partition->owner_nodes().size(), 1u);
        original_owner.emplace(partition->id(), partition->owner_nodes().front());
    }

    const PromoteDatasetResult promoted =
        promote_dataset(node, "datasets", "cached", 3, options.members);
    ASSERT_TRUE(promoted.ok()) << promoted.error;
    EXPECT_EQ(promoted.replication_factor, 3u);
    EXPECT_EQ(promoted.partitions, added.partitions.size());
    EXPECT_EQ(promoted.promoted, added.partitions.size());
    EXPECT_EQ(promoted.table_id, added.table->id());

    for (const auto& partition : node.dataset_partitions()) {
        if (partition->table() != added.table->id()) continue;
        const std::vector<std::string>& owners = partition->owner_nodes();
        EXPECT_EQ(owners.size(), 3u);
        EXPECT_EQ(std::set<std::string>(owners.begin(), owners.end()).size(), 3u)
            << "an owner was recorded twice";
        // The node that already held the body is still an owner, and still first.
        ASSERT_TRUE(original_owner.contains(partition->id()));
        EXPECT_EQ(owners.front(), original_owner.at(partition->id()));
    }

    node.stop();
}

// Promotion only ever raises. Asking for fewer owners than a partition has must
// not drop one — a "promotion" that discarded the only copy would be the exact
// opposite of what it is for.
TEST(PromoteDataset, NeverRemovesAnOwnerOrExceedsTheRoster) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    AddDatasetOptions options;
    options.table_name = "durable";
    options.partition_count = 4;
    options.replication_factor = 3;
    options.members = {"node-1", "node-2", "node-3", "node-4"};
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;

    const PromoteDatasetResult shrunk =
        promote_dataset(node, "datasets", "durable", 1, options.members);
    ASSERT_TRUE(shrunk.ok()) << shrunk.error;
    EXPECT_EQ(shrunk.promoted, 0u) << "nothing should have changed";
    for (const auto& partition : added.partitions) {
        EXPECT_EQ(partition->owner_nodes().size(), 3u);
    }

    // A factor beyond the roster clamps to it rather than recording owners that
    // do not exist.
    const PromoteDatasetResult clamped =
        promote_dataset(node, "datasets", "durable", 99, options.members);
    ASSERT_TRUE(clamped.ok()) << clamped.error;
    EXPECT_EQ(clamped.replication_factor, 4u);
    for (const auto& partition : added.partitions) {
        EXPECT_EQ(partition->owner_nodes().size(), 4u);
    }

    node.stop();
}

TEST(PromoteDataset, RejectsUnknownDatasetsAndAnEmptyOne) {
    TempDir dir;
    Node node{config_for(dir)};
    node.start();

    EXPECT_FALSE(promote_dataset(node, "nope", "cached").ok());

    ASSERT_TRUE(node.create_database("datasets"));
    ASSERT_TRUE(node.find_database("datasets")->create_table("empty"));
    const PromoteDatasetResult unplaced = promote_dataset(node, "datasets", "empty");
    EXPECT_FALSE(unplaced.ok());
    EXPECT_NE(unplaced.error.find("no placed partitions"), std::string::npos)
        << unplaced.error;

    EXPECT_FALSE(promote_dataset(node, "datasets", "empty", 0).ok());

    node.stop();
}

// End to end across two nodes: promoting makes the peer an owner, and the body
// follows over the existing large-object transport — no copy loop of our own.
TEST(PromoteDataset, ThePeerBecomesAnOwnerAndTheBodyFollows) {
    TempDir dir_a;
    TempDir dir_b;
    ReplicationManager repl_a{kNodeA};
    ReplicationManager repl_b{kNodeB};
    ReplicatedBlobStore blobs_a{dir_a.path(), repl_a};
    ReplicatedBlobStore blobs_b{dir_b.path(), repl_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43720;
    cfg_a.port_range = 1;
    Cluster cluster_a{kNodeA, cfg_a, &repl_a, &blobs_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43721;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43720"};
    Cluster cluster_b{kNodeB, cfg_b, &repl_b, &blobs_b};
    cluster_b.start();

    // A cache owned entirely by A.
    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.replication_factor = 1;
    options.members = {kNodeA};
    const AddDatasetResult added = add_dataset_rows(a, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    const std::string blob_ref = added.partitions.front()->blob_ref();
    for (const auto& partition : added.partitions) {
        ASSERT_EQ(partition->owner_nodes(), std::vector<std::string>{kNodeA});
    }

    ASSERT_TRUE(converge(a, b, [&] {
        return b.dataset_partition_count() == added.partitions.size();
    }));

    // B holds none of the bodies while it owns none of them.
    const auto b_body = [&] {
        const auto database = b.find_database("datasets");
        return database ? database->large_objects().read(blob_ref)
                        : std::optional<std::string>{};
    };
    EXPECT_FALSE(b_body().has_value());

    const PromoteDatasetResult promoted =
        promote_dataset(a, "datasets", "cached", 2, {kNodeA, kNodeB});
    ASSERT_TRUE(promoted.ok()) << promoted.error;
    EXPECT_EQ(promoted.replication_factor, 2u);
    EXPECT_EQ(promoted.promoted, added.partitions.size());

    // The new placement replicates, and the body follows it to B.
    ASSERT_TRUE(converge(a, b, [&] { return b_body().has_value(); }));

    const auto b_partition = [&] {
        for (const auto& partition : b.dataset_partitions()) {
            if (partition->blob_ref() == blob_ref) return partition;
        }
        return std::shared_ptr<zetta::data::DatasetPartition>{};
    }();
    ASSERT_TRUE(b_partition);
    const std::vector<std::string>& owners = b_partition->owner_nodes();
    EXPECT_NE(std::find(owners.begin(), owners.end(), kNodeB), owners.end());
    EXPECT_NE(std::find(owners.begin(), owners.end(), kNodeA), owners.end());

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
