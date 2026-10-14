// Lake and zetta-tunnel's network-record REST/MCP contract (#1281): typed
// write/read parity, least-privilege roles, and one-workspace-per-Zetta
// isolation.
#include <gtest/gtest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

nlohmann::json session_record() {
  return {{"schema", 1},
          {"event", "session_start"},
          {"role", "relay"},
          {"session_id", "0123456789abcdef0123456789abcdef"},
          {"timestamp", "2026-10-14T20:01:02.003Z"},
          {"started_at", "2026-10-14T20:00:00.000Z"},
          {"grant_id", "11111111-1111-4111-8111-111111111111"},
          {"workspace", "workspace-a"},
          {"peer", "203.0.113.8:54321"},
          {"endpoint", "endpoint.workspace-a.svc:7444"},
          {"reason", ""},
          {"bytes_up", 0},
          {"bytes_down", 0}};
}

nlohmann::json egress_record() {
  return {{"schema", 1},
          {"event", "egress_decision"},
          {"role", "egress"},
          {"timestamp", "2026-10-14T20:02:00.000Z"},
          {"started_at", "2026-10-14T20:01:59.900Z"},
          {"decision", "deny"},
          {"reason", "rule_absent"},
          {"rule_id", "22222222-2222-4222-8222-222222222222"},
          {"workspace", "workspace-a"},
          {"source", "10.224.1.17:42000"},
          {"dns_name", "blocked.example.com"},
          {"port", 443},
          {"protocol", "https_sni"},
          {"resolved_address", ""}};
}

struct NetworkCapabilities : ::testing::Test {
  zetta::test::TempDir directory;
  zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
  zetta::Node node{zetta::NodeConfig{directory.path().string(), false},
                   &replication};
  Registry registry;
  zetta::observability::TraceContext trace =
      zetta::observability::make_root_trace();

  void SetUp() override {
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();
    zetta::api::register_core_capabilities(registry);
  }

  void TearDown() override { node.stop(); }

  ApiResult call(const std::string &name, nlohmann::json input) {
    ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
    return registry.call(name, input, context);
  }

  ApiResult call_as(const std::string &user, const std::string &name,
                    nlohmann::json input) {
    ApiContext context{node, nullptr, user, trace, &node.span_recorder()};
    context.acting_is_user = true;
    return registry.call(name, input, context);
  }
};

} // namespace

TEST_F(NetworkCapabilities, TunnelWritesAndLakeReadsSessionAndEgressRecords) {
  zetta::query::QueryService sql(node);
  for (const char *statement :
       {"CREATE ROLE tunnel_writer", "CREATE ROLE lake_network_reader",
        "GRANT mcp:call, network:write TO ROLE tunnel_writer",
        "GRANT mcp:call, network:read TO ROLE lake_network_reader"}) {
    const auto result = sql.run(statement, "");
    ASSERT_TRUE(result.ok()) << statement << ": " << result.error;
  }
  auto tunnel = node.create_user("zetta-tunnel");
  auto lake = node.create_user("lake");
  auto unrelated = node.create_user("unrelated");
  ASSERT_TRUE(tunnel);
  ASSERT_TRUE(lake);
  ASSERT_TRUE(unrelated);
  ASSERT_TRUE(
      node.assign_role(tunnel->id(), node.find_role("tunnel_writer")->id()));
  ASSERT_TRUE(node.assign_role(lake->id(),
                               node.find_role("lake_network_reader")->id()));

  const auto session =
      call_as(tunnel->id(), "write_network_record", session_record());
  ASSERT_TRUE(session.ok) << session.error.message;
  EXPECT_EQ(session.value.at("type"), "tunnel_session");
  const auto egress =
      call_as(tunnel->id(), "write_network_record", egress_record());
  ASSERT_TRUE(egress.ok) << egress.error.message;
  EXPECT_EQ(egress.value.at("type"), "activity");

  const auto sessions =
      call_as(lake->id(), "list_network_records", {{"type", "tunnel_session"}});
  ASSERT_TRUE(sessions.ok) << sessions.error.message;
  ASSERT_EQ(sessions.value.at("records").size(), 1u);
  EXPECT_EQ(sessions.value.at("records")[0].at("record").at("grant_id"),
            "11111111-1111-4111-8111-111111111111");
  const auto activity = call_as(
      lake->id(), "list_network_records",
      {{"type", "activity"}, {"filter", "decision"}, {"value", "deny"}});
  ASSERT_TRUE(activity.ok) << activity.error.message;
  ASSERT_EQ(activity.value.at("records").size(), 1u);
  EXPECT_EQ(activity.value.at("records")[0].at("record").at("reason"),
            "rule_absent");

  EXPECT_EQ(
      call_as(tunnel->id(), "list_network_records", {{"type", "activity"}})
          .error.code,
      "unauthorized");
  EXPECT_EQ(
      call_as(lake->id(), "write_network_record", egress_record()).error.code,
      "unauthorized");
  EXPECT_EQ(
      call_as(unrelated->id(), "list_network_records", {{"type", "activity"}})
          .error.code,
      "unauthorized");
}

TEST_F(NetworkCapabilities, AnotherWorkspaceZettaHasNoRecordsFromThisOne) {
  ASSERT_TRUE(call("write_network_record", session_record()).ok);

  zetta::test::TempDir other_directory;
  zetta::cluster::ReplicationManager other_replication{zetta::util::new_uuid()};
  zetta::Node other_node{
      zetta::NodeConfig{other_directory.path().string(), false},
      &other_replication};
  ASSERT_TRUE(other_node.start());
  Registry other_registry;
  zetta::api::register_core_capabilities(other_registry);
  ApiContext context{other_node, nullptr, "", trace,
                     &other_node.span_recorder()};
  const auto records = other_registry.call(
      "list_network_records", {{"type", "tunnel_session"}}, context);
  ASSERT_TRUE(records.ok) << records.error.message;
  EXPECT_TRUE(records.value.at("records").empty());
  other_node.stop();
}
