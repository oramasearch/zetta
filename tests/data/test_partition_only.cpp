#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
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
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

// #833 section 4 — **partition-only tables**. A node lands its slice of an
// ingest as partitions it owns WITHOUT capturing the rows into the replicated
// document; otherwise the rows replicate straight back to every peer and cross
// the wire a second time, which is what tiling the fetch was for.
//
// The properties that make that safe, asserted here:
//   * no `row:` key is ever written for such a table, while the table itself and
//     its partition catalog DO replicate (so peers can read it);
//   * the dataset survives a node restart with every row readable exactly once
//     (reconcile prunes databases / tables / large objects but never rows, and
//     the partition bodies are large objects held by their owners);
//   * `system.datasets` counts rows from the catalog's row ranges, so a node
//     holding none of the slices still reports the whole dataset.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::data::AddDatasetOptions;
using zetta::data::AddDatasetResult;
using zetta::data::add_dataset_rows;
using zetta::test::TempDir;

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// The dataset used throughout: 40 rows over two columns, enough to spread across
// several partitions.
std::vector<nlohmann::json> sample_rows(std::size_t count = 40) {
    std::vector<nlohmann::json> rows;
    rows.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        rows.push_back(nlohmann::json{{"account", "acct-" + std::to_string(i)},
                                      {"amount", static_cast<int>(i)}});
    }
    return rows;
}

// Read every partition body of `table_id` from the catalog and return the rows,
// keyed by row id, together with how many times each id was seen. Reading the
// bodies (rather than a store) is how a partition-only dataset is read back:
// the bodies plus the replicate-to-all catalog ARE the dataset.
std::map<std::string, std::pair<nlohmann::json, int>> rows_from_partitions(
    Node& node, const std::string& table_id) {
    std::map<std::string, std::pair<nlohmann::json, int>> seen;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (!partition || partition->table() != table_id) continue;
        if (partition->blob_ref().empty()) continue;
        std::optional<std::string> body;
        for (const std::string& name : node.database_names()) {
            const std::shared_ptr<zetta::data::Database> database =
                node.find_database(name);
            if (!database) continue;
            if ((body = database->large_objects().read(partition->blob_ref()))) break;
        }
        if (!body) continue;
        const zetta::data::PartitionBodyReadResult read =
            zetta::data::read_partition_body(*body, partition->id());
        EXPECT_TRUE(read.ok()) << read.error;
        for (const zetta::data::PartitionRow& row : read.rows) {
            auto [it, inserted] = seen.try_emplace(row.id, row.doc, 0);
            ++it->second.second;
        }
    }
    return seen;
}

// Tick both nodes and wait until `done` holds. Patient by construction: it polls
// against a seconds-long deadline rather than a fixed small iteration count.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

std::size_t row_keys_for_table(const nlohmann::json& snapshot,
                               const std::string& table_id) {
    std::size_t count = 0;
    for (const auto& [key, value] : snapshot.items()) {
        if (!key.starts_with("row:")) continue;
        if (value.is_object() && value.value("tbl", std::string{}) == table_id) {
            ++count;
        }
    }
    return count;
}

}  // namespace

// The load-bearing property: the rows never reach the document, while the table
// and its partition catalog do.
TEST(PartitionOnlyTable, RowsAreNeverCapturedButThePlacementIs) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    ReplicationManager replication{node_id};
    Node node{config_for(dir), &replication};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_TRUE(added.table);
    EXPECT_TRUE(added.table->partition_only());
    EXPECT_EQ(added.rows, 40u);
    EXPECT_FALSE(added.partitions.empty());

    const nlohmann::json snapshot = replication.snapshot();
    EXPECT_EQ(row_keys_for_table(snapshot, added.table->id()), 0u)
        << "a partition-only table must not put its rows on the wire";
    // The table itself still replicates, carrying the marker so a peer reads it
    // the same way.
    ASSERT_TRUE(snapshot.contains("tbl:" + added.table->id()));
    EXPECT_TRUE(snapshot.at("tbl:" + added.table->id()).value("partition_only", false));

    // ...and so does the placement, so a peer can find the owners.
    std::size_t catalog_entries = 0;
    for (const auto& [key, value] : snapshot.items()) {
        if (key.starts_with("part:") &&
            value.value("table", std::string{}) == added.table->id()) {
            ++catalog_entries;
        }
    }
    EXPECT_EQ(catalog_entries, added.partitions.size());

    node.stop();
}

// A capture-free write is still durable on its owning node: reconcile prunes
// databases, tables and large objects, but never rows, and the partition bodies
// are large objects on disk.
TEST(PartitionOnlyTable, SurvivesARestartWithEveryRowReadableExactlyOnce) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::string table_id;
    std::set<std::string> accounts_before;

    {
        ReplicationManager replication{node_id};
        Node node{config_for(dir), &replication};
        node.start();

        AddDatasetOptions options;
        options.table_name = "cached";
        options.partition_count = 4;
        options.partition_only = true;
        const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
        ASSERT_TRUE(added.ok()) << added.error;
        table_id = added.table->id();
        for (const auto& [id, row] : rows_from_partitions(node, table_id)) {
            EXPECT_EQ(row.second, 1) << "row " << id << " is in two partitions";
            accounts_before.insert(row.first.at("account").get<std::string>());
        }
        EXPECT_EQ(accounts_before.size(), 40u);
        node.tick();  // persist the document + the projection
        node.stop();
    }

    // Restart the node over the same data dir, with a FRESH replication manager
    // (the document reloads from disk) — nothing re-materializes the rows from
    // the CRDT, because they were never there.
    ReplicationManager replication{node_id};
    Node node{config_for(dir), &replication};
    node.start();
    node.tick();

    const std::shared_ptr<zetta::data::Database> database =
        node.find_database("datasets");
    ASSERT_TRUE(database);
    const std::shared_ptr<zetta::data::Table> table = database->find_table("cached");
    ASSERT_TRUE(table);
    EXPECT_EQ(table->id(), table_id);
    EXPECT_TRUE(table->partition_only()) << "the marker must survive the restart";
    EXPECT_EQ(row_keys_for_table(replication.snapshot(), table_id), 0u)
        << "a restart must not capture the rows either";

    const auto after = rows_from_partitions(node, table_id);
    ASSERT_EQ(after.size(), 40u);
    std::set<std::string> accounts_after;
    for (const auto& [id, row] : after) {
        EXPECT_EQ(row.second, 1) << "row " << id << " read more than once";
        accounts_after.insert(row.first.at("account").get<std::string>());
    }
    EXPECT_EQ(accounts_after, accounts_before);

    node.stop();
}

// system.datasets counts a partition-only dataset from the catalog's row ranges.
// The peer holds none of the rows in its own store, so a store-based count would
// report a fraction — the exact bug this replaces.
TEST(PartitionOnlyTable, DatasetRowCountComesFromTheCatalogOnAPeer) {
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
    cfg_a.listen_port = 43700;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43701;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43700"};
    Cluster cluster_b{id_b, cfg_b, &repl_b};
    cluster_b.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    options.members = {id_a, id_b};
    const AddDatasetResult added = add_dataset_rows(a, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    const std::string table_id = added.table->id();

    const auto dataset_on = [](Node& node) -> std::optional<zetta::data::DatasetSummary> {
        for (const zetta::data::DatasetSummary& dataset : node.datasets()) {
            if (dataset.name == "cached") return dataset;
        }
        return std::nullopt;
    };

    const bool converged = converge(a, b, [&] {
        const auto dataset = dataset_on(b);
        return dataset && dataset->partitions.size() == added.partitions.size();
    });
    ASSERT_TRUE(converged);

    const std::shared_ptr<zetta::data::Database> b_database = b.find_database("datasets");
    ASSERT_TRUE(b_database);
    const std::shared_ptr<zetta::data::Table> b_table = b_database->find_table("cached");
    ASSERT_TRUE(b_table);
    EXPECT_TRUE(b_table->partition_only());
    EXPECT_EQ(b_table->store().size(), 0u)
        << "the peer must not have received the rows through the document";

    const auto b_dataset = dataset_on(b);
    ASSERT_TRUE(b_dataset);
    EXPECT_EQ(b_dataset->rows, 40u) << "the count comes from the catalog's row ranges";
    EXPECT_EQ(b_dataset->table_id, table_id);
    // The schema rides in the catalog too, so the peer describes the same dataset.
    std::vector<std::string> columns = b_dataset->columns;
    std::sort(columns.begin(), columns.end());
    EXPECT_EQ(columns, (std::vector<std::string>{"account", "amount"}));

    const auto a_dataset = dataset_on(a);
    ASSERT_TRUE(a_dataset);
    EXPECT_EQ(a_dataset->rows, b_dataset->rows)
        << "every node reports the same dataset";

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}
