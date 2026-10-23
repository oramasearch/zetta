// TC-5 (#252): a low-privilege caller cannot get out of its capability scope, and
// every attempt is refused AND recorded. The persona is the one in
// docs/testing/test-scripts/tc-5-jailbreak-attempt.md: an analyst who may read one
// table (and not one of its columns) and call tools, nothing else. Every attempt
// goes through Registry::call, the one entry point REST `/v1/<name>` and MCP
// `tools/call` share, so what is asserted here is what a remote caller gets.
//
// This file covers the TC-5 steps that no other suite does. The map of every step
// to the test that covers it (here, the authz regression harness, the parser,
// step authority and egress suites) is docs/testing/tc-5-coverage.md.
//
// A gap still open is a KNOWN_DEFECT probe, as in the authz regression harness:
// while it reproduces the test skips naming the issue; once it stops, the test
// fails so the probe is replaced with the plain assertion beside it.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"

#define KNOWN_DEFECT(issue, still_present, what)                                  \
    do {                                                                          \
        if (still_present) {                                                      \
            GTEST_SKIP() << "open defect zetta#" #issue ": " << (what);           \
        }                                                                         \
        FAIL() << "zetta#" #issue " no longer reproduces (" << (what)             \
               << "). It looks fixed: replace this KNOWN_DEFECT probe with the "  \
                  "plain assertion next to it, and update "                       \
                  "docs/testing/tc-5-coverage.md.";                               \
    } while (false)

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::governance::AuditAction;

// One node with the built-in roles and two tables:
//   app.open   (id, region, salary): the analyst reads id and region; salary is masked;
//   app.hidden (id, secret):         the analyst holds no right on it at all.
// The analyst's role is `mcp:call` (call tools) plus that one column grant.
struct SandboxEscape : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    zetta::query::QueryService local{node};  // the trusted session builds the fixture
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    std::string analyst;

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        zetta::api::register_core_capabilities(registry);
        for (const char* sql :
             {"CREATE DATABASE app;", "CREATE TABLE app.open (id int);",
              "INSERT INTO app.open (id, region, salary) VALUES "
              "(1, 'eu', 91000), (2, 'us', 87000), (3, 'eu', 123000);",
              "CREATE TABLE app.hidden (id int);",
              "INSERT INTO app.hidden (id, secret) VALUES (1, 'launch-codes');",
              "CREATE ROLE analyst;", "GRANT mcp:call TO ROLE analyst;",
              "GRANT sql:read ON app.open(id, region) TO ROLE analyst;"}) {
            const auto result = local.run(sql, "");
            ASSERT_TRUE(result.ok()) << result.error << "  (" << sql << ")";
        }
        auto user = node.create_user("analyst");
        ASSERT_TRUE(user);
        node.assign_role(user->id(), node.find_role("analyst")->id());
        analyst = user->id();
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, analyst, trace, &node.span_recorder()};
        context.acting_is_user = true;
        return registry.call(name, input, context);
    }
    ApiResult sql(const std::string& statement) {
        return call("sql", {{"statement", statement}});
    }

    // The refused audit rows recorded for the analyst after `from`.
    std::vector<zetta::governance::AuditEventValue> refusals_since(std::size_t from) {
        std::vector<zetta::governance::AuditEventValue> refused;
        const auto events = node.audit_events();
        for (std::size_t i = from; i < events.size(); ++i) {
            const auto& value = events[i]->value();
            if (value.actor == analyst && !value.allowed) refused.push_back(value);
        }
        return refused;
    }

    // The node still answers a legitimate read after the attempts: it stayed up.
    void expect_still_serving() {
        const ApiResult ok = sql("SELECT id, region FROM app.open;");
        ASSERT_TRUE(ok.ok) << ok.error.message;
        EXPECT_EQ(ok.value.at("rows").size(), 3u);
    }
};

bool is_permission_refusal(const ApiResult& result) {
    return !result.ok && result.error.message.find("permission denied") != std::string::npos;
}

}  // namespace

// ===== Step 3 + 12: a direct read of a table with no grant is refused, recorded ====

TEST_F(SandboxEscape, ReadingAnUngrantedTableIsRefusedAndTheRefusalIsAudited) {
    const std::size_t before = node.audit_events().size();
    const ApiResult denied = sql("SELECT * FROM app.hidden;");
    EXPECT_TRUE(is_permission_refusal(denied)) << denied.error.message;
    EXPECT_EQ(denied.error.message.find("launch-codes"), std::string::npos);

    // The refusal is recorded as a refusal, as the analyst, naming the table: not
    // only the gate's "the sql tool was called" row.
    const auto refused = refusals_since(before);
    ASSERT_EQ(refused.size(), 1u);
    EXPECT_EQ(refused[0].action, AuditAction::DataAccess);
    EXPECT_EQ(refused[0].target, "sql:table:app.hidden");
    EXPECT_NE(refused[0].detail.find("permission denied"), std::string::npos);
}

// ===== Step 3, error text as a channel: existence is not disclosed ================
// "An error revealing whether a hidden table exists has already leaked." A caller
// with no right on a name gets the same answer whether it names a real table, a
// table that does not exist, or a database that does not exist.

TEST_F(SandboxEscape, TheRefusalDoesNotSayWhetherAHiddenTableExists) {
    const std::vector<std::string> statements = {
        "SELECT * FROM {};",
        "INSERT INTO {} (id) VALUES (9);",
        "UPDATE {} SET id = 9;",
        "DELETE FROM {};",
        "TRUNCATE {};",
    };
    const auto answer = [&](const std::string& pattern, const std::string& table) {
        std::string statement = pattern;
        statement.replace(statement.find("{}"), 2, table);
        const ApiResult result = sql(statement);
        EXPECT_FALSE(result.ok) << statement;
        // The table name is the caller's own input; take it out to compare.
        std::string message = result.error.message;
        if (const auto at = message.find(table); at != std::string::npos) {
            message.replace(at, table.size(), "<name>");
        }
        return message;
    };
    for (const std::string& pattern : statements) {
        const std::string existing = answer(pattern, "app.hidden");
        EXPECT_NE(existing.find("permission denied"), std::string::npos) << pattern;
        EXPECT_EQ(answer(pattern, "app.no_such_table"), existing) << pattern;
        EXPECT_EQ(answer(pattern, "no_such_db.hidden"), existing) << pattern;
    }
    // Every one of those fifteen attempts is recorded as refused.
    EXPECT_EQ(refusals_since(0).size(), statements.size() * 3);
    expect_still_serving();
}

// A caller WITH a right on the table may still learn it does not exist: the
// change only hides names from callers who may not see them.
TEST_F(SandboxEscape, ACallerWithARightStillLearnsATableIsMissing) {
    const auto ok = local.run("GRANT sql:read ON app.* TO ROLE analyst;", "");
    ASSERT_TRUE(ok.ok()) << ok.error;
    const ApiResult missing = sql("SELECT * FROM app.no_such_table;");
    EXPECT_FALSE(missing.ok);
    EXPECT_NE(missing.error.message.find("does not exist"), std::string::npos)
        << missing.error.message;
}

// ===== Steps 4 + 5: the masked column cannot be recovered indirectly ==============

TEST_F(SandboxEscape, AMaskedColumnCannotBeRecoveredIndirectly) {
    const std::size_t before = node.audit_events().size();
    const ApiResult star = sql("SELECT * FROM app.open;");
    ASSERT_TRUE(star.ok) << star.error.message;
    for (const auto& row : star.value.at("rows")) {
        EXPECT_TRUE(row.at("salary").is_null()) << row.dump();
    }
    // Every way of learning about a value without printing it is refused.
    const std::vector<std::string> attempts = {
        "SELECT id FROM app.open WHERE salary > 100000;",           // which rows match
        "SELECT id FROM app.open ORDER BY salary DESC LIMIT 1;",    // who is highest
        "SELECT salary, count(*) FROM app.open GROUP BY salary;",   // count by value
        "SELECT region, max(salary) FROM app.open GROUP BY region;",
        "SELECT min(salary) FROM app.open;",
        "SELECT sum(salary) FROM app.open;",
        "SELECT avg(salary) FROM app.open;",
        "SELECT count(salary) FROM app.open;",
    };
    for (const std::string& attempt : attempts) {
        const ApiResult result = sql(attempt);
        EXPECT_TRUE(is_permission_refusal(result))
            << attempt << " -> " << (result.ok ? result.value.dump() : result.error.message);
        if (!result.ok) {
            for (const char* value : {"91000", "87000", "123000"}) {
                EXPECT_EQ(result.error.message.find(value), std::string::npos) << attempt;
            }
        }
    }
    EXPECT_EQ(refusals_since(before).size(), attempts.size());
}

// Joins, subqueries, views and file readers are not in the SQL subset at all: each
// is refused as a statement, and the node stays up.
TEST_F(SandboxEscape, IndirectSqlFormsAreRefusedAndTheNodeStaysUp) {
    for (const char* attempt :
         {"SELECT * FROM app.open JOIN app.hidden ON app.open.id = app.hidden.id;",
          "SELECT * FROM app.open WHERE id IN (SELECT id FROM app.hidden);",
          "SELECT (SELECT secret FROM app.hidden) FROM app.open;",
          "CREATE VIEW app.peek AS SELECT * FROM app.hidden;",
          "WITH h AS (SELECT * FROM app.hidden) SELECT * FROM h;",
          "SELECT * FROM app.open UNION SELECT * FROM app.hidden;",
          "COPY app.hidden TO '/tmp/out.csv';",
          "COPY app.open FROM '/etc/passwd';",
          "SELECT pg_read_file('/etc/passwd');",
          "SELECT * FROM '/etc/passwd';"}) {
        const ApiResult result = sql(attempt);
        EXPECT_FALSE(result.ok) << attempt << " -> " << result.value.dump();
        if (result.ok) continue;
        EXPECT_EQ(result.error.message.find("launch-codes"), std::string::npos) << attempt;
        EXPECT_EQ(result.error.message.find("root:"), std::string::npos) << attempt;
    }
    expect_still_serving();
}

// ===== Step 8: no route to more access ==========================================

TEST_F(SandboxEscape, TheAnalystCannotGiveItselfMoreAccess) {
    const std::size_t before = node.audit_events().size();
    const std::vector<std::string> attempts = {
        "GRANT ROLE admin TO analyst;",
        "GRANT sql:read ON app.hidden TO ROLE analyst;",
        "GRANT sql:read ON app.open(salary) TO ROLE analyst;",
        "CREATE ROLE escape;",
        "DROP ROLE analyst;",
        "CREATE USER shadow;",
        "CREATE TOKEN FOR analyst;",
        "CREATE POLICY open_all ON app.hidden FOR SELECT TO ROLE analyst USING (id = 1);",
    };
    for (const std::string& attempt : attempts) {
        const ApiResult result = sql(attempt);
        EXPECT_FALSE(result.ok) << attempt << " was allowed";
    }
    // Still exactly where it started.
    EXPECT_TRUE(is_permission_refusal(sql("SELECT * FROM app.hidden;")));
    EXPECT_TRUE(is_permission_refusal(sql("SELECT min(salary) FROM app.open;")));
    EXPECT_FALSE(node.find_role("escape"));
    EXPECT_FALSE(node.find_user("shadow"));
    EXPECT_GE(refusals_since(before).size(), attempts.size());
}

// ===== Steps 9-11: no network, no filesystem, no credential it was not granted ====
// Each of these capabilities reaches outside the node (a path on its disk, a saved
// connection's credential, a remote warehouse). Holding `mcp:call` is not holding
// them: each needs its own orchestration right, the call is refused before the
// handler runs, and the refusal is audited.

TEST_F(SandboxEscape, CapabilitiesThatReachOutsideTheNodeAreRefusedAndAudited) {
    const struct {
        const char* name;
        nlohmann::json input;
    } attempts[] = {
        {"add_dataset", {{"path", "/etc"}}},
        {"add_dataset", {{"path", "../../../../etc/passwd"}}},
        {"create_connection", {{"name", "exfil"}, {"connector", {{"type", "postgres"}}}}},
        {"list_connections", nlohmann::json::object()},
        {"list_source_tables", {{"connection", "someone_elses"}}},
        {"create_dataset_from_query",
         {{"connection", "someone_elses"}, {"query", "SELECT * FROM payroll"}}},
        {"delete_connection", {{"name", "someone_elses"}}},
    };
    for (const auto& attempt : attempts) {
        const std::size_t before = node.audit_events().size();
        const ApiResult result = call(attempt.name, attempt.input);
        EXPECT_FALSE(result.ok) << attempt.name << " was allowed";
        if (result.error.code == "invalid_input") {
            ADD_FAILURE() << attempt.name << ": the probe's input is invalid ("
                          << result.error.message << "), so it never reached the gate";
            continue;
        }
        EXPECT_EQ(result.error.code, "unauthorized")
            << attempt.name << ": " << result.error.message;
        const auto refused = refusals_since(before);
        ASSERT_EQ(refused.size(), 1u) << attempt.name;
        EXPECT_EQ(refused[0].target, std::string("api:") + attempt.name);
    }
    // Nothing was created on the analyst's behalf.
    const auto connections = local.run("SELECT name FROM system.connections;", "");
    ASSERT_TRUE(connections.ok()) << connections.error;
    EXPECT_TRUE(connections.rows.empty());
    EXPECT_FALSE(node.find_database("etc"));
}

// SQL does not reach the store that holds saved connections' sealed credentials.
TEST_F(SandboxEscape, TheConnectionStoreIsNotReachableFromSql) {
    const std::size_t before = node.audit_events().size();
    for (const char* attempt : {"SELECT * FROM zetta_connect.connections;",
                                "DELETE FROM zetta_connect.connections;",
                                "DROP DATABASE zetta_connect;"}) {
        const ApiResult result = sql(attempt);
        EXPECT_FALSE(result.ok) << attempt;
    }
    EXPECT_EQ(refusals_since(before).size(), 3u);
}

// ===== Step 14: the record cannot be altered ====================================

TEST_F(SandboxEscape, TheAuditTrailCannotBeAlteredFromSql) {
    ASSERT_TRUE(is_permission_refusal(sql("SELECT * FROM app.hidden;")));
    const std::size_t recorded = node.audit_events().size();
    for (const char* attempt :
         {"DELETE FROM system.audit;", "TRUNCATE system.audit;",
          "UPDATE system.audit SET allowed = true;", "DROP TABLE system.audit;",
          "DROP DATABASE system;", "INSERT INTO system.audit (actor) VALUES ('x');"}) {
        const ApiResult result = sql(attempt);
        EXPECT_FALSE(result.ok) << attempt << " was allowed";
    }
    // Nothing was removed: every earlier row is still there, the refusal included.
    const auto events = node.audit_events();
    EXPECT_GE(events.size(), recorded);
    EXPECT_TRUE(std::ranges::any_of(events, [&](const auto& event) {
        return event->value().actor == analyst && !event->value().allowed &&
               event->value().target == "sql:table:app.hidden";
    }));
}

// ===== Step 7: the catalogue is not free information (open, #766) ==================
// The system.* views are dispatched before any identity check, so the analyst
// reads every user's audit rows, every dataset's name and every saved connection.

TEST_F(SandboxEscape, TheAnalystCannotReadTheWholeAuditTrailOrCatalog) {
    // Plain assertion once #766 restricts the system.* views by identity:
    //   EXPECT_FALSE(sql("SELECT * FROM system.audit;").ok);
    //   EXPECT_FALSE(sql("SELECT * FROM system.datasets;").ok);
    const ApiResult audit = sql("SELECT * FROM system.audit;");
    const ApiResult datasets = sql("SELECT * FROM system.datasets;");
    const bool hidden_table_listed =
        datasets.ok && datasets.value.dump().find("hidden") != std::string::npos;
    KNOWN_DEFECT(766, audit.ok || hidden_table_listed,
                 "a caller with no grant reads system.audit and sees app.hidden in "
                 "system.datasets");
}
