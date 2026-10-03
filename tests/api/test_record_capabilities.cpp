// The record store and time series on the control surface (#1231): the registry
// journey, conflict details, collection-scoped grants, and the latency budget.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct RecordCapabilities : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                     &replication};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        node.ensure_builtin_roles();
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }
    ApiResult call_as(const std::string& user, const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, user, trace, &node.span_recorder()};
        context.acting_is_user = true;
        return registry.call(name, input, context);
    }
};

}  // namespace

TEST_F(RecordCapabilities, RecordJourneyWithConditionalWritesAndPagedLists) {
    auto created = call("create_collection", {{"collection", "lake_ask_messages"},
                                              {"indexes", {"conversation"}},
                                              {"retention_ms", 86400000}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_TRUE(created.value["created"].get<bool>());
    EXPECT_EQ(created.value["collection"]["indexes"][0], "conversation");

    auto put = call("put_record", {{"collection", "lake_ask_messages"},
                                   {"key", "m-1"},
                                   {"value", "c2VhbGVk"},
                                   {"index", {{"conversation", "c-1"}}},
                                   {"order", "0001"},
                                   {"if_absent", true}});
    ASSERT_TRUE(put.ok) << put.error.message;
    const std::uint64_t revision = put.value["revision"].get<std::uint64_t>();
    EXPECT_EQ(put.value["replicas_expected"], 0);
    EXPECT_TRUE(put.value.contains("expires_ms"));

    auto conflict = call("put_record", {{"collection", "lake_ask_messages"},
                                        {"key", "m-1"},
                                        {"value", "b3RoZXI="},
                                        {"if_absent", true}});
    EXPECT_FALSE(conflict.ok);
    EXPECT_EQ(conflict.error.code, "conflict");
    EXPECT_EQ(conflict.error.details["revision"], revision);

    // if_absent and if_revision together are refused at the schema boundary.
    EXPECT_EQ(call("put_record", {{"collection", "lake_ask_messages"},
                                  {"key", "m-1"},
                                  {"value", "eA=="},
                                  {"if_absent", true},
                                  {"if_revision", revision}})
                  .error.code,
              "invalid_input");

    ASSERT_TRUE(call("put_record", {{"collection", "lake_ask_messages"},
                                    {"key", "m-2"},
                                    {"value", "eA=="},
                                    {"index", {{"conversation", "c-1"}}},
                                    {"order", "0002"}})
                    .ok);
    auto got = call("get_record", {{"collection", "lake_ask_messages"}, {"key", "m-1"}});
    ASSERT_TRUE(got.ok) << got.error.message;
    EXPECT_EQ(got.value["record"]["value"], "c2VhbGVk");
    EXPECT_EQ(got.value["record"]["revision"], revision);

    auto page = call("list_records", {{"collection", "lake_ask_messages"},
                                      {"index", "conversation"},
                                      {"value", "c-1"},
                                      {"limit", 1}});
    ASSERT_TRUE(page.ok) << page.error.message;
    ASSERT_EQ(page.value["records"].size(), 1u);
    EXPECT_EQ(page.value["records"][0]["key"], "m-1");
    auto next = call("list_records", {{"collection", "lake_ask_messages"},
                                      {"index", "conversation"},
                                      {"value", "c-1"},
                                      {"after", page.value["next_after"]}});
    ASSERT_TRUE(next.ok);
    ASSERT_EQ(next.value["records"].size(), 1u);
    EXPECT_EQ(next.value["records"][0]["key"], "m-2");
    EXPECT_FALSE(next.value.contains("next_after"));
    // An index without a value is refused at the boundary.
    EXPECT_EQ(call("list_records", {{"collection", "lake_ask_messages"},
                                    {"index", "conversation"}})
                  .error.code,
              "invalid_input");

    auto stale_delete = call("delete_record", {{"collection", "lake_ask_messages"},
                                               {"key", "m-1"},
                                               {"if_revision", revision + 1}});
    EXPECT_EQ(stale_delete.error.code, "conflict");
    ASSERT_TRUE(call("delete_record", {{"collection", "lake_ask_messages"},
                                       {"key", "m-1"},
                                       {"if_revision", revision}})
                    .ok);
    EXPECT_EQ(call("get_record", {{"collection", "lake_ask_messages"}, {"key", "m-1"}})
                  .error.code,
              "not_found");
}

TEST_F(RecordCapabilities, MetricJourney) {
    auto created = call("create_metric", {{"metric", "lake.requests"}, {"kind", "counter"}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["metric"]["resolution_ms"], 60000);
    ASSERT_TRUE(call("record_metric",
                     {{"metric", "lake.requests"},
                      {"points",
                       {{{"labels", {{"route", "/ask"}}}, {"value", 2}, {"at_ms", 120000}},
                        {{"labels", {{"route", "/train"}}}, {"value", 3}, {"at_ms", 130000}}}}})
                    .ok);
    auto queried = call("query_metric", {{"metric", "lake.requests"},
                                         {"from_ms", 0},
                                         {"to_ms", 600000},
                                         {"group_by", {"route"}}});
    ASSERT_TRUE(queried.ok) << queried.error.message;
    EXPECT_EQ(queried.value["aggregate"], "sum");
    ASSERT_EQ(queried.value["series"].size(), 2u);
    EXPECT_EQ(queried.value["series"][0]["labels"]["route"], "/ask");
    EXPECT_EQ(queried.value["series"][0]["total"], 2.0);
    EXPECT_EQ(call("record_metric", {{"metric", "lake.requests"},
                                     {"points", {{{"value", -1}}}}})
                  .error.code,
              "invalid_input");
}

// "Only lake's service user writes it": a user whose role holds records:write ON
// COLLECTION lake_ writes the lake_ namespace and nothing else; a reader without
// any records right is refused, and so is a grant on another prefix.
TEST_F(RecordCapabilities, CollectionGrantsReserveANamespace) {
    ASSERT_TRUE(call("create_collection", {{"collection", "lake_sessions"}}).ok);
    ASSERT_TRUE(call("create_collection", {{"collection", "other_sessions"}}).ok);

    zetta::query::QueryService admin(node);
    for (const char* statement :
         {"CREATE ROLE lake_service",
          "GRANT mcp:call TO ROLE lake_service",
          "GRANT records:read, records:write ON COLLECTION lake_ TO ROLE lake_service"}) {
        const auto outcome = admin.run(statement, "");
        ASSERT_TRUE(outcome.ok()) << statement << ": " << outcome.error;
    }
    auto service = node.create_user("lake-service");
    ASSERT_TRUE(service);
    ASSERT_TRUE(node.assign_role(service->id(), node.find_role("lake_service")->id()));
    auto reader = node.create_user("someone");
    ASSERT_TRUE(reader);
    ASSERT_TRUE(node.assign_role(reader->id(), node.find_role("reader")->id()));

    const nlohmann::json lake_write{{"collection", "lake_sessions"}, {"key", "s"}, {"value", "eA=="}};
    const nlohmann::json other_write{{"collection", "other_sessions"}, {"key", "s"}, {"value", "eA=="}};
    auto allowed = call_as(service->id(), "put_record", lake_write);
    EXPECT_TRUE(allowed.ok) << allowed.error.message;
    EXPECT_TRUE(call_as(service->id(), "get_record",
                        {{"collection", "lake_sessions"}, {"key", "s"}})
                    .ok);
    EXPECT_EQ(call_as(service->id(), "put_record", other_write).error.code, "unauthorized");
    EXPECT_EQ(call_as(reader->id(), "put_record", lake_write).error.code, "unauthorized");
    EXPECT_EQ(call_as(reader->id(), "get_record",
                      {{"collection", "lake_sessions"}, {"key", "s"}})
                  .error.code,
              "unauthorized");
    // Creating collections is its own right (records:manage).
    EXPECT_EQ(call_as(service->id(), "create_collection", {{"collection", "lake_more"}})
                  .error.code,
              "unauthorized");

    const auto roles = admin.run("SELECT scoped_grants FROM system.roles WHERE name = 'lake_service'", "");
    ASSERT_TRUE(roles.ok()) << roles.error;
    ASSERT_EQ(roles.rows.size(), 1u);
    EXPECT_NE(roles.rows[0]["scoped_grants"].get<std::string>().find("collection:lake_"),
              std::string::npos);
}

// The latency budget through the governed call path (#1231): get_record and
// list_records as REST and MCP run them — schema validation, authorization,
// audit and the handler — over a workspace's 50,000-message log.
TEST_F(RecordCapabilities, GetAndListCallsStayInsideTheTenMillisecondBudget) {
    zetta::data::RecordStore& store = *node.record_store();
    zetta::data::RecordCollection collection;
    collection.name = "ask_messages";
    collection.indexes = {"conversation"};
    ASSERT_TRUE(store.create_collection(collection).ok());
    const std::string body(1024, 'A');  // 1 KiB of base64
    std::vector<std::string> keys;
    for (int conversation = 0; conversation < 500; ++conversation) {
        for (int message = 0; message < 100; ++message) {
            zetta::data::RecordWrite write;
            write.collection = "ask_messages";
            write.key = zetta::util::new_uuid();
            write.value = body;
            write.index = {{"conversation", "conversation-" + std::to_string(conversation)}};
            char order[24];
            std::snprintf(order, sizeof(order), "%016d", message);
            write.order = order;
            ASSERT_TRUE(store.write(write).ok());
            if (conversation == 0) keys.push_back(write.key);
        }
    }
    const auto time_ms = [](auto&& work) {
        const auto started = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         started)
            .count();
    };
    std::vector<double> gets;
    std::vector<double> lists;
    for (int sample = 0; sample < 200; ++sample) {
        gets.push_back(time_ms([&] {
            ASSERT_TRUE(call("get_record", {{"collection", "ask_messages"},
                                            {"key", keys[static_cast<std::size_t>(sample) % keys.size()]}})
                            .ok);
        }));
        lists.push_back(time_ms([&] {
            const auto page = call("list_records",
                                   {{"collection", "ask_messages"},
                                    {"index", "conversation"},
                                    {"value", "conversation-" + std::to_string((sample * 37) % 500)},
                                    {"limit", 50}});
            ASSERT_TRUE(page.ok);
            ASSERT_EQ(page.value["records"].size(), 50u);
        }));
    }
    const auto percentile = [](std::vector<double> samples, double fraction) {
        std::ranges::sort(samples);
        return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1))];
    };
    std::printf("[record store] registry calls over 50000 records: get_record median %.3f ms "
                "p99 %.3f ms; list_records(50) median %.3f ms p99 %.3f ms\n",
                percentile(gets, 0.5), percentile(gets, 0.99), percentile(lists, 0.5),
                percentile(lists, 0.99));
    EXPECT_LT(percentile(gets, 0.5), 10.0);
    EXPECT_LT(percentile(lists, 0.5), 10.0);
    EXPECT_LT(percentile(gets, 0.99), 10.0);
    EXPECT_LT(percentile(lists, 0.99), 10.0);
}
