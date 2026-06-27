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
using zetta::query::AggregateFunc;
using zetta::query::CompareOp;
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

// ---- WHERE comparison operators (parser + row path) ------------------------

TEST(PostgresParser, ParsesComparisonOperators) {
    PostgresDialect pg;
    const auto check = [&](const std::string& sql, CompareOp expected) {
        const auto parsed = pg.parse(sql);
        ASSERT_TRUE(parsed.ok()) << parsed.error;
        const auto* stmt = std::get_if<SelectStmt>(&parsed.statements[0]);
        ASSERT_NE(stmt, nullptr);
        ASSERT_TRUE(stmt->where.has_value());
        EXPECT_EQ(stmt->where->op, expected);
    };
    check("SELECT * FROM t WHERE a = 1;", CompareOp::Equal);
    check("SELECT * FROM t WHERE a != 1;", CompareOp::NotEqual);
    check("SELECT * FROM t WHERE a <> 1;", CompareOp::NotEqual);
    check("SELECT * FROM t WHERE a < 1;", CompareOp::Less);
    check("SELECT * FROM t WHERE a <= 1;", CompareOp::LessEqual);
    check("SELECT * FROM t WHERE a > 1;", CompareOp::Greater);
    check("SELECT * FROM t WHERE a >= 1;", CompareOp::GreaterEqual);
}

TEST(QueryService, WhereComparisonsOnRowPath) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run("CREATE DATABASE d; CREATE TABLE d.t (a int);", "");
    svc.run("INSERT INTO d.t (a) VALUES (1), (2), (3), (4);", "");

    EXPECT_EQ(svc.run("SELECT a FROM d.t WHERE a >= 3;", "").rows.size(), 2u);
    EXPECT_EQ(svc.run("SELECT a FROM d.t WHERE a < 2;", "").rows.size(), 1u);
    EXPECT_EQ(svc.run("SELECT a FROM d.t WHERE a != 2;", "").rows.size(), 3u);
    EXPECT_EQ(svc.run("SELECT a FROM d.t WHERE a <= 2;", "").rows.size(), 2u);

    node.stop();
}

// ---- GROUP BY through the DDF engine ---------------------------------------

TEST(PostgresParser, ParsesGroupByWithAggregates) {
    PostgresDialect pg;
    const auto parsed = pg.parse(
        "SELECT segment, SUM(amount), COUNT(*) FROM events "
        "WHERE amount > 0 GROUP BY segment;");
    ASSERT_TRUE(parsed.ok()) << parsed.error;
    const auto* stmt = std::get_if<SelectStmt>(&parsed.statements[0]);
    ASSERT_NE(stmt, nullptr);
    EXPECT_TRUE(stmt->grouped());
    ASSERT_EQ(stmt->group_by.size(), 1u);
    EXPECT_EQ(stmt->group_by[0], "segment");
    ASSERT_EQ(stmt->items.size(), 3u);
    EXPECT_FALSE(stmt->items[0].aggregate.has_value());  // key passthrough
    EXPECT_EQ(stmt->items[0].column, "segment");
    ASSERT_TRUE(stmt->items[1].aggregate.has_value());
    EXPECT_EQ(stmt->items[1].aggregate->func, AggregateFunc::Sum);
    EXPECT_EQ(stmt->items[1].aggregate->column, "amount");
    ASSERT_TRUE(stmt->items[2].aggregate.has_value());
    EXPECT_EQ(stmt->items[2].aggregate->func, AggregateFunc::Count);
    EXPECT_TRUE(stmt->items[2].aggregate->column.empty());  // COUNT(*)
    ASSERT_TRUE(stmt->where.has_value());
    EXPECT_EQ(stmt->where->op, CompareOp::Greater);
}

TEST(PostgresParser, RejectsAggregateWithoutGroupBy) {
    PostgresDialect pg;
    EXPECT_FALSE(pg.parse("SELECT SUM(amount) FROM t;").ok());
    EXPECT_FALSE(pg.parse("SELECT * FROM t GROUP BY a;").ok());
}

TEST(QueryService, GroupByAggregatesThroughDdfEngine) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run(
        "CREATE DATABASE churn;"
        "CREATE TABLE churn.events (segment int, amount int);",
        "");
    svc.run(
        "INSERT INTO churn.events (segment, amount) VALUES "
        "(1, 10), (1, 5), (2, 100), (2, 200), (1, 20), (3, 7), (3, 3), (2, 50);",
        "");

    // The acceptance: a SELECT … GROUP BY runs through the DDF engine and returns
    // correct per-group rows (ascending key order).
    auto sel = svc.run(
        "SELECT segment, SUM(amount), COUNT(*) FROM churn.events GROUP BY segment;",
        "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 3u);
    EXPECT_DOUBLE_EQ(sel.rows[0]["segment"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(sel.rows[0]["sum_amount"].get<double>(), 35.0);
    EXPECT_DOUBLE_EQ(sel.rows[0]["count"].get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(sel.rows[1]["segment"].get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(sel.rows[1]["sum_amount"].get<double>(), 350.0);
    EXPECT_DOUBLE_EQ(sel.rows[1]["count"].get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(sel.rows[2]["segment"].get<double>(), 3.0);
    EXPECT_DOUBLE_EQ(sel.rows[2]["sum_amount"].get<double>(), 10.0);
    EXPECT_DOUBLE_EQ(sel.rows[2]["count"].get<double>(), 2.0);
    // Integer columns come back as JSON integers (not 1.0 / 35.0).
    EXPECT_TRUE(sel.rows[0]["segment"].is_number_integer());
    EXPECT_TRUE(sel.rows[0]["sum_amount"].is_number_integer());
    EXPECT_TRUE(sel.rows[0]["count"].is_number_integer());

    node.stop();
}

TEST(QueryService, GroupByWithWhereFiltersThroughDdf) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run(
        "CREATE DATABASE churn;"
        "CREATE TABLE churn.events (segment int, amount int);",
        "");
    // amount as a float column so SUM/MIN/MAX come back as doubles.
    svc.run(
        "INSERT INTO churn.events (segment, amount) VALUES "
        "(1, 10.0), (1, 5.0), (2, 100.0), (2, 8.0), (3, 4.0);",
        "");

    // WHERE amount >= 10 drops (1,5), (2,8), (3,4) — segment 3 vanishes entirely.
    auto sel = svc.run(
        "SELECT segment, SUM(amount) AS total, MIN(amount), MAX(amount) "
        "FROM churn.events WHERE amount >= 10 GROUP BY segment;",
        "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 2u);
    EXPECT_DOUBLE_EQ(sel.rows[0]["segment"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(sel.rows[0]["total"].get<double>(), 10.0);          // alias
    EXPECT_DOUBLE_EQ(sel.rows[0]["min_amount"].get<double>(), 10.0);
    EXPECT_DOUBLE_EQ(sel.rows[0]["max_amount"].get<double>(), 10.0);
    EXPECT_DOUBLE_EQ(sel.rows[1]["segment"].get<double>(), 2.0);
    EXPECT_DOUBLE_EQ(sel.rows[1]["total"].get<double>(), 100.0);
    // A float column's sum is emitted as a JSON float.
    EXPECT_TRUE(sel.rows[0]["total"].is_number_float());

    node.stop();
}

TEST(QueryService, GroupByAvgAndDistinctKeys) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run("CREATE DATABASE d; CREATE TABLE d.t (g int, v int);", "");
    svc.run("INSERT INTO d.t (g, v) VALUES (1, 10), (1, 20), (2, 9);", "");

    // AVG (mean) always returns a float, even over an integer column.
    auto avg = svc.run("SELECT g, AVG(v) AS avg_value FROM d.t GROUP BY g;", "");
    ASSERT_TRUE(avg.ok()) << avg.error;
    ASSERT_EQ(avg.rows.size(), 2u);
    EXPECT_DOUBLE_EQ(avg.rows[0]["avg_value"].get<double>(), 15.0);
    EXPECT_DOUBLE_EQ(avg.rows[1]["avg_value"].get<double>(), 9.0);

    // A GROUP BY with no aggregate returns the distinct keys (ascending).
    auto distinct = svc.run("SELECT g FROM d.t GROUP BY g;", "");
    ASSERT_TRUE(distinct.ok()) << distinct.error;
    ASSERT_EQ(distinct.rows.size(), 2u);
    EXPECT_DOUBLE_EQ(distinct.rows[0]["g"].get<double>(), 1.0);
    EXPECT_DOUBLE_EQ(distinct.rows[1]["g"].get<double>(), 2.0);

    node.stop();
}

TEST(QueryService, GroupByOnEmptyTableReturnsNoRows) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run("CREATE DATABASE d; CREATE TABLE d.t (segment int, amount int);", "");

    auto sel = svc.run(
        "SELECT segment, SUM(amount) FROM d.t GROUP BY segment;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    EXPECT_EQ(sel.rows.size(), 0u);
    // The output columns are still reported.
    ASSERT_EQ(sel.columns.size(), 2u);
    EXPECT_EQ(sel.columns[0], "segment");
    EXPECT_EQ(sel.columns[1], "sum_amount");

    node.stop();
}

TEST(QueryService, GroupByRejectsBadQueries) {
    TempDir tmp;
    Node node{config_for(tmp)};
    node.start();
    QueryService svc{node};
    svc.run("CREATE DATABASE d; CREATE TABLE d.t (region text, amount int);", "");
    svc.run("INSERT INTO d.t (region, amount) VALUES ('US', 5), ('EU', 3);", "");

    // The grouping key is not a dense numeric column (DDF is numeric only).
    EXPECT_FALSE(svc.run(
        "SELECT region, SUM(amount) FROM d.t GROUP BY region;", "").ok());

    // A non-aggregated select column that is not the grouping key.
    svc.run("CREATE TABLE d.n (g int, v int);", "");
    svc.run("INSERT INTO d.n (g, v) VALUES (1, 2);", "");
    EXPECT_FALSE(svc.run("SELECT g, v FROM d.n GROUP BY g;", "").ok());

    // A non-numeric WHERE predicate in a grouped query.
    EXPECT_FALSE(svc.run(
        "SELECT g, SUM(v) FROM d.n WHERE g = 'x' GROUP BY g;", "").ok());

    // GROUP BY over a system table is not supported.
    EXPECT_FALSE(svc.run(
        "SELECT kind, COUNT(*) FROM system.zetta_jobs GROUP BY kind;", "").ok());

    node.stop();
}
