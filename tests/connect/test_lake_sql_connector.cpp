#include <gtest/gtest.h>

#include <cstdlib>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/api/native_http_transport.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/http_transport.hpp"
#include "zetta/connect/source_catalog.hpp"

// #1014 — lake's SQL-over-HTTP services (lake-sqlite, lake-duckdb, lake-polars)
// as source connectors. All three answer `POST /v1/query {"sql"}` with
// `{"columns", "rows", "truncated"}` behind a bearer token. Every case here runs
// against a fake transport: no network, no service, no credential. The one
// live case is guarded by ZETTA_LAKE_SQL_TEST_ENDPOINT.
namespace {

using zetta::connect::ConnectorConfig;
using zetta::connect::ConnectorResult;
using zetta::connect::HttpRequest;
using zetta::connect::HttpResponse;
using zetta::connect::HttpTransport;
using zetta::connect::run_connector_query;

constexpr const char* kToken = "lake-service-bearer-0123456789";

ConnectorConfig lake_config(const std::string& type = "lake_sqlite",
                            const std::string& endpoint =
                                "http://orders.ws-1.svc.cluster.local:8080") {
    ConnectorConfig config;
    config.type = type;
    config.endpoint = endpoint;
    config.token = kToken;
    return config;
}

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

// A transport that must never be called.
HttpTransport unreachable(bool* called) {
    return [called](const HttpRequest&) {
        *called = true;
        return HttpResponse{};
    };
}

std::string header(const HttpRequest& request, const std::string& name) {
    for (const auto& [key, value] : request.headers) {
        if (key == name) return value;
    }
    return {};
}

// =====================================================================
// The config boundary
// =====================================================================

TEST(LakeSqlConfig, ParsesEachServiceType) {
    for (const char* type : {"lake_sqlite", "lake_duckdb", "lake_polars"}) {
        const auto config = ConnectorConfig::from_json(
            {{"type", type},
             {"endpoint", "http://svc.ns.svc.cluster.local:8080"},
             {"token", "t"}});
        ASSERT_TRUE(config.has_value()) << type;
        EXPECT_EQ(config->endpoint, "http://svc.ns.svc.cluster.local:8080");
    }
}

TEST(LakeSqlConfig, RequiresAnEndpointAndAToken) {
    EXPECT_FALSE(ConnectorConfig::from_json({{"type", "lake_sqlite"}, {"token", "t"}}));
    EXPECT_FALSE(ConnectorConfig::from_json(
        {{"type", "lake_duckdb"}, {"endpoint", "http://svc:8080"}}));
}

TEST(LakeSqlConfig, RefusesAnEndpointThatIsNotAPlainBaseUrl) {
    for (const char* endpoint :
         {"ftp://svc:8080", "http://user:pass@svc:8080", "http://svc:8080/?x=1",
          "http://svc:8080/#f", "http://:8080", "http://svc:0", "http://svc:99999",
          "http://svc:80a", "http://[::1"}) {
        EXPECT_FALSE(ConnectorConfig::from_json(
            {{"type", "lake_sqlite"}, {"endpoint", endpoint}, {"token", "t"}}))
            << endpoint;
    }
}

TEST(LakeSqlConfig, NeverSerializesTheTokenButKeepsTheEndpoint) {
    const nlohmann::json json = lake_config().to_json();
    EXPECT_EQ(json.at("endpoint"), "http://orders.ws-1.svc.cluster.local:8080");
    EXPECT_FALSE(json.contains("token"));
    EXPECT_EQ(json.dump().find(kToken), std::string::npos);
}

TEST(LakeSqlConfig, EndpointHostReadsTheHost) {
    EXPECT_EQ(zetta::connect::endpoint_host("http://127.0.0.1:8080/base"), "127.0.0.1");
    EXPECT_EQ(zetta::connect::endpoint_host("https://[::1]:8443"), "::1");
    EXPECT_EQ(zetta::connect::endpoint_host("svc.ns.svc.cluster.local"),
              "svc.ns.svc.cluster.local");
    EXPECT_EQ(zetta::connect::endpoint_host("ftp://x"), "");
}

// =====================================================================
// Read-only: the guard
// =====================================================================

TEST(LakeSqlGuard, AdmitsReadsIncludingWordsInsideLiteralsAndQuotedNames) {
    const ConnectorConfig config = lake_config();
    for (const char* sql :
         {"SELECT * FROM orders", "select id from t where note = 'delete me'",
          "WITH recent AS (SELECT * FROM orders) SELECT count(*) FROM recent",
          "SELECT \"update\", \"set\" FROM audit",
          "SELECT replace(name, 'a', 'b') FROM t",
          "SELECT 1 -- insert into nothing\n",
          "SELECT /* drop */ 1;",
          "SELECT last_update, created_at FROM t"}) {
        EXPECT_EQ(zetta::connect::connector_query_rejection(config, sql), "") << sql;
    }
}

TEST(LakeSqlGuard, RefusesWritesAndSessionChangesWhereverTheyHide) {
    const ConnectorConfig config = lake_config("lake_duckdb");
    for (const char* sql :
         {"INSERT INTO t VALUES (1)", "WITH x AS (SELECT 1) DELETE FROM t",
          "WITH x AS (SELECT 1) INSERT INTO t SELECT * FROM x",
          "WITH x AS (SELECT 1) UPDATE t SET a = 1", "SELECT 1; DROP TABLE t",
          "WITH x AS (SELECT 1) REPLACE INTO t SELECT * FROM x",
          "ATTACH 'other.db' AS o", "PRAGMA query_only = 0",
          "SELECT 1 FROM t WHERE 'a' = 'a' AND (SELECT 1) = 1 UNION SELECT 1 FROM "
          "(SELECT 1) x; COPY t TO 'f'",
          "WITH x AS (SELECT 1) INSTALL httpfs", "with \"q\" as (select 1) Delete from t",
          "SET memory_limit = '1TB'", "",
          // DuckDB's E'…' strings take backslash escapes: E'a\'' is one string,
          // so the DELETE after it is code, whichever way the quote is read.
          "WITH x AS (SELECT E'a\\'') DELETE FROM t WHERE 'b' = 'b'",
          // `--1` is not a comment to every engine; what follows is scanned.
          "WITH x AS (SELECT 1 --1 ) DELETE FROM t\n"}) {
        EXPECT_NE(zetta::connect::connector_query_rejection(config, sql), "") << sql;
    }
}

TEST(LakeSqlGuard, ARefusedQueryNeverReachesTheService) {
    bool called = false;
    const ConnectorResult result = run_connector_query(
        lake_config(), "WITH x AS (SELECT 1) DELETE FROM orders", 10,
        unreachable(&called));
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(called);
}

// =====================================================================
// The query exchange
// =====================================================================

TEST(LakeSqlConnector, PostsTheSqlWithTheBearerMarkedSecret) {
    std::vector<HttpRequest> seen;
    const ConnectorResult result = run_connector_query(
        lake_config("lake_duckdb", "http://svc.ns.svc.cluster.local:8080/"),
        "SELECT 1 AS n", 10, canned(200, R"({"columns":["n"],"rows":[[1]]})", &seen));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].method, "POST");
    EXPECT_EQ(seen[0].url, "http://svc.ns.svc.cluster.local:8080/v1/query");
    EXPECT_EQ(header(seen[0], "Authorization"), std::string("Bearer ") + kToken);
    ASSERT_EQ(seen[0].secret_headers.size(), 1u);
    EXPECT_EQ(seen[0].secret_headers[0], "Authorization");
    // Only `sql`: the services refuse unknown fields.
    EXPECT_EQ(nlohmann::json::parse(seen[0].body),
              (nlohmann::json{{"sql", "SELECT 1 AS n"}}));
}

TEST(LakeSqlConnector, AnEndpointWithoutASchemeIsHttps) {
    std::vector<HttpRequest> seen;
    const ConnectorResult result = run_connector_query(
        lake_config("lake_polars", "polars.example.com/base"), "SELECT 1 AS n", 10,
        canned(200, R"({"columns":["n"],"rows":[[1]]})", &seen));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].url, "https://polars.example.com/base/v1/query");
}

TEST(LakeSqlConnector, KeepsTheServicesTypesAndFlattensNestedValues) {
    const ConnectorResult result = run_connector_query(
        lake_config("lake_duckdb"), "SELECT * FROM t", 10,
        canned(200, R"({"columns":["i","r","s","b","n","blob","list","interval"],
                       "rows":[[42, 2.5, "ada", true, null, {"$base64":"iVBO"},
                                [1,2], {"months":1,"days":2,"micros":3}]]})"));
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.data.rows.size(), 1u);
    const nlohmann::json& row = result.data.rows[0];
    EXPECT_EQ(result.data.columns.size(), 8u);
    EXPECT_TRUE(row.at("i").is_number_integer());
    EXPECT_EQ(row.at("i"), 42);
    EXPECT_DOUBLE_EQ(row.at("r").get<double>(), 2.5);
    EXPECT_EQ(row.at("s"), "ada");
    EXPECT_EQ(row.at("b"), true);
    EXPECT_TRUE(row.at("n").is_null());
    EXPECT_EQ(row.at("blob"), "iVBO");
    EXPECT_EQ(row.at("list"), "[1,2]");
    EXPECT_EQ(nlohmann::json::parse(row.at("interval").get<std::string>()).at("days"), 2);
}

TEST(LakeSqlConnector, CapsAtMaxRows) {
    const ConnectorResult result = run_connector_query(
        lake_config(), "SELECT n FROM t", 2,
        canned(200, R"({"columns":["n"],"rows":[[1],[2],[3]]})"));
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.data.rows.size(), 2u);
}

TEST(LakeSqlConnector, ATruncatedAnswerShortOfMaxRowsIsAnErrorNotAShortDataset) {
    const ConnectorResult short_of_ask = run_connector_query(
        lake_config(), "SELECT n FROM t", 50000,
        canned(200, R"({"columns":["n"],"rows":[[1],[2]],"truncated":true})"));
    EXPECT_FALSE(short_of_ask.ok());
    EXPECT_NE(short_of_ask.error.find("10000"), std::string::npos) << short_of_ask.error;
    EXPECT_TRUE(short_of_ask.data.rows.empty());

    // Asking for no more than the service sent is fine: those are the first rows.
    const ConnectorResult within = run_connector_query(
        lake_config(), "SELECT n FROM t", 2,
        canned(200, R"({"columns":["n"],"rows":[[1],[2]],"truncated":true})"));
    ASSERT_TRUE(within.ok()) << within.error;
    EXPECT_EQ(within.data.rows.size(), 2u);
}

TEST(LakeSqlConnector, SurfacesTheServicesErrorWithoutTheToken) {
    const ConnectorResult sql_error = run_connector_query(
        lake_config(), "SELECT * FROM missing", 10,
        canned(400, R"({"error":{"code":"sql_error","message":"no such table: missing"}})"));
    EXPECT_FALSE(sql_error.ok());
    EXPECT_NE(sql_error.error.find("SQLite"), std::string::npos) << sql_error.error;
    EXPECT_NE(sql_error.error.find("sql_error: no such table: missing"), std::string::npos)
        << sql_error.error;
    EXPECT_EQ(sql_error.error.find(kToken), std::string::npos);

    const ConnectorResult unauthorized = run_connector_query(
        lake_config(), "SELECT 1", 10,
        canned(401, R"({"error":{"code":"unauthorized","message":"bad token"}})"));
    EXPECT_FALSE(unauthorized.ok());
    EXPECT_NE(unauthorized.error.find("refused the token"), std::string::npos)
        << unauthorized.error;
    EXPECT_EQ(unauthorized.error.find(kToken), std::string::npos);
}

TEST(LakeSqlConnector, SurfacesATransportFailure) {
    const ConnectorResult result = run_connector_query(
        lake_config(), "SELECT 1", 10, [](const HttpRequest&) {
            HttpResponse response;
            response.error = "connection refused";
            return response;
        });
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("could not reach the SQLite service at "
                                "http://orders.ws-1.svc.cluster.local:8080"),
              std::string::npos)
        << result.error;
    EXPECT_NE(result.error.find("connection refused"), std::string::npos);
}

TEST(LakeSqlConnector, RejectsAMalformedAnswer) {
    for (const char* body :
         {"not json", R"({"rows":[[1]]})", R"({"columns":["a"]})",
          R"({"columns":["a","b"],"rows":[[1]]})", R"({"columns":[1],"rows":[]})"}) {
        const ConnectorResult result =
            run_connector_query(lake_config(), "SELECT 1", 10, canned(200, body));
        EXPECT_FALSE(result.ok()) << body;
        EXPECT_NE(result.error.find("malformed"), std::string::npos) << result.error;
    }
}

TEST(LakeSqlConnector, WithoutAnInstalledTransportFailsCleanly) {
    zetta::connect::set_default_http_transport({}, {});
    const ConnectorResult result = run_connector_query(lake_config(), "SELECT 1", 10);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("no outbound HTTP transport"), std::string::npos)
        << result.error;
}

// =====================================================================
// The catalog (list_source_tables)
// =====================================================================

// Answers the two catalog statements the way the service would.
HttpTransport catalog_service(std::vector<std::string>* statements) {
    return [statements](const HttpRequest& request) {
        const std::string sql = nlohmann::json::parse(request.body).at("sql");
        statements->push_back(sql);
        HttpResponse response;
        response.status = 200;
        if (sql.find("column_name") != std::string::npos) {
            response.body = R"({"columns":["table_schema","table_name","column_name",
                "data_type","is_nullable","ordinal_position"],
                "rows":[["main","orders","id","INTEGER","NO",1],
                        ["main","orders","note","TEXT","YES",2]]})";
        } else {
            response.body = R"({"columns":["table_schema","table_name","table_type"],
                "rows":[["main","orders","BASE TABLE"]]})";
        }
        return response;
    };
}

TEST(LakeSqlCatalog, SqliteReadsSqliteMasterAndPragmaTableInfo) {
    std::vector<std::string> statements;
    zetta::connect::SourceCatalogRequest request;
    request.match = "Ord";
    const zetta::connect::SourceCatalog catalog = zetta::connect::list_source_catalog(
        lake_config("lake_sqlite"), request, catalog_service(&statements));
    ASSERT_TRUE(catalog.ok()) << catalog.error;
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_NE(statements[0].find("FROM sqlite_master"), std::string::npos) << statements[0];
    EXPECT_NE(statements[0].find("LIKE '%ord%'"), std::string::npos) << statements[0];
    EXPECT_NE(statements[1].find("pragma_table_info(m.name)"), std::string::npos)
        << statements[1];
    ASSERT_EQ(catalog.tables.size(), 1u);
    EXPECT_EQ(catalog.tables[0].at("schema"), "main");
    EXPECT_EQ(catalog.tables[0].at("table"), "orders");
    ASSERT_EQ(catalog.tables[0].at("columns").size(), 2u);
    EXPECT_EQ(catalog.tables[0].at("columns")[0].at("nullable"), false);
    EXPECT_EQ(catalog.tables[0].at("columns")[1].at("nullable"), true);
}

TEST(LakeSqlCatalog, DuckdbReadsInformationSchema) {
    std::vector<std::string> statements;
    const zetta::connect::SourceCatalog catalog = zetta::connect::list_source_catalog(
        lake_config("lake_duckdb"), {}, catalog_service(&statements));
    ASSERT_TRUE(catalog.ok()) << catalog.error;
    ASSERT_EQ(statements.size(), 2u);
    EXPECT_NE(statements[0].find("information_schema.tables"), std::string::npos);
    EXPECT_NE(statements[1].find("information_schema.columns"), std::string::npos);
    ASSERT_EQ(catalog.tables.size(), 1u);
    EXPECT_EQ(catalog.tables[0].at("columns").size(), 2u);
}

TEST(LakeSqlCatalog, PolarsListsItsDatasets) {
    std::vector<HttpRequest> seen;
    const std::string body = R"({"datasets":[
        {"name":"customers","rows":3,"bytes":10,"columns":[{"name":"id","type":"i64"}]},
        {"name":"orders","rows":5,"bytes":20,"columns":[{"name":"amount","type":"f64"},
                                                         {"name":"day","type":"date"}]}]})";
    const zetta::connect::SourceCatalog all = zetta::connect::list_source_catalog(
        lake_config("lake_polars"), {}, canned(200, body, &seen));
    ASSERT_TRUE(all.ok()) << all.error;
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].method, "GET");
    EXPECT_EQ(seen[0].url, "http://orders.ws-1.svc.cluster.local:8080/v1/datasets");
    EXPECT_EQ(seen[0].secret_headers, std::vector<std::string>{"Authorization"});
    ASSERT_EQ(all.tables.size(), 2u);
    EXPECT_EQ(all.tables[1].at("table"), "orders");
    EXPECT_EQ(all.tables[1].at("kind"), "DATASET");
    EXPECT_EQ(all.tables[1].at("columns")[1].at("type"), "date");

    zetta::connect::SourceCatalogRequest matching;
    matching.match = "ORD";
    const auto matched = zetta::connect::list_source_catalog(lake_config("lake_polars"),
                                                             matching, canned(200, body));
    ASSERT_EQ(matched.tables.size(), 1u);
    EXPECT_EQ(matched.tables[0].at("table"), "orders");

    zetta::connect::SourceCatalogRequest capped;
    capped.max_tables = 1;
    capped.include_columns = false;
    const auto first = zetta::connect::list_source_catalog(lake_config("lake_polars"),
                                                           capped, canned(200, body));
    ASSERT_EQ(first.tables.size(), 1u);
    EXPECT_TRUE(first.truncated);
    EXPECT_FALSE(first.tables[0].contains("columns"));

    zetta::connect::SourceCatalogRequest schema;
    schema.schema = "main";
    EXPECT_TRUE(zetta::connect::list_source_catalog(lake_config("lake_polars"), schema,
                                                    canned(200, body))
                    .tables.empty());
}

TEST(LakeSqlCatalog, ACatalogErrorSurfaces) {
    const auto catalog = zetta::connect::list_source_catalog(
        lake_config("lake_polars"), {},
        canned(401, R"({"error":{"code":"unauthorized","message":"x"}})"));
    EXPECT_FALSE(catalog.ok());
    EXPECT_NE(catalog.error.find("refused the token"), std::string::npos) << catalog.error;
}

// =====================================================================
// A live service (guarded)
// =====================================================================

// Runs against a real lake-sqlite / lake-duckdb / lake-polars when
// ZETTA_LAKE_SQL_TEST_ENDPOINT, _TOKEN and _TYPE are set. With
// ZETTA_LAKE_SQL_TEST_TABLE it also pulls that table and finds it in the
// catalog. Nothing is written to the service.
TEST(LakeSqlLive, RunsAgainstARealServiceWhenConfigured) {
    const char* endpoint = std::getenv("ZETTA_LAKE_SQL_TEST_ENDPOINT");
    if (endpoint == nullptr || endpoint[0] == '\0') {
        GTEST_SKIP() << "set ZETTA_LAKE_SQL_TEST_ENDPOINT, _TOKEN, _TYPE (and "
                        "optionally _TABLE) to run against a real lake service";
    }
    const auto env_or = [](const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return std::string(value != nullptr ? value : fallback);
    };
    // Uninstalled on every exit, a failed assertion included: the client's
    // thread must be joined before static destruction.
    struct NativeTransport {
        NativeTransport() { zetta::api::install_native_http_transport(); }
        ~NativeTransport() { zetta::api::shutdown_native_http_transport(); }
    } native_transport;
    const ConnectorConfig config =
        lake_config(env_or("ZETTA_LAKE_SQL_TEST_TYPE", "lake_sqlite"), endpoint);
    ConnectorConfig with_token = config;
    with_token.token = env_or("ZETTA_LAKE_SQL_TEST_TOKEN", "");

    const ConnectorResult typed = run_connector_query(
        // 2.5 is a DECIMAL to DuckDB, which its service answers as exact text;
        // the CAST makes it a DOUBLE for every engine. (`nothing` is an SQLite
        // keyword, hence `absent`.)
        with_token, "SELECT 42 AS n, CAST(2.5 AS DOUBLE) AS r, 'hi' AS s, NULL AS absent",
        10);
    ASSERT_TRUE(typed.ok()) << typed.error;
    ASSERT_EQ(typed.data.rows.size(), 1u);
    EXPECT_EQ(typed.data.rows[0].at("n"), 42);
    EXPECT_DOUBLE_EQ(typed.data.rows[0].at("r").get<double>(), 2.5);
    EXPECT_EQ(typed.data.rows[0].at("s"), "hi");
    EXPECT_TRUE(typed.data.rows[0].at("absent").is_null());

    ConnectorConfig wrong_token = with_token;
    wrong_token.token = "not-the-token";
    const ConnectorResult refused = run_connector_query(wrong_token, "SELECT 1 AS n", 1);
    EXPECT_FALSE(refused.ok());
    // Usually "refused the token (401)"; the service answers 401 and closes
    // without reading the body, and on some runs the reset reaches the client
    // before the answer does (a transport failure). Either way: no rows, and no
    // token in the reason.
    EXPECT_TRUE(refused.error.find("refused the token") != std::string::npos ||
                refused.error.find("could not reach") != std::string::npos)
        << refused.error;
    EXPECT_EQ(refused.error.find("not-the-token"), std::string::npos);

    const std::string table = env_or("ZETTA_LAKE_SQL_TEST_TABLE", "");
    if (!table.empty()) {
        const ConnectorResult pulled =
            run_connector_query(with_token, "SELECT * FROM " + table, 1000);
        ASSERT_TRUE(pulled.ok()) << pulled.error;
        EXPECT_FALSE(pulled.data.rows.empty());

        const zetta::connect::SourceCatalog catalog =
            zetta::connect::list_source_catalog(with_token, {});
        ASSERT_TRUE(catalog.ok()) << catalog.error;
        bool found = false;
        for (const nlohmann::json& entry : catalog.tables) {
            if (entry.at("table") == table) {
                found = true;
                EXPECT_FALSE(entry.value("columns", nlohmann::json::array()).empty())
                    << entry.dump();
            }
        }
        EXPECT_TRUE(found) << catalog.tables.dump();
    }
}

}  // namespace
