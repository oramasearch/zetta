#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "temp_dir.hpp"
#include "zetta/cluster.hpp"
#include "zetta/database.hpp"
#include "zetta/node.hpp"
#include "zetta/query/postgres.hpp"
#include "zetta/query/service.hpp"
#include "zetta/replication.hpp"
#include "zetta/table.hpp"
#include "zetta/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::test::TempDir;
using zetta::query::CreateTableStmt;
using zetta::query::PostgresDialect;
using zetta::query::QueryService;
using zetta::query::SelectStmt;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

}  // namespace

// ---- parser ----------------------------------------------------------------

TEST(PostgresParser, ParsesQualifiedCreateTableWithColumns) {
    PostgresDialect pg;
    const auto parsed = pg.parse("CREATE TABLE shop.items (sku text, qty int);");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    ASSERT_EQ(parsed.statements.size(), 1u);
    const auto* stmt = std::get_if<CreateTableStmt>(&parsed.statements[0]);
    ASSERT_NE(stmt, nullptr);
    EXPECT_EQ(stmt->table.database, "shop");
    EXPECT_EQ(stmt->table.table, "items");
    ASSERT_EQ(stmt->columns.size(), 2u);
    EXPECT_EQ(stmt->columns[0].name, "sku");
    EXPECT_EQ(stmt->columns[1].name, "qty");
}

TEST(PostgresParser, ParsesSelectWithWhereAndComments) {
    PostgresDialect pg;
    const auto parsed = pg.parse(
        "-- a comment\nSELECT sku, qty FROM items WHERE sku = 'A1';");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    ASSERT_EQ(parsed.statements.size(), 1u);
    const auto* stmt = std::get_if<SelectStmt>(&parsed.statements[0]);
    ASSERT_NE(stmt, nullptr);
    EXPECT_FALSE(stmt->all);
    EXPECT_EQ(stmt->columns.size(), 2u);
    EXPECT_EQ(stmt->table.table, "items");
    ASSERT_TRUE(stmt->where.has_value());
    EXPECT_EQ(stmt->where->column, "sku");
    EXPECT_EQ(stmt->where->value.get<std::string>(), "A1");
}

TEST(PostgresParser, ReportsErrorOnGarbage) {
    PostgresDialect pg;
    const auto parsed = pg.parse("SELCT * FROM t;");
    EXPECT_FALSE(parsed.ok());
    EXPECT_FALSE(parsed.error.empty());
}

TEST(PostgresParser, ReportsErrorOnUnterminatedString) {
    PostgresDialect pg;
    const auto parsed = pg.parse("SELECT * FROM t WHERE a = 'oops;");
    EXPECT_FALSE(parsed.ok());
}

// ---- end-to-end through QueryService --------------------------------------

TEST(QueryService, DdlInsertSelectRoundTrip) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};

    auto r = svc.run(
        "CREATE DATABASE shop;"
        "CREATE TABLE shop.items (sku text, qty int);"
        "INSERT INTO shop.items (sku, qty) VALUES ('A1', 5), ('B2', 3);",
        /*current_database=*/"");
    ASSERT_TRUE(r.ok()) << r.error;
    EXPECT_EQ(r.messages.size(), 3u);
    EXPECT_EQ(node.database_count(), 1u);

    auto sel = svc.run("SELECT * FROM shop.items;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 2u);
    EXPECT_EQ(sel.rows[0]["sku"].get<std::string>(), "A1");
    EXPECT_EQ(sel.rows[0]["qty"].get<int>(), 5);

    node.stop();
}

TEST(QueryService, WhereFiltersAndProjects) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run("CREATE DATABASE d; CREATE TABLE d.t (a int, b int);", "");
    svc.run("INSERT INTO d.t (a, b) VALUES (1, 10), (2, 20), (1, 30);", "");

    auto sel = svc.run("SELECT b FROM d.t WHERE a = 1;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 2u);
    // projection keeps only requested columns
    EXPECT_TRUE(sel.rows[0].contains("b"));
    EXPECT_FALSE(sel.rows[0].contains("a"));

    node.stop();
}

TEST(QueryService, UnqualifiedNamesUseCurrentDatabase) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run("CREATE DATABASE app;", "");

    // current_database = "app", so unqualified "users" resolves there
    auto r = svc.run(
        "CREATE TABLE users (id int);"
        "INSERT INTO users (id) VALUES (7);"
        "SELECT * FROM users;",
        /*current_database=*/"app");
    ASSERT_TRUE(r.ok()) << r.error;
    ASSERT_EQ(r.rows.size(), 1u);
    EXPECT_EQ(r.rows[0]["id"].get<int>(), 7);

    node.stop();
}

TEST(QueryService, PersistsThroughQueries) {
    TempDir tmp;
    {
        Node a{config_for(tmp)};
        a.start();
        QueryService svc{a};
        auto r = svc.run(
            "CREATE DATABASE d; CREATE TABLE d.t (x int);"
            "INSERT INTO d.t (x) VALUES (1), (2), (3);",
            "");
        ASSERT_TRUE(r.ok()) << r.error;
        a.stop();
    }
    Node b{config_for(tmp)};
    b.start();
    QueryService svc{b};
    auto sel = svc.run("SELECT * FROM d.t;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    EXPECT_EQ(sel.rows.size(), 3u);
    b.stop();
}

TEST(QueryService, ReportsExecutionErrors) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};

    auto missing_db = svc.run("SELECT * FROM nope.t;", "");
    EXPECT_FALSE(missing_db.ok());

    svc.run("CREATE DATABASE d;", "");
    auto missing_table = svc.run("SELECT * FROM d.ghost;", "");
    EXPECT_FALSE(missing_table.ok());

    auto no_db_selected = svc.run("SELECT * FROM floating;", /*current=*/"");
    EXPECT_FALSE(no_db_selected.ok());

    node.stop();
}

// ---- zetta_nodes system table ---------------------------------------------

TEST(QueryService, SelectsClusterNodesSystemTable) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();

    const std::string node_id = zetta::new_uuid();
    zetta::ReplicationManager replication{node_id};
    replication.set(node_id, nlohmann::json{{"address", "127.0.0.1:43500"}});

    zetta::ClusterConfig config;
    config.listen_host = "127.0.0.1";
    config.listen_port = 43500;
    config.port_range = 1;
    zetta::Cluster cluster{node_id, config, &replication};
    cluster.start();

    QueryService svc{node, &cluster};
    // It lives in the `system` database.
    auto sel = svc.run("SELECT * FROM system.zetta_nodes;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);  // just this node
    EXPECT_EQ(sel.rows[0]["node_id"].get<std::string>(), node_id);
    EXPECT_TRUE(sel.rows[0]["is_self"].get<bool>());
    EXPECT_EQ(sel.rows[0]["entries"].get<int>(), 1);

    // The bare name still resolves (back-compat alias).
    EXPECT_TRUE(svc.run("SELECT * FROM zetta_nodes;", "").ok());

    // WHERE + projection work over the system table too.
    auto projected = svc.run(
        "SELECT node_id, address FROM system.zetta_nodes WHERE node_id = '" +
            node_id + "';",
        "");
    ASSERT_TRUE(projected.ok()) << projected.error;
    ASSERT_EQ(projected.rows.size(), 1u);
    EXPECT_TRUE(projected.rows[0].contains("address"));
    EXPECT_FALSE(projected.rows[0].contains("role"));

    // Unknown system table and writes to `system` are rejected.
    EXPECT_FALSE(svc.run("SELECT * FROM system.nope;", "").ok());
    EXPECT_FALSE(svc.run("CREATE DATABASE system;", "").ok());
    EXPECT_FALSE(svc.run("CREATE TABLE system.t (a int);", "").ok());

    cluster.stop();
    node.stop();
}

TEST(QueryService, ClusterNodesNeedsACluster) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};  // no cluster wired in
    auto sel = svc.run("SELECT * FROM zetta_nodes;", "");
    EXPECT_FALSE(sel.ok());
    node.stop();
}
