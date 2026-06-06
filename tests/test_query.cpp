#include <gtest/gtest.h>

#include <string>
#include <variant>

#include "temp_dir.hpp"
#include "zetta/database.hpp"
#include "zetta/node.hpp"
#include "zetta/query/postgres.hpp"
#include "zetta/query/service.hpp"
#include "zetta/table.hpp"

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
