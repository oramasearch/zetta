// Pub/sub inside a zetta node (#1029, #1031, #1032, #1034).
#include <gtest/gtest.h>

#include <set>

#include "temp_dir.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/node.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/query/service.hpp"

namespace zetta::pubsub {
namespace {

using namespace std::chrono_literals;

Address address(std::string_view text) { return *parse_address(text); }

const Identity kEngine{Address{"service", "engine"}, {}};

zetta::NodeConfig config_for(const zetta::test::TempDir& directory) {
    zetta::NodeConfig config;
    config.data_dir = directory.path().string();
    return config;
}

TEST(NodePubSub, StartsWithTheNodeAndIsListedInSystemPubsubTopics) {
    zetta::test::TempDir directory;
    zetta::Node node{config_for(directory)};
    ASSERT_TRUE(node.start());
    ASSERT_NE(node.pubsub(), nullptr);
    ASSERT_TRUE(node.pubsub()->create_topic(kEngine, address("topic:orders"), {}));
    auto subscription = node.pubsub()->subscribe(kEngine, address("agent:1"), address("topic:orders"));
    PublishRequest request;
    request.to = {address("topic:orders")};
    request.payload = "x";
    ASSERT_TRUE(node.pubsub()->publish(kEngine, request).accepted);

    zetta::query::QueryService service{node};
    auto result = service.run(
        "SELECT topic, subscribers, held_events, dispatch_mode, dispatch_threads, "
        "dispatch_keys, topics_per_dispatch_thread, dispatch_min_threads, "
        "dispatch_max_threads FROM system.pubsub_topics;",
        "");
    ASSERT_TRUE(result.ok()) << result.error;
    ASSERT_EQ(result.rows.size(), 1u);
    EXPECT_EQ(result.rows[0].at("topic").get<std::string>(), "topic:orders");
    EXPECT_EQ(result.rows[0].at("subscribers").get<std::size_t>(), 1u);
    EXPECT_EQ(result.rows[0].at("held_events").get<std::uint64_t>(), 1u);
    EXPECT_EQ(result.rows[0].at("dispatch_mode"), "pool");
    EXPECT_EQ(result.rows[0].at("dispatch_threads"), 1);
    EXPECT_GE(result.rows[0].at("dispatch_keys").get<std::size_t>(), 1u);
    EXPECT_EQ(result.rows[0].at("topics_per_dispatch_thread"), 250);
    EXPECT_EQ(result.rows[0].at("dispatch_min_threads"), 1);
    EXPECT_GE(result.rows[0].at("dispatch_max_threads").get<std::size_t>(), 1u);

    // Every pub/sub operation is a span in the node's ring (#1034).
    std::set<std::string> names;
    for (const auto& span : node.span_recorder().snapshot()) names.insert(span.name);
    EXPECT_TRUE(names.contains("pubsub:create_topic"));
    EXPECT_TRUE(names.contains("pubsub:subscribe"));
    EXPECT_TRUE(names.contains("pubsub:publish"));
    node.stop();
    EXPECT_EQ(node.pubsub(), nullptr);
}

TEST(NodePubSub, SizesThePoolForOneThousandTopics) {
    zetta::test::TempDir directory;
    auto config = config_for(directory);
    config.preflight = false;
    config.pubsub_dispatch_max_threads = 4;
    zetta::Node node{config};
    ASSERT_TRUE(node.start());
    for (std::size_t topic = 0; topic < 1000; ++topic) {
        ASSERT_TRUE(node.pubsub()->create_topic(
            kEngine,
            address("topic:sizing-" + std::to_string(topic)),
            {}));
    }
    EXPECT_EQ(node.pubsub()->dispatch_keys(), 1000u);
    EXPECT_EQ(node.pubsub()->dispatch_threads(), 4u);
    node.stop();
}

TEST(NodePubSub, TableAddressesInsertRows) {
    zetta::test::TempDir directory;
    zetta::Node node{config_for(directory)};
    node.start();
    auto database = node.create_database("app");
    ASSERT_TRUE(database);
    auto table = database->create_table("logs");
    ASSERT_TRUE(table);
    PublishRequest request;
    request.to = {address("table:app.logs")};
    request.encoding = Encoding::Json;
    request.payload = R"({"level":"info","message":"hello"})";
    ASSERT_TRUE(node.pubsub()->publish(kEngine, request).accepted);
    request.payload = "not json";
    ASSERT_TRUE(node.pubsub()->publish(kEngine, request).accepted);  // accepted, then fails in the drain
    node.pubsub()->drain_dispatch();
    EXPECT_EQ(table->row_count(), 1u);
    // The bad payload is recorded on the table's error topic.
    ASSERT_TRUE(node.pubsub()->topic_settings(PubSub::error_topic_for(address("table:app.logs"))));
    node.stop();
}

TEST(NodePubSub, RightsComeFromTheNodesUsersAndRoles) {
    zetta::test::TempDir directory;
    zetta::Node node{config_for(directory)};
    node.start();
    node.ensure_builtin_roles();
    auto admin = node.create_user("ada");
    auto nobody = node.create_user("nobody");
    ASSERT_TRUE(admin && nobody);
    ASSERT_TRUE(node.assign_role(admin->id(), node.find_role("admin")->id()));
    auto& pubsub = *node.pubsub();
    std::string reason;
    EXPECT_TRUE(pubsub.create_topic(Identity{address("user:ada"), {}}, address("topic:a"), {}, &reason)) << reason;
    EXPECT_FALSE(pubsub.create_topic(Identity{address("user:nobody"), {}}, address("topic:b"), {}, &reason));
    EXPECT_FALSE(pubsub.create_topic(Identity{address("user:ghost"), {}}, address("topic:c"), {}, &reason));
    EXPECT_FALSE(pubsub.create_topic(Identity{address("agent:405"), {}}, address("topic:d"), {}, &reason));
    PublishRequest request;
    request.to = {address("topic:a")};
    request.payload = "x";
    EXPECT_TRUE(pubsub.publish(Identity{address("user:ada"), {}}, request).accepted);
    EXPECT_FALSE(pubsub.publish(Identity{address("user:nobody"), {}}, request).accepted);
    node.stop();
}

TEST(NodePubSub, StopFlushesAndTheNextStartReloads) {
    zetta::test::TempDir directory;
    {
        zetta::Node node{config_for(directory)};
        node.start();
        node.pubsub()->create_topic(kEngine, address("topic:keep"), {});
        PublishRequest request;
        request.to = {address("topic:keep")};
        request.payload = "survives";
        node.pubsub()->publish(kEngine, request);
        node.stop();
    }
    zetta::Node node{config_for(directory)};
    node.start();
    SubscribeOptions options;
    options.start = StartPosition::Oldest;
    auto subscription = node.pubsub()->subscribe(kEngine, address("agent:1"), address("topic:keep"), options);
    ASSERT_TRUE(subscription);
    auto delivery = subscription->receive(1s);
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->event.payload.bytes(), "survives");
    subscription.reset();
    node.stop();
}

TEST(NodePubSub, CanBeDisabled) {
    zetta::test::TempDir directory;
    auto config = config_for(directory);
    config.pubsub = false;
    zetta::Node node{config};
    node.start();
    EXPECT_EQ(node.pubsub(), nullptr);
    zetta::query::QueryService service{node};
    auto result = service.run("SELECT * FROM system.pubsub_topics;", "");
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_TRUE(result.rows.empty());
    node.stop();
}

}  // namespace
}  // namespace zetta::pubsub
