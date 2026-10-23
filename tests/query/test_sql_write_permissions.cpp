// Every data-changing SQL statement checks its right (#1229). INSERT has always
// checked sql:write; UPDATE, DELETE, TRUNCATE and the DDL did not, so a
// principal allowed only to read could change or destroy data. These tests pin:
//   - a reader is refused for every statement kind, a writer allowed;
//   - object scope: a grant ON a table or database admits only that object;
//     a column-scoped write admits only those columns, and never a DELETE;
//   - row-level security applies to UPDATE and DELETE (and refuses TRUNCATE);
//   - CREATE USER / CREATE TOKEN need users:manage / tokens:manage;
//   - a legacy AccessKey needs the flat right too;
//   - every data-changing statement is audited as a data_changed action;
//   - SQL never reaches the reserved connection store (#1230).
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/governance/principal.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::governance::AuditAction;
using zetta::governance::CapabilityDomain;
using zetta::governance::CapabilitySet;
using zetta::governance::PrincipalKind;
using zetta::governance::Scope;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    cfg.preflight = false;
    return cfg;
}

struct SqlWritePermissions : ::testing::Test {
    TempDir dir;
    Node node{config_for(dir)};
    QueryService local{node};  // the trusted local session builds the fixture

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        for (const char* sql :
             {"CREATE DATABASE app;", "CREATE TABLE app.docs (id int);",
              "CREATE TABLE app.notes (id int);", "CREATE DATABASE other;",
              "INSERT INTO app.docs (id, owner, title, secret) VALUES "
              "(1, 'ada', 'a1', 's1'), (2, 'bo', 'b1', 's2'), (3, 'ada', 'a2', 's3');",
              "INSERT INTO app.notes (id) VALUES (1);"}) {
            const auto r = local.run(sql, "");
            ASSERT_TRUE(r.ok()) << r.error << "  (" << sql << ")";
        }
    }
    void TearDown() override { node.stop(); }

    std::string user_with_roles(const std::string& name,
                                const std::vector<std::string>& role_names) {
        auto user = node.create_user(name);
        EXPECT_TRUE(user);
        for (const std::string& role_name : role_names) {
            auto role = node.find_role(role_name);
            EXPECT_TRUE(role) << role_name;
            if (user && role) node.assign_role(user->id(), role->id());
        }
        return user ? user->id() : std::string{};
    }

    void run_local(const std::string& sql) {
        const auto r = local.run(sql, "");
        ASSERT_TRUE(r.ok()) << r.error << "  (" << sql << ")";
    }

    std::size_t row_count(const std::string& table) {
        const auto r = local.run("SELECT * FROM " + table + ";", "");
        EXPECT_TRUE(r.ok()) << r.error;
        return r.rows.size();
    }

    // The statements' own rows (target `sql:...`); the Node's rows for the
    // effects (a database created or dropped, #847) are counted separately.
    std::size_t changed_events(bool allowed) {
        std::size_t count = 0;
        for (const auto& event : node.audit_events()) {
            if (event->action() == AuditAction::DataChanged &&
                event->target().rfind("sql:", 0) == 0 &&
                event->allowed() == allowed) {
                ++count;
            }
        }
        return count;
    }
};

// One statement of each kind that changes data or schema.
const std::vector<std::string>& change_statements() {
    static const std::vector<std::string> statements = {
        "INSERT INTO app.docs (id) VALUES (9);",
        "UPDATE app.docs SET title = 'x' WHERE id = 1;",
        "DELETE FROM app.docs WHERE id = 2;",
        "TRUNCATE app.notes;",
        "CREATE TABLE app.extra (id int);",
        "DROP TABLE app.notes;",
        "CREATE DATABASE fresh;",
        "DROP DATABASE other;",
    };
    return statements;
}

}  // namespace

// The acceptance: a read-only principal is refused for every statement kind,
// and the data is untouched.
TEST_F(SqlWritePermissions, ReaderIsRefusedForEveryStatementKind) {
    const std::string reader = user_with_roles("rita", {"reader"});
    QueryService as_reader{node, nullptr, reader, /*acting_is_user=*/true};

    for (const std::string& sql : change_statements()) {
        const auto r = as_reader.run(sql, "");
        EXPECT_FALSE(r.ok()) << sql;
        EXPECT_NE(r.error.find("permission denied"), std::string::npos)
            << sql << ": " << r.error;
    }
    for (const std::string& sql :
         {std::string("CREATE VIRTUAL TABLE my_nodes OVER CRDT 'node:' "
                      "(node_id TEXT DURABLE KEY);"),
          std::string("CREATE USER mallory;"),
          std::string("CREATE TOKEN FOR rita;")}) {
        const auto r = as_reader.run(sql, "");
        EXPECT_FALSE(r.ok()) << sql;
    }
    EXPECT_EQ(row_count("app.docs"), 3u);
    EXPECT_EQ(row_count("app.notes"), 1u);
    EXPECT_TRUE(node.find_database("other"));
    EXPECT_FALSE(node.find_database("fresh"));
    EXPECT_FALSE(node.find_user("mallory"));
    // Reading is still allowed.
    EXPECT_TRUE(as_reader.run("SELECT * FROM app.docs;", "").ok());
    // Every refusal is on the audit trail as a refused action.
    EXPECT_EQ(changed_events(/*allowed=*/false), change_statements().size() + 3);
}

// The other half of the acceptance: the writer role (sql:write + sql:ddl) runs
// every kind.
TEST_F(SqlWritePermissions, WriterIsAllowedForEveryStatementKind) {
    const std::string writer = user_with_roles("wes", {"writer"});
    QueryService as_writer{node, nullptr, writer, /*acting_is_user=*/true};
    const std::size_t allowed_before = changed_events(true);

    for (const std::string& sql : change_statements()) {
        const auto r = as_writer.run(sql, "");
        EXPECT_TRUE(r.ok()) << sql << ": " << r.error;
    }
    EXPECT_EQ(row_count("app.docs"), 3u);  // +1 inserted, -1 deleted
    EXPECT_FALSE(node.find_database("other"));
    EXPECT_TRUE(node.find_database("fresh"));
    EXPECT_EQ(changed_events(/*allowed=*/true) - allowed_before,
              change_statements().size());
    // ...and the databases the statements created and dropped are on the trail
    // as the Node's own rows, attributed to the writer (#847).
    for (const char* target : {"database:fresh", "database:other"}) {
        std::shared_ptr<zetta::governance::AuditEvent> latest;
        for (const auto& event : node.audit_events()) {
            if (event->action() == AuditAction::DataChanged && event->target() == target) {
                latest = event;
            }
        }
        ASSERT_TRUE(latest) << target;
        EXPECT_EQ(latest->actor(), writer) << target;
    }
}

// CREATE USER and CREATE TOKEN mint identities and credentials: a writer may
// change data but not mint a token for the owner.
TEST_F(SqlWritePermissions, UserAndTokenStatementsNeedTheirManageRights) {
    const std::string writer = user_with_roles("wes", {"writer"});
    QueryService as_writer{node, nullptr, writer, true};
    EXPECT_FALSE(as_writer.run("CREATE USER mallory;", "").ok());
    EXPECT_FALSE(as_writer.run("CREATE TOKEN FOR wes;", "").ok());

    const std::string admin = user_with_roles("ann", {"admin"});
    QueryService as_admin{node, nullptr, admin, true};
    EXPECT_TRUE(as_admin.run("CREATE USER carol;", "").ok());
    const auto token = as_admin.run("CREATE TOKEN FOR carol;", "");
    ASSERT_TRUE(token.ok()) << token.error;
    // The audit row names the action, never the minted secret.
    const std::string secret = token.rows.front()["secret"].get<std::string>();
    for (const auto& event : node.audit_events()) {
        EXPECT_EQ(event->detail().find(secret), std::string::npos);
    }
}

// A grant ON one table admits writes to that table only; ON db.* admits the
// tables of that database and DDL there.
TEST_F(SqlWritePermissions, ObjectScopedGrantsAdmitOnlyTheirObject) {
    run_local("CREATE ROLE docs_editor;");
    run_local("GRANT sql:read, sql:write ON app.docs TO ROLE docs_editor;");
    run_local("CREATE ROLE other_owner;");
    run_local("GRANT sql:ddl, sql:write ON other.* TO ROLE other_owner;");
    const std::string editor = user_with_roles("ed", {"docs_editor", "other_owner"});
    QueryService as_editor{node, nullptr, editor, true};

    EXPECT_TRUE(as_editor.run("UPDATE app.docs SET title = 't' WHERE id = 1;", "").ok());
    EXPECT_TRUE(as_editor.run("DELETE FROM app.docs WHERE id = 2;", "").ok());
    EXPECT_FALSE(as_editor.run("UPDATE app.notes SET id = 5;", "").ok());
    EXPECT_FALSE(as_editor.run("TRUNCATE app.notes;", "").ok());
    EXPECT_FALSE(as_editor.run("DROP TABLE app.docs;", "").ok());  // no sql:ddl there

    EXPECT_TRUE(as_editor.run("CREATE TABLE other.t (id int);", "").ok());
    EXPECT_TRUE(as_editor.run("DROP TABLE other.t;", "").ok());
    EXPECT_FALSE(as_editor.run("CREATE TABLE app.t (id int);", "").ok());
    EXPECT_TRUE(as_editor.run("DROP DATABASE other;", "").ok());
    EXPECT_FALSE(as_editor.run("DROP DATABASE app;", "").ok());
    EXPECT_FALSE(as_editor.run("CREATE DATABASE elsewhere;", "").ok());
}

// A column-scoped write admits UPDATE of those columns only, and never a DELETE
// or TRUNCATE (they remove whole rows). A WHERE over a column the caller may not
// read is refused, like SELECT.
TEST_F(SqlWritePermissions, ColumnScopedWritesApplyToUpdateAndDelete) {
    run_local("CREATE ROLE titler;");
    run_local("GRANT sql:read ON app.docs(id, title) TO ROLE titler;");
    run_local("GRANT sql:write ON app.docs(title) TO ROLE titler;");
    const std::string titler = user_with_roles("tia", {"titler"});
    QueryService as_titler{node, nullptr, titler, true};

    EXPECT_TRUE(as_titler.run("UPDATE app.docs SET title = 'new' WHERE id = 1;", "").ok());
    const auto other_column =
        as_titler.run("UPDATE app.docs SET owner = 'tia' WHERE id = 1;", "");
    EXPECT_FALSE(other_column.ok());
    EXPECT_NE(other_column.error.find("owner"), std::string::npos) << other_column.error;
    const auto hidden_where =
        as_titler.run("UPDATE app.docs SET title = 'x' WHERE secret = 's1';", "");
    EXPECT_FALSE(hidden_where.ok());
    EXPECT_NE(hidden_where.error.find("WHERE"), std::string::npos) << hidden_where.error;
    // #912: a hidden column behind an AND is refused too.
    const auto hidden_compound = as_titler.run(
        "UPDATE app.docs SET title = 'x' WHERE id = 1 AND secret = 's1';", "");
    EXPECT_FALSE(hidden_compound.ok());
    EXPECT_NE(hidden_compound.error.find("\"secret\" used in WHERE"), std::string::npos)
        << hidden_compound.error;
    EXPECT_FALSE(as_titler.run("DELETE FROM app.docs WHERE id = 1;", "").ok());
    EXPECT_FALSE(as_titler.run("TRUNCATE app.docs;", "").ok());
    EXPECT_EQ(row_count("app.docs"), 3u);
}

// Row-level security applies to UPDATE: only rows the caller can see and could
// have written are changed, and the result must still pass WITH CHECK.
TEST_F(SqlWritePermissions, RowSecurityLimitsUpdate) {
    run_local("CREATE POLICY own_read ON app.docs FOR SELECT "
              "USING (owner = current_user());");
    run_local("CREATE POLICY own_write ON app.docs FOR UPDATE "
              "USING (owner = current_user()) WITH CHECK (owner = current_user());");
    const std::string ada = user_with_roles("ada", {"writer"});
    QueryService as_ada{node, nullptr, ada, true};

    const auto mine = as_ada.run("UPDATE app.docs SET title = 'mine';", "");
    ASSERT_TRUE(mine.ok()) << mine.error;
    EXPECT_EQ(mine.messages.back(), "UPDATE 2");  // ada's two rows, not bo's
    const auto bo_row = local.run("SELECT * FROM app.docs WHERE id = 2;", "");
    ASSERT_TRUE(bo_row.ok());
    EXPECT_EQ(bo_row.rows.front()["title"], "b1");

    // Handing a row to someone else fails WITH CHECK, and nothing changes.
    const auto give_away =
        as_ada.run("UPDATE app.docs SET owner = 'bo' WHERE id = 1;", "");
    EXPECT_FALSE(give_away.ok());
    EXPECT_NE(give_away.error.find("WITH CHECK"), std::string::npos) << give_away.error;
    // Taking someone else's row is not possible either: it is not visible.
    const auto take = as_ada.run("UPDATE app.docs SET owner = 'ada' WHERE id = 2;", "");
    ASSERT_TRUE(take.ok()) << take.error;
    EXPECT_EQ(take.messages.back(), "UPDATE 0");
}

// …and to DELETE: a write-class policy decides which rows may be removed (there
// is no FOR DELETE command); TRUNCATE on a protected table is refused.
TEST_F(SqlWritePermissions, RowSecurityLimitsDeleteAndRefusesTruncate) {
    run_local("CREATE POLICY own_rows ON app.docs FOR ALL "
              "USING (owner = current_user());");
    const std::string ada = user_with_roles("ada", {"writer"});
    QueryService as_ada{node, nullptr, ada, true};

    const auto removed = as_ada.run("DELETE FROM app.docs;", "");
    ASSERT_TRUE(removed.ok()) << removed.error;
    EXPECT_EQ(removed.messages.back(), "DELETE 2");
    EXPECT_EQ(row_count("app.docs"), 1u);  // bo's row survives

    const auto truncate = as_ada.run("TRUNCATE app.docs;", "");
    EXPECT_FALSE(truncate.ok());
    EXPECT_NE(truncate.error.find("row-level security"), std::string::npos)
        << truncate.error;
    EXPECT_EQ(row_count("app.docs"), 1u);

    // admin bypasses row security, as for SELECT.
    const std::string boss = user_with_roles("boss", {"admin"});
    QueryService as_boss{node, nullptr, boss, true};
    EXPECT_TRUE(as_boss.run("TRUNCATE app.docs;", "").ok());
    EXPECT_EQ(row_count("app.docs"), 0u);
}

// A legacy AccessKey has no object tier, so it used to skip the write check
// entirely. It now needs the flat right for every data-changing statement.
TEST_F(SqlWritePermissions, LegacyAccessKeyNeedsTheFlatRight) {
    auto principal = node.create_principal("svc", PrincipalKind::Service);
    ASSERT_TRUE(principal);
    CapabilitySet read_only;
    read_only.grant(CapabilityDomain::Api, "query").grant(CapabilityDomain::Data, "read");
    auto viewer = principal->create_key(Scope{}, read_only);
    ASSERT_TRUE(viewer);
    QueryService as_viewer{node, nullptr, viewer->id(), /*acting_is_user=*/false};
    for (const std::string& sql : change_statements()) {
        EXPECT_FALSE(as_viewer.run(sql, "").ok()) << sql;
    }

    CapabilitySet full;
    full.grant(CapabilityDomain::Api, "query")
        .grant(CapabilityDomain::Data, "read")
        .grant(CapabilityDomain::Data, "write")
        .grant(CapabilityDomain::Data, "ddl");
    auto writer = principal->create_key(Scope{}, full);
    ASSERT_TRUE(writer);
    QueryService as_writer{node, nullptr, writer->id(), false};
    for (const std::string& sql : change_statements()) {
        EXPECT_TRUE(as_writer.run(sql, "").ok()) << sql;
    }
}

// The audit record of a data-changing statement: data_changed, the actor, the
// object, and the outcome.
TEST_F(SqlWritePermissions, DataChangesAreAuditedAsActions) {
    const std::string reader = user_with_roles("rita", {"reader"});
    QueryService as_reader{node, nullptr, reader, true};
    ASSERT_FALSE(as_reader.run("DELETE FROM app.docs;", "").ok());
    ASSERT_TRUE(local.run("UPDATE app.docs SET title = 'z' WHERE id = 3;", "").ok());
    // A SELECT is not a change.
    const std::size_t before = node.audit_event_count();
    ASSERT_TRUE(local.run("SELECT * FROM app.docs;", "").ok());
    EXPECT_EQ(node.audit_event_count(), before);

    bool saw_refusal = false;
    bool saw_update = false;
    for (const auto& event : node.audit_events()) {
        if (event->action() != AuditAction::DataChanged) continue;
        if (event->actor() == reader) {
            saw_refusal = true;
            EXPECT_FALSE(event->allowed());
            EXPECT_EQ(event->target(), "sql:table:app.docs");
            EXPECT_NE(event->detail().find("permission denied"), std::string::npos);
        }
        if (event->actor() == "local" && event->detail() == "UPDATE 1") {
            saw_update = true;
            EXPECT_TRUE(event->allowed());
            EXPECT_EQ(event->target(), "sql:table:app.docs");
        }
    }
    EXPECT_TRUE(saw_refusal);
    EXPECT_TRUE(saw_update);
}

// The reserved connection store is not reachable through SQL — not even by
// the trusted local session (#1230).
TEST_F(SqlWritePermissions, ConnectionStoreIsNotReachableThroughSql) {
    zetta::connect::ConnectorConfig config;
    config.type = "snowflake";
    config.account = "ab12345.us-east-1";
    config.token = "a-warehouse-token";
    ASSERT_TRUE(zetta::connect::save_connection(node, "warehouse", config).empty());

    const std::string admin = user_with_roles("ann", {"admin"});
    QueryService as_admin{node, nullptr, admin, true};
    for (QueryService* service : {&local, &as_admin}) {
        for (const char* sql :
             {"SELECT * FROM zetta_connect.connections;",
              "UPDATE zetta_connect.connections SET name = 'x';",
              "DELETE FROM zetta_connect.connections;",
              "INSERT INTO zetta_connect.connections (name) VALUES ('x');",
              "TRUNCATE zetta_connect.connections;",
              "DROP TABLE zetta_connect.connections;",
              "DROP DATABASE zetta_connect;"}) {
            const auto r = service->run(sql, "");
            EXPECT_FALSE(r.ok()) << sql;
            EXPECT_NE(r.error.find("reserved connection store"), std::string::npos)
                << sql << ": " << r.error;
        }
    }
    EXPECT_TRUE(zetta::connect::find_connection(node, "warehouse").has_value());
}
