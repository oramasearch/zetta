// Saved connections (#827, docs/data/connector-query.md): the replicate-to-all
// credential store, its boundary, and — the load-bearing property — that the
// token reaches the store and NOWHERE else.
#include <gtest/gtest.h>

#include <string>

#include "temp_dir.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

namespace {

zetta::connect::ConnectorConfig snowflake_config() {
    zetta::connect::ConnectorConfig config;
    config.type = "snowflake";
    config.account = "ab12345.us-east-1";
    config.token = "the-secret-programmatic-access-token";
    config.warehouse = "COMPUTE_WH";
    config.database = "ANALYTICS";
    config.schema = "PUBLIC";
    return config;
}

struct ConnectionStoreFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};

    void SetUp() override { ASSERT_TRUE(node.start()); }
    void TearDown() override { node.stop(); }
};

TEST_F(ConnectionStoreFixture, SavesAndResolvesAConnectionWithItsToken) {
    EXPECT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());

    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "warehouse");
    EXPECT_EQ(found->config.account, "ab12345.us-east-1");
    // The resolution path — the one caller allowed to see the credential.
    EXPECT_EQ(found->config.token, "the-secret-programmatic-access-token");
    EXPECT_FALSE(found->created.empty());
}

TEST_F(ConnectionStoreFixture, LivesInAReplicateToAllTableSoEveryNodeHoldsIt) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());

    const auto database =
        node.find_database(zetta::connect::kConnectionDatabase);
    ASSERT_TRUE(database);
    const auto table = database->find_table(zetta::connect::kConnectionTable);
    ASSERT_TRUE(table);
    // The whole point of storing it as a plain table: the default strategy puts
    // the row on every member, which is what lets each node fetch its own
    // stripe with its own copy of the credential.
    EXPECT_TRUE(table->replication_strategy().is_default());  // ReplicateToAll
    EXPECT_FALSE(table->partitioner());
    EXPECT_EQ(table->row_count(), 1u);
}

TEST_F(ConnectionStoreFixture, SavingTheSameNameReplacesRatherThanDuplicates) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());
    zetta::connect::ConnectorConfig rotated = snowflake_config();
    rotated.token = "the-rotated-token";
    rotated.warehouse = "BIG_WH";
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", rotated).empty());

    EXPECT_EQ(zetta::connect::list_connections(node).size(), 1u);
    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->config.token, "the-rotated-token");
    EXPECT_EQ(found->config.warehouse, "BIG_WH");
}

TEST_F(ConnectionStoreFixture, ListsAndRemoves) {
    ASSERT_TRUE(zetta::connect::save_connection(node, "first", snowflake_config())
                    .empty());
    ASSERT_TRUE(zetta::connect::save_connection(node, "second", snowflake_config())
                    .empty());
    EXPECT_EQ(zetta::connect::list_connections(node).size(), 2u);

    EXPECT_TRUE(zetta::connect::remove_connection(node, "first"));
    EXPECT_FALSE(zetta::connect::remove_connection(node, "first"));
    EXPECT_FALSE(zetta::connect::find_connection(node, "first").has_value());
    EXPECT_EQ(zetta::connect::list_connections(node).size(), 1u);
}

TEST_F(ConnectionStoreFixture, RejectsABadNameOrAMissingToken) {
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "", snowflake_config()).empty());
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "-leading", snowflake_config())
            .empty());
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "has space", snowflake_config())
            .empty());
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "has;semicolon", snowflake_config())
            .empty());
    EXPECT_FALSE(zetta::connect::save_connection(node, std::string(65, 'a'),
                                                 snowflake_config())
                     .empty());

    zetta::connect::ConnectorConfig tokenless = snowflake_config();
    tokenless.token.clear();
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "warehouse", tokenless).empty());

    EXPECT_TRUE(zetta::connect::list_connections(node).empty());
}

// The security contract, asserted rather than assumed: the credential is in the
// store (every node needs it) and in nothing a caller can read back.
TEST_F(ConnectionStoreFixture, NoReadSurfaceEverCarriesTheToken) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());
    const std::string secret = "the-secret-programmatic-access-token";

    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found.has_value());
    const std::string redacted = found->to_redacted_json().dump();
    EXPECT_EQ(redacted.find(secret), std::string::npos) << redacted;
    EXPECT_EQ(found->config.to_json().dump().find(secret), std::string::npos);
    // …and the one serialization that DOES carry it is the store's own.
    EXPECT_NE(found->config.to_storage_json().dump().find(secret),
              std::string::npos);

    zetta::query::QueryService service(node);
    const auto view = service.run("SELECT * FROM system.connections;", "");
    ASSERT_TRUE(view.error.empty()) << view.error;
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows.front().dump().find(secret), std::string::npos)
        << view.rows.front().dump();
    EXPECT_EQ(view.rows.front().at("name"), "warehouse");
    EXPECT_EQ(view.rows.front().at("account"), "ab12345.us-east-1");
}

}  // namespace
