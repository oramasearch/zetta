// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/partition_body.hpp"
#include "zetta/node.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/partitioner.hpp"
#include "zetta/query/service.hpp"
#include "zetta/data/rendezvous_placement.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/data/table.hpp"
#include "zetta/storage/engine.hpp"

using zetta::data::Database;
using zetta::data::HashPartitioner;
using zetta::Node;
using zetta::NodeConfig;
using zetta::data::PartitionWriteResult;
using zetta::data::RendezvousPlacement;
using zetta::data::ReplicationStrategy;
using zetta::data::RoundRobinPartitioner;
using zetta::data::SinglePartitioner;
using zetta::data::Table;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A node + a partitioned table holding `count` rows (each `{id, n}` with a distinct
// id) — the substrate every test partitions. The table inherits the Partitioned
// replication strategy; the operative signal is the Partitioner the test sets.
struct Fixture {
    explicit Fixture(const TempDir& tmp, std::size_t count = 12) {
        node = std::make_unique<Node>(config_for(tmp));
        node->start();
        database = node->create_database("warehouse");
        table = database->create_table("events", ReplicationStrategy::partitioned());
        for (std::size_t i = 0; i < count; ++i) {
            table->add_row(nlohmann::json{{"id", "row-" + std::to_string(i)},
                                          {"n", static_cast<int>(i)}});
        }
    }

    std::unique_ptr<Node> node;
    std::shared_ptr<Database> database;
    std::shared_ptr<Table> table;
};

// The rows a partition's serialized body carries, read back through the shared
// partition-body reader (which sniffs Parquet vs the legacy JSON Lines).
std::vector<zetta::data::PartitionRow> body_rows(const std::string& blob,
                                                 const std::string& partition_id) {
    const zetta::data::PartitionBodyReadResult read =
        zetta::data::read_partition_body(blob, partition_id);
    EXPECT_TRUE(read.ok()) << read.error;
    return read.rows;
}

// Assert the partition row ranges tile [0, total) contiguously with no gaps or
// overlaps, in the order the catalog returns them.
void expect_ranges_tile(
    const std::vector<std::shared_ptr<zetta::data::DatasetPartition>>& partitions,
    std::uint64_t total) {
    std::uint64_t expected_start = 0;
    for (const auto& partition : partitions) {
        EXPECT_EQ(partition->row_range().start, expected_start)
            << "partition ranges must be contiguous";
        EXPECT_GT(partition->row_range().end, partition->row_range().start)
            << "a recorded partition holds at least one row";
        expected_start = partition->row_range().end;
    }
    EXPECT_EQ(expected_start, total) << "ranges must cover the whole table";
}

}  // namespace

// ---- the acceptance: hash-partitioned load → N blobs + N catalog rows ---------

TEST(PartitionWriter, HashPartitionedLoadProducesBlobsAndCatalogRows) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/12};
    fx.table->set_partitioner(std::make_shared<HashPartitioner>(
        /*P=*/4, std::vector<std::string>{"id"}));

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    // Every row was distributed, into at least one and at most P (or row-count)
    // non-empty partitions.
    EXPECT_EQ(result.rows_written, 12u);
    EXPECT_GE(result.partitions.size(), 1u);
    EXPECT_LE(result.partitions.size(), 4u);

    // N partition blobs + N catalog rows.
    EXPECT_EQ(result.partitions.size(), fx.node->dataset_partition_count());

    // The ranges tile [0, 12), and every blob exists with the right number of rows.
    expect_ranges_tile(result.partitions, 12u);
    std::size_t total_blob_rows = 0;
    std::set<std::string> blob_refs;
    for (const auto& partition : result.partitions) {
        EXPECT_EQ(partition->table(), fx.table->id());
        EXPECT_FALSE(partition->blob_ref().empty());
        blob_refs.insert(partition->blob_ref());

        const auto blob = fx.database->large_objects().read(partition->blob_ref());
        ASSERT_TRUE(blob) << "partition blob must be stored as a large object";
        const std::size_t rows = body_rows(*blob, partition->id()).size();
        // The catalog range size matches the blob's row count.
        EXPECT_EQ(rows, partition->row_range().end - partition->row_range().start);
        total_blob_rows += rows;
    }
    // Distinct blobs, and together they hold exactly the table's rows.
    EXPECT_EQ(blob_refs.size(), result.partitions.size());
    EXPECT_EQ(total_blob_rows, 12u);
}

// ---- the blob is a faithful, parseable slice of the table ---------------------

TEST(PartitionWriter, BlobsCarryEveryRowExactlyOnce) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/10};
    fx.table->set_partitioner(std::make_shared<HashPartitioner>(
        /*P=*/8, std::vector<std::string>{"id"}));

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    // Collect every row id across all partition blobs — the union must be exactly
    // the table's rows, each appearing once (no drops, no duplication).
    std::set<std::string> ids;
    std::set<std::string> row_ids;
    for (const auto& partition : result.partitions) {
        const auto blob = fx.database->large_objects().read(partition->blob_ref());
        ASSERT_TRUE(blob);
        // A Parquet body: PAR1 at both ends, the row id in its own column.
        EXPECT_EQ(zetta::data::sniff_partition_body(*blob),
                  zetta::data::PartitionBodyFormat::Parquet);
        for (const zetta::data::PartitionRow& row : body_rows(*blob, partition->id())) {
            ASSERT_TRUE(row.doc.is_object());
            EXPECT_FALSE(row.doc.contains("_zid")) << "the id is the key, not a field";
            EXPECT_FALSE(row.id.empty());
            row_ids.insert(row.id);
            ids.insert(row.doc["id"].get<std::string>());
        }
    }
    EXPECT_EQ(row_ids.size(), 10u) << "every row keeps its own stable id";
    EXPECT_EQ(ids.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(ids.count("row-" + std::to_string(i))) << "missing row-" << i;
    }
}

// ---- the round trip: what was written is what reads back ----------------------

TEST(PartitionWriter, RowsRoundTripThroughTheBodyReadPath) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/12};
    fx.table->set_partitioner(std::make_shared<HashPartitioner>(
        /*P=*/4, std::vector<std::string>{"id"}));

    // What the table holds, before it is partitioned.
    std::map<std::string, nlohmann::json> expected;
    const zetta::storage::StorageEngine& store = fx.table->store();
    for (std::size_t i = 0; i < store.size(); ++i) {
        const auto row = store.at(i);
        ASSERT_TRUE(row.ok()) << row.error();
        expected.emplace(store.id_at(i), *row);
    }

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    std::map<std::string, nlohmann::json> round_tripped;
    for (const auto& partition : result.partitions) {
        // The catalog records the body's format, and the body is Parquet.
        ASSERT_TRUE(partition->stats().contains("format"));
        EXPECT_EQ(partition->stats()["format"].get<std::string>(), "parquet");

        const auto blob = fx.database->large_objects().read(partition->blob_ref());
        ASSERT_TRUE(blob);
        for (const zetta::data::PartitionRow& row :
             body_rows(*blob, partition->id())) {
            EXPECT_TRUE(round_tripped.emplace(row.id, row.doc).second)
                << "row " << row.id << " appears in more than one partition";
        }
    }

    // Every row, its id, and its value — identical, not merely equivalent.
    EXPECT_EQ(round_tripped, expected);
}

// ---- the body is named for its format -----------------------------------------

TEST(PartitionWriter, BodiesAreNamedParquet) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/5};
    fx.table->set_partitioner(std::make_shared<SinglePartitioner>());

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    std::set<std::string> names;
    for (const zetta::data::LargeObject& object : fx.database->large_objects().list()) {
        names.insert(object.name);
    }
    ASSERT_EQ(names.size(), 1u);
    const std::string& name = *names.begin();
    EXPECT_TRUE(name.ends_with(".parquet")) << name;
}

// ---- single partitioner: one blob covering the whole table --------------------

TEST(PartitionWriter, SinglePartitionerWritesOneFullRangeBlob) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/7};
    fx.table->set_partitioner(std::make_shared<SinglePartitioner>());

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table, members);
    ASSERT_TRUE(result.ok()) << result.error;

    ASSERT_EQ(result.partitions.size(), 1u);
    EXPECT_EQ(result.rows_written, 7u);
    const auto& partition = result.partitions.front();
    EXPECT_EQ(partition->row_range().start, 0u);
    EXPECT_EQ(partition->row_range().end, 7u);
    // SinglePartitioner places replicate-to-all: every member owns it.
    EXPECT_EQ(partition->owner_nodes(), members);
}

// ---- round-robin: the write path stamps the surrogate, distributes evenly ------

TEST(PartitionWriter, RoundRobinStampsSurrogateAndBalances) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/8};  // rows have no _seq; the write path stamps it
    fx.table->set_partitioner(std::make_shared<RoundRobinPartitioner>(/*P=*/4));

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    // 8 sequential ordinals over P=4 → exactly 2 rows in each of 4 partitions.
    ASSERT_EQ(result.partitions.size(), 4u);
    EXPECT_EQ(result.rows_written, 8u);
    for (const auto& partition : result.partitions) {
        EXPECT_EQ(partition->row_range().end - partition->row_range().start, 2u);
    }
    expect_ranges_tile(result.partitions, 8u);
}

// ---- placement: rendezvous shards owners across the members -------------------

TEST(PartitionWriter, RendezvousPlacementRecordsShardedOwners) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/20};
    fx.table->set_partitioner(std::make_shared<RendezvousPlacement>(
        /*partition_count=*/8, /*replication_factor=*/2));

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table, members);
    ASSERT_TRUE(result.ok()) << result.error;

    ASSERT_GE(result.partitions.size(), 1u);
    const std::set<std::string> member_set(members.begin(), members.end());
    for (const auto& partition : result.partitions) {
        // Each partition is placed onto exactly replication_factor=2 distinct
        // owners, all drawn from the cluster membership.
        ASSERT_EQ(partition->owner_nodes().size(), 2u);
        std::set<std::string> owners(partition->owner_nodes().begin(),
                                     partition->owner_nodes().end());
        EXPECT_EQ(owners.size(), 2u) << "owners are distinct";
        for (const auto& owner : owners) {
            EXPECT_TRUE(member_set.count(owner)) << owner << " is a cluster member";
        }
    }
}

// ---- placement default: a standalone write owns the partitions itself ----------

TEST(PartitionWriter, DefaultMembersUseThisNode) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/3};
    fx.table->set_partitioner(std::make_shared<SinglePartitioner>());

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);  // no members → this node
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.partitions.size(), 1u);
    EXPECT_EQ(result.partitions.front()->owner_nodes(),
              std::vector<std::string>{fx.node->node_id()});
}

// ---- guard: an unpartitioned table is rejected --------------------------------

TEST(PartitionWriter, UnpartitionedTableIsRejected) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/4};
    // No partitioner configured.
    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.error.empty());
    EXPECT_EQ(fx.node->dataset_partition_count(), 0u);
}

// ---- an empty table writes nothing, cleanly -----------------------------------

TEST(PartitionWriter, EmptyTableWritesNothing) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/0};
    fx.table->set_partitioner(std::make_shared<HashPartitioner>(
        /*P=*/4, std::vector<std::string>{"id"}));

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.partitions.size(), 0u);
    EXPECT_EQ(result.rows_written, 0u);
    EXPECT_EQ(fx.node->dataset_partition_count(), 0u);
}

// ---- the recorded partitions are visible in system.dataset_partitions ----------

TEST(PartitionWriter, RecordedPartitionsAreQueryable) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/9};
    fx.table->set_partitioner(std::make_shared<HashPartitioner>(
        /*P=*/4, std::vector<std::string>{"id"}));

    const PartitionWriteResult result = zetta::data::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    QueryService svc{*fx.node};
    auto sel = svc.run(
        "SELECT partition_id, table, blob_ref, row_count FROM "
        "system.dataset_partitions;",
        "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    EXPECT_EQ(sel.rows.size(), result.partitions.size());

    std::size_t row_count_sum = 0;
    for (const auto& row : sel.rows) {
        EXPECT_EQ(row["table"].get<std::string>(), fx.table->id());
        EXPECT_FALSE(row["blob_ref"].get<std::string>().empty());
        row_count_sum += row["row_count"].get<int>();
    }
    EXPECT_EQ(row_count_sum, 9u);
}
