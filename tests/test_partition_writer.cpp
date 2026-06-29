#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/database.hpp"
#include "zetta/dataset_partition.hpp"
#include "zetta/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/partition_writer.hpp"
#include "zetta/partitioner.hpp"
#include "zetta/query/service.hpp"
#include "zetta/rendezvous_placement.hpp"
#include "zetta/replication_strategy.hpp"
#include "zetta/table.hpp"

using zetta::Database;
using zetta::HashPartitioner;
using zetta::Node;
using zetta::NodeConfig;
using zetta::PartitionWriteResult;
using zetta::RendezvousPlacement;
using zetta::ReplicationStrategy;
using zetta::RoundRobinPartitioner;
using zetta::SinglePartitioner;
using zetta::Table;
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

// Number of lines (rows) in a partition's serialized JSONL blob.
std::size_t blob_line_count(const std::string& blob) {
    std::size_t lines = 0;
    std::istringstream in(blob);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) ++lines;
    }
    return lines;
}

// Assert the partition row ranges tile [0, total) contiguously with no gaps or
// overlaps, in the order the catalog returns them.
void expect_ranges_tile(
    const std::vector<std::shared_ptr<zetta::DatasetPartition>>& partitions,
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

    const PartitionWriteResult result = zetta::write_partitioned(
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
        const std::size_t lines = blob_line_count(*blob);
        // The catalog range size matches the blob's row count.
        EXPECT_EQ(lines, partition->row_range().end - partition->row_range().start);
        total_blob_rows += lines;
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

    const PartitionWriteResult result = zetta::write_partitioned(
        *fx.node, *fx.database, *fx.table);
    ASSERT_TRUE(result.ok()) << result.error;

    // Collect every row id across all partition blobs — the union must be exactly
    // the table's rows, each appearing once (no drops, no duplication).
    std::set<std::string> ids;
    for (const auto& partition : result.partitions) {
        const auto blob = fx.database->large_objects().read(partition->blob_ref());
        ASSERT_TRUE(blob);
        std::istringstream in(*blob);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty()) continue;
            const auto doc = nlohmann::json::parse(line, nullptr, false);
            ASSERT_FALSE(doc.is_discarded());
            ASSERT_TRUE(doc.contains("_zid"));      // the JsonlStore shape
            ASSERT_TRUE(doc.contains("doc"));
            ids.insert(doc["doc"]["id"].get<std::string>());
        }
    }
    EXPECT_EQ(ids.size(), 10u);
    for (int i = 0; i < 10; ++i) {
        EXPECT_TRUE(ids.count("row-" + std::to_string(i))) << "missing row-" << i;
    }
}

// ---- single partitioner: one blob covering the whole table --------------------

TEST(PartitionWriter, SinglePartitionerWritesOneFullRangeBlob) {
    TempDir tmp;
    Fixture fx{tmp, /*count=*/7};
    fx.table->set_partitioner(std::make_shared<SinglePartitioner>());

    const std::vector<std::string> members{"node-a", "node-b", "node-c"};
    const PartitionWriteResult result = zetta::write_partitioned(
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

    const PartitionWriteResult result = zetta::write_partitioned(
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
    const PartitionWriteResult result = zetta::write_partitioned(
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

    const PartitionWriteResult result = zetta::write_partitioned(
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
    const PartitionWriteResult result = zetta::write_partitioned(
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

    const PartitionWriteResult result = zetta::write_partitioned(
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

    const PartitionWriteResult result = zetta::write_partitioned(
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
