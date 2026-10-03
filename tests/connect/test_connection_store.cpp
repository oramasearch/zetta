// Saved connections (#827, docs/data/connector-query.md): the replicate-to-all
// credential store, its boundary, and — the load-bearing property — that the
// token is stored only sealed (#1230) and reaches NO read surface.
#include <gtest/gtest.h>

#include <string>
#include <vector>
#include <iterator>
#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/connect/connection_store.hpp"
#include "zetta/connect/credential_seal.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/replication_strategy.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"
#include "zetta/storage/engine.hpp"

namespace {

zetta::connect::ConnectorConfig snowflake_config() {
    zetta::connect::ConnectorConfig config;
    config.type = "snowflake";
    config.account = "ab12345.us-east-1";
    config.token = "the-secret-programmatic-access-token";
    config.warehouse = "COMPUTE_WH";
    config.database = "ANALYTICS";
    config.schema = "PUBLIC";
    return config;
}

struct ConnectionStoreFixture : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};

    void SetUp() override { ASSERT_TRUE(node.start()); }
    void TearDown() override { node.stop(); }
};

TEST_F(ConnectionStoreFixture, SavesAndResolvesAConnectionWithItsToken) {
    EXPECT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());

    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->name, "warehouse");
    EXPECT_EQ(found->config.account, "ab12345.us-east-1");
    // The resolution path — the one caller allowed to see the credential.
    EXPECT_EQ(found->config.token, "the-secret-programmatic-access-token");
    EXPECT_FALSE(found->created.empty());
}

TEST_F(ConnectionStoreFixture, LivesInAReplicateToAllTableSoEveryNodeHoldsIt) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());

    const auto database =
        node.find_database(zetta::connect::kConnectionDatabase);
    ASSERT_TRUE(database);
    const auto table = database->find_table(zetta::connect::kConnectionTable);
    ASSERT_TRUE(table);
    // The whole point of storing it as a plain table: the default strategy puts
    // the row on every member, which is what lets each node fetch its own
    // stripe with its own copy of the credential.
    EXPECT_TRUE(table->replication_strategy().is_default());  // ReplicateToAll
    EXPECT_FALSE(table->partitioner());
    EXPECT_EQ(table->row_count(), 1u);
}

TEST_F(ConnectionStoreFixture, SavingTheSameNameReplacesRatherThanDuplicates) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());
    zetta::connect::ConnectorConfig rotated = snowflake_config();
    rotated.token = "the-rotated-token";
    rotated.warehouse = "BIG_WH";
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", rotated).empty());

    EXPECT_EQ(zetta::connect::list_connections(node).size(), 1u);
    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->config.token, "the-rotated-token");
    EXPECT_EQ(found->config.warehouse, "BIG_WH");
}

TEST_F(ConnectionStoreFixture, ListsAndRemoves) {
    ASSERT_TRUE(zetta::connect::save_connection(node, "first", snowflake_config())
                    .empty());
    ASSERT_TRUE(zetta::connect::save_connection(node, "second", snowflake_config())
                    .empty());
    EXPECT_EQ(zetta::connect::list_connections(node).size(), 2u);

    EXPECT_TRUE(zetta::connect::remove_connection(node, "first"));
    EXPECT_FALSE(zetta::connect::remove_connection(node, "first"));
    EXPECT_FALSE(zetta::connect::find_connection(node, "first").has_value());
    EXPECT_EQ(zetta::connect::list_connections(node).size(), 1u);
}

TEST_F(ConnectionStoreFixture, RejectsABadNameOrAMissingToken) {
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "", snowflake_config()).empty());
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "-leading", snowflake_config())
            .empty());
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "has space", snowflake_config())
            .empty());
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "has;semicolon", snowflake_config())
            .empty());
    EXPECT_FALSE(zetta::connect::save_connection(node, std::string(65, 'a'),
                                                 snowflake_config())
                     .empty());

    zetta::connect::ConnectorConfig tokenless = snowflake_config();
    tokenless.token.clear();
    EXPECT_FALSE(
        zetta::connect::save_connection(node, "warehouse", tokenless).empty());

    EXPECT_TRUE(zetta::connect::list_connections(node).empty());
}

// The security contract, asserted rather than assumed: the credential is in the
// store (every node needs it) and in nothing a caller can read back.
TEST_F(ConnectionStoreFixture, NoReadSurfaceEverCarriesTheToken) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config())
            .empty());
    const std::string secret = "the-secret-programmatic-access-token";

    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found.has_value());
    const std::string redacted = found->to_redacted_json().dump();
    EXPECT_EQ(redacted.find(secret), std::string::npos) << redacted;
    EXPECT_EQ(found->config.to_json().dump().find(secret), std::string::npos);
    // …while the node itself still opens the sealed credential (#1230).
    EXPECT_EQ(found->config.token, secret);

    zetta::query::QueryService service(node);
    const auto view = service.run("SELECT * FROM system.connections;", "");
    ASSERT_TRUE(view.error.empty()) << view.error;
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows.front().dump().find(secret), std::string::npos)
        << view.rows.front().dump();
    EXPECT_EQ(view.rows.front().at("name"), "warehouse");
    EXPECT_EQ(view.rows.front().at("account"), "ab12345.us-east-1");
}


// ---- sealed at rest (#1230) -------------------------------------------------

constexpr const char* kSecret = "the-secret-programmatic-access-token";

// The one stored row of the connection store, as the engine holds it.
nlohmann::json stored_row(const zetta::Node& node) {
    const auto database = node.find_database(zetta::connect::kConnectionDatabase);
    if (!database) return {};
    const auto table = database->find_table(zetta::connect::kConnectionTable);
    if (!table || table->store().size() != 1) return {};
    const zetta::storage::ParquetLazyPtr row = table->store().at(0);
    return row ? nlohmann::json(*row) : nlohmann::json();
}

zetta::NodeConfig config_with_secret(const zetta::test::TempDir& dir,
                                     std::string secret,
                                     std::vector<std::string> retired = {}) {
    zetta::NodeConfig config{dir.path().string(), /*preflight=*/false};
    config.connection_root_secret = std::move(secret);
    config.retired_connection_root_secrets = std::move(retired);
    return config;
}

// The stored row — on disk and in the replicated catalogue every node gossips —
// carries the credential only sealed.
TEST(ConnectionStoreSealed, TheStoredRowAndTheCatalogueHoldOnlyTheSealedCredential) {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication("node-a");
    zetta::Node node{config_with_secret(dir, "cluster-join-token"), &replication};
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config()).empty());

    const nlohmann::json row = stored_row(node);
    ASSERT_TRUE(row.is_object()) << row.dump();
    EXPECT_EQ(row.dump().find(kSecret), std::string::npos) << row.dump();
    EXPECT_FALSE(row.at("connector").contains("token"));
    EXPECT_TRUE(zetta::connect::is_sealed_credential(row.at("credential")));
    EXPECT_EQ(replication.snapshot().dump().find(kSecret), std::string::npos);
    node.stop();

    // Nothing written under the data directory carries it in plaintext either.
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(dir.path())) {
        if (!entry.is_regular_file()) continue;
        std::ifstream in(entry.path(), std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
        EXPECT_EQ(bytes.find(kSecret), std::string::npos) << entry.path();
    }
}

// Every read surface says only whether a credential is set: the redacted
// value, system.connections, and the list/create capabilities.
TEST_F(ConnectionStoreFixture, ReadSurfacesSayOnlyThatACredentialIsSet) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config()).empty());
    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->credential, zetta::connect::CredentialState::Set);
    EXPECT_EQ(found->to_redacted_json().at("credential"), "set");

    zetta::query::QueryService service(node);
    const auto view = service.run("SELECT * FROM system.connections;", "");
    ASSERT_TRUE(view.ok()) << view.error;
    ASSERT_EQ(view.rows.size(), 1u);
    EXPECT_EQ(view.rows.front().at("credential"), "set");
    // The store itself is not reachable through SQL.
    EXPECT_FALSE(service.run("SELECT * FROM zetta_connect.connections;", "").ok());

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();
    zetta::api::ApiContext api{node, nullptr, "", trace, &node.span_recorder()};
    const zetta::api::ApiResult listed =
        registry.call("list_connections", nlohmann::json::object(), api);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    EXPECT_EQ(listed.value.dump().find(kSecret), std::string::npos);
    EXPECT_EQ(listed.value.at("connections").at(0).at("credential"), "set");

    nlohmann::json connector = snowflake_config().to_json();
    connector["token"] = "a-rotated-token";
    const zetta::api::ApiResult created = registry.call(
        "create_connection", {{"name", "second"}, {"connector", connector}}, api);
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value.dump().find("a-rotated-token"), std::string::npos);
    EXPECT_EQ(created.value.at("credential"), "set");
}

// A row written before #1230 holds the token in plaintext. It still resolves,
// and the next start re-seals it.
TEST(ConnectionStoreSealed, ALegacyPlaintextRowIsResealedOnStart) {
    zetta::test::TempDir dir;
    {
        zetta::Node node{config_with_secret(dir, "cluster-join-token")};
        ASSERT_TRUE(node.start());
        auto database = node.create_database(zetta::connect::kConnectionDatabase);
        ASSERT_TRUE(database);
        auto table = database->create_table(zetta::connect::kConnectionTable);
        ASSERT_TRUE(table);
        nlohmann::json legacy_connector = snowflake_config().to_json();
        legacy_connector["token"] = kSecret;
        table->add_row({{"name", "warehouse"},
                        {"connector", legacy_connector},
                        {"created", "2026-09-01T00:00:00.000Z"}});
        const auto found = zetta::connect::find_connection(node, "warehouse");
        ASSERT_TRUE(found);
        EXPECT_EQ(found->credential, zetta::connect::CredentialState::Plaintext);
        EXPECT_EQ(found->config.token, kSecret);
        node.stop();
    }
    zetta::Node node{config_with_secret(dir, "cluster-join-token")};
    ASSERT_TRUE(node.start());
    const nlohmann::json row = stored_row(node);
    EXPECT_EQ(row.dump().find(kSecret), std::string::npos) << row.dump();
    const auto found = zetta::connect::find_connection(node, "warehouse");
    ASSERT_TRUE(found);
    EXPECT_EQ(found->credential, zetta::connect::CredentialState::Set);
    EXPECT_EQ(found->config.token, kSecret);
    EXPECT_EQ(found->created, "2026-09-01T00:00:00.000Z");
    node.stop();
}

// Rotating the root secret (the join token) re-seals every entry under the new
// key the first time the node starts with it; a node holding only the old
// secret can then no longer open it, and says so without leaking anything.
TEST(ConnectionStoreSealed, RotationResealsUnderTheNewRootSecret) {
    zetta::test::TempDir dir;
    std::string old_key;
    {
        zetta::Node node{config_with_secret(dir, "old-join-token")};
        ASSERT_TRUE(node.start());
        ASSERT_TRUE(
            zetta::connect::save_connection(node, "warehouse", snowflake_config()).empty());
        old_key = zetta::connect::sealed_key_id(stored_row(node).at("credential"));
        node.stop();
    }
    {
        zetta::Node node{config_with_secret(dir, "new-join-token", {"old-join-token"})};
        ASSERT_TRUE(node.start());
        const std::string new_key =
            zetta::connect::sealed_key_id(stored_row(node).at("credential"));
        EXPECT_FALSE(new_key.empty());
        EXPECT_NE(new_key, old_key);
        EXPECT_EQ(new_key, node.connection_keyring()->current()->id);
        const auto found = zetta::connect::find_connection(node, "warehouse");
        ASSERT_TRUE(found);
        EXPECT_EQ(found->config.token, kSecret);
        // A steady store is not rewritten.
        EXPECT_EQ(zetta::connect::reseal_connections(node).resealed, 0u);
        node.stop();
    }
    zetta::Node stale{config_with_secret(dir, "old-join-token")};
    ASSERT_TRUE(stale.start());
    const auto found = zetta::connect::find_connection(stale, "warehouse");
    ASSERT_TRUE(found);  // still listed, by name and settings
    EXPECT_EQ(found->credential, zetta::connect::CredentialState::Unreadable);
    EXPECT_TRUE(found->config.token.empty());
    EXPECT_EQ(found->to_redacted_json().at("credential"), "unreadable");
    stale.stop();
}

// Saving a new credential under an existing name re-seals it (credential
// rotation): the old one no longer opens anywhere.
TEST_F(ConnectionStoreFixture, SavingANewCredentialReseals) {
    ASSERT_TRUE(
        zetta::connect::save_connection(node, "warehouse", snowflake_config()).empty());
    const nlohmann::json first = stored_row(node).at("credential");
    zetta::connect::ConnectorConfig rotated = snowflake_config();
    rotated.token = "the-rotated-token";
    ASSERT_TRUE(zetta::connect::save_connection(node, "warehouse", rotated).empty());
    const nlohmann::json second = stored_row(node).at("credential");
    EXPECT_NE(first.at("ciphertext"), second.at("ciphertext"));
    EXPECT_EQ(zetta::connect::find_connection(node, "warehouse")->config.token,
              "the-rotated-token");
}

}  // namespace
