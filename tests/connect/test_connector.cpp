#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/connect/connector.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/postgres_runner.hpp"
#include "zetta/connect/source_catalog.hpp"

// #827 — source connectors: run a read-only query against an external system and
// return rows, which data::add_dataset_rows then shards across the cluster. The
// HTTP exchange is injected (the CommandRunner pattern), so every case here runs
// with NO network, NO credentials, and NO warehouse — a fake transport answers
// with canned Snowflake SQL API v2 payloads.
namespace {

using zetta::connect::ConnectorConfig;
using zetta::connect::ConnectorResult;
using zetta::connect::HttpRequest;
using zetta::connect::HttpResponse;
using zetta::connect::HttpTransport;
using zetta::connect::read_only_query_rejection;
using zetta::connect::run_connector_query;

ConnectorConfig snowflake_config() {
    ConnectorConfig config;
    config.type = "snowflake";
    config.account = "ab12345.us-east-1";
    config.token = "secret-token";
    config.warehouse = "COMPUTE_WH";
    config.database = "ANALYTICS";
    config.schema = "PUBLIC";
    return config;
}

// A canned SQL API v2 success body: two typed columns, `rows` data rows.
std::string result_body(const std::vector<std::string>& data_rows,
                        int partition_count = 1,
                        const std::string& handle = "handle-1") {
    std::string data;
    for (std::size_t i = 0; i < data_rows.size(); ++i) {
        if (i) data += ",";
        data += data_rows[i];
    }
    std::string partitions;
    for (int i = 0; i < partition_count; ++i) {
        if (i) partitions += ",";
        partitions += "{\"rowCount\": 1}";
    }
    return R"({"resultSetMetaData": {"rowType": [)"
           R"({"name": "ACCOUNT_ID", "type": "text"},)"
           R"({"name": "MRR", "type": "fixed"},)"
           R"({"name": "CHURNED", "type": "boolean"}],)"
           R"("partitionInfo": [)" + partitions + R"(]},)"
           R"("statementHandle": ")" + handle + R"(",)"
           R"("data": [)" + data + "]}";
}

// A transport that always answers with one canned response, recording requests.
HttpTransport canned(int status, std::string body,
                     std::vector<HttpRequest>* seen = nullptr) {
    return [status, body = std::move(body), seen](const HttpRequest& request) {
        if (seen) seen->push_back(request);
        HttpResponse response;
        response.status = status;
        response.body = body;
        return response;
    };
}

// =====================================================================
// ConnectorConfig boundary
// =====================================================================

TEST(ConnectorConfig, ParsesAValidSnowflakeConfig) {
    const auto value = nlohmann::json::parse(R"({
        "type": "snowflake", "account": "ab12345", "token": "tok",
        "warehouse": "WH", "database": "DB", "schema": "S", "role": "R"
    })");
    const auto config = ConnectorConfig::from_json(value);
    ASSERT_TRUE(config.has_value());
    EXPECT_EQ(config->type, "snowflake");
    EXPECT_EQ(config->account, "ab12345");
    EXPECT_EQ(config->token, "tok");
    EXPECT_EQ(config->auth, "pat");  // default
    EXPECT_EQ(config->warehouse, "WH");
    EXPECT_EQ(config->role, "R");
}

TEST(ConnectorConfig, NeverSerializesTheToken) {
    const ConnectorConfig config = snowflake_config();
    const nlohmann::json value = config.to_json();
    EXPECT_FALSE(value.contains("token"));
    // …and the secret does not leak through any other field.
    EXPECT_EQ(value.dump().find("secret-token"), std::string::npos);
    EXPECT_EQ(value["account"], "ab12345.us-east-1");
}

TEST(ConnectorConfig, RejectsMalformedConfigs) {
    const auto rejected = [](const char* json) {
        const auto value =
            nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
        EXPECT_FALSE(ConnectorConfig::from_json(value).has_value()) << json;
    };
    rejected(R"({"account": "a", "token": "t"})");                    // no type
    rejected(R"({"type": "redshift", "account": "a", "token": "t"})"); // unknown type
    rejected(R"({"type": "snowflake", "token": "t"})");               // no account
    rejected(R"({"type": "snowflake", "account": "a"})");             // no token
    rejected(R"({"type": "snowflake", "account": "a", "token": ""})"); // empty token
    rejected(R"({"type": "snowflake", "account": "a", "token": "t", "auth": "basic"})");
    rejected(R"({"type": "snowflake", "account": "a", "token": "t", "nope": 1})");
    rejected(R"({"type": "snowflake", "account": 7, "token": "t"})");  // wrong type
    rejected(R"(["not", "an", "object"])");
}

// =====================================================================
// The read-only guard — a connector reads, never writes
// =====================================================================

TEST(ConnectorQuery, AcceptsOnlyReadOnlySingleStatements) {
    EXPECT_EQ(read_only_query_rejection("SELECT * FROM churn"), "");
    EXPECT_EQ(read_only_query_rejection("  select 1"), "");
    EXPECT_EQ(read_only_query_rejection("WITH x AS (SELECT 1) SELECT * FROM x"), "");
    EXPECT_EQ(read_only_query_rejection("-- a comment\nSELECT 1"), "");
    EXPECT_EQ(read_only_query_rejection("/* block */ SELECT 1"), "");
    EXPECT_EQ(read_only_query_rejection("SELECT 1;"), "");  // trailing ; is fine

    EXPECT_NE(read_only_query_rejection(""), "");
    EXPECT_NE(read_only_query_rejection("DROP TABLE accounts"), "");
    EXPECT_NE(read_only_query_rejection("INSERT INTO t VALUES (1)"), "");
    EXPECT_NE(read_only_query_rejection("UPDATE t SET a = 1"), "");
    EXPECT_NE(read_only_query_rejection("DELETE FROM t"), "");
    EXPECT_NE(read_only_query_rejection("COPY INTO t FROM @s"), "");
    EXPECT_NE(read_only_query_rejection("CALL proc()"), "");
    // A second statement smuggled after a read-only one.
    EXPECT_NE(read_only_query_rejection("SELECT 1; DROP TABLE accounts"), "");
    // "selection" is not "select".
    EXPECT_NE(read_only_query_rejection("selectivity FROM t"), "");
}

TEST(ConnectorQuery, WriteStatementNeverReachesTheTransport) {
    std::vector<HttpRequest> seen;
    const ConnectorResult result = run_connector_query(
        snowflake_config(), "DROP TABLE accounts", 100, canned(200, "{}", &seen));
    EXPECT_FALSE(result.ok());
    EXPECT_TRUE(seen.empty()) << "a write statement must be rejected before egress";
}

// =====================================================================
// Snowflake: request shape + typed rows
// =====================================================================

TEST(SnowflakeConnector, BuildsTheSqlApiRequestAndTypesTheRows) {
    std::vector<HttpRequest> seen;
    const std::string body = result_body(
        {R"(["acct_globex", "1200", "false"])", R"(["acct_initech", "3400", "true"])"});
    const ConnectorResult result =
        run_connector_query(snowflake_config(), "SELECT * FROM churn", 100,
                            canned(200, body, &seen));
    ASSERT_TRUE(result.ok()) << result.error;

    // The request went to the SQL API v2 statements endpoint…
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].method, "POST");
    EXPECT_EQ(seen[0].url,
              "https://ab12345.us-east-1.snowflakecomputing.com/api/v2/statements");

    // …as a bearer PAT, with the token marked secret so it never hits argv.
    bool has_bearer = false, has_token_type = false;
    for (const auto& [name, value] : seen[0].headers) {
        if (name == "Authorization") {
            has_bearer = (value == "Bearer secret-token");
        }
        if (name == "X-Snowflake-Authorization-Token-Type") {
            has_token_type = (value == "PROGRAMMATIC_ACCESS_TOKEN");
        }
    }
    EXPECT_TRUE(has_bearer);
    EXPECT_TRUE(has_token_type);
    ASSERT_EQ(seen[0].secret_headers.size(), 1u);
    EXPECT_EQ(seen[0].secret_headers[0], "Authorization");

    // The body carried the statement + session context.
    const auto sent =
        nlohmann::json::parse(seen[0].body, nullptr, /*allow_exceptions=*/false);
    ASSERT_TRUE(sent.is_object());
    EXPECT_EQ(sent["statement"], "SELECT * FROM churn");
    EXPECT_EQ(sent["warehouse"], "COMPUTE_WH");
    EXPECT_EQ(sent["database"], "ANALYTICS");

    // Columns come from rowType; cells are coerced to REAL JSON types, so the
    // landed dataset is featurizable rather than a table of strings.
    ASSERT_EQ(result.data.columns.size(), 3u);
    EXPECT_EQ(result.data.columns[0], "ACCOUNT_ID");
    ASSERT_EQ(result.data.rows.size(), 2u);
    EXPECT_EQ(result.data.rows[0]["ACCOUNT_ID"], "acct_globex");
    EXPECT_TRUE(result.data.rows[0]["MRR"].is_number());
    EXPECT_EQ(result.data.rows[0]["MRR"], 1200);
    EXPECT_TRUE(result.data.rows[0]["CHURNED"].is_boolean());
    EXPECT_EQ(result.data.rows[0]["CHURNED"], false);
    EXPECT_EQ(result.data.rows[1]["CHURNED"], true);
}

TEST(SnowflakeConnector, OAuthUsesTheOauthTokenType) {
    std::vector<HttpRequest> seen;
    ConnectorConfig config = snowflake_config();
    config.auth = "oauth";
    run_connector_query(config, "SELECT 1", 10, canned(200, result_body({}), &seen));
    ASSERT_EQ(seen.size(), 1u);
    bool oauth = false;
    for (const auto& [name, value] : seen[0].headers) {
        if (name == "X-Snowflake-Authorization-Token-Type") oauth = (value == "OAUTH");
    }
    EXPECT_TRUE(oauth);
}

TEST(SnowflakeConnector, NullCellsBecomeJsonNull) {
    const ConnectorResult result = run_connector_query(
        snowflake_config(), "SELECT 1", 10,
        canned(200, result_body({R"(["acct", null, null])"})));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.data.rows.size(), 1u);
    EXPECT_TRUE(result.data.rows[0]["MRR"].is_null());
    EXPECT_TRUE(result.data.rows[0]["CHURNED"].is_null());
}

TEST(SnowflakeConnector, CapsRowsAtMaxRows) {
    const ConnectorResult result = run_connector_query(
        snowflake_config(), "SELECT 1", 2,
        canned(200, result_body({R"(["a", "1", "true"])", R"(["b", "2", "true"])",
                                 R"(["c", "3", "true"])"})));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.data.rows.size(), 2u);
}

// =====================================================================
// Failure paths
// =====================================================================

TEST(SnowflakeConnector, SurfacesAnErrorStatusWithTheSourceMessage) {
    const ConnectorResult result = run_connector_query(
        snowflake_config(), "SELECT * FROM missing", 10,
        canned(422, R"({"message": "SQL compilation error: table does not exist"})"));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("422"), std::string::npos);
    EXPECT_NE(result.error.find("SQL compilation error"), std::string::npos);
}

TEST(SnowflakeConnector, SurfacesATransportFailure) {
    const HttpTransport broken = [](const HttpRequest&) {
        HttpResponse response;
        response.error = "could not resolve host";
        return response;
    };
    const ConnectorResult result =
        run_connector_query(snowflake_config(), "SELECT 1", 10, broken);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("could not resolve host"), std::string::npos);
}

TEST(SnowflakeConnector, RejectsAMalformedResponse) {
    const ConnectorResult result = run_connector_query(
        snowflake_config(), "SELECT 1", 10, canned(200, R"({"nope": true})"));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("resultSetMetaData"), std::string::npos);
}

TEST(ConnectorQuery, RejectsAnUnknownConnectorType) {
    ConnectorConfig config = snowflake_config();
    config.type = "redshift";
    const ConnectorResult result =
        run_connector_query(config, "SELECT 1", 10, canned(200, "{}"));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("redshift"), std::string::npos);
}

// =====================================================================
// Multi-partition result sets + async execution
// =====================================================================

TEST(SnowflakeConnector, FetchesAdditionalResultPartitions) {
    // First response advertises two partitions and carries partition 0; the
    // connector must fetch partition 1 off the same statement handle.
    std::vector<HttpRequest> seen;
    const HttpTransport paged = [&seen](const HttpRequest& request) {
        seen.push_back(request);
        HttpResponse response;
        response.status = 200;
        response.body = request.url.find("partition=1") != std::string::npos
                            ? result_body({R"(["b", "2", "true"])"}, 2)
                            : result_body({R"(["a", "1", "false"])"}, 2);
        return response;
    };

    const ConnectorResult result =
        run_connector_query(snowflake_config(), "SELECT * FROM churn", 100, paged);
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.data.rows.size(), 2u) << "both partitions should land";
    EXPECT_EQ(result.data.rows[0]["ACCOUNT_ID"], "a");
    EXPECT_EQ(result.data.rows[1]["ACCOUNT_ID"], "b");

    ASSERT_EQ(seen.size(), 2u);
    EXPECT_EQ(seen[1].method, "GET");
    EXPECT_NE(seen[1].url.find("/handle-1?partition=1"), std::string::npos);
}

TEST(SnowflakeConnector, PollsAnAsynchronouslyExecutingStatement) {
    // 202 => still running: the connector polls the handle until it completes.
    int calls = 0;
    const HttpTransport async_then_done = [&calls](const HttpRequest& request) {
        HttpResponse response;
        if (calls++ == 0) {
            response.status = 202;
            response.body = R"({"statementHandle": "handle-1", "message": "running"})";
            return response;
        }
        EXPECT_EQ(request.method, "GET");
        EXPECT_NE(request.url.find("/handle-1"), std::string::npos);
        response.status = 200;
        response.body = result_body({R"(["acct", "42", "true"])"});
        return response;
    };

    const ConnectorResult result = run_connector_query(
        snowflake_config(), "SELECT * FROM churn", 10, async_then_done);
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.data.rows.size(), 1u);
    EXPECT_EQ(result.data.rows[0]["MRR"], 42);
    EXPECT_EQ(calls, 2);
}

TEST(SnowflakeConnector, AsyncWithoutAHandleFails) {
    const ConnectorResult result = run_connector_query(
        snowflake_config(), "SELECT 1", 10, canned(202, R"({"message": "running"})"));
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("statementHandle"), std::string::npos);
}

// Decode a single-quoted constant under the source's backslash rules. Checking
// the SQL after its closing delimiter catches early closes and value drift.
void expect_sql_literal(std::string_view sql, const std::string& expected,
                        std::string_view suffix, bool backslash_escapes = true) {
    ASSERT_TRUE(sql.starts_with('\''));
    sql.remove_prefix(1);
    std::string decoded;
    while (!sql.empty()) {
        const char character = sql.front();
        sql.remove_prefix(1);
        if (backslash_escapes && character == '\\') {
            ASSERT_FALSE(sql.empty());
            // The encoder must protect literal backslashes, including escape
            // spellings such as \n, \x27 and \u0027 supplied as text.
            EXPECT_EQ(sql.front(), '\\');
            decoded += sql.front();
            sql.remove_prefix(1);
        } else if (character == '\'') {
            if (sql.starts_with('\'')) {
                decoded += '\'';
                sql.remove_prefix(1);
            } else {
                EXPECT_EQ(decoded, expected);
                EXPECT_EQ(sql, suffix);
                return;
            }
        } else {
            decoded += character;
        }
    }
    FAIL() << "unterminated SQL literal";
}

const std::vector<std::string> catalog_filters = {
    "plain", "O'Reilly", "\\", "\\\\", "\\' OR 1=1 --", "\\\\'",
    "' UNION SELECT 'private', 'data', 'VIEW' --", "$$", "雪\\'",
    R"(\n\r\t\x27\u0027\101)", "line\nquote'\\tail"};

// A table response ensures include_columns also reaches the second query.
std::vector<HttpRequest> snowflake_catalog_requests(
    const zetta::connect::SourceCatalogRequest& request) {
    std::vector<HttpRequest> seen;
    const auto catalog = zetta::connect::list_source_catalog(
        snowflake_config(), request,
        canned(200, R"({"resultSetMetaData":{"rowType":[
            {"name":"table_schema","type":"text"},
            {"name":"table_name","type":"text"},
            {"name":"table_type","type":"text"}]},
            "data":[["PUBLIC","accounts","BASE TABLE"]]})", &seen));
    EXPECT_TRUE(catalog.ok()) << catalog.error;
    EXPECT_EQ(catalog.tables.size(), 1u);
    return seen;
}

TEST(SnowflakeCatalog, SchemaFiltersRemainOneLiteralInBothQueries) {
    for (const auto& filter : catalog_filters) {
        SCOPED_TRACE(filter);
        zetta::connect::SourceCatalogRequest request;
        request.schema = filter;
        const auto seen = snowflake_catalog_requests(request);
        ASSERT_EQ(seen.size(), 2u);
        for (std::size_t index = 0; index < seen.size(); ++index) {
            const auto body = nlohmann::json::parse(seen[index].body);
            const std::string sql = body.at("statement").get<std::string>();
            const std::string prefix = index == 0
                ? "SELECT t.table_schema, t.table_name, t.table_type"
                  " FROM information_schema.tables t"
                  " WHERE t.table_schema NOT IN ('pg_catalog', 'information_schema')"
                  " AND t.table_schema = "
                : "SELECT c.table_schema, c.table_name, c.column_name, c.data_type,"
                  " c.is_nullable, c.ordinal_position FROM information_schema.columns c"
                  " WHERE c.table_schema NOT IN ('pg_catalog', 'information_schema')"
                  " AND c.table_schema = ";
            ASSERT_TRUE(sql.starts_with(prefix)) << sql;
            expect_sql_literal(std::string_view(sql).substr(prefix.size()), filter,
                index == 0 ? " ORDER BY t.table_schema, t.table_name"
                           : " ORDER BY c.table_schema, c.table_name, c.ordinal_position");
        }
    }
}

TEST(SnowflakeCatalog, TableMatchesRemainOneLiteral) {
    for (const auto& filter : catalog_filters) {
        SCOPED_TRACE(filter);
        zetta::connect::SourceCatalogRequest request;
        request.match = filter;
        request.include_columns = false;
        const auto seen = snowflake_catalog_requests(request);
        ASSERT_EQ(seen.size(), 1u);
        const auto body = nlohmann::json::parse(seen.front().body);
        const std::string sql = body.at("statement").get<std::string>();
        const std::string prefix =
            "SELECT t.table_schema, t.table_name, t.table_type"
            " FROM information_schema.tables t"
            " WHERE t.table_schema NOT IN ('pg_catalog', 'information_schema')"
            " AND LOWER(t.table_name) LIKE ";
        ASSERT_TRUE(sql.starts_with(prefix)) << sql;
        std::string expected = "%" + filter + "%";
        std::ranges::transform(expected, expected.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        expect_sql_literal(std::string_view(sql).substr(prefix.size()), expected,
                                  " ORDER BY t.table_schema, t.table_name");
    }
}

// The fake runner observes the same SQL with standard_conforming_strings on
// and off. An E prefix enables escapes in either setting; ordinary literals
// take backslash escapes only when the setting is off.
class PostgresCatalog : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        previous_runner = zetta::connect::postgres_query_runner();
        zetta::connect::install_postgres_query_runner(
            [this](const ConnectorConfig&, const std::string& sql, std::size_t) {
                statements.push_back(sql);
                ConnectorResult result;
                result.data.rows.push_back({{"table_schema", "PUBLIC"},
                                            {"table_name", "accounts"},
                                            {"table_type", "BASE TABLE"}});
                return result;
            });
    }

    void TearDown() override {
        zetta::connect::install_postgres_query_runner(std::move(previous_runner));
    }

    void list(const zetta::connect::SourceCatalogRequest& request) {
        statements.clear();
        ConnectorConfig config;
        config.type = "postgres";
        const auto catalog = zetta::connect::list_source_catalog(config, request);
        EXPECT_TRUE(catalog.ok()) << catalog.error;
        EXPECT_EQ(catalog.tables.size(), 1u);
    }

    void expect_literal(std::string_view sql, const std::string& expected,
                         std::string_view suffix) {
        bool backslash_escapes = !GetParam();
        if (sql.starts_with("E'")) {
            sql.remove_prefix(1);
            backslash_escapes = true;
        }
        expect_sql_literal(sql, expected, suffix, backslash_escapes);
    }

    std::vector<std::string> statements;

private:
    zetta::connect::PostgresQueryRunner previous_runner;
};

TEST_P(PostgresCatalog, SchemaFiltersRemainOneLiteralInBothQueries) {
    for (const auto& filter : catalog_filters) {
        SCOPED_TRACE(filter);
        zetta::connect::SourceCatalogRequest request;
        request.schema = filter;
        list(request);
        ASSERT_EQ(statements.size(), 2u);
        for (std::size_t index = 0; index < statements.size(); ++index) {
            const std::string marker = index == 0 ? " AND t.table_schema = "
                                                   : " AND c.table_schema = ";
            const auto found = statements[index].find(marker);
            ASSERT_NE(found, std::string::npos);
            expect_literal(std::string_view(statements[index]).substr(found + marker.size()),
                filter, index == 0 ? " ORDER BY t.table_schema, t.table_name"
                                   : " ORDER BY c.table_schema, c.table_name, c.ordinal_position");
        }
    }
}

TEST_P(PostgresCatalog, TableMatchesRemainOneLiteral) {
    for (const auto& filter : catalog_filters) {
        SCOPED_TRACE(filter);
        zetta::connect::SourceCatalogRequest request;
        request.match = filter;
        request.include_columns = false;
        list(request);
        ASSERT_EQ(statements.size(), 1u);
        const std::string marker = " AND LOWER(t.table_name) LIKE ";
        const auto found = statements.front().find(marker);
        ASSERT_NE(found, std::string::npos);
        std::string expected = "%" + filter + "%";
        std::ranges::transform(expected, expected.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        expect_literal(std::string_view(statements.front()).substr(found + marker.size()),
                         expected, " ORDER BY t.table_schema, t.table_name");
    }
}

INSTANTIATE_TEST_SUITE_P(StringSettings, PostgresCatalog, testing::Bool());

}  // namespace
