// Workspace network records (#1281): fixed collection retention, producer
// schema validation, session lifecycle updates, and egress-decision reads.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/network/records.hpp"
#include "zetta/util/uuid.hpp"

namespace {

nlohmann::json session(std::string event, std::string reason,
                       std::uint64_t bytes_up = 0,
                       std::uint64_t bytes_down = 0) {
  return {{"schema", 1},
          {"event", std::move(event)},
          {"role", "relay"},
          {"session_id", "0123456789abcdef0123456789abcdef"},
          {"timestamp", "2026-10-14T20:01:02.003Z"},
          {"started_at", "2026-10-14T20:00:00.000Z"},
          {"grant_id", "11111111-1111-4111-8111-111111111111"},
          {"workspace", "workspace-example"},
          {"peer", "203.0.113.8:54321"},
          {"endpoint", "endpoint.workspace-example.svc:7444"},
          {"reason", std::move(reason)},
          {"bytes_up", bytes_up},
          {"bytes_down", bytes_down}};
}

nlohmann::json egress_decision() {
  return {{"schema", 1},
          {"event", "egress_decision"},
          {"role", "egress"},
          {"timestamp", "2026-10-14T20:02:00.000Z"},
          {"started_at", "2026-10-14T20:01:59.900Z"},
          {"decision", "allow"},
          {"reason", "rule_allowed"},
          {"rule_id", "22222222-2222-4222-8222-222222222222"},
          {"workspace", "workspace-example"},
          {"source", "10.224.1.17:42000"},
          {"dns_name", "api.example.com"},
          {"port", 443},
          {"protocol", "https_sni"},
          {"resolved_address", "198.51.100.20:443"}};
}

struct NetworkRecords : ::testing::Test {
  zetta::test::TempDir directory;
  zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
  zetta::data::RecordStore store{replication, replication.node_id(),
                                 directory.path() / "records"};

  void SetUp() override {
    ASSERT_TRUE(store.open());
    ASSERT_TRUE(zetta::network::initialize_network_record_collections(store));
  }
};

} // namespace

TEST_F(NetworkRecords, InstallsTheFourFixedRetentionCollections) {
  constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;
  ASSERT_EQ(zetta::network::network_record_collections().size(), 4u);
  const auto connections = store.find_collection("network_connections");
  const auto grants = store.find_collection("network_grants");
  const auto sessions = store.find_collection("network_tunnel_sessions");
  const auto activity = store.find_collection("network_activity");
  ASSERT_TRUE(connections);
  ASSERT_TRUE(grants);
  ASSERT_TRUE(sessions);
  ASSERT_TRUE(activity);
  EXPECT_EQ(connections->created_ms, grants->created_ms);
  EXPECT_EQ(connections->created_ms, sessions->created_ms);
  EXPECT_EQ(connections->created_ms, activity->created_ms);
  EXPECT_EQ(store.find_collection("network_connections")->retention_ms,
            365LL * kDayMs);
  EXPECT_EQ(store.find_collection("network_grants")->retention_ms,
            365LL * kDayMs);
  EXPECT_EQ(store.find_collection("network_tunnel_sessions")->retention_ms,
            90LL * kDayMs);
  EXPECT_EQ(store.find_collection("network_activity")->retention_ms,
            30LL * kDayMs);
}

TEST_F(NetworkRecords, SessionEndReplacesTheLiveStartAndRemainsReadable) {
  const auto started =
      zetta::network::write_network_record(store, session("session_start", ""));
  ASSERT_TRUE(started.ok()) << started.message;

  zetta::network::NetworkRecordListRequest live;
  live.type = zetta::network::NetworkRecordType::TunnelSession;
  live.filter = "event";
  live.value = "session_start";
  auto page = zetta::network::list_network_records(store, live);
  ASSERT_TRUE(page.ok()) << page.message;
  ASSERT_EQ(page.records.size(), 1u);
  EXPECT_EQ(page.records[0].value.at("grant_id"),
            "11111111-1111-4111-8111-111111111111");

  const auto ended = zetta::network::write_network_record(
      store, session("session_end", "peer_closed", 4096, 8192));
  ASSERT_TRUE(ended.ok()) << ended.message;
  EXPECT_EQ(ended.record->key, started.record->key);
  EXPECT_GT(ended.record->revision, started.record->revision);

  page = zetta::network::list_network_records(store, live);
  ASSERT_TRUE(page.ok());
  EXPECT_TRUE(page.records.empty());
  live.value = "session_end";
  page = zetta::network::list_network_records(store, live);
  ASSERT_EQ(page.records.size(), 1u);
  EXPECT_EQ(page.records[0].value.at("bytes_down"), 8192);
}

TEST_F(NetworkRecords, EgressDecisionIsValidatedAndListedAsActivity) {
  const auto written =
      zetta::network::write_network_record(store, egress_decision());
  ASSERT_TRUE(written.ok()) << written.message;

  zetta::network::NetworkRecordListRequest request;
  request.type = zetta::network::NetworkRecordType::Activity;
  request.filter = "decision";
  request.value = "allow";
  const auto page = zetta::network::list_network_records(store, request);
  ASSERT_TRUE(page.ok()) << page.message;
  ASSERT_EQ(page.records.size(), 1u);
  EXPECT_EQ(page.records[0].value.at("dns_name"), "api.example.com");

  nlohmann::json misplaced = egress_decision();
  misplaced["workspace"] = "";
  const auto refused = zetta::network::write_network_record(store, misplaced);
  EXPECT_EQ(refused.status, zetta::data::RecordStatus::InvalidInput);
  EXPECT_EQ(refused.path, "/workspace");
}

TEST_F(NetworkRecords, RefusesAnIndexThatTheRecordTypeDoesNotDeclare) {
  zetta::network::NetworkRecordListRequest request;
  request.type = zetta::network::NetworkRecordType::TunnelSession;
  request.filter = "rule_id";
  request.value = "anything";
  const auto page = zetta::network::list_network_records(store, request);
  EXPECT_EQ(page.status, zetta::data::RecordStatus::InvalidInput);
  EXPECT_EQ(page.path, "/filter");
}
