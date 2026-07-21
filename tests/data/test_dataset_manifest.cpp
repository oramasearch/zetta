#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset.hpp"
#include "zetta/data/dataset_manifest.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

// #837 — **the partition manifest**. A scatter-gather SELECT planned over the
// `part:` entries that had reached the coordinating node, and an entry still in
// flight simply did not appear in the plan: the unreachable-owner guard never
// fired, because such a partition is not unreachable, it is *invisible*. The
// query answered short and looked healthy.
//
// The manifest is the table-level authority that closes it: how many partitions
// the finished dataset HAS, written once by the node that completed the write.
// Every reader compares what it can see against it and FAILS when short.
//
// The load-bearing test here is AnIncompleteCatalogFailsTheScatterGatherSelect,
// and it is deterministic on purpose — a `part:` entry is dropped directly
// rather than raced against a gossip round, so the property is asserted instead
// of sampled. See docs/data/partition-manifest.md.
namespace {

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::ReplicationManager;
using zetta::data::AddDatasetOptions;
using zetta::data::AddDatasetResult;
using zetta::data::DatasetManifestValue;
using zetta::data::add_dataset_rows;
using zetta::data::catalog_shortfall;
using zetta::data::parse_dataset_manifest_value;
using zetta::data::to_json;
using zetta::data::validate_dataset_manifest_value;
using zetta::query::QueryService;
using zetta::test::TempDir;

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

std::vector<nlohmann::json> sample_rows(std::size_t count = 40) {
    std::vector<nlohmann::json> rows;
    rows.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        rows.push_back(nlohmann::json{{"account", "acct-" + std::to_string(index)},
                                      {"bucket", static_cast<int>(index % 2)},
                                      {"amount", static_cast<int>(index)}});
    }
    return rows;
}

// The catalog entries this node can currently see for `table_id`.
std::vector<std::shared_ptr<zetta::data::DatasetPartition>> visible_partitions(
    Node& node, const std::string& table_id) {
    std::vector<std::shared_ptr<zetta::data::DatasetPartition>> found;
    for (const std::shared_ptr<zetta::data::DatasetPartition>& partition :
         node.dataset_partitions()) {
        if (partition && partition->table() == table_id) found.push_back(partition);
    }
    return found;
}

}  // namespace

// --- The boundary ----------------------------------------------------------

TEST(DatasetManifest, RoundTripsThroughItsSchema) {
    DatasetManifestValue value;
    value.table = "tbl-1";
    value.partitions = 8;
    value.rows = 540;
    value.bytes = 73412;
    value.partition_ids = {"a", "b"};
    value.created = "2026-07-18T00:00:00Z";

    const nlohmann::json document = to_json(value);
    std::string error;
    ASSERT_TRUE(validate_dataset_manifest_value(document, &error)) << error;
    const std::optional<DatasetManifestValue> parsed =
        parse_dataset_manifest_value(document, &error);
    ASSERT_TRUE(parsed) << error;
    EXPECT_EQ(*parsed, value);
}

TEST(DatasetManifest, OmitsThePartitionIdsWhenTheWriterDidNotKnowThemAll) {
    // A co-launched ingest agrees on COUNTS through the allreduce, never on a
    // list of ids — the absence must say "counts only", not "no partitions".
    DatasetManifestValue value;
    value.table = "tbl-1";
    value.partitions = 3;
    const nlohmann::json document = to_json(value);
    EXPECT_FALSE(document.contains("partition_ids"));
    const std::optional<DatasetManifestValue> parsed =
        parse_dataset_manifest_value(document);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->partitions, 3u);
    EXPECT_TRUE(parsed->partition_ids.empty());
}

TEST(DatasetManifest, RejectsAMalformedValue) {
    // A corrupt count would fail every read of a HEALTHY dataset, so it is
    // rejected at the boundary rather than adopted as the authority.
    std::string error;
    EXPECT_FALSE(parse_dataset_manifest_value(nlohmann::json{{"partitions", 4}},
                                              &error));  // no table
    EXPECT_FALSE(error.empty());
    EXPECT_FALSE(parse_dataset_manifest_value(
        nlohmann::json{{"table", "t"}, {"partitions", -1}}));
    EXPECT_FALSE(parse_dataset_manifest_value(
        nlohmann::json{{"table", ""}, {"partitions", 4}}));
    EXPECT_FALSE(parse_dataset_manifest_value(
        nlohmann::json{{"table", "t"}, {"partitions", 4}, {"unexpected", 1}}));
    EXPECT_FALSE(parse_dataset_manifest_value(nlohmann::json::array()));
}

TEST(DatasetManifest, ShortfallIsReportedOnlyWhenShort) {
    DatasetManifestValue manifest;
    manifest.table = "tbl-1";
    manifest.partitions = 8;

    EXPECT_TRUE(catalog_shortfall(manifest, 8).empty());
    // MORE entries than declared is not short — a table written twice
    // accumulates entries.
    EXPECT_TRUE(catalog_shortfall(manifest, 9).empty());
    const std::string reason = catalog_shortfall(manifest, 6);
    EXPECT_FALSE(reason.empty());
    EXPECT_NE(reason.find("6 of 8"), std::string::npos) << reason;
}

// --- Written by the write path ---------------------------------------------

TEST(DatasetManifest, PartitionedWriteRecordsIt) {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_TRUE(added.table);

    const std::optional<DatasetManifestValue>& manifest =
        added.table->partition_manifest();
    ASSERT_TRUE(manifest);
    EXPECT_EQ(manifest->table, added.table->id());
    EXPECT_EQ(manifest->partitions, added.partitions.size());
    EXPECT_EQ(manifest->rows, 40u);
    EXPECT_GT(manifest->bytes, 0u);
    EXPECT_EQ(manifest->partition_ids.size(), added.partitions.size());
    EXPECT_FALSE(manifest->created.empty());

    // It replicates under its OWN key — one writer, so it cannot lose a merge.
    const nlohmann::json snapshot = replication.snapshot();
    ASSERT_TRUE(snapshot.contains("mfst:" + added.table->id()));
    EXPECT_EQ(snapshot.at("mfst:" + added.table->id()).at("partitions").get<std::uint64_t>(),
              added.partitions.size());

    node.stop();
}

TEST(DatasetManifest, SurvivesARestart) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::size_t partitions = 0;

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
        partitions = added.partitions.size();
        node.tick();
        node.stop();
    }

    // A FRESH ReplicationManager: the manifest comes back from table.json, so a
    // restarted node knows the count before the first gossip round.
    ReplicationManager replication{node_id};
    Node node{config_for(dir), &replication};
    node.start();
    const auto database = node.find_database("datasets");
    ASSERT_TRUE(database);
    const auto table = database->find_table("cached");
    ASSERT_TRUE(table);
    ASSERT_TRUE(table->partition_manifest());
    EXPECT_EQ(table->partition_manifest()->partitions, partitions);
    EXPECT_EQ(table->partition_manifest()->rows, 40u);
    node.stop();
}

TEST(DatasetManifest, PartitionStatsCarryTheBodySchemaAndSize) {
    // A node must be able to plan and prune WITHOUT reading a body: the CRDT
    // replicates the metadata describing the blobs, never the blobs.
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_FALSE(added.partitions.empty());

    for (const auto& partition : added.partitions) {
        const nlohmann::json& stats = partition->stats();
        ASSERT_TRUE(stats.contains("schema")) << stats.dump();
        ASSERT_TRUE(stats.at("schema").is_array());
        EXPECT_GT(stats.at("bytes").get<std::uint64_t>(), 0u);
        std::set<std::string> columns;
        for (const nlohmann::json& column : stats.at("schema")) {
            EXPECT_TRUE(column.at("name").is_string());
            EXPECT_FALSE(column.at("type").get<std::string>().empty());
            EXPECT_TRUE(column.at("optional").is_boolean());
            columns.insert(column.at("name").get<std::string>());
        }
        EXPECT_TRUE(columns.contains("account")) << stats.dump();
        EXPECT_TRUE(columns.contains("amount")) << stats.dump();
    }
    node.stop();
}

// --- The point of the issue -------------------------------------------------

TEST(DatasetManifest, ACompleteCatalogStillAnswers) {
    // The no-false-positive direction: the check must not break a healthy read.
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;

    QueryService service{node};
    const auto counted = service.run(
        "SELECT bucket, count(*) FROM datasets.cached GROUP BY bucket;", "");
    ASSERT_TRUE(counted.ok()) << counted.error;
    ASSERT_EQ(counted.rows.size(), 2u);
    std::uint64_t total = 0;
    for (const nlohmann::json& row : counted.rows) {
        total += row.at("count").get<std::uint64_t>();
    }
    EXPECT_EQ(total, 40u);

    const auto all = service.run("SELECT * FROM datasets.cached;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(all.rows.size(), 40u);
    node.stop();
}

TEST(DatasetManifest, AnIncompleteCatalogFailsTheScatterGatherSelect) {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    const std::string table_id = added.table->id();
    const std::size_t complete = visible_partitions(node, table_id).size();
    ASSERT_GE(complete, 2u) << "the dataset needs more than one partition";

    // Construct the hole directly: the manifest stays, one `part:` entry goes
    // away. That is exactly the state a coordinator is in while an entry is
    // still in flight — and it is invisible, not unreachable, so nothing else
    // in the read path would notice.
    ASSERT_TRUE(node.drop_dataset_partition(
        visible_partitions(node, table_id).front()->id()));
    ASSERT_EQ(visible_partitions(node, table_id).size(), complete - 1);
    ASSERT_TRUE(added.table->partition_manifest());
    EXPECT_EQ(added.table->partition_manifest()->partitions, complete);

    QueryService service{node};
    const auto counted = service.run(
        "SELECT bucket, count(*) FROM datasets.cached GROUP BY bucket;", "");
    EXPECT_FALSE(counted.ok())
        << "a scatter-gather SELECT answered from a SUBSET of the partitions — "
           "count(*) came back "
        << (counted.rows.empty() ? std::string{"(no rows)"}
                                 : counted.rows[0].dump());
    EXPECT_NE(counted.error.find("partition catalog is incomplete"),
              std::string::npos)
        << counted.error;

    // A plain SELECT * is failed for the same reason — the guard is at plan
    // time, not per-aggregate.
    const auto all = service.run("SELECT * FROM datasets.cached;", "");
    EXPECT_FALSE(all.ok());

    node.stop();
}

TEST(DatasetManifest, AnIncompleteCatalogReportsTheDatasetDegraded) {
    // system.datasets says so plainly rather than under-reporting `rows`: an
    // agent reading `rows` must be able to tell a total from a fraction.
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    Node node{config_for(dir), &replication};
    node.start();

    AddDatasetOptions options;
    options.table_name = "cached";
    options.partition_count = 4;
    options.partition_only = true;
    const AddDatasetResult added = add_dataset_rows(node, sample_rows(), options);
    ASSERT_TRUE(added.ok()) << added.error;
    const std::string table_id = added.table->id();
    const std::size_t complete = visible_partitions(node, table_id).size();

    for (const zetta::data::DatasetSummary& dataset : node.datasets()) {
        if (dataset.table_id != table_id) continue;
        EXPECT_FALSE(dataset.degraded);
        EXPECT_EQ(dataset.expected_partitions, complete);
        EXPECT_EQ(dataset.expected_rows, 40u);
    }

    ASSERT_TRUE(node.drop_dataset_partition(
        visible_partitions(node, table_id).front()->id()));

    bool seen = false;
    for (const zetta::data::DatasetSummary& dataset : node.datasets()) {
        if (dataset.table_id != table_id) continue;
        seen = true;
        EXPECT_TRUE(dataset.degraded);
        EXPECT_FALSE(dataset.degraded_reason.empty());
        EXPECT_EQ(dataset.expected_partitions, complete);
        EXPECT_LT(dataset.rows, 40u) << "the visible rows really are a fraction";
    }
    EXPECT_TRUE(seen);
    node.stop();
}
