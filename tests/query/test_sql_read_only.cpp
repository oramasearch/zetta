// `sql` with read_only: true (#1501): the engine itself decides what is a read.
//
// lake's workspace_sql forwards a reader's SQL under the workspace
// administrator's credential, and its own lexical guard decided whether the text
// was a read (LSR-017). Under read_only, QueryService parses the text with the
// engine's parser and refuses, before any statement runs, unless the text is
// exactly one SELECT. These tests pin:
//   - one SELECT runs, in any comment and quote arrangement the parser accepts;
//   - every mutating statement kind is refused, and nothing it names changes;
//   - a second statement is refused, even when the first is a read, so a write
//     hidden after a read never runs;
//   - the `sql` capability answers invalid_input for a refusal;
//   - without the flag, nothing changes.
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::BackgroundActions;
using zetta::api::Registry;
using zetta::query::QueryService;
using zetta::test::TempDir;

namespace {

struct SqlReadOnly : ::testing::Test {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};
    QueryService local{node};
    Registry registry;
    BackgroundActions background;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
        for (const char* statement :
             {"CREATE DATABASE app;", "CREATE TABLE app.docs (id int, title text);",
              "INSERT INTO app.docs (id, title) VALUES (1, 'a'), (2, 'b');"}) {
            const auto outcome = local.run(statement, "");
            ASSERT_TRUE(outcome.ok()) << statement << ": " << outcome.error;
        }
    }
    void TearDown() override {
        background.prune();
        node.stop();
    }

    QueryService::Outcome read_only(const std::string& statement) {
        return local.run(statement, "app", QueryService::RunOptions{.read_only = true});
    }

    std::size_t doc_count() {
        const auto outcome = local.run("SELECT * FROM app.docs;", "");
        EXPECT_TRUE(outcome.ok()) << outcome.error;
        return outcome.rows.size();
    }
};

TEST_F(SqlReadOnly, OneSelectRunsInEveryArrangement) {
    for (const char* statement : {
             "SELECT * FROM app.docs;",
             "select id from docs",
             "  -- a leading comment\nSELECT title FROM app.docs WHERE id = 1;",
             "SELECT * FROM app.docs WHERE title = 'it''s; DROP TABLE app.docs';",
             "SELECT * FROM app.docs; -- trailing comment; DELETE FROM app.docs",
             "SELECT * FROM system.tables;",
         }) {
        const auto outcome = read_only(statement);
        EXPECT_TRUE(outcome.ok()) << statement << ": " << outcome.error;
        EXPECT_FALSE(outcome.refused) << statement;
    }
    EXPECT_EQ(doc_count(), 2u);
}

TEST_F(SqlReadOnly, EveryMutatingStatementIsRefusedBeforeItRuns) {
    for (const char* statement : {
             "INSERT INTO app.docs (id, title) VALUES (3, 'c');",
             "UPDATE app.docs SET title = 'x';",
             "DELETE FROM app.docs;",
             "TRUNCATE app.docs;",
             "DROP TABLE app.docs;",
             "CREATE TABLE app.more (id int);",
             "CREATE DATABASE other;",
             "DROP DATABASE app;",
             "CREATE USER mallory;",
             "-- looks like a comment\nDELETE FROM app.docs",
         }) {
        const auto outcome = read_only(statement);
        EXPECT_FALSE(outcome.ok()) << statement;
        EXPECT_TRUE(outcome.refused) << statement;
        EXPECT_NE(outcome.error.find("read_only"), std::string::npos) << outcome.error;
    }
    EXPECT_EQ(doc_count(), 2u);
    EXPECT_TRUE(node.find_database("app") != nullptr);
    EXPECT_TRUE(node.find_database("other") == nullptr);
}

TEST_F(SqlReadOnly, ASecondStatementIsRefusedEvenAfterARead) {
    for (const char* statement : {
             "SELECT * FROM app.docs; DELETE FROM app.docs;",
             "SELECT * FROM app.docs; SELECT * FROM app.docs;",
             "SELECT * FROM app.docs WHERE title = 'a'; DROP TABLE app.docs",
         }) {
        const auto outcome = read_only(statement);
        EXPECT_FALSE(outcome.ok()) << statement;
        EXPECT_TRUE(outcome.refused) << statement;
    }
    EXPECT_EQ(doc_count(), 2u);
}

TEST_F(SqlReadOnly, TheCapabilityAnswersInvalidInputAndRunsNothing) {
    ApiContext context{node, nullptr, {}, trace, &node.span_recorder(), &background};
    const ApiResult refused = registry.call(
        "sql", {{"statement", "DELETE FROM app.docs;"}, {"read_only", true}}, context);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.error.code, "invalid_input");
    EXPECT_EQ(doc_count(), 2u);

    const ApiResult read = registry.call(
        "sql", {{"statement", "SELECT * FROM app.docs;"}, {"read_only", true}}, context);
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value.at("rows").size(), 2u);
}

TEST_F(SqlReadOnly, WithoutTheFlagNothingChanges) {
    const auto outcome = local.run("SELECT * FROM app.docs; DELETE FROM app.docs;", "");
    EXPECT_TRUE(outcome.ok()) << outcome.error;
    EXPECT_EQ(doc_count(), 0u);
}

}  // namespace
