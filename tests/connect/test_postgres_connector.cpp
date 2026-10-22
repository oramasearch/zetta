// The Postgres source connector (#892), validated against a real Postgres.
//
// Compiled only with ZETTA_WITH_POSTGRES (the libpqxx submodule present). Each
// case installs the libpqxx runner and connects to a local Postgres; if none is
// reachable it SKIPS (so CI without a database stays green). Point it at a
// specific instance with ZETTA_PG_TEST_HOST/PORT/USER/PASSWORD/DB.

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/postgres_protocol.hpp"
#include "zetta/connect/connector.hpp"
#include "zetta/connect/distributed_ingest.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/query/service.hpp"

#if defined(ZETTA_WITH_POSTGRES)

namespace {

std::string env_or(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return value != nullptr ? std::string(value) : fallback;
}

// A config pointing at the local Postgres (trust auth by default — no password).
zetta::connect::ConnectorConfig local_pg(const std::string& database) {
    zetta::connect::ConnectorConfig config;
    config.type = "postgres";
    config.host = env_or("ZETTA_PG_TEST_HOST", "127.0.0.1");
    config.port = env_or("ZETTA_PG_TEST_PORT", "5432");
    config.user = env_or("ZETTA_PG_TEST_USER", env_or("USER", "postgres"));
    config.token = env_or("ZETTA_PG_TEST_PASSWORD", "");  // password (trust => empty)
    config.database = env_or("ZETTA_PG_TEST_DB", database);
    return config;
}

// Reachable? Run a trivial query; skip the suite if the node has no local PG.
bool postgres_reachable() {
    zetta::connect::install_postgres_protocol();
    const auto result =
        zetta::connect::run_connector_query(local_pg("postgres"), "SELECT 1 AS one", 1);
    return result.ok();
}

class ScopedClientEncoding {
public:
    explicit ScopedClientEncoding(const char* encoding) {
        if (const char* existing = std::getenv("PGCLIENTENCODING")) previous = existing;
        EXPECT_EQ(::setenv("PGCLIENTENCODING", encoding, 1), 0);
    }

    ~ScopedClientEncoding() {
        if (previous) {
            ::setenv("PGCLIENTENCODING", previous->c_str(), 1);
        } else {
            ::unsetenv("PGCLIENTENCODING");
        }
    }

private:
    std::optional<std::string> previous;
};

}  // namespace

TEST(PostgresConnector, PinsUtf8EvenWhenTheEnvironmentRequestsSjis) {
    const ScopedClientEncoding encoding{"SJIS"};
    if (!postgres_reachable()) GTEST_SKIP() << "no local Postgres reachable";
    const auto result = zetta::connect::run_connector_query(
        local_pg("postgres"), "SELECT current_setting('client_encoding') AS encoding", 1);
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.data.rows.size(), 1u);
    EXPECT_EQ(result.data.rows.front().at("encoding"), "UTF8");
}

TEST(PostgresConnector, RunsAReadOnlyQueryAndCoercesTypes) {
    if (!postgres_reachable()) GTEST_SKIP() << "no local Postgres reachable";
    zetta::connect::install_postgres_protocol();

    const auto result = zetta::connect::run_connector_query(
        local_pg("postgres"),
        "SELECT 42::int AS n, 3.5::float8 AS f, true AS b, 'hi'::text AS s, "
        "NULL::text AS nothing",
        10);
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.data.rows.size(), 1u);
    const auto& row = result.data.rows[0];
    // Cells are coerced to real JSON types, not a table of strings.
    EXPECT_TRUE(row.at("n").is_number_integer());
    EXPECT_EQ(row.at("n").get<long long>(), 42);
    EXPECT_TRUE(row.at("f").is_number());
    EXPECT_DOUBLE_EQ(row.at("f").get<double>(), 3.5);
    EXPECT_TRUE(row.at("b").is_boolean());
    EXPECT_TRUE(row.at("b").get<bool>());
    EXPECT_EQ(row.at("s").get<std::string>(), "hi");
    EXPECT_TRUE(row.at("nothing").is_null());
}

TEST(PostgresConnector, CapsRowsAtMaxRows) {
    if (!postgres_reachable()) GTEST_SKIP() << "no local Postgres reachable";
    zetta::connect::install_postgres_protocol();
    const auto result = zetta::connect::run_connector_query(
        local_pg("postgres"), "SELECT generate_series(1, 1000) AS g", 5);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.data.rows.size(), 5u);  // the LIMIT cap held
}

TEST(PostgresConnector, RejectsAWriteBeforeItReachesTheSource) {
    // The shared read-only guard blocks anything but SELECT/WITH — no connection
    // is even opened. Needs no database, so it never skips.
    const auto result = zetta::connect::run_connector_query(
        local_pg("postgres"), "DELETE FROM pg_class", 10);
    EXPECT_FALSE(result.ok());
    EXPECT_NE(result.error.find("read-only"), std::string::npos);
}

TEST(PostgresConnector, MissingRequiredFieldsFailValidation) {
    // Postgres requires host + user + database (from_json boundary).
    EXPECT_FALSE(zetta::connect::ConnectorConfig::from_json(
        nlohmann::json{{"type", "postgres"}, {"host", "h"}}));  // no user/database
    EXPECT_TRUE(zetta::connect::ConnectorConfig::from_json(nlohmann::json{
        {"type", "postgres"}, {"host", "h"}, {"user", "u"}, {"database", "d"}}));
}

// `create_dataset_from_query` end to end against a real Postgres — the call
// from https://github.com/oramasearch/zetta/issues/994. It never worked: the
// capability routes through run_distributed_connector_ingest, which submitted
// every config through Snowflake's REST protocol regardless of `type`, so a
// Postgres connection failed with "snowflake: could not resolve
// .snowflakecomputing.com". The consent gate refused the call first, which is
// why the dispatch bug went unseen for as long as the gate was on.
TEST(PostgresConnector, DistributedIngestLandsARealPostgresResultSet) {
    if (!postgres_reachable()) GTEST_SKIP() << "no local Postgres reachable";
    zetta::connect::install_postgres_protocol();

    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    ASSERT_TRUE(node.start());

    zetta::connect::DistributedIngestOptions options;
    options.query = "SELECT g AS n, g * 2 AS doubled FROM generate_series(1, 7) AS g";
    options.table_name = "pg_probe";
    options.max_rows = 100;
    options.replication_factor = 1;

    const zetta::connect::DistributedIngestResult result =
        run_distributed_connector_ingest(node, local_pg("postgres"), options);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.rows, 7u);
    EXPECT_EQ(result.columns, (std::vector<std::string>{"n", "doubled"}));
    ASSERT_TRUE(result.table);
    // One source, one stream: this node pulled it and owns what it landed.
    EXPECT_FALSE(result.coordinated);
    ASSERT_FALSE(result.partitions.empty());

    // Landed as a real dataset, readable through the same scatter-gather SELECT
    // the Snowflake path's output is read with.
    zetta::query::QueryService service{node};
    const auto all =
        service.run("SELECT * FROM datasets." + result.table->name() + ";", "");
    ASSERT_TRUE(all.ok()) << all.error;
    EXPECT_EQ(all.rows.size(), 7u);

    node.stop();
}

#endif  // ZETTA_WITH_POSTGRES
