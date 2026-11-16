// SPDX-License-Identifier: MIT

// Fine-grained scoped grants (FEAT-9 / #747, epic #743 item 4): the object tiers
// on Scope/ResourceKind (table / column / blob / MCP tool), the object-covering
// decision, the ScopedGrant store on a role, and the executor's table denial +
// column masking + write rejection driven off a user's effective object grants.
// Admin / an unscoped (flat) right / the trusted-local session bypass.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/grant.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::CapabilityDomain;
using zetta::governance::ColumnAccess;
using zetta::governance::Operation;
using zetta::governance::Resource;
using zetta::governance::ResourceKind;
using zetta::governance::ScopedGrant;
using zetta::query::QueryResult;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

const Operation kRead{CapabilityDomain::Data, "read"};
const Operation kWrite{CapabilityDomain::Data, "write"};

}  // namespace

// ===== object_covers: the deny-by-default object-covering decision ===========

TEST(ScopedGrantCovering, DatabaseGrantCoversItsTablesAndColumns) {
    const Resource db{ResourceKind::Database, "sales"};
    EXPECT_TRUE(object_covers(db, Resource::table("sales", "customers")));
    EXPECT_TRUE(object_covers(
        db, Resource::column("sales", "customers", {"ssn"})));
    // A different database is not covered.
    EXPECT_FALSE(object_covers(db, Resource::table("hr", "customers")));
}

TEST(ScopedGrantCovering, TableGrantCoversTheWholeTableOnly) {
    const Resource table = Resource::table("sales", "customers");
    EXPECT_TRUE(object_covers(table, Resource::table("sales", "customers")));
    EXPECT_TRUE(object_covers(
        table, Resource::column("sales", "customers", {"ssn"})));
    EXPECT_FALSE(object_covers(table, Resource::table("sales", "orders")));
}

TEST(ScopedGrantCovering, ColumnGrantCoversOnlyItsColumns) {
    const Resource cols = Resource::column("sales", "customers", {"id", "name"});
    // The table is reachable (a column grant implies you can touch the table).
    EXPECT_TRUE(object_covers(cols, Resource::table("sales", "customers")));
    // Only the listed columns are covered.
    EXPECT_TRUE(object_covers(
        cols, Resource::column("sales", "customers", {"name"})));
    EXPECT_FALSE(object_covers(
        cols, Resource::column("sales", "customers", {"ssn"})));
}

TEST(ScopedGrantCovering, BlobGrantIsAPrefix) {
    const Resource prefix = Resource::blob("doc/");
    EXPECT_TRUE(object_covers(prefix, Resource::blob("doc/report.pdf")));
    EXPECT_TRUE(object_covers(prefix, Resource::blob("doc/")));
    EXPECT_FALSE(object_covers(prefix, Resource::blob("img/logo.png")));
}

// A blob grant matches on a `/` path boundary, never a bare text prefix (#767):
// `report` must not admit `report-secret-2026`, nor `2026/01` admit
// `2026/01-executive-comp/...`.
TEST(ScopedGrantCovering, BlobGrantMatchesOnAPathSegmentBoundary) {
    const Resource report = Resource::blob("report");
    EXPECT_TRUE(object_covers(report, Resource::blob("report")));
    EXPECT_TRUE(object_covers(report, Resource::blob("report/q1.pdf")));
    EXPECT_FALSE(object_covers(report, Resource::blob("report-secret-2026")));
    EXPECT_FALSE(object_covers(report, Resource::blob("reports")));
    EXPECT_FALSE(object_covers(report, Resource::blob("repo")));

    const Resource month = Resource::blob("2026/01");
    EXPECT_TRUE(object_covers(month, Resource::blob("2026/01")));
    EXPECT_TRUE(object_covers(month, Resource::blob("2026/01/payroll.csv")));
    EXPECT_FALSE(object_covers(
        month, Resource::blob("2026/01-executive-comp/salaries.csv")));
    EXPECT_FALSE(object_covers(month, Resource::blob("2026/010")));

    // A trailing-slash grant covers everything under it, not its sibling names.
    const Resource year = Resource::blob("2026/");
    EXPECT_TRUE(object_covers(year, Resource::blob("2026/01-executive-comp/x")));
    EXPECT_FALSE(object_covers(year, Resource::blob("2026")));
    EXPECT_FALSE(object_covers(year, Resource::blob("2026-archive/x")));

    // An empty blob grant covers nothing (a flat blob:read grants every blob).
    EXPECT_FALSE(object_covers(Resource::blob(""), Resource::blob("anything")));
    EXPECT_FALSE(object_covers(Resource::blob(""), Resource::blob("")));
}

TEST(ScopedGrantCovering, ToolGrantIsExact) {
    const Resource tool = Resource::tool("search");
    EXPECT_TRUE(object_covers(tool, Resource::tool("search")));
    EXPECT_FALSE(object_covers(tool, Resource::tool("deploy")));
    // Cross-kind never matches.
    EXPECT_FALSE(object_covers(tool, Resource::blob("search")));
}

// ===== ScopedGrant + Resource JSON round-trip (the trust boundary) ===========

TEST(ScopedGrantJson, RoundTripsThroughTheBoundary) {
    const ScopedGrant grant{kRead,
                            Resource::column("sales", "customers", {"id", "name"})};
    const nlohmann::json doc = grant;
    const auto parsed = zetta::governance::scoped_grant_from_json(doc);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, grant);

    // A coarse database/project resource still serializes without the object-tier
    // fields (backward compatible on disk + over the CRDT).
    nlohmann::json coarse = Resource{ResourceKind::Database, "sales"};
    EXPECT_FALSE(coarse.contains("database"));
    EXPECT_FALSE(coarse.contains("columns"));

    // Malformed entries are rejected, not silently accepted.
    EXPECT_FALSE(zetta::governance::scoped_grant_from_json(nlohmann::json::array()));
    EXPECT_FALSE(zetta::governance::scoped_grant_from_json(
        nlohmann::json{{"operation", {{"domain", "data"}, {"action", "read"}}}}));
}

// ===== column_access_for: folding grants into column access ==================

TEST(ScopedGrantColumnAccess, GlobalGrantAllowsEveryColumn) {
    const ColumnAccess access = zetta::governance::column_access_for(
        /*global=*/true, {}, kRead, "sales", "customers");
    EXPECT_TRUE(access.table_allowed);
    EXPECT_TRUE(access.all_columns);
    EXPECT_TRUE(access.allows("ssn"));
}

TEST(ScopedGrantColumnAccess, ColumnSetMasksTheRest) {
    const std::vector<ScopedGrant> grants{
        {kRead, Resource::column("sales", "customers", {"id", "name"})}};
    const ColumnAccess access = zetta::governance::column_access_for(
        /*global=*/false, grants, kRead, "sales", "customers");
    EXPECT_TRUE(access.table_allowed);
    EXPECT_FALSE(access.all_columns);
    EXPECT_TRUE(access.allows("id"));
    EXPECT_TRUE(access.allows("name"));
    EXPECT_FALSE(access.allows("ssn"));
}

TEST(ScopedGrantColumnAccess, NoGrantLeavesTheTableOffLimits) {
    const std::vector<ScopedGrant> grants{
        {kRead, Resource::table("sales", "customers")}};
    // A grant on a DIFFERENT table does not reach `orders`.
    const ColumnAccess access = zetta::governance::column_access_for(
        /*global=*/false, grants, kRead, "sales", "orders");
    EXPECT_FALSE(access.table_allowed);
    EXPECT_FALSE(access.allows("id"));
}

// ===== Node object decisions over a user's effective grants ==================

struct ScopedGrantNode : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
    }
    void TearDown() override { node.stop(); }

    std::string user_with_role(const std::string& user_name,
                               const std::string& role_name) {
        auto user = node.create_user(user_name);
        auto role = node.find_role(role_name);
        EXPECT_TRUE(user && role);
        node.assign_role(user->id(), role->id());
        return user->id();
    }
};

TEST_F(ScopedGrantNode, ColumnScopeMasksUngrantedColumns) {
    node.create_role("clerk");
    ASSERT_TRUE(node.grant_scoped_permissions(
        "clerk", {"sql:read"},
        Resource::column("sales", "customers", {"id", "name"})));
    const std::string uid = user_with_role("cleo", "clerk");

    ColumnAccess access =
        node.column_scope(uid, /*acting_is_user=*/true, kRead, "sales", "customers");
    EXPECT_TRUE(access.table_allowed);
    EXPECT_FALSE(access.all_columns);
    EXPECT_TRUE(access.allows("name"));
    EXPECT_FALSE(access.allows("ssn"));

    // A table the role has no grant on is off-limits entirely.
    ColumnAccess other =
        node.column_scope(uid, /*acting_is_user=*/true, kRead, "sales", "orders");
    EXPECT_FALSE(other.table_allowed);
}

TEST_F(ScopedGrantNode, AdminAndLocalBypassObjectScoping) {
    const std::string admin_id = user_with_role("boss", "admin");
    // Admin holds flat sql:read -> every column, no masking.
    ColumnAccess admin_access =
        node.column_scope(admin_id, /*acting_is_user=*/true, kRead, "any", "table");
    EXPECT_TRUE(admin_access.table_allowed);
    EXPECT_TRUE(admin_access.all_columns);

    // The trusted unauthenticated-local session (empty acting id) also bypasses.
    ColumnAccess local_access =
        node.column_scope("", /*acting_is_user=*/false, kRead, "any", "table");
    EXPECT_TRUE(local_access.all_columns);
}

TEST_F(ScopedGrantNode, PerToolMcpCallScope) {
    node.create_role("caller");
    ASSERT_TRUE(node.grant_scoped_permissions("caller", {"mcp:call"},
                                              Resource::tool("search")));
    const std::string uid = user_with_role("cara", "caller");

    EXPECT_TRUE(node.may_call_tool(uid, /*acting_is_user=*/true, "search"));
    EXPECT_FALSE(node.may_call_tool(uid, /*acting_is_user=*/true, "deploy"));

    // A flat mcp:call (the built-in reader) covers every tool.
    const std::string reader_id = user_with_role("ross", "reader");
    EXPECT_TRUE(node.may_call_tool(reader_id, /*acting_is_user=*/true, "deploy"));
}

TEST_F(ScopedGrantNode, BlobPrefixGrantAdmitsExactlyThosePrefixes) {
    node.create_role("archivist");
    ASSERT_TRUE(node.grant_scoped_permissions("archivist", {"blob:read"},
                                              Resource::blob("2026/")));
    const std::string uid = user_with_role("ari", "archivist");

    EXPECT_TRUE(node.authorize_blob(uid, /*acting_is_user=*/true, "2026/q1.parquet"));
    EXPECT_FALSE(node.authorize_blob(uid, /*acting_is_user=*/true, "2025/q4.parquet"));
    EXPECT_FALSE(node.authorize_blob(uid, /*acting_is_user=*/true, "2026-secret/q1.parquet"));

    // The admin (flat blob:read) reads every blob; the local session bypasses.
    const std::string admin_id = user_with_role("boss", "admin");
    EXPECT_TRUE(node.authorize_blob(admin_id, /*acting_is_user=*/true, "any/id"));
    EXPECT_TRUE(node.authorize_blob("", /*acting_is_user=*/false, "any/id"));
}

// The node decision inherits the path boundary (#767): a grant on `report`
// admits `report` and `report/...`, never `report-secret-2026`.
TEST_F(ScopedGrantNode, BlobGrantWithoutTrailingSlashStopsAtThePathBoundary) {
    node.create_role("reporter");
    ASSERT_TRUE(node.grant_scoped_permissions("reporter", {"blob:read"},
                                              Resource::blob("report")));
    const std::string uid = user_with_role("rita", "reporter");

    EXPECT_TRUE(node.authorize_blob(uid, /*acting_is_user=*/true, "report"));
    EXPECT_TRUE(node.authorize_blob(uid, /*acting_is_user=*/true, "report/2026.pdf"));
    EXPECT_FALSE(node.authorize_blob(uid, /*acting_is_user=*/true, "report-secret-2026"));
}

TEST_F(ScopedGrantNode, RevokeRemovesTheScopedGrant) {
    node.create_role("temp");
    const Resource obj = Resource::table("sales", "customers");
    ASSERT_TRUE(node.grant_scoped_permissions("temp", {"sql:read"}, obj));
    const std::string uid = user_with_role("tom", "temp");
    EXPECT_TRUE(node.column_scope(uid, true, kRead, "sales", "customers")
                    .table_allowed);
    ASSERT_TRUE(node.revoke_scoped_permissions("temp", {"sql:read"}, obj));
    EXPECT_FALSE(node.column_scope(uid, true, kRead, "sales", "customers")
                     .table_allowed);
}

// ===== end-to-end through the SQL executor ==================================

struct ScopedGrantSql : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        // Seed data as the trusted-local session (empty acting key -> bypass).
        QueryService local{node, nullptr, "", /*acting_is_user=*/false};
        ASSERT_TRUE(local.run("CREATE DATABASE sales;", "").ok());
        ASSERT_TRUE(local.run("CREATE TABLE sales.customers (id INT);", "").ok());
        ASSERT_TRUE(
            local
                .run("INSERT INTO sales.customers (id, name, ssn) "
                     "VALUES (1, 'Ada', '111-22-3333');",
                     "")
                .ok());
        ASSERT_TRUE(local.run("CREATE TABLE sales.orders (id INT);", "").ok());
        ASSERT_TRUE(
            local.run("INSERT INTO sales.orders (id, total) VALUES (7, 42);", "")
                .ok());
    }
    void TearDown() override { node.stop(); }

    std::string reader_scoped_to(const std::string& grant_sql) {
        QueryService admin{node, nullptr, "", false};
        EXPECT_TRUE(admin.run("CREATE ROLE readerx;", "").ok());
        EXPECT_TRUE(admin.run(grant_sql, "").ok()) << grant_sql;
        auto user = node.create_user("clerk");
        node.assign_role(user->id(), node.find_role("readerx")->id());
        return user->id();
    }
};

TEST_F(ScopedGrantSql, ReaderWithColumnGrantGetsSsnMasked) {
    // reader may read sales.customers but only (id, name) -> ssn is masked.
    const std::string uid = reader_scoped_to(
        "GRANT sql:read ON sales.customers(id, name) TO ROLE readerx;");
    QueryService clerk{node, nullptr, uid, /*acting_is_user=*/true};

    auto star = clerk.run("SELECT * FROM sales.customers;", "");
    ASSERT_TRUE(star.ok()) << star.error;
    ASSERT_EQ(star.rows.size(), 1u);
    EXPECT_EQ(star.rows[0]["id"], 1);
    EXPECT_EQ(star.rows[0]["name"], "Ada");
    EXPECT_TRUE(star.rows[0]["ssn"].is_null());  // masked

    // A projected read of the masked column also comes back null.
    auto projected = clerk.run("SELECT name, ssn FROM sales.customers;", "");
    ASSERT_TRUE(projected.ok()) << projected.error;
    EXPECT_EQ(projected.rows[0]["name"], "Ada");
    EXPECT_TRUE(projected.rows[0]["ssn"].is_null());
}

TEST_F(ScopedGrantSql, ReaderCannotReadAnUngrantedTable) {
    const std::string uid = reader_scoped_to(
        "GRANT sql:read ON sales.customers(id) TO ROLE readerx;");
    QueryService clerk{node, nullptr, uid, /*acting_is_user=*/true};
    auto denied = clerk.run("SELECT * FROM sales.orders;", "");
    EXPECT_FALSE(denied.ok());
    EXPECT_NE(denied.error.find("permission denied"), std::string::npos);
}

TEST_F(ScopedGrantSql, FilteringOnAMaskedColumnIsDenied) {
    const std::string uid = reader_scoped_to(
        "GRANT sql:read ON sales.customers(id, name) TO ROLE readerx;");
    QueryService clerk{node, nullptr, uid, /*acting_is_user=*/true};
    auto denied =
        clerk.run("SELECT id FROM sales.customers WHERE ssn = '111-22-3333';", "");
    EXPECT_FALSE(denied.ok());  // can't infer a value you can't read
    EXPECT_NE(denied.error.find("WHERE"), std::string::npos);
}

// #912: every column of a compound WHERE is checked, not just the first; a masked
// column behind an AND, an OR or a NOT is refused the same way.
TEST_F(ScopedGrantSql, FilteringOnAMaskedColumnInsideACompoundWhereIsDenied) {
    const std::string uid = reader_scoped_to(
        "GRANT sql:read ON sales.customers(id, name) TO ROLE readerx;");
    QueryService clerk{node, nullptr, uid, /*acting_is_user=*/true};
    for (const std::string sql :
         {"SELECT id FROM sales.customers WHERE name = 'Ada' AND ssn = '111-22-3333';",
          "SELECT id FROM sales.customers WHERE name = 'x' OR ssn = '111-22-3333';",
          "SELECT id FROM sales.customers WHERE NOT (ssn = '111-22-3333');"}) {
        SCOPED_TRACE(sql);
        auto denied = clerk.run(sql, "");
        EXPECT_FALSE(denied.ok());
        EXPECT_NE(denied.error.find("\"ssn\" used in WHERE"), std::string::npos)
            << denied.error;
    }
    auto allowed =
        clerk.run("SELECT id FROM sales.customers WHERE name = 'Ada' AND id >= 0;", "");
    EXPECT_TRUE(allowed.ok()) << allowed.error;
}

TEST_F(ScopedGrantSql, WriteToAnUngrantedColumnIsBlocked) {
    // A writer scoped to write only (id, name) cannot write ssn.
    QueryService admin{node, nullptr, "", false};
    ASSERT_TRUE(admin.run("CREATE ROLE writerx;", "").ok());
    ASSERT_TRUE(admin
                    .run("GRANT sql:write ON sales.customers(id, name) TO ROLE "
                         "writerx;",
                         "")
                    .ok());
    auto user = node.create_user("wanda");
    node.assign_role(user->id(), node.find_role("writerx")->id());
    QueryService wanda{node, nullptr, user->id(), /*acting_is_user=*/true};

    EXPECT_TRUE(
        wanda.run("INSERT INTO sales.customers (id, name) VALUES (2, 'Bo');", "")
            .ok());
    auto denied = wanda.run(
        "INSERT INTO sales.customers (id, ssn) VALUES (3, '999');", "");
    EXPECT_FALSE(denied.ok());
    EXPECT_NE(denied.error.find("permission denied"), std::string::npos);
}

TEST_F(ScopedGrantSql, AdminSeesEveryColumn) {
    auto admin_user = node.create_user("root");
    node.assign_role(admin_user->id(), node.find_role("admin")->id());
    QueryService root{node, nullptr, admin_user->id(), /*acting_is_user=*/true};
    auto all = root.run("SELECT * FROM sales.customers;", "");
    ASSERT_TRUE(all.ok()) << all.error;
    ASSERT_EQ(all.rows.size(), 1u);
    EXPECT_EQ(all.rows[0]["ssn"], "111-22-3333");  // NOT masked for admin
}
