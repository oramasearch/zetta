#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/solutions/snapshot.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using nlohmann::json;
using zetta::solutions::AsOfJoinSpec;
using zetta::solutions::SnapshotSpec;

zetta::NodeConfig node_config(const zetta::test::TempDir& directory) {
    return zetta::NodeConfig{directory.path().string(), /*preflight=*/false};
}

SnapshotSpec numeric_snapshot_spec() {
    return SnapshotSpec{.database = "features",
                        .source_table = "events",
                        .target_table = "account_snapshots",
                        .entity_column = "account",
                        .time_column = "observed_at",
                        .as_of = 20.0};
}

}  // namespace

TEST(Snapshot, MaterializesLatestRowPerEntityAtOrBeforeCutoff) {
    zetta::test::TempDir directory;
    zetta::Node node{node_config(directory)};
    ASSERT_TRUE(node.start());
    const auto database = node.create_database("features");
    ASSERT_TRUE(database);
    const auto events = database->create_table("events");
    ASSERT_TRUE(events);
    events->add_row({{"account", "a"}, {"observed_at", 10}, {"spend", 1}});
    events->add_row({{"account", "a"}, {"observed_at", 20}, {"spend", 2}});
    events->add_row({{"account", "a"}, {"observed_at", 30}, {"spend", 99}});
    events->add_row({{"account", "b"}, {"observed_at", 5}, {"spend", 4}});

    const zetta::solutions::SnapshotResult result =
        zetta::solutions::materialize_snapshot(node, numeric_snapshot_spec());
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.source_rows, 4u);
    EXPECT_EQ(result.materialized_rows, 2u);
    EXPECT_EQ(result.future_rows_excluded, 1u);
    ASSERT_EQ(result.table->row_count(), 2u);

    std::map<std::string, json> by_entity;
    for (std::size_t index = 0; index < result.table->store().size(); ++index) {
        const auto row = result.table->store().at(index);
        ASSERT_TRUE(row) << row.error();
        by_entity.emplace(row->at("entity_id").get<std::string>(), *row);
    }
    ASSERT_EQ(by_entity.size(), 2u);
    EXPECT_EQ(by_entity.at("a").at("spend"), 2);
    EXPECT_EQ(by_entity.at("a").at("_source_as_of"), 20);
    EXPECT_EQ(by_entity.at("a").at("as_of"), 20.0);
    EXPECT_EQ(by_entity.at("b").at("spend"), 4);

    const zetta::solutions::SnapshotResult retry =
        zetta::solutions::materialize_snapshot(node, numeric_snapshot_spec());
    ASSERT_TRUE(retry.ok()) << retry.error;
    EXPECT_EQ(retry.materialized_rows, 0u);
    EXPECT_EQ(retry.existing_rows, 2u);
    EXPECT_EQ(retry.table->row_count(), 2u);
    node.stop();
}

TEST(Snapshot, ReplicatesMaterializedTableThroughTheNodeCatalog) {
    zetta::test::TempDir directory_a;
    zetta::test::TempDir directory_b;
    zetta::cluster::ReplicationManager replication_a{zetta::util::new_uuid()};
    zetta::cluster::ReplicationManager replication_b{zetta::util::new_uuid()};
    zetta::Node node_a{node_config(directory_a), &replication_a};
    zetta::Node node_b{node_config(directory_b), &replication_b};
    ASSERT_TRUE(node_a.start());
    ASSERT_TRUE(node_b.start());
    const auto database = node_a.create_database("features");
    ASSERT_TRUE(database);
    const auto events = database->create_table("events");
    ASSERT_TRUE(events);
    events->add_row({{"account", "a"}, {"observed_at", 10}, {"spend", 7}});

    const zetta::solutions::SnapshotResult result =
        zetta::solutions::materialize_snapshot(node_a, numeric_snapshot_spec());
    ASSERT_TRUE(result.ok()) << result.error;
    node_a.tick();
    replication_b.merge(replication_a.save());
    node_b.tick();

    const auto replicated_database = node_b.find_database("features");
    ASSERT_TRUE(replicated_database);
    const auto replicated_table =
        replicated_database->find_table("account_snapshots");
    ASSERT_TRUE(replicated_table);
    EXPECT_EQ(replicated_table->row_count(), 1u);
    const auto row = replicated_table->store().at(0);
    ASSERT_TRUE(row) << row.error();
    EXPECT_EQ(row->at("entity_id"), "a");
    EXPECT_EQ(row->at("spend"), 7);

    node_a.stop();
    node_b.stop();
}

TEST(Snapshot, PreservesDistinctIntegerEntityKeysBeyondDoublePrecision) {
    zetta::test::TempDir directory;
    zetta::Node node{node_config(directory)};
    ASSERT_TRUE(node.start());
    const auto database = node.create_database("features");
    ASSERT_TRUE(database);
    const auto events = database->create_table("events");
    ASSERT_TRUE(events);
    constexpr std::uint64_t first = 9007199254740992ULL;
    constexpr std::uint64_t second = 9007199254740993ULL;
    events->add_row(
        {{"account", first}, {"observed_at", 10}, {"spend", 1}});
    events->add_row(
        {{"account", second}, {"observed_at", 10}, {"spend", 2}});

    const zetta::solutions::SnapshotResult result =
        zetta::solutions::materialize_snapshot(node, numeric_snapshot_spec());
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.table->row_count(), 2u);
    const auto first_row = result.table->store().at(0);
    const auto second_row = result.table->store().at(1);
    ASSERT_TRUE(first_row) << first_row.error();
    ASSERT_TRUE(second_row) << second_row.error();
    EXPECT_NE(first_row->at("entity_id"), second_row->at("entity_id"));
    node.stop();
}

TEST(AsOfJoin, ChoosesOnlyTheLatestSnapshotAtOrBeforeEachDecision) {
    const std::vector<json> decisions = {
        {{"account", "a"}, {"prediction_time", 15}, {"request", 1}},
        {{"account", "a"}, {"prediction_time", 25}, {"request", 2}},
        {{"account", "b"}, {"prediction_time", 10}, {"request", 3}}};
    const std::vector<json> snapshots = {
        {{"entity_id", "a"}, {"as_of", 10}, {"risk", 0.1}},
        {{"entity_id", "a"}, {"as_of", 20}, {"risk", 0.2}},
        {{"entity_id", "a"}, {"as_of", 30}, {"risk", 0.9}},
        {{"entity_id", "b"}, {"as_of", 11}, {"risk", 0.8}}};
    const AsOfJoinSpec spec{.left_entity_column = "account",
                            .left_time_column = "prediction_time"};

    const zetta::solutions::AsOfJoinResult joined =
        zetta::solutions::as_of_join(decisions, snapshots, spec);
    ASSERT_TRUE(joined.ok()) << joined.error;
    ASSERT_EQ(joined.rows.size(), 3u);
    EXPECT_EQ(joined.matched, 2u);
    EXPECT_EQ(joined.unmatched, 1u);
    EXPECT_EQ(joined.rows[0].at("snapshot_as_of"), 10);
    EXPECT_EQ(joined.rows[0].at("snapshot_risk"), 0.1);
    EXPECT_LE(joined.rows[0].at("snapshot_as_of").get<double>(),
              joined.rows[0].at("prediction_time").get<double>());
    EXPECT_EQ(joined.rows[1].at("snapshot_as_of"), 20);
    EXPECT_EQ(joined.rows[1].at("snapshot_risk"), 0.2);
    EXPECT_LE(joined.rows[1].at("snapshot_as_of").get<double>(),
              joined.rows[1].at("prediction_time").get<double>());
    EXPECT_FALSE(joined.rows[2].contains("snapshot_as_of"));
}

TEST(SnapshotSchema, RefusesMalformedCutoffAtTheBoundary) {
    json document = numeric_snapshot_spec().to_json();
    document["as_of"] = "tomorrow";
    std::string error;
    EXPECT_FALSE(SnapshotSpec::from_json(document, &error));
    EXPECT_NE(error.find("/as_of"), std::string::npos) << error;
}
