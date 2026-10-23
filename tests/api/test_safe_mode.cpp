// Safe mode per database (#1373; docs/api/safe-mode.md): a Zetta database or a
// saved source connection carries a level — off, warn_writes, warn_all. A call
// that would run a statement the level warns on is refused with
// `safe_mode_warning`, naming the database, the level, the exact statements and
// a confirmation bound to them; the same call repeated with that confirmation
// runs. Levels are changed with set_safe_mode by someone who may manage the
// database, kept in a reserved replicated table, and audited.
#include <gtest/gtest.h>

#include <string>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/safe_mode.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;
using zetta::governance::AuditAction;

struct SafeModeFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::api::BackgroundActions background;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        node.ensure_builtin_roles();
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input,
                   const std::string& user = {}) {
        ApiContext context{node, nullptr, user, trace, &node.span_recorder(), &background};
        context.acting_is_user = !user.empty();
        return registry.call(name, input, context);
    }

    std::string user_with(const std::string& name, const std::string& role) {
        auto user = node.create_user(name);
        auto found = node.find_role(role);
        EXPECT_TRUE(user && found);
        node.assign_role(user->id(), found->id());
        return user->id();
    }

    void make_table(const std::string& database) {
        ASSERT_TRUE(call("sql", {{"statement", "CREATE DATABASE " + database}}).ok);
        ASSERT_TRUE(call("sql", {{"statement", "CREATE TABLE " + database +
                                                   ".t (id INT, name TEXT)"}})
                        .ok);
    }

    void save_connection(const std::string& name, const std::string& token = "pat-secret-1373") {
        // An unreachable endpoint: a call that gets past safe mode fails on the
        // network, which is how a test tells "ran" from "warned".
        const auto saved = call("create_connection",
                                {{"name", name},
                                 {"connector",
                                  {{"type", "snowflake"},
                                   {"account", "ab12345"},
                                   {"token", token},
                                   {"endpoint", "http://127.0.0.1:1"}}}});
        ASSERT_TRUE(saved.ok) << saved.error.message;
    }

    std::size_t audit_count(AuditAction action) {
        std::size_t count = 0;
        for (const auto& event : node.audit_events()) {
            if (event->value().action == action) ++count;
        }
        return count;
    }
    std::string last_audit_detail(AuditAction action) {
        std::string detail;
        for (const auto& event : node.audit_events()) {
            if (event->value().action == action) detail = event->value().detail;
        }
        return detail;
    }
};

// --- The levels -------------------------------------------------------------

TEST(SafeModeLevels, ParseAndSpellTheThreeLevels) {
    using zetta::governance::SafeMode;
    for (const SafeMode mode : {SafeMode::Off, SafeMode::WarnWrites, SafeMode::WarnAll}) {
        EXPECT_EQ(zetta::governance::parse_safe_mode(zetta::governance::to_string(mode)),
                  mode);
    }
    EXPECT_EQ(zetta::governance::parse_safe_mode("WARN_ALL"), std::nullopt);
    EXPECT_EQ(zetta::governance::parse_safe_mode("on"), std::nullopt);
}

TEST(SafeModeLevels, WhichLevelWarnsOnWhat) {
    using zetta::governance::SafeMode;
    using zetta::governance::safe_mode_warns;
    const zetta::governance::SafeModeStatements read{
        zetta::governance::SafeModeTarget::Database, "d", {"SELECT 1"}, false};
    const zetta::governance::SafeModeStatements write{
        zetta::governance::SafeModeTarget::Database, "d", {"DELETE FROM d.t"}, true};
    EXPECT_FALSE(safe_mode_warns(SafeMode::Off, read));
    EXPECT_FALSE(safe_mode_warns(SafeMode::Off, write));
    EXPECT_FALSE(safe_mode_warns(SafeMode::WarnWrites, read));
    EXPECT_TRUE(safe_mode_warns(SafeMode::WarnWrites, write));
    EXPECT_TRUE(safe_mode_warns(SafeMode::WarnAll, read));
    EXPECT_TRUE(safe_mode_warns(SafeMode::WarnAll, write));
}

TEST(SafeModeLevels, TheConfirmationIsBoundToTheStatementAndTheDatabase) {
    using zetta::governance::SafeModeStatements;
    using zetta::governance::SafeModeTarget;
    const auto confirm = [](SafeModeTarget target, std::string name, std::string sql) {
        return zetta::governance::safe_mode_confirmation(
            {SafeModeStatements{target, std::move(name), {std::move(sql)}, true}});
    };
    const std::string base = confirm(SafeModeTarget::Database, "sales", "DELETE FROM sales.t");
    EXPECT_EQ(base.size(), 64u);
    EXPECT_EQ(base, confirm(SafeModeTarget::Database, "sales", "DELETE FROM sales.t"));
    EXPECT_NE(base, confirm(SafeModeTarget::Database, "sales", "DELETE FROM sales.u"));
    EXPECT_NE(base, confirm(SafeModeTarget::Database, "other", "DELETE FROM sales.t"));
    EXPECT_NE(base, confirm(SafeModeTarget::Connection, "sales", "DELETE FROM sales.t"));
}

TEST(SafeModeLevels, ASecretInAStatementIsRedacted) {
    EXPECT_EQ(zetta::governance::redact_secret("SELECT 'pat-xyz', 'pat-xyz'", "pat-xyz"),
              "SELECT '[redacted]', '[redacted]'");
    // A short secret would redact ordinary words, so it is left alone.
    EXPECT_EQ(zetta::governance::redact_secret("SELECT a FROM b", "a"), "SELECT a FROM b");
}

// --- Defaults, set and get ----------------------------------------------------

TEST_F(SafeModeFixture, AConnectionDefaultsToWarnWritesAndADatabaseToOff) {
    make_table("sales");
    save_connection("warehouse");
    const auto database = call("get_safe_mode", {{"database", "sales"}});
    ASSERT_TRUE(database.ok) << database.error.message;
    EXPECT_EQ(database.value.at("mode"), "off");
    EXPECT_EQ(database.value.at("is_default"), true);
    EXPECT_EQ(database.value.at("database"), "sales");
    const auto connection = call("get_safe_mode", {{"connection", "warehouse"}});
    ASSERT_TRUE(connection.ok) << connection.error.message;
    EXPECT_EQ(connection.value.at("mode"), "warn_writes");
    EXPECT_EQ(connection.value.at("is_default"), true);
    // The connection listing carries the level too.
    const auto listed = call("list_connections", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    EXPECT_EQ(listed.value.at("connections")[0].at("safe_mode"), "warn_writes");
}

TEST_F(SafeModeFixture, SetChangesTheLevelAndAuditsTheChange) {
    make_table("sales");
    const auto set = call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_all"}});
    ASSERT_TRUE(set.ok) << set.error.message;
    EXPECT_EQ(set.value.at("mode"), "warn_all");
    EXPECT_EQ(set.value.at("previous"), "off");
    const auto read = call("get_safe_mode", {{"database", "sales"}});
    EXPECT_EQ(read.value.at("mode"), "warn_all");
    EXPECT_EQ(read.value.at("is_default"), false);
    EXPECT_EQ(audit_count(AuditAction::SafeModeChanged), 1u);
    EXPECT_EQ(last_audit_detail(AuditAction::SafeModeChanged), "off -> warn_all");

    // Set again: the row is replaced, not duplicated.
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "off"}}).ok);
    EXPECT_EQ(call("get_safe_mode", {{"database", "sales"}}).value.at("mode"), "off");
    EXPECT_EQ(last_audit_detail(AuditAction::SafeModeChanged), "warn_all -> off");
}

TEST_F(SafeModeFixture, TheSelectorMustNameExactlyOneThingThatExists) {
    make_table("sales");
    save_connection("warehouse");
    EXPECT_EQ(call("set_safe_mode", {{"mode", "off"}}).error.code, "invalid_input");
    EXPECT_EQ(call("set_safe_mode", {{"database", "sales"}, {"connection", "warehouse"},
                                     {"mode", "off"}})
                  .error.code,
              "invalid_input");
    EXPECT_EQ(call("set_safe_mode", {{"database", "nope"}, {"mode", "off"}}).error.code,
              "not_found");
    EXPECT_EQ(call("get_safe_mode", {{"connection", "nope"}}).error.code, "not_found");
    EXPECT_EQ(call("set_safe_mode", {{"database", "system"}, {"mode", "off"}}).error.code,
              "invalid_input");
    EXPECT_EQ(call("set_safe_mode", {{"database", "sales"}, {"mode", "loud"}}).error.code,
              "invalid_input");
}

TEST_F(SafeModeFixture, ChangingALevelNeedsTheRightToManageThatDatabaseOrConnection) {
    make_table("sales");
    save_connection("warehouse");
    const std::string reader = user_with("viewer", "reader");
    const std::string admin = user_with("boss", "admin");
    const auto database = call("set_safe_mode",
                               {{"database", "sales"}, {"mode", "off"}}, reader);
    EXPECT_EQ(database.error.code, "unauthorized") << database.error.message;
    EXPECT_NE(database.error.message.find("sql:ddl"), std::string::npos);
    const auto connection = call("set_safe_mode",
                                 {{"connection", "warehouse"}, {"mode", "off"}}, reader);
    EXPECT_EQ(connection.error.code, "unauthorized") << connection.error.message;
    EXPECT_NE(connection.error.message.find("create_connection"), std::string::npos);
    EXPECT_EQ(audit_count(AuditAction::SafeModeChanged), 0u);

    EXPECT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_all"}}, admin).ok);
    EXPECT_TRUE(
        call("set_safe_mode", {{"connection", "warehouse"}, {"mode", "off"}}, admin).ok);
    // Reading a database's level needs only the call right.
    EXPECT_TRUE(call("get_safe_mode", {{"database", "sales"}}, reader).ok);
}

TEST_F(SafeModeFixture, TheLevelSurvivesARestart) {
    make_table("sales");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_writes"}}).ok);
    node.stop();
    zetta::Node reopened{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(reopened.start());
    EXPECT_EQ(zetta::governance::effective_safe_mode(
                  reopened, zetta::governance::SafeModeTarget::Database, "sales"),
              zetta::governance::SafeMode::WarnWrites);
    reopened.stop();
    ASSERT_TRUE(node.start());
}

TEST_F(SafeModeFixture, SqlCannotReachTheReservedStore) {
    make_table("sales");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_all"}}).ok);
    const auto read = call("sql", {{"statement", "SELECT * FROM zetta_safe_mode.levels"}});
    EXPECT_FALSE(read.ok);
    EXPECT_NE(read.error.message.find("reserved safe mode store"), std::string::npos)
        << read.error.message;
    const auto drop = call("sql", {{"statement", "DROP DATABASE zetta_safe_mode"}});
    EXPECT_FALSE(drop.ok);
    EXPECT_EQ(call("get_safe_mode", {{"database", "sales"}}).value.at("mode"), "warn_all");
}

TEST_F(SafeModeFixture, DroppingADatabaseOrConnectionForgetsItsLevel) {
    make_table("sales");
    save_connection("warehouse");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_all"}}).ok);
    ASSERT_TRUE(call("set_safe_mode", {{"connection", "warehouse"}, {"mode", "off"}}).ok);
    ASSERT_TRUE(call("sql", {{"statement", "DROP DATABASE sales"},
                             {"safe_mode_confirmation",
                              zetta::governance::safe_mode_confirmation(
                                  {{zetta::governance::SafeModeTarget::Database,
                                    "sales",
                                    {"DROP DATABASE sales"},
                                    true}})}})
                    .ok);
    ASSERT_TRUE(call("delete_connection", {{"name", "warehouse"}}).ok);
    make_table("sales");
    save_connection("warehouse");
    EXPECT_EQ(call("get_safe_mode", {{"database", "sales"}}).value.at("is_default"), true);
    EXPECT_EQ(call("get_safe_mode", {{"connection", "warehouse"}}).value.at("is_default"),
              true);
}

// --- Zetta SQL ---------------------------------------------------------------

TEST_F(SafeModeFixture, OffRunsEverythingWithoutAWarning) {
    make_table("sales");  // the default level of a database is off
    EXPECT_TRUE(call("sql", {{"statement", "INSERT INTO sales.t (id, name) VALUES (1, 'a')"}})
                    .ok);
    EXPECT_TRUE(call("sql", {{"statement", "SELECT * FROM sales.t"}}).ok);
    EXPECT_EQ(audit_count(AuditAction::SafeModeConfirmed), 0u);
}

TEST_F(SafeModeFixture, WarnWritesRefusesAWriteWithTheStatementAndRunsItOnceConfirmed) {
    make_table("sales");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_writes"}}).ok);
    const std::string insert = "INSERT INTO sales.t (id, name) VALUES (1, 'a')";

    const auto warned = call("sql", {{"statement", insert}});
    ASSERT_FALSE(warned.ok);
    ASSERT_EQ(warned.error.code, "safe_mode_warning") << warned.error.message;
    const nlohmann::json& details = warned.error.details;
    EXPECT_EQ(details.at("database"), "sales");
    EXPECT_EQ(details.at("mode"), "warn_writes");
    EXPECT_EQ(details.at("statements"), nlohmann::json::array({insert}));
    const std::string confirmation = details.at("confirmation").get<std::string>();
    EXPECT_EQ(confirmation.size(), 64u);
    // Nothing ran.
    EXPECT_EQ(call("sql", {{"statement", "SELECT * FROM sales.t"}}).value.at("rows").size(),
              0u);

    // A read is not a write: it runs without a prompt.
    EXPECT_TRUE(call("sql", {{"statement", "SELECT * FROM sales.t"}}).ok);

    const auto confirmed =
        call("sql", {{"statement", insert}, {"safe_mode_confirmation", confirmation}});
    ASSERT_TRUE(confirmed.ok) << confirmed.error.message;
    EXPECT_EQ(call("sql", {{"statement", "SELECT * FROM sales.t"}}).value.at("rows").size(),
              1u);

    // Audited with the statement hash, never the statement.
    EXPECT_EQ(audit_count(AuditAction::SafeModeConfirmed), 1u);
    const std::string detail = last_audit_detail(AuditAction::SafeModeConfirmed);
    EXPECT_NE(detail.find(confirmation), std::string::npos) << detail;
    EXPECT_NE(detail.find("warn_writes"), std::string::npos) << detail;
    EXPECT_EQ(detail.find("INSERT"), std::string::npos) << detail;
}

TEST_F(SafeModeFixture, AConfirmationForADifferentStatementIsRefused) {
    make_table("sales");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_writes"}}).ok);
    const auto harmless =
        call("sql", {{"statement", "INSERT INTO sales.t (id, name) VALUES (1, 'a')"}});
    ASSERT_EQ(harmless.error.code, "safe_mode_warning");
    const std::string confirmation =
        harmless.error.details.at("confirmation").get<std::string>();

    const auto other = call("sql", {{"statement", "DELETE FROM sales.t"},
                                    {"safe_mode_confirmation", confirmation}});
    EXPECT_EQ(other.error.code, "safe_mode_warning");
    EXPECT_NE(other.error.message.find("different statement"), std::string::npos)
        << other.error.message;
    EXPECT_NE(other.error.details.at("confirmation"), confirmation);
    EXPECT_EQ(audit_count(AuditAction::SafeModeConfirmed), 0u);
}

TEST_F(SafeModeFixture, WarnAllWarnsOnAReadToo) {
    make_table("sales");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_all"}}).ok);
    const std::string select = "SELECT * FROM t";
    const auto warned = call("sql", {{"statement", select}, {"database", "sales"}});
    ASSERT_EQ(warned.error.code, "safe_mode_warning") << warned.error.message;
    EXPECT_EQ(warned.error.details.at("mode"), "warn_all");
    EXPECT_EQ(warned.error.details.at("database"), "sales");
    const auto confirmed =
        call("sql", {{"statement", select},
                     {"database", "sales"},
                     {"safe_mode_confirmation", warned.error.details.at("confirmation")}});
    EXPECT_TRUE(confirmed.ok) << confirmed.error.message;
    // system.* is not a user database and is never warned on.
    const auto system = call("sql", {{"statement", "SELECT * FROM system.models"}});
    EXPECT_TRUE(system.ok) << system.error.code << ": " << system.error.message;
}

TEST_F(SafeModeFixture, OneConfirmationCoversEveryDatabaseAMultiStatementInputWarnsOn) {
    make_table("sales");
    make_table("ops");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_writes"}}).ok);
    ASSERT_TRUE(call("set_safe_mode", {{"database", "ops"}, {"mode", "warn_writes"}}).ok);
    const std::string both =
        "INSERT INTO sales.t (id, name) VALUES (1, 'a'); "
        "INSERT INTO ops.t (id, name) VALUES (2, 'b')";
    const auto warned = call("sql", {{"statement", both}});
    ASSERT_EQ(warned.error.code, "safe_mode_warning");
    EXPECT_EQ(warned.error.details.at("database"), "sales");
    ASSERT_EQ(warned.error.details.at("also").size(), 1u);
    EXPECT_EQ(warned.error.details.at("also")[0].at("database"), "ops");
    const auto confirmed = call(
        "sql", {{"statement", both},
                {"safe_mode_confirmation", warned.error.details.at("confirmation")}});
    ASSERT_TRUE(confirmed.ok) << confirmed.error.message;
    EXPECT_EQ(call("sql", {{"statement", "SELECT * FROM ops.t"}}).value.at("rows").size(), 1u);
}

TEST_F(SafeModeFixture, TheWarningIsCheckedAfterAuthorization) {
    make_table("sales");
    ASSERT_TRUE(call("set_safe_mode", {{"database", "sales"}, {"mode", "warn_all"}}).ok);
    // An unknown user is refused before safe mode names any statement.
    const auto refused = call("sql", {{"statement", "SELECT * FROM sales.t"}}, "no-such-user");
    EXPECT_EQ(refused.error.code, "unauthorized");
}

// --- Source connections --------------------------------------------------------

TEST_F(SafeModeFixture, UnderTheDefaultASourceReadRunsWithoutAPrompt) {
    save_connection("warehouse");
    const auto pulled = call("create_dataset_from_query",
                             {{"connection", "warehouse"}, {"query", "SELECT 1"}});
    // It reached the source (and failed on the unreachable endpoint).
    EXPECT_NE(pulled.error.code, "safe_mode_warning") << pulled.error.message;
    const auto listed = call("list_source_tables", {{"connection", "warehouse"}});
    EXPECT_NE(listed.error.code, "safe_mode_warning") << listed.error.message;
}

TEST_F(SafeModeFixture, WarnAllOnAConnectionNamesTheQueryWithItsCredentialRedacted) {
    save_connection("warehouse", "pat-secret-1373");
    ASSERT_TRUE(call("set_safe_mode", {{"connection", "warehouse"}, {"mode", "warn_all"}}).ok);
    const std::string query = "SELECT 'pat-secret-1373' AS leaked, id FROM accounts";
    const auto warned = call("create_dataset_from_query",
                             {{"connection", "warehouse"}, {"query", query}});
    ASSERT_EQ(warned.error.code, "safe_mode_warning") << warned.error.message;
    EXPECT_EQ(warned.error.details.at("connection"), "warehouse");
    EXPECT_EQ(warned.error.details.at("mode"), "warn_all");
    const std::string shown = warned.error.details.at("statements")[0].get<std::string>();
    EXPECT_EQ(shown, "SELECT '[redacted]' AS leaked, id FROM accounts");
    EXPECT_EQ(warned.error.details.dump().find("pat-secret-1373"), std::string::npos);

    const auto confirmed = call(
        "create_dataset_from_query",
        {{"connection", "warehouse"},
         {"query", query},
         {"safe_mode_confirmation", warned.error.details.at("confirmation")}});
    EXPECT_NE(confirmed.error.code, "safe_mode_warning") << confirmed.error.message;
    EXPECT_EQ(audit_count(AuditAction::SafeModeConfirmed), 1u);
    EXPECT_EQ(last_audit_detail(AuditAction::SafeModeConfirmed).find("pat-secret"),
              std::string::npos);
}

TEST_F(SafeModeFixture, WarnAllOnAConnectionNamesTheCatalogStatementsListSourceTablesRuns) {
    save_connection("warehouse");
    ASSERT_TRUE(call("set_safe_mode", {{"connection", "warehouse"}, {"mode", "warn_all"}}).ok);
    const auto warned = call("list_source_tables",
                             {{"connection", "warehouse"}, {"match", "score"}});
    ASSERT_EQ(warned.error.code, "safe_mode_warning") << warned.error.message;
    const nlohmann::json& statements = warned.error.details.at("statements");
    ASSERT_EQ(statements.size(), 2u);  // the tables, then the columns
    EXPECT_NE(statements[0].get<std::string>().find("information_schema.tables"),
              std::string::npos);
    EXPECT_NE(statements[0].get<std::string>().find("score"), std::string::npos);
    EXPECT_NE(statements[1].get<std::string>().find("information_schema.columns"),
              std::string::npos);
    // Without columns, only the table statement runs.
    const auto tables_only =
        call("list_source_tables", {{"connection", "warehouse"}, {"include_columns", false}});
    EXPECT_EQ(tables_only.error.details.at("statements").size(), 1u);
}

TEST_F(SafeModeFixture, AStatementTheReadOnlyGuardRefusesIsRefusedNotWarned) {
    save_connection("warehouse");
    ASSERT_TRUE(call("set_safe_mode", {{"connection", "warehouse"}, {"mode", "warn_all"}}).ok);
    const auto refused = call("create_dataset_from_query",
                              {{"connection", "warehouse"}, {"query", "DELETE FROM accounts"}});
    EXPECT_FALSE(refused.ok);
    EXPECT_NE(refused.error.code, "safe_mode_warning")
        << "a statement that can never run is not offered for confirmation";
}

TEST_F(SafeModeFixture, ConnectionOffRunsWithoutAPrompt) {
    save_connection("warehouse");
    ASSERT_TRUE(call("set_safe_mode", {{"connection", "warehouse"}, {"mode", "off"}}).ok);
    const auto listed = call("list_source_tables", {{"connection", "warehouse"}});
    EXPECT_NE(listed.error.code, "safe_mode_warning");
}

TEST_F(SafeModeFixture, TheConfirmationFieldIsInTheGuardedSchemasOnly) {
    for (const char* name : {"sql", "create_dataset_from_query", "list_source_tables"}) {
        const auto capability = registry.find(name);
        ASSERT_TRUE(capability) << name;
        EXPECT_TRUE(capability->input_schema.at("properties").contains(
            "safe_mode_confirmation"))
            << name;
    }
    EXPECT_FALSE(registry.find("create_connection")
                     ->input_schema.at("properties")
                     .contains("safe_mode_confirmation"));
}

}  // namespace
