// One SQL statement from any client must never take a node down (#793, #819).
//
// A malformed statement used to reach a `throw ParseError` inside the SQL parser
// that escaped the parser's own try/catch and std::terminate()d the process when
// libc++abi was paired with the wrong unwinder (#758): `SELECT * FROM system.*`,
// `REVOKE TOKEN <id>`, a column-scoped `GRANT ... ON t(a, b)` and
// `SELECT current_user()` each aborted the node. Linked against the system
// unwinder on macOS, the parser at origin/dev aborts on the first test below. The
// parser now reports every error without throwing, so these tests pin, for a
// large corpus of malformed
// statements (the named inputs from the issues, every prefix of a valid statement
// of every grammar branch, single-character edits of those statements, and a
// seeded random token soup):
//   - the parser answers each one with a result (an error with a message, or a
//     parse) and never throws;
//   - the `sql` capability, the path /v1/sql, MCP `tools/call sql`, the web
//     console and the GUI all take, answers each one on a real node and the node
//     keeps answering afterwards.
#include <gtest/gtest.h>

#include <cstddef>
#include <random>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/postgres.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::BackgroundActions;
using zetta::api::Registry;
using zetta::query::PostgresDialect;
using zetta::test::TempDir;

namespace {

// Malformed statements that must each be refused with a message. The first block
// is the inputs named in the issues.
const std::vector<std::string>& malformed_statements() {
    static const std::vector<std::string> statements = {
        // #819: a malformed table reference.
        "SELECT * FROM system.*",
        "SELECT * FROM system.*;",
        "SELECT * FROM .",
        "SELECT * FROM 1",
        "SELECT * FROM system.",
        "SELECT * FROM *",
        "SELECT * FROM 'system'",
        "SELECT * FROM",
        // #793: REVOKE TOKEN, a column-scoped GRANT, current_user().
        "REVOKE TOKEN abc",
        "REVOKE TOKEN 'abc'",
        "REVOKE TOKEN",
        "GRANT sql:read ON t(a,b) TO x",
        "GRANT sql:read ON app.t(a,b) TO x",
        "GRANT sql:read ON app.t(a, TO ROLE r",
        "GRANT sql:read ON app.t( TO ROLE r",
        "GRANT sql:read ON app.t() TO ROLE r",
        "SELECT current_user()",
        "SELECT current_user",
        "SELECT current_roles()",
        // Every other statement family, cut short or bent.
        "SELCT * FROM t;",
        "CREATE",
        "CREATE TOKEN FOR",
        "CREATE TOKEN FOR 1",
        "CREATE TOKEN FOR u TYPE",
        "CREATE ROLE",
        "CREATE ROLE 'x'",
        "CREATE USER",
        "CREATE DATABASE",
        "CREATE TABLE app.t (",
        "CREATE TABLE app.t (a int(",
        "CREATE TABLE app.t (a int, )",
        "CREATE VIRTUAL TABLE",
        "CREATE VIRTUAL TABLE v",
        "CREATE VIRTUAL TABLE v OVER CRDT",
        "CREATE VIRTUAL TABLE v OVER CRDT 'p' (a TEXT EPHEMERAL)",
        "CREATE POLICY p ON app.t FOR SELECT USING (",
        "CREATE POLICY p ON app.t FOR SELECT USING (a = ANY(",
        "CREATE POLICY p ON app.t FOR DELETE USING (true)",
        "CREATE POLICY p ON app.t FOR SELECT USING (a = -)",
        "CREATE POLICY p ON app.t FOR SELECT WITH (true)",
        "DROP",
        "DROP ROLE",
        "DROP POLICY",
        "DROP TABLE app.*",
        "INSERT INTO app.t VALUES (",
        "INSERT INTO app.t VALUES (-",
        "INSERT INTO app.t VALUES (-'x')",
        "INSERT INTO app.t VALUES (1.2.3)",
        "INSERT INTO app.t (a, VALUES (1)",
        "UPDATE t SET",
        "UPDATE t SET a =",
        "UPDATE t SET a = 1 WHERE a",
        "DELETE",
        "DELETE FROM t WHERE a ! 1",
        "TRUNCATE",
        "SELECT a FROM t LIMIT 99999999999999999999999999",
        "SELECT a FROM t LIMIT -1",
        "SELECT a FROM t LIMIT 1.5",
        "SELECT a FROM t LIMIT 1 LIMIT 2",
        "SELECT a FROM t LIMIT 1 FETCH FIRST 2 ROWS ONLY",
        "SELECT a FROM t FETCH FIRST 2 ROWS",
        "SELECT a FROM t OFFSET",
        "SELECT count(*) AS FROM t",
        "SELECT sum(*) FROM t",
        "SELECT a AS b FROM t",
        "SELECT * FROM t GROUP BY a",
        "SELECT * FROM t WHERE a = 'oops",
        "SELECT * FROM t WHERE",
        "SELECT * FROM t extra",
        "SELECT * FROM t;;; SELECT",
        "GRANT",
        "GRANT ROLE r TO",
        "GRANT sql TO ROLE r",
        "GRANT sql:read ON TO ROLE r",
        "GRANT sql:read ON DATABASE TO ROLE r",
        "GRANT sql:read ON TOOL TO ROLE r",
        "GRANT sql:read TO r",
        "REVOKE",
        "REVOKE ROLE r FROM",
        "REVOKE sql:read ON app.t(a FROM ROLE r",
        "@",
        "SELECT * FROM t WHERE a = $1",
        std::string("SELECT \x01 FROM t"),
        std::string("SELECT * FROM t WHERE a = '\xff\xfe"),
    };
    return statements;
}

// Valid statements covering every branch of the grammar. Their prefixes and
// single-character edits reach every error path the parser has.
const std::vector<std::string>& seed_statements() {
    static const std::vector<std::string> statements = {
        "CREATE DATABASE app;",
        "CREATE TABLE app.t (id int, name varchar(255), price numeric(10,2));",
        "CREATE TABLE t ();",
        "CREATE VIRTUAL TABLE v OVER CRDT 'presence/' (id TEXT DURABLE KEY, "
        "cursor TEXT EPHEMERAL LOCAL, status TEXT EPHEMERAL SYNCED);",
        "CREATE USER ada;",
        "CREATE TOKEN FOR ada TYPE access;",
        "CREATE ROLE analyst;",
        "CREATE POLICY own ON app.t FOR SELECT TO ROLE analyst, auditor USING "
        "(owner = current_user()) WITH CHECK ('admin' = ANY(current_roles()));",
        "CREATE POLICY open ON app.t FOR ALL USING (true);",
        "CREATE POLICY lit ON app.t FOR UPDATE USING (n >= -3);",
        "DROP DATABASE app;",
        "DROP TABLE app.t;",
        "DROP ROLE analyst;",
        "DROP POLICY own ON app.t;",
        "INSERT INTO app.t (id, name) VALUES (1, 'a''b'), (-2.5, null), "
        "(true, false);",
        "UPDATE app.t SET name = 'x', price = 3 WHERE id <> 1;",
        "DELETE FROM app.t WHERE id != 2;",
        "TRUNCATE TABLE app.t;",
        "SELECT * FROM app.t WHERE id <= 3 ORDER BY id DESC, name ASC LIMIT 5 "
        "OFFSET 2;",
        "SELECT id, count(*) AS n, sum(price), avg(price) FROM app.t GROUP BY "
        "id;",
        "SELECT id FROM app.t OFFSET 1 ROWS FETCH NEXT 3 ROWS ONLY;",
        "SELECT * FROM system.tables; -- a trailing comment",
        "GRANT ROLE analyst TO ada;",
        "GRANT sql:read, sql:write ON app.t(id, name) TO ROLE analyst;",
        "GRANT sql:read ON app.* TO ROLE analyst;",
        "GRANT sql:read ON DATABASE app TO ROLE analyst;",
        "GRANT mcp:call ON TOOL 'search' TO ROLE analyst;",
        "GRANT blobs:read ON BLOB 'b/' TO ROLE analyst;",
        "GRANT records:read ON COLLECTION c TO ROLE analyst;",
        "GRANT objects:read ON BUCKET bk TO ROLE analyst;",
        "REVOKE ROLE analyst FROM ada;",
        "REVOKE sql:read ON TABLE app.t FROM ROLE analyst;",
    };
    return statements;
}

// Every byte prefix of every seed statement.
std::vector<std::string> prefixes_of_seeds() {
    std::vector<std::string> out;
    for (const std::string& seed : seed_statements()) {
        for (std::size_t length = 0; length < seed.size(); ++length) {
            out.push_back(seed.substr(0, length));
        }
    }
    return out;
}

// Every seed with one character deleted, or replaced by a character that bends
// the grammar.
std::vector<std::string> edits_of_seeds() {
    static constexpr std::string_view kReplacements = "*.(),;'-:=x1";
    std::vector<std::string> out;
    for (const std::string& seed : seed_statements()) {
        for (std::size_t i = 0; i < seed.size(); ++i) {
            std::string deleted = seed;
            deleted.erase(i, 1);
            out.push_back(std::move(deleted));
            for (const char c : kReplacements) {
                if (seed[i] == c) continue;
                std::string replaced = seed;
                replaced[i] = c;
                out.push_back(std::move(replaced));
            }
        }
    }
    return out;
}

// Statements of random tokens drawn from the grammar's own vocabulary, from a
// fixed seed so a failure reproduces.
std::vector<std::string> random_token_soup(std::size_t count) {
    static const std::vector<std::string> kTokens = {
        "SELECT", "FROM", "WHERE", "CREATE", "DROP", "TABLE", "DATABASE",
        "VIRTUAL", "OVER", "CRDT", "TOKEN", "ROLE", "POLICY", "USER", "FOR",
        "TYPE", "GRANT", "REVOKE", "ON", "TO", "INSERT", "INTO", "VALUES",
        "UPDATE", "SET", "DELETE", "TRUNCATE", "GROUP", "BY", "ORDER", "LIMIT",
        "OFFSET", "FETCH", "FIRST", "ROWS", "ONLY", "USING", "WITH", "CHECK",
        "ANY", "current_user", "current_roles", "count", "sum", "AS", "system",
        "t", "*", ".", "(", ")", ",", ";", "=", "<", ">", "!", "-", ":", "'s'",
        "'", "1", "1.5", "99999999999999999999999", "true", "null", "--"};
    std::mt19937 random(0x2a5eed);
    std::uniform_int_distribution<std::size_t> pick(0, kTokens.size() - 1);
    std::uniform_int_distribution<std::size_t> length(1, 14);
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t n = 0; n < count; ++n) {
        std::string statement;
        const std::size_t tokens = length(random);
        for (std::size_t t = 0; t < tokens; ++t) {
            if (t != 0) statement += ' ';
            statement += kTokens[pick(random)];
        }
        out.push_back(std::move(statement));
    }
    return out;
}

std::vector<std::string> fuzz_corpus() {
    std::vector<std::string> corpus = malformed_statements();
    for (auto&& s : prefixes_of_seeds()) corpus.push_back(std::move(s));
    for (auto&& s : edits_of_seeds()) corpus.push_back(std::move(s));
    for (auto&& s : random_token_soup(4000)) corpus.push_back(std::move(s));
    return corpus;
}

}  // namespace

// The parser refuses each named malformed statement with a located message.
TEST(ParseErrorsNeverCrash, ParserRefusesEveryNamedMalformedStatement) {
    const PostgresDialect pg;
    for (const std::string& sql : malformed_statements()) {
        const auto parsed = pg.parse(sql);
        EXPECT_FALSE(parsed.ok()) << sql;
        EXPECT_FALSE(parsed.error.empty()) << sql;
        EXPECT_TRUE(parsed.statements.empty()) << sql;
    }
}

// The issues' inputs say why they failed, at the right token.
TEST(ParseErrorsNeverCrash, ParserNamesTheOffendingToken) {
    const PostgresDialect pg;
    const auto table = pg.parse("SELECT * FROM system.*");
    EXPECT_EQ(table.error,
              "expected an identifier, got '*' (at position 22, near '*')");
    const auto revoke = pg.parse("REVOKE TOKEN abc");
    EXPECT_EQ(revoke.error,
              "expected ':', got 'abc' (at position 14, near 'abc')");
    const auto user = pg.parse("SELECT current_user()");
    EXPECT_EQ(user.error,
              "expected 'from', got '(' (at position 20, near '(')");
    const auto unterminated = pg.parse("SELECT 'oops");
    EXPECT_EQ(unterminated.error, "unterminated string literal (at position 8)");
}

// Every statement of the fuzz corpus gets an answer from the parser: a parse, or
// an error with a message and no statements. None throws, none aborts.
TEST(ParseErrorsNeverCrash, ParserAnswersEveryStatementOfTheFuzzCorpus) {
    const PostgresDialect pg;
    const std::vector<std::string> corpus = fuzz_corpus();
    ASSERT_GT(corpus.size(), 10000u);
    std::size_t refused = 0;
    for (const std::string& sql : corpus) {
        const auto parsed = pg.parse(sql);
        if (!parsed.ok()) {
            ++refused;
            EXPECT_TRUE(parsed.statements.empty()) << sql;
        }
    }
    EXPECT_GT(refused, corpus.size() / 2);
}

// The real query path on a real node: the `sql` capability (what /v1/sql, MCP
// `tools/call sql`, the web console and the GUI dispatch) answers every malformed
// statement with a refusal, and the node keeps answering.
struct ParseErrorsNeverCrashOnANode : ::testing::Test {
    TempDir dir;
    Node node{NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    BackgroundActions background;
    zetta::observability::TraceContext trace =
        zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override {
        background.prune();
        node.stop();
    }

    ApiResult sql(const std::string& statement) {
        ApiContext context{node,  nullptr, {}, trace, &node.span_recorder(),
                           &background};
        return registry.call("sql", {{"statement", statement}}, context);
    }

    void expect_node_answers() {
        const ApiResult probe = sql("SELECT * FROM system.tables;");
        EXPECT_TRUE(probe.ok) << probe.error.message;
    }
};

TEST_F(ParseErrorsNeverCrashOnANode, SqlCapabilityRefusesEveryNamedMalformedStatement) {
    for (const std::string& statement : malformed_statements()) {
        const ApiResult result = sql(statement);
        EXPECT_FALSE(result.ok) << statement;
        EXPECT_EQ(result.error.code, "engine_error") << statement;
        EXPECT_FALSE(result.error.message.empty()) << statement;
    }
    expect_node_answers();
}

TEST_F(ParseErrorsNeverCrashOnANode, SqlCapabilityAnswersTheFuzzCorpusAndStaysUp) {
    // Prefixes and the named inputs on the node (the edits and the token soup run
    // through the parser above; a valid prefix here really executes, which is
    // fine: the point is that every call returns).
    std::vector<std::string> corpus = malformed_statements();
    for (auto&& s : prefixes_of_seeds()) corpus.push_back(std::move(s));
    for (auto&& s : random_token_soup(500)) corpus.push_back(std::move(s));
    for (const std::string& statement : corpus) {
        const ApiResult result = sql(statement);
        if (!result.ok) EXPECT_FALSE(result.error.message.empty()) << statement;
    }
    expect_node_answers();
}
