// SPDX-License-Identifier: MIT

// Pub/sub on the control surface (#1033): exposure is off by default.
#include <gtest/gtest.h>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/governance/permission.hpp"
#include "zetta/governance/role.hpp"
#include "zetta/governance/user.hpp"
#include "zetta/util/sha256.hpp"

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

TEST_F(PubSubCapabilities, SettingsAndMutationAuditContainHashesWithoutPayloads) {
    auto created = call("create_topic", {{"topic", "audited"}, {"settings", {
        {"exposed", true}, {"max_events", 10}, {"bounds", {{"max_priority", 3}}},
        {"delivery", {{"priority", 2}, {"guarantee", "at_least_once"}}}}}});
    ASSERT_TRUE(created.ok) << created.error.message;
    EXPECT_EQ(created.value["settings"]["delivery"]["priority"], 2);
    ASSERT_TRUE(call("publish_event", {{"topic", "audited"}, {"payload", "private original"}}).ok);
    ASSERT_TRUE(call("publish_event", {{"topic", "audited"}, {"payload", "private second"}}).ok);
    ASSERT_TRUE(call("update_event", {{"topic", "audited"}, {"sequence", 1},
        {"payload", "private edited"}, {"expected_revision", 1}}).ok);
    EXPECT_EQ(call("update_event", {{"topic", "audited"}, {"sequence", 1},
        {"payload", "stale"}, {"expected_revision", 1}}).error.code, "conflict");
    ASSERT_TRUE(call("delete_events", {{"topic", "audited"}, {"sequences", {1}}}).ok);
    ASSERT_TRUE(call("update_topic", {{"topic", "audited"}, {"settings", {{"max_events", 0}}}}).ok);
    std::size_t edits = 0;
    std::size_t deletions = 0;
    std::size_t settings = 0;
    for (const auto& event : node.audit_events()) {
        const auto action = event->action();
        if (action == zetta::governance::AuditAction::PubSubEventUpdated) ++edits;
        else if (action == zetta::governance::AuditAction::PubSubEventDeleted) ++deletions;
        else if (action == zetta::governance::AuditAction::PubSubTopicUpdated) ++settings;
        else continue;
        EXPECT_EQ(event->actor(), "service:local");
        EXPECT_FALSE(event->time().empty());
        EXPECT_EQ(event->detail().find("private"), std::string::npos);
        const auto detail = nlohmann::json::parse(event->detail());
        EXPECT_EQ(detail["before_sha256"].get<std::string>().size(), 64u);
        if (action == zetta::governance::AuditAction::PubSubEventUpdated) {
            EXPECT_EQ(detail["sequence"], 1);
            EXPECT_EQ(detail["before_sha256"], zetta::util::sha256_hex("private original"));
            EXPECT_EQ(detail["after_sha256"], zetta::util::sha256_hex("private edited"));
        }
    }
    EXPECT_EQ(edits, 1u);
    EXPECT_EQ(deletions, 2u);
    EXPECT_EQ(settings, 1u);
}

TEST_F(PubSubCapabilities, CrudScopesAreSeparateFromSubscriptionAndEachOther) {
    ASSERT_TRUE(call("create_topic", {{"topic", "scoped"}, {"exposed", true}}).ok);
    ASSERT_TRUE(call("publish_event", {{"topic", "scoped"}, {"payload", "one"}}).ok);
    const std::vector<std::pair<std::string, nlohmann::json>> operations = {
        {"get_topic", {{"topic", "scoped"}}}, {"list_topics", nlohmann::json::object()},
        {"browse_topic", {{"topic", "scoped"}}}, {"create_topic", {{"topic", "new-scoped"}}},
        {"update_topic", {{"topic", "scoped"}, {"settings", {{"max_events", 10}}}}},
        {"drop_topic", {{"topic", "scoped"}}}, {"publish_event", {{"topic", "scoped"}, {"payload", "two"}}},
        {"update_event", {{"topic", "scoped"}, {"sequence", 1}, {"payload", "edit"}, {"expected_revision", 1}}},
        {"delete_events", {{"topic", "scoped"}, {"sequences", {999}}}},
        {"receive_events", {{"topic", "scoped"}}}};
    for (const auto& permission : {"topics:read", "topics:write", "events:write", "pubsub:subscribe"}) {
        auto role = node.create_role(permission);
        ASSERT_TRUE(role);
        ASSERT_TRUE(node.grant_permissions(permission, {permission, "mcp:call"}));
        auto user = node.create_user(permission);
        ASSERT_TRUE(user);
        ASSERT_TRUE(node.assign_role(user->id(), role->id()));
        ApiContext context{node, nullptr, user->id(), trace, &node.span_recorder()};
        context.acting_is_user = true;
        for (std::size_t index = 0; index < operations.size(); ++index) {
            const std::string required = index < 3 ? "topics:read" : index < 6 ? "topics:write" :
                index < 9 ? "events:write" : "pubsub:subscribe";
            // Destructive authorized calls are covered by their own journeys.
            if (required == permission && (index == 3 || index == 5)) continue;
            const auto& [name, input] = operations[index];
            const auto result = registry.call(name, input, context);
            if (required == permission) EXPECT_TRUE(result.ok) << name << ": " << result.error.message;
            else EXPECT_EQ(result.error.code, "unauthorized") << permission << " invoked " << name;
        }
    }
}

TEST_F(PubSubCapabilities, RejectsUnknownSettingsAndAmbiguousDeletes) {
    EXPECT_FALSE(call("create_topic", {{"topic", "bad"}, {"settings", {{"mystery", 1}}}}).ok);
    EXPECT_FALSE(call("delete_events", {{"topic", "bad"}}).ok);
    EXPECT_FALSE(call("delete_events", {{"topic", "bad"}, {"sequences", {1}}, {"before_sequence", 2}}).ok);
    EXPECT_FALSE(call("update_event", {{"topic", "bad"}, {"sequence", 1}, {"payload", "x"}}).ok);
    EXPECT_FALSE(call("browse_topic", {{"topic", "bad"}, {"limit", 1001}}).ok);
    EXPECT_FALSE(call("browse_topic", {{"topic", "bad"}, {"direction", "sideways"}}).ok);
    for (const auto* name : {"get_topic", "browse_topic"}) {
        ASSERT_TRUE(call("create_topic", {{"topic", name}}).ok);
        EXPECT_EQ(call(name, {{"topic", name}}).error.code, "not_found");
    }
}

TEST_F(PubSubCapabilities, ExposedInboxesUseTheirConcreteTopicAddress) {
    ASSERT_TRUE(call("create_topic", {{"topic", "inbox:agent.viewer"}, {"settings", {
        {"exposed", true}, {"max_events", 10}}}}).ok);
    ASSERT_TRUE(call("publish_event", {{"topic", "inbox:agent.viewer"}, {"payload", "one"}}).ok);
    auto read = call("browse_topic", {{"topic", "inbox:agent.viewer"}});
    ASSERT_TRUE(read.ok) << read.error.message;
    EXPECT_EQ(read.value["events"][0]["payload"], "one");
    ASSERT_TRUE(call("update_event", {{"topic", "inbox:agent.viewer"}, {"sequence", 1},
        {"payload", "two"}, {"expected_revision", 1}}).ok);
    const auto owner = *zetta::pubsub::parse_address("agent:viewer");
    auto subscription = node.pubsub()->open_inbox({{"service", "local"}, {}}, owner);
    ASSERT_TRUE(subscription);
    const auto delivered = subscription->receive(std::chrono::milliseconds(0));
    ASSERT_TRUE(delivered);
    EXPECT_EQ(delivered->event.payload.bytes(), "two");
    auto listed = call("list_topics", nlohmann::json::object());
    EXPECT_EQ(listed.value["topics"][0]["topic"], "inbox:agent.viewer");
    EXPECT_FALSE(call("browse_topic", {{"topic", "inbox:agent.*"}}).ok);
    ASSERT_TRUE(call("drop_topic", {{"topic", "inbox:agent.viewer"}}).ok);
    EXPECT_TRUE(subscription->closed());
}

TEST_F(PubSubCapabilities, ReadScopeFiltersEachConcreteTopicAndBoundsGovernPublish) {
    ASSERT_TRUE(call("create_topic", {{"topic", "allowed.orders"}, {"settings", {
        {"exposed", true}, {"bounds", {{"max_priority", 3}}}}}}).ok);
    ASSERT_TRUE(call("create_topic", {{"topic", "denied.orders"}, {"exposed", true}}).ok);
    ASSERT_TRUE(node.create_role("topic-reader"));
    ASSERT_TRUE(node.grant_permissions("topic-reader", {"mcp:call"}));
    ASSERT_TRUE(node.grant_scoped_permissions("topic-reader", {"topics:read"},
        zetta::governance::Resource::topic("allowed.")));
    auto user = node.create_user("topic-reader");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), node.find_role("topic-reader")->id()));
    ApiContext context{node, nullptr, user->id(), trace, &node.span_recorder()};
    context.acting_is_user = true;
    const auto listed = registry.call("list_topics", nlohmann::json::object(), context);
    ASSERT_TRUE(listed.ok) << listed.error.message;
    ASSERT_EQ(listed.value["topics"].size(), 1u);
    EXPECT_EQ(listed.value["topics"][0]["topic"], "topic:allowed.orders");
    EXPECT_EQ(registry.call("browse_topic", {{"topic", "denied.orders"}}, context).error.code, "unauthorized");
    const auto published = call("publish_event", {{"topic", "allowed.orders"}, {"payload", "one"}, {"priority", 10}});
    ASSERT_TRUE(published.ok);
    EXPECT_FALSE(published.value["clamped"].empty());
    ASSERT_TRUE(call("update_topic", {{"topic", "allowed.orders"}, {"settings", {{"over_bound", "reject"}}}}).ok);
    EXPECT_FALSE(call("publish_event", {{"topic", "allowed.orders"}, {"payload", "two"}, {"priority", 10}}).ok);
}

// #1432: an inbox is not a topic of its namespace. A topic-prefix grant on `agent.` does not
// reach the exposed inbox `inbox:agent.viewer`; an explicit grant on `inbox:agent.viewer` does.
TEST_F(PubSubCapabilities, ATopicPrefixGrantDoesNotCoverAnInbox) {
    ASSERT_TRUE(call("create_topic", {{"topic", "inbox:agent.viewer"}, {"settings", {{"exposed", true}}}}).ok);
    ASSERT_TRUE(call("create_topic", {{"topic", "agent.events"}, {"exposed", true}}).ok);
    ASSERT_TRUE(node.create_role("agent-topics"));
    ASSERT_TRUE(node.grant_permissions("agent-topics", {"mcp:call"}));
    ASSERT_TRUE(node.grant_scoped_permissions("agent-topics", {"topics:read"},
        zetta::governance::Resource::topic("agent.")));
    auto user = node.create_user("agent-topics-user");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), node.find_role("agent-topics")->id()));
    ApiContext context{node, nullptr, user->id(), trace, &node.span_recorder()};
    context.acting_is_user = true;
    EXPECT_TRUE(registry.call("browse_topic", {{"topic", "agent.events"}}, context).ok);
    EXPECT_EQ(registry.call("browse_topic", {{"topic", "inbox:agent.viewer"}}, context).error.code,
              "unauthorized");
    ASSERT_TRUE(node.grant_scoped_permissions("agent-topics", {"topics:read"},
        zetta::governance::Resource::topic("inbox:agent.viewer")));
    EXPECT_TRUE(registry.call("browse_topic", {{"topic", "inbox:agent.viewer"}}, context).ok);
}

TEST_F(PubSubCapabilities, IdlePollsAddNoSummariesAndDenialsStayPerCall) {
    ASSERT_TRUE(call("create_topic", {{"topic", "idle"}, {"exposed", true}}).ok);
    const auto before = node.audit_event_count();
    // Six hundred one-hertz returns represent ten minutes of an idle agent.
    for (std::size_t call_index = 0; call_index < 600; ++call_index) {
        const auto received = call("receive_events", {{"topic", "idle"}});
        ASSERT_TRUE(received.ok) << received.error.message;
        ASSERT_TRUE(received.value["events"].empty());
    }
    EXPECT_EQ(node.audit_event_count(), before + 1);

    node.flush_audit_summaries();
    EXPECT_EQ(node.audit_event_count(), before + 1);
    for (const auto& event : node.audit_events()) {
        EXPECT_FALSE(
            event->action() == zetta::governance::AuditAction::DataAccessSummary &&
            event->target() == "api:receive_events");
    }

    ASSERT_TRUE(node.create_role("poll-denied"));
    ASSERT_TRUE(node.grant_permissions("poll-denied", {"mcp:call"}));
    auto user = node.create_user("poll-denied");
    ASSERT_TRUE(user);
    ASSERT_TRUE(node.assign_role(user->id(), node.find_role("poll-denied")->id()));
    ApiContext context{node, nullptr, user->id(), trace, &node.span_recorder()};
    context.acting_is_user = true;
    const auto before_denials = node.audit_event_count();
    for (std::size_t call_index = 0; call_index < 3; ++call_index) {
        const auto denied = registry.call(
            "receive_events", {{"topic", "idle"}}, context);
        EXPECT_EQ(denied.error.code, "unauthorized");
    }
    EXPECT_EQ(node.audit_event_count(), before_denials + 3);
}

}  // namespace
