// Pub/sub on the control surface (#1033): exposure is off by default.
#include <gtest/gtest.h>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pubsub/pubsub.hpp"

namespace {

using zetta::api::ApiContext;
using zetta::api::ApiResult;
using zetta::api::Registry;

struct PubSubCapabilities : ::testing::Test {
    zetta::test::TempDir dir;
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}};
    Registry registry;
    zetta::observability::TraceContext trace = zetta::observability::make_root_trace();

    void SetUp() override {
        ASSERT_TRUE(node.start());
        zetta::api::register_core_capabilities(registry);
    }
    void TearDown() override { node.stop(); }

    ApiResult call(const std::string& name, nlohmann::json input) {
        ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call(name, input, context);
    }
};

TEST_F(PubSubCapabilities, UnexposedTopicsAreInvisible) {
    ASSERT_TRUE(call("create_topic", {{"topic", "internal"}}).ok);
    EXPECT_TRUE(node.pubsub()->topic_settings(*zetta::pubsub::parse_address("topic:internal")));
    auto published = call("publish_event", {{"topic", "internal"}, {"payload", "x"}});
    EXPECT_FALSE(published.ok);
    EXPECT_EQ(published.error.code, "not_found");
    auto listed = call("list_topics", nlohmann::json::object());
    ASSERT_TRUE(listed.ok);
    EXPECT_TRUE(listed.value["topics"].empty());
    EXPECT_EQ(call("receive_events", {{"topic", "internal"}}).error.code, "not_found");
    EXPECT_EQ(call("drop_topic", {{"topic", "internal"}}).error.code, "not_found");
}

TEST_F(PubSubCapabilities, ExposedTopicsPublishAndLongPoll) {
    auto created = call("create_topic", {{"topic", "topic:events"}, {"exposed", true}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["topic"], "topic:events");
    for (int index = 0; index < 3; ++index) {
        auto published = call("publish_event", {{"topic", "events"},
                                                {"payload", "event " + std::to_string(index)},
                                                {"correlation_id", "c-" + std::to_string(index)}});
        ASSERT_TRUE(published.ok) << published.error.message;
    }
    auto listed = call("list_topics", nlohmann::json::object());
    ASSERT_EQ(listed.value["topics"].size(), 1u);
    EXPECT_EQ(listed.value["topics"][0]["held_events"], 3);

    auto first = call("receive_events", {{"topic", "events"}, {"max", 2}});
    ASSERT_TRUE(first.ok) << first.error.message;
    ASSERT_EQ(first.value["events"].size(), 2u);
    EXPECT_EQ(first.value["events"][0]["payload"], "event 0");
    EXPECT_EQ(first.value["events"][0]["from"], "service:local");
    EXPECT_EQ(first.value["events"][1]["correlation_id"], "c-1");
    auto rest = call("receive_events", {{"topic", "events"}, {"from_sequence", first.value["next_sequence"]}});
    ASSERT_EQ(rest.value["events"].size(), 1u);
    EXPECT_EQ(rest.value["events"][0]["payload"], "event 2");
    auto empty = call("receive_events",
                      {{"topic", "events"}, {"from_sequence", rest.value["next_sequence"]}, {"wait_ms", 20}});
    ASSERT_TRUE(empty.ok);
    EXPECT_TRUE(empty.value["events"].empty());

    EXPECT_TRUE(call("drop_topic", {{"topic", "events"}}).ok);
    EXPECT_FALSE(node.pubsub()->topic_settings(*zetta::pubsub::parse_address("topic:events")));
}

TEST_F(PubSubCapabilities, RejectsMalformedTopics) {
    EXPECT_EQ(call("create_topic", {{"topic", "agent:405"}}).error.code, "invalid_input");
    EXPECT_EQ(call("create_topic", {{"topic", "topic:a*"}}).error.code, "invalid_input");
    EXPECT_EQ(call("create_topic", {{"topic", "Bad:x"}}).error.code, "invalid_input");
}

}  // namespace
