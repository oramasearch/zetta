// The pub/sub facade end to end (#1027, #1028, #1029, #1030, #1031, #1032).
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/pubsub/pubsub.hpp"

namespace zetta::pubsub {
namespace {

using namespace std::chrono_literals;

Address address(std::string_view text) { return *parse_address(text); }

struct CapturedLog {
    std::mutex mutex;
    std::vector<std::string> lines;
    LogSink sink() {
        return [this](LogLevel level, std::string_view message) {
            std::lock_guard lock(mutex);
            lines.push_back(std::string(to_string(level)) + " " + std::string(message));
        };
    }
    bool contains(std::string_view text) {
        std::lock_guard lock(mutex);
        for (const auto& line : lines) {
            if (line.find(text) != std::string::npos) return true;
        }
        return false;
    }
};

const Identity kSteve{address("user:steve"), {}};

PubSubConfig test_config(const std::filesystem::path& directory,
                         std::uint64_t memory = std::uint64_t{64} << 20) {
    PubSubConfig config;
    config.data_directory = directory;
    config.detect_resources = false;
    config.limits = PubSubLimits{.memory_bytes = memory,
                                 .spill_disk_bytes = std::uint64_t{256} << 20,
                                 .topics_per_thread = 250,
                                 .dispatch_min_threads = 1,
                                 .dispatch_max_threads = 2,
                                 .hold_publisher = 200ms,
                                 .shutdown_budget = 5000ms};
    config.maintenance_interval = 50ms;
    return config;
}

PublishRequest text_to(std::vector<Address> to, std::string payload) {
    PublishRequest request;
    request.to = std::move(to);
    request.encoding = Encoding::Text;
    request.payload = std::move(payload);
    return request;
}

std::string text_of(const std::optional<Delivery>& delivery) {
    return delivery ? std::string(delivery->event.payload.bytes()) : std::string("<none>");
}

// --- Topics and broadcast ----------------------------------------------------

TEST(PubSubCrud, BrowseDoesNotConsumeAndEditsRespectDeliveryAndRevision) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    const auto topic = address("topic:crud");
    TopicSettings settings;
    settings.exposed = true;
    settings.max_events = 100;
    settings.delivery.guarantee = Guarantee::ExactlyOnce;
    ASSERT_TRUE(pubsub.create_topic(kSteve, topic, settings));
    auto early = pubsub.subscribe(kSteve, address("agent:early"), topic);
    auto pending = pubsub.subscribe(kSteve, address("agent:pending"), topic);
    ASSERT_TRUE(early && pending);
    ASSERT_TRUE(pubsub.publish(kSteve, text_to({topic}, "original")).accepted);
    const auto before = pubsub.get_topic(kSteve, topic);
    ASSERT_TRUE(before);
    auto browsed = pubsub.browse_topic(kSteve, topic, {}, 10, false);
    ASSERT_TRUE(browsed);
    EXPECT_EQ((*browsed)["events"][0]["revision"], 1);
    EXPECT_EQ(pubsub.get_topic(kSteve, topic), before);
    auto received = early->receive(0ms);
    ASSERT_TRUE(received);
    EXPECT_EQ(received->event.payload.bytes(), "original");
    ASSERT_TRUE(early->ack(received->sequence));
    auto edited = pubsub.update_event(kSteve, topic, 1, "edited", 1, false);
    ASSERT_TRUE(edited.ok) << edited.reason;
    EXPECT_FALSE(pubsub.update_event(kSteve, topic, 1, "stale", 1, false).ok);
    EXPECT_FALSE(early->receive(0ms));
    received = pending->receive(0ms);
    ASSERT_TRUE(received);
    EXPECT_EQ(received->event.payload.bytes(), "edited");
    EXPECT_EQ(received->event.revision, 2u);
    ASSERT_TRUE(pending->ack(1));
    ASSERT_TRUE(pubsub.update_event(kSteve, topic, 1, "explicit", 2, true).ok);
    for (auto* subscriber : {early.get(), pending.get()}) {
        auto redelivery = subscriber->receive(0ms);
        ASSERT_TRUE(redelivery);
        EXPECT_TRUE(redelivery->redelivery);
        EXPECT_EQ(redelivery->event.revision, 3u);
        EXPECT_EQ(redelivery->event.payload.bytes(), "explicit");
        ASSERT_TRUE(subscriber->ack(1));
        EXPECT_FALSE(subscriber->receive(0ms));
    }
}

TEST(PubSubCrud, DeletionsLeaveHolesAndCancelInflightAndExplicitRedelivery) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    const auto topic = address("topic:purge");
    TopicSettings settings;
    settings.exposed = true;
    settings.max_events = 100;
    settings.delivery.guarantee = Guarantee::AtLeastOnce;
    ASSERT_TRUE(pubsub.create_topic(kSteve, topic, settings));
    auto early = pubsub.subscribe(kSteve, address("agent:early"), topic);
    auto pending = pubsub.subscribe(kSteve, address("agent:pending"), topic);
    for (int index = 1; index <= 5; ++index) {
        ASSERT_TRUE(pubsub.publish(kSteve, text_to({topic}, std::to_string(index))).accepted);
    }
    ASSERT_TRUE(early->receive(0ms));
    ASSERT_TRUE(pubsub.update_event(kSteve, topic, 1, "queued", 1, true).ok);
    auto removed = pubsub.delete_events(kSteve, topic, {1, 3, 99}, {});
    ASSERT_TRUE(removed.ok);
    EXPECT_EQ(removed.changes.size(), 2u);
    EXPECT_EQ(text_of(early->receive(0ms)), "2");
    EXPECT_EQ(text_of(pending->receive(0ms)), "2");
    EXPECT_EQ(text_of(pending->receive(0ms)), "4");
    EXPECT_FALSE(early->ack(1));
    auto backwards = pubsub.browse_topic(kSteve, topic, 4, 2, true);
    ASSERT_TRUE(backwards);
    ASSERT_EQ((*backwards)["events"].size(), 2u);
    EXPECT_EQ((*backwards)["events"][0]["sequence"], 4);
    EXPECT_EQ((*backwards)["events"][1]["sequence"], 2);
    EXPECT_EQ((*backwards)["earliest_sequence"], 2);
    EXPECT_EQ((*backwards)["next_sequence"], 6);
    removed = pubsub.delete_events(kSteve, topic, {}, 5);
    EXPECT_EQ(removed.changes.size(), 2u);
    EXPECT_EQ(text_of(pending->receive(0ms)), "5");
}

TEST(PubSubCrud, SettingsTrimAndRefuseImmutableFieldsAndSurviveRestart) {
    zetta::test::TempDir directory;
    const auto topic = address("topic:durable");
    {
        PubSub pubsub(test_config(directory.path()));
        pubsub.start();
        TopicSettings settings;
        settings.exposed = true;
        settings.max_events = 100;
        ASSERT_TRUE(pubsub.create_topic(kSteve, topic, settings));
        for (int index = 1; index <= 4; ++index) {
            ASSERT_TRUE(pubsub.publish(kSteve, text_to({topic}, std::to_string(index))).accepted);
        }
        auto changed = pubsub.update_topic(kSteve, topic, {{"max_events", 2}});
        ASSERT_TRUE(changed.ok);
        EXPECT_EQ(changed.changes.size(), 2u);
        changed = pubsub.update_topic(kSteve, topic, {{"store", "queue"}});
        EXPECT_FALSE(changed.ok);
        EXPECT_NE(changed.reason.find("store is immutable"), std::string::npos);
        ASSERT_TRUE(pubsub.update_event(kSteve, topic, 3, "persisted edit", 1, false).ok);
        ASSERT_TRUE(pubsub.delete_events(kSteve, topic, {4}, {}).ok);
        pubsub.shutdown();
    }
    PubSub restored(test_config(directory.path()));
    restored.start();
    auto browsed = restored.browse_topic(kSteve, topic, {}, 10, false);
    ASSERT_TRUE(browsed);
    ASSERT_EQ((*browsed)["events"].size(), 1u);
    EXPECT_EQ((*browsed)["events"][0]["sequence"], 3);
    EXPECT_EQ((*browsed)["events"][0]["payload"], "persisted edit");
    EXPECT_EQ((*browsed)["events"][0]["revision"], 2);
    EXPECT_EQ((*browsed)["events"][0]["edited_by"], "user:steve");
    EXPECT_EQ((*browsed)["next_sequence"], 5);
    EXPECT_EQ(restored.topic_settings(topic)->max_events, 2u);
    ASSERT_TRUE(restored.update_topic(kSteve, topic, {{"retention_ms", 0}}).ok);
    EXPECT_EQ((*restored.get_topic(kSteve, topic))["held_events"], 0);
}

TEST(PubSubCrud, ConcurrentEditorsHaveOneRevisionWinner) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    const auto topic = address("topic:edit-race");
    TopicSettings settings;
    settings.exposed = true;
    ASSERT_TRUE(pubsub.create_topic(kSteve, topic, settings));
    ASSERT_TRUE(pubsub.publish(kSteve, text_to({topic}, "original")).accepted);
    std::atomic<int> succeeded{0};
    std::vector<std::thread> editors;
    for (int index = 0; index < 8; ++index) {
        editors.emplace_back([&, index] {
            if (pubsub.update_event(kSteve, topic, 1, std::to_string(index), 1, false).ok) ++succeeded;
        });
    }
    for (auto& editor : editors) editor.join();
    EXPECT_EQ(succeeded.load(), 1);
    const auto result = pubsub.browse_topic(kSteve, topic, {}, 1, false);
    ASSERT_TRUE(result);
    EXPECT_EQ((*result)["events"][0]["revision"], 2);
    EXPECT_EQ((*result)["next_sequence"], 2);
}

TEST(PubSubCrud, ExplicitRedeliveryOnlyTargetsTheGroupMemberThatReceived) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    const auto topic = address("topic:group-edits");
    TopicSettings settings;
    settings.exposed = true;
    settings.max_events = 10;
    ASSERT_TRUE(pubsub.create_topic(kSteve, topic, settings));
    SubscribeOptions options;
    options.group = "workers";
    auto first = pubsub.subscribe(kSteve, address("agent:first"), topic, options);
    auto second = pubsub.subscribe(kSteve, address("agent:second"), topic, options);
    ASSERT_TRUE(first && second);
    ASSERT_TRUE(pubsub.publish(kSteve, text_to({topic}, "one")).accepted);
    ASSERT_TRUE(first->receive(0ms));
    ASSERT_TRUE(pubsub.update_event(kSteve, topic, 1, "two", 1, true).ok);
    EXPECT_FALSE(second->receive(0ms));
    const auto redelivery = first->receive(0ms);
    ASSERT_TRUE(redelivery);
    EXPECT_TRUE(redelivery->redelivery);
    EXPECT_EQ(redelivery->event.payload.bytes(), "two");
    EXPECT_FALSE(first->receive(0ms));
}

TEST(PubSub, BroadcastsToEverySubscriberInOrder) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    ASSERT_TRUE(pubsub.create_topic(kSteve, address("topic:orders"), {}));
    auto first = pubsub.subscribe(kSteve, address("agent:1"), address("topic:orders"));
    auto second = pubsub.subscribe(kSteve, address("agent:2"), address("topic:orders"));
    ASSERT_TRUE(first && second);
    for (int index = 0; index < 3; ++index) {
        auto result = pubsub.publish(kSteve, text_to({address("topic:orders")}, std::to_string(index)));
        ASSERT_TRUE(result.accepted) << result.error;
    }
    for (auto* subscription : {first.get(), second.get()}) {
        for (int index = 0; index < 3; ++index) EXPECT_EQ(text_of(subscription->receive(1s)), std::to_string(index));
        EXPECT_FALSE(subscription->receive(20ms));
    }
}

TEST(PubSub, EnvelopeIsStampedFromTheIdentityNotTheRequest) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"));
    const Identity acting{address("user:steve"), {address("agent:ask-orama"), address("agent:security-scan")}};
    auto request = text_to({address("topic:t")}, "x");
    request.correlation_id = "req-1";
    request.reply_to = {address("agent:ask-orama")};
    ASSERT_TRUE(pubsub.publish(acting, request).accepted);
    auto delivery = subscription->receive(1s);
    ASSERT_TRUE(delivery);
    const auto& envelope = delivery->event.envelope;
    EXPECT_EQ(envelope.from, address("user:steve"));
    ASSERT_EQ(envelope.sender.size(), 2u);
    EXPECT_EQ(envelope.sender[1], address("agent:security-scan"));
    EXPECT_EQ(envelope.correlation_id, "req-1");
    ASSERT_EQ(envelope.topics_chain.size(), 1u);
    EXPECT_EQ(envelope.topics_chain[0], address("topic:t"));
}

TEST(PubSub, NewSubscribersStartAtTheNextEventByDefault) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    pubsub.publish(kSteve, text_to({address("topic:t")}, "before"));
    auto late = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"));
    SubscribeOptions oldest;
    oldest.start = StartPosition::Oldest;
    auto replaying = pubsub.subscribe(kSteve, address("agent:2"), address("topic:t"), oldest);
    pubsub.publish(kSteve, text_to({address("topic:t")}, "after"));
    EXPECT_EQ(text_of(late->receive(1s)), "after");
    EXPECT_EQ(text_of(replaying->receive(1s)), "before");
    EXPECT_EQ(text_of(replaying->receive(1s)), "after");
}

// --- Recipients --------------------------------------------------------------

TEST(PubSub, AGroupSharesEachEventOnce) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:tasks"), {});
    SubscribeOptions workers;
    workers.group = "workers";
    auto first = pubsub.subscribe(kSteve, address("agent:1"), address("topic:tasks"), workers);
    auto second = pubsub.subscribe(kSteve, address("agent:2"), address("topic:tasks"), workers);
    for (int index = 0; index < 10; ++index) pubsub.publish(kSteve, text_to({address("topic:tasks")}, std::to_string(index)));
    std::set<std::string> seen;
    for (auto* subscription : {first.get(), second.get()}) {
        while (auto delivery = subscription->receive(20ms)) {
            EXPECT_TRUE(seen.insert(std::string(delivery->event.payload.bytes())).second);
        }
    }
    EXPECT_EQ(seen.size(), 10u);
}

TEST(PubSub, AMessageCanAskForNRecipients) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    std::vector<std::unique_ptr<Subscription>> subscriptions;
    for (int index = 0; index < 5; ++index) {
        subscriptions.push_back(pubsub.subscribe(kSteve, address("agent:" + std::to_string(index)), address("topic:t")));
    }
    auto request = text_to({address("topic:t")}, "two of you");
    request.delivery.recipients = Recipients{Recipients::Mode::Count, 2};
    ASSERT_TRUE(pubsub.publish(kSteve, request).accepted);
    int received = 0;
    for (auto& subscription : subscriptions) received += subscription->receive(30ms) ? 1 : 0;
    EXPECT_EQ(received, 2);
}

// --- Priority ----------------------------------------------------------------

TEST(PubSub, PriorityTopicsDeliverHigherFirstAndAgeTheWaiting) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    TopicSettings settings;
    settings.priority_levels = 4;
    settings.priority_aging = 100ms;
    pubsub.create_topic(kSteve, address("topic:p"), settings);
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:p"));
    auto send = [&](std::string text, std::uint8_t priority) {
        auto request = text_to({address("topic:p")}, std::move(text));
        request.delivery.priority = priority;
        pubsub.publish(kSteve, request);
    };
    send("low", 0);
    send("high", 250);
    send("mid", 128);
    EXPECT_EQ(text_of(subscription->receive(1s)), "high");
    EXPECT_EQ(text_of(subscription->receive(1s)), "mid");
    EXPECT_EQ(text_of(subscription->receive(1s)), "low");

    send("old-low", 0);
    std::this_thread::sleep_for(150ms);  // past the aging limit
    send("fresh-high", 250);
    EXPECT_EQ(text_of(subscription->receive(1s)), "old-low");
    EXPECT_EQ(text_of(subscription->receive(1s)), "fresh-high");
}

// --- Guarantees --------------------------------------------------------------

TEST(PubSub, AtLeastOnceRedeliversUntilAcknowledged) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    TopicSettings settings;
    settings.delivery.guarantee = Guarantee::AtLeastOnce;
    settings.delivery.ack_deadline = 50ms;
    pubsub.create_topic(kSteve, address("topic:t"), settings);
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"));
    pubsub.publish(kSteve, text_to({address("topic:t")}, "task"));
    auto first = subscription->receive(1s);
    ASSERT_TRUE(first);
    EXPECT_FALSE(first->redelivery);
    auto again = subscription->receive(1s);
    ASSERT_TRUE(again);
    EXPECT_TRUE(again->redelivery);
    EXPECT_EQ(again->sequence, first->sequence);
    EXPECT_TRUE(subscription->ack(first->sequence));
    EXPECT_FALSE(subscription->receive(120ms));
}

TEST(PubSub, ExactlyOnceDropsARepublishedDuplicate) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    TopicSettings settings;
    settings.delivery.guarantee = Guarantee::ExactlyOnce;
    pubsub.create_topic(kSteve, address("topic:t"), settings);
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"));
    auto request = text_to({address("topic:t")}, "once");
    request.id = "fixed-id";
    pubsub.publish(kSteve, request);
    auto delivery = subscription->receive(1s);
    ASSERT_TRUE(delivery);
    subscription->ack(delivery->sequence);
    pubsub.publish(kSteve, request);  // a retry with the same id
    EXPECT_FALSE(subscription->receive(50ms));
}

// --- Addressing, inboxes, wildcards -------------------------------------------

TEST(PubSub, AgentsGetAnInboxAndBccIsHidden) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    auto request = text_to({address("agent:405")}, "hello");
    request.bcc = {address("agent:audit")};
    ASSERT_TRUE(pubsub.publish(kSteve, request).accepted);  // agent:405 has not connected yet
    auto inbox = pubsub.open_inbox(Identity{address("agent:405"), {}}, address("agent:405"));
    auto delivery = inbox->receive(1s);
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->event.payload.bytes(), "hello");
    EXPECT_TRUE(delivery->event.envelope.bcc.empty());  // the to-recipient never sees bcc
    auto audit = pubsub.open_inbox(Identity{address("agent:audit"), {}}, address("agent:audit"));
    auto audit_copy = audit->receive(1s);
    ASSERT_TRUE(audit_copy);
    ASSERT_EQ(audit_copy->event.envelope.bcc.size(), 1u);  // only itself
    EXPECT_EQ(audit_copy->event.envelope.bcc[0], address("agent:audit"));
}

TEST(PubSub, WildcardsResolveThroughACacheThatFollowsTheSubscriberList) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    auto scan = pubsub.open_inbox(Identity{address("agent:security-scan"), {}}, address("agent:security-scan"));
    auto billing = pubsub.open_inbox(Identity{address("agent:billing"), {}}, address("agent:billing"));
    auto result = pubsub.publish(kSteve, text_to({address("agent:security-*")}, "alert"));
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.delivered_to.size(), 1u);
    EXPECT_EQ(text_of(scan->receive(1s)), "alert");
    EXPECT_FALSE(billing->receive(20ms));
    // A new matching agent joins: the cached resolution is refreshed.
    auto triage = pubsub.open_inbox(Identity{address("agent:security-triage"), {}}, address("agent:security-triage"));
    result = pubsub.publish(kSteve, text_to({address("agent:security-*")}, "alert 2"));
    EXPECT_EQ(result.delivered_to.size(), 2u);
    EXPECT_EQ(text_of(triage->receive(1s)), "alert 2");
}

// --- Drains, replies, errors, dead letters ------------------------------------

TEST(PubSub, TableDrainInsertsRowsAndQueriesReplyToReplyTo) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    std::mutex mutex;
    std::vector<std::string> rows;
    pubsub.register_drain("table", std::make_shared<CallbackDrain>([&](const Address&, const Event& event) {
        std::lock_guard lock(mutex);
        rows.emplace_back(event.payload.bytes());
        return DrainResult{.ok = true, .result = R"({"inserted":1})"};
    }));  // an insert does not reply by default
    pubsub.register_drain("function", std::make_shared<CallbackDrain>(
                                          [](const Address&, const Event&) {
                                              return DrainResult{.ok = true, .result = R"({"answer":42})"};
                                          },
                                          ReplyPolicy::ReplyTo));  // a query does
    auto caller = pubsub.open_inbox(Identity{address("agent:caller"), {}}, address("agent:caller"));

    auto insert = text_to({address("table:logs")}, R"({"line":"x"})");
    insert.reply_to = {address("agent:caller")};
    ASSERT_TRUE(pubsub.publish(kSteve, insert).accepted);
    auto query = text_to({address("function:answer")}, "{}");
    query.reply_to = {address("agent:caller")};
    query.correlation_id = "q-7";
    ASSERT_TRUE(pubsub.publish(kSteve, query).accepted);
    pubsub.drain_dispatch();

    {
        std::lock_guard lock(mutex);
        ASSERT_EQ(rows.size(), 1u);
        EXPECT_EQ(rows[0], R"({"line":"x"})");
    }
    auto reply = caller->receive(1s);
    ASSERT_TRUE(reply);
    EXPECT_EQ(reply->event.payload.bytes(), R"({"answer":42})");
    EXPECT_EQ(reply->event.envelope.correlation_id, "q-7");
    EXPECT_EQ(reply->event.envelope.from, address("function:answer"));
    EXPECT_FALSE(caller->receive(30ms));  // no reply for the insert
}

TEST(PubSub, DrainFailuresGoToALazilyCreatedErrorTopic) {
    zetta::test::TempDir directory;
    CapturedLog log;
    PubSub pubsub(test_config(directory.path()), nullptr, log.sink());
    pubsub.start();
    pubsub.register_drain("model", std::make_shared<CallbackDrain>([](const Address&, const Event&) {
        return DrainResult{.ok = false, .error = "inference failed: bad input"};
    }));
    const auto error_topic = PubSub::error_topic_for(address("model:churn"));
    EXPECT_FALSE(pubsub.topic_settings(error_topic));  // not created up front
    auto request = text_to({address("model:churn")}, "{}");
    request.correlation_id = "c-1";
    pubsub.publish(kSteve, request);
    pubsub.drain_dispatch();
    ASSERT_TRUE(pubsub.topic_settings(error_topic));
    auto errors = pubsub.subscribe(kSteve, address("agent:ops"), error_topic, {.start = StartPosition::Oldest});
    auto failure = errors->receive(1s);
    ASSERT_TRUE(failure);
    EXPECT_EQ(failure->event.headers.at("error"), "inference failed: bad input");
    EXPECT_EQ(failure->event.envelope.correlation_id, "c-1");
    EXPECT_TRUE(log.contains("model:churn failed event"));
}

TEST(PubSub, UndeliverableEventsAreDeadLetteredWithAReason) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    auto result = pubsub.publish(kSteve, text_to({address("widget:nothing")}, "lost"));
    EXPECT_FALSE(result.accepted);
    pubsub.publish(kSteve, text_to({address("agent:nobody-*")}, "no match"));
    auto letters = pubsub.subscribe(kSteve, address("agent:ops"), PubSub::dead_letter_address(),
                                    {.start = StartPosition::Oldest});
    auto first = letters->receive(1s);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->event.headers.at("reason"), "no such address widget:nothing");
    auto second = letters->receive(1s);
    ASSERT_TRUE(second);
    EXPECT_EQ(second->event.headers.at("reason"), "no address matches agent:nobody-*");
}

TEST(PubSub, ExpiredEventsAreDeadLetteredNotDelivered) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"));
    auto request = text_to({address("topic:t")}, "stale");
    request.ttl = 10ms;
    pubsub.publish(kSteve, request);
    std::this_thread::sleep_for(30ms);
    EXPECT_FALSE(subscription->receive(30ms));
    auto letters = pubsub.subscribe(kSteve, address("agent:ops"), PubSub::dead_letter_address(),
                                    {.start = StartPosition::Oldest});
    auto letter = letters->receive(1s);
    ASSERT_TRUE(letter);
    EXPECT_EQ(letter->event.headers.at("reason"), "time to live expired");
}

TEST(PubSub, TopicLoopsAreStopped) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:a"), {});
    auto request = text_to({address("topic:a")}, "looping");
    request.topics_chain = {address("topic:a")};  // it has already been through topic:a
    auto result = pubsub.publish(kSteve, request);
    EXPECT_FALSE(result.accepted);
    ASSERT_FALSE(result.refused.empty());
    EXPECT_EQ(result.refused[0].second, "topic loop");
}

// --- Governance --------------------------------------------------------------

class TestAuthorizer final : public Authorizer {
public:
    bool authorize(const Address& principal, PubSubOperation operation, const Address& target) override {
        if (principal == Address{"user", "mallory"}) return false;
        if (operation == PubSubOperation::SendWildcard) return principal == Address{"user", "steve"};
        (void)target;
        return true;
    }
    DeliveryBounds bounds(const Address&, const Address& target) override {
        DeliveryBounds bounds;
        if (target.name == "bounded" || target.name == "strict") bounds.max_priority = 5;
        return bounds;
    }
    bool may_act_for(const Address& actor, const Address& principal) override {
        return !(actor == Address{"agent", "rogue"}) || principal.kind != "user";
    }
};

TEST(PubSub, GovernanceClampsByDefaultAndRejectsWhenTheTopicSaysSo) {
    zetta::test::TempDir directory;
    std::vector<AuditRecord> audits;
    std::mutex mutex;
    PubSub pubsub(test_config(directory.path()), std::make_shared<TestAuthorizer>());
    pubsub.set_audit_sink([&](const AuditRecord& record) {
        std::lock_guard lock(mutex);
        audits.push_back(record);
    });
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:bounded"), {});
    TopicSettings strict;
    strict.over_bound = OverBoundPolicy::Reject;
    pubsub.create_topic(kSteve, address("topic:strict"), strict);
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:bounded"));

    auto request = text_to({address("topic:bounded")}, "urgent");
    request.delivery.priority = 9;
    auto result = pubsub.publish(kSteve, request);
    ASSERT_TRUE(result.accepted);
    ASSERT_EQ(result.clamped.size(), 1u);
    EXPECT_EQ(result.clamped[0], "topic:bounded.priority");
    auto delivery = subscription->receive(1s);
    ASSERT_TRUE(delivery);
    EXPECT_EQ(delivery->event.delivery.priority, 5);

    request.to = {address("topic:strict")};
    result = pubsub.publish(kSteve, request);
    EXPECT_FALSE(result.accepted);
    EXPECT_NE(result.error.find("priority 9 exceeds the bound 5"), std::string::npos);

    // No right at all: denied, never clamped.
    result = pubsub.publish(Identity{address("user:mallory"), {}}, text_to({address("topic:bounded")}, "x"));
    EXPECT_FALSE(result.accepted);
    // Wildcards are their own right.
    result = pubsub.publish(Identity{address("user:alice"), {}}, text_to({address("topic:*")}, "x"));
    EXPECT_FALSE(result.accepted);
    std::lock_guard lock(mutex);
    EXPECT_TRUE(std::ranges::any_of(audits, [](const AuditRecord& record) { return record.action == "clamped"; }));
    EXPECT_TRUE(std::ranges::any_of(audits, [](const AuditRecord& record) { return record.action == "publish_refused"; }));
}

TEST(PubSub, EveryDelegationLinkIsCheckedAndTheChainIsCapped) {
    zetta::test::TempDir directory;
    CapturedLog log;
    auto config = test_config(directory.path());
    config.max_sender_chain = 3;
    PubSub pubsub(config, std::make_shared<TestAuthorizer>(), log.sink());
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    auto result = pubsub.publish(Identity{address("user:steve"), {address("agent:rogue")}},
                                 text_to({address("topic:t")}, "x"));
    EXPECT_FALSE(result.accepted);
    EXPECT_EQ(result.error, "agent:rogue may not act for user:steve");
    result = pubsub.publish(Identity{address("user:steve"),
                                     {address("agent:a"), address("agent:b"), address("agent:c"), address("agent:d")}},
                            text_to({address("topic:t")}, "x"));
    EXPECT_FALSE(result.accepted);
    EXPECT_TRUE(log.contains("ERROR dropped event"));
    EXPECT_TRUE(log.contains("exceeds the limit of 3"));
}

// --- Auto-creation -----------------------------------------------------------

TEST(PubSub, AutoCreationFollowsConfiguredPrefixesAndIsLogged) {
    zetta::test::TempDir directory;
    CapturedLog log;
    auto config = test_config(directory.path());
    config.auto_create_prefixes = {"dev."};
    PubSub pubsub(config, nullptr, log.sink());
    pubsub.start();
    EXPECT_TRUE(pubsub.publish(kSteve, text_to({address("topic:dev.scratch")}, "x")).accepted);
    EXPECT_TRUE(pubsub.topic_settings(address("topic:dev.scratch")));
    EXPECT_TRUE(log.contains("auto-created topic:dev.scratch"));
    EXPECT_FALSE(pubsub.publish(kSteve, text_to({address("topic:prod.orders")}, "x")).accepted);
    EXPECT_FALSE(pubsub.topic_settings(address("topic:prod.orders")));
}

// --- Limits and overflow -----------------------------------------------------

TEST(PubSub, SkipAheadGivesTheSlowSubscriberAGapNotice) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    TopicSettings settings;
    settings.ring_capacity = 4;
    settings.memory_cap_bytes = 8 * 1024;
    settings.overflow = OverflowPolicy::SkipAhead;
    pubsub.create_topic(kSteve, address("topic:t"), settings);
    auto slow = pubsub.subscribe(kSteve, address("agent:slow"), address("topic:t"));
    for (int index = 0; index < 200; ++index) {
        ASSERT_TRUE(pubsub.publish(kSteve, text_to({address("topic:t")}, std::string(200, 'x'))).accepted);
    }
    auto delivery = slow->receive(1s);
    ASSERT_TRUE(delivery);
    EXPECT_GT(delivery->missed, 0u);
    EXPECT_GT(slow->missed(), 0u);
}

TEST(PubSub, RejectNewFailsThePublishWhenFull) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    TopicSettings settings;
    settings.memory_cap_bytes = 4 * 1024;
    settings.overflow = OverflowPolicy::RejectNew;
    pubsub.create_topic(kSteve, address("topic:t"), settings);
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"));
    bool refused = false;
    for (int index = 0; index < 100 && !refused; ++index) {
        refused = !pubsub.publish(kSteve, text_to({address("topic:t")}, std::string(200, 'x'))).accepted;
    }
    EXPECT_TRUE(refused);
}

TEST(PubSub, SpillsToDiskAndLogsItThenReadsBack) {
    zetta::test::TempDir directory;
    CapturedLog log;
    auto config = test_config(directory.path());
    config.spill_log_interval = 0ms;
    PubSub pubsub(config, nullptr, log.sink());
    pubsub.start();
    TopicSettings settings;
    settings.ring_capacity = 8;
    settings.memory_cap_bytes = 16 * 1024;
    settings.delivery.guarantee = Guarantee::AtLeastOnce;  // default overflow: spill
    pubsub.create_topic(kSteve, address("topic:big"), settings);
    auto subscription = pubsub.subscribe(kSteve, address("agent:reader"), address("topic:big"));
    for (int index = 0; index < 200; ++index) {
        ASSERT_TRUE(pubsub.publish(kSteve, text_to({address("topic:big")}, std::to_string(index) + std::string(300, '.')))
                        .accepted);
    }
    EXPECT_TRUE(log.contains("WARN topic:big spilling to disk"));
    const auto stats = pubsub.topic_stats();
    const auto big = std::ranges::find_if(stats, [](const TopicStats& entry) { return entry.address == "topic:big"; });
    ASSERT_NE(big, stats.end());
    EXPECT_GT(big->disk_bytes, 0u);
    EXPECT_TRUE(big->spilling_since.has_value());
    pubsub.maintain();
    EXPECT_TRUE(log.contains("still spilling"));
    // Every event is still delivered, in order, spilled or not.
    for (int index = 0; index < 200; ++index) {
        auto delivery = subscription->receive(1s);
        ASSERT_TRUE(delivery) << index;
        EXPECT_TRUE(std::string(delivery->event.payload.bytes()).starts_with(std::to_string(index) + "."));
        subscription->ack(delivery->sequence);
    }
    pubsub.maintain();
    EXPECT_TRUE(log.contains("no longer spilling"));
}

TEST(PubSub, EphemeralTopicsNeverSpill) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    TopicSettings settings;
    settings.lifecycle = TopicLifecycle::Ephemeral;
    settings.ring_capacity = 4;
    settings.memory_cap_bytes = 4 * 1024;
    settings.overflow = OverflowPolicy::SpillToDisk;
    pubsub.create_topic(kSteve, address("topic:e"), settings);
    auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:e"));
    bool refused = false;
    for (int index = 0; index < 100 && !refused; ++index) {
        refused = !pubsub.publish(kSteve, text_to({address("topic:e")}, std::string(200, 'x'))).accepted;
    }
    EXPECT_TRUE(refused);  // spill not allowed: held, then failed
    const auto stats = pubsub.topic_stats();
    for (const auto& entry : stats) {
        if (entry.address == "topic:e") EXPECT_EQ(entry.disk_bytes, 0u);
    }
}

// --- Restart -----------------------------------------------------------------

TEST(PubSub, ShutdownFlushesAndStartReloadsDefaultTopicsNotEphemeral) {
    zetta::test::TempDir directory;
    CapturedLog log;
    {
        PubSub pubsub(test_config(directory.path()), nullptr, log.sink());
        pubsub.start();
        pubsub.create_topic(kSteve, address("topic:keep"), {});
        TopicSettings ephemeral;
        ephemeral.lifecycle = TopicLifecycle::Ephemeral;
        pubsub.create_topic(kSteve, address("topic:scratch"), ephemeral);
        for (int index = 0; index < 5; ++index) {
            pubsub.publish(kSteve, text_to({address("topic:keep"), address("topic:scratch")}, std::to_string(index)));
        }
        pubsub.publish(kSteve, text_to({address("agent:away")}, "waiting for you"));
        pubsub.shutdown();
    }
    EXPECT_TRUE(log.contains("shutdown: flushing"));
    EXPECT_TRUE(log.contains("shutdown: flushed"));

    PubSub restarted(test_config(directory.path()), nullptr, log.sink());
    restarted.start();
    EXPECT_TRUE(log.contains("topic:keep recovered 5 events"));
    ASSERT_TRUE(restarted.topic_settings(address("topic:scratch")));  // the definition survives
    auto keep = restarted.subscribe(kSteve, address("agent:1"), address("topic:keep"), {.start = StartPosition::Oldest});
    for (int index = 0; index < 5; ++index) EXPECT_EQ(text_of(keep->receive(1s)), std::to_string(index));
    auto scratch = restarted.subscribe(kSteve, address("agent:1"), address("topic:scratch"), {.start = StartPosition::Oldest});
    EXPECT_FALSE(scratch->receive(20ms));  // ephemeral events were discarded
    auto inbox = restarted.open_inbox(Identity{address("agent:away"), {}}, address("agent:away"));
    EXPECT_EQ(text_of(inbox->receive(1s)), "waiting for you");
    // Sequences continue after a restart.
    restarted.publish(kSteve, text_to({address("topic:keep")}, "after"));
    auto after = keep->receive(1s);
    ASSERT_TRUE(after);
    EXPECT_GT(after->sequence, 5u);
}

TEST(PubSub, RestartAfterACrashRecoversSpilledRingsBestEffort) {
    zetta::test::TempDir directory;
    {
        PubSub pubsub(test_config(directory.path()));
        pubsub.start();
        TopicSettings settings;
        settings.ring_capacity = 4;
        settings.memory_cap_bytes = 8 * 1024;
        settings.overflow = OverflowPolicy::SpillToDisk;
        pubsub.create_topic(kSteve, address("topic:c"), settings);
        auto holder = pubsub.subscribe(kSteve, address("agent:1"), address("topic:c"));
        for (int index = 0; index < 100; ++index) {
            pubsub.publish(kSteve, text_to({address("topic:c")}, std::to_string(index) + std::string(200, '.')));
        }
        // Simulate a crash: no shutdown flush. (The destructor's zero budget
        // flushes nothing when the budget is already spent.)
        pubsub.shutdown(0ms);
    }
    CapturedLog log;
    PubSub restarted(test_config(directory.path()), nullptr, log.sink());
    restarted.start();
    EXPECT_TRUE(log.contains("topic:c recovered"));
    auto reader = restarted.subscribe(kSteve, address("agent:1"), address("topic:c"), {.start = StartPosition::Oldest});
    auto first = reader->receive(1s);
    ASSERT_TRUE(first);
    EXPECT_TRUE(std::string(first->event.payload.bytes()).starts_with("0."));
}

// --- Retention ---------------------------------------------------------------

TEST(PubSub, SubscriptionsWithoutRetentionAreRemovedOnDisconnect) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    {
        auto subscription = pubsub.subscribe(kSteve, address("service:x"), address("topic:t"));
        EXPECT_EQ(pubsub.topic_stats().front().subscribers, 1u);
    }
    for (const auto& stats : pubsub.topic_stats()) {
        if (stats.address == "topic:t") EXPECT_EQ(stats.subscribers, 0u);
    }
}

TEST(PubSub, AgentSubscriptionsOutliveADisconnectForTheirRetention) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:t"), {});
    SubscribeOptions options;
    options.retention = 50ms;
    {
        auto subscription = pubsub.subscribe(kSteve, address("agent:1"), address("topic:t"), options);
    }
    auto count = [&] {
        for (const auto& stats : pubsub.topic_stats()) {
            if (stats.address == "topic:t") return stats.subscribers;
        }
        return std::size_t{0};
    };
    EXPECT_EQ(count(), 1u);  // kept while disconnected
    std::this_thread::sleep_for(80ms);
    pubsub.maintain();
    EXPECT_EQ(count(), 0u);
}

TEST(PubSub, ConcurrentPublishersAndSubscribersLoseNothing) {
    zetta::test::TempDir directory;
    PubSub pubsub(test_config(directory.path()));
    pubsub.start();
    pubsub.create_topic(kSteve, address("topic:busy"), {});
    constexpr int kPublishers = 4;
    constexpr int kEach = 500;
    std::vector<std::unique_ptr<Subscription>> subscriptions;
    for (int index = 0; index < 3; ++index) {
        subscriptions.push_back(pubsub.subscribe(kSteve, address("agent:" + std::to_string(index)), address("topic:busy")));
    }
    std::vector<std::thread> threads;
    std::vector<int> counts(subscriptions.size(), 0);
    for (std::size_t index = 0; index < subscriptions.size(); ++index) {
        threads.emplace_back([&, index] {
            while (counts[index] < kPublishers * kEach) {
                if (!subscriptions[index]->receive(2s)) break;
                ++counts[index];
            }
        });
    }
    for (int publisher = 0; publisher < kPublishers; ++publisher) {
        threads.emplace_back([&, publisher] {
            for (int index = 0; index < kEach; ++index) {
                pubsub.publish(kSteve, text_to({address("topic:busy")}, std::to_string(publisher)));
            }
        });
    }
    for (auto& thread : threads) thread.join();
    for (const auto count : counts) EXPECT_EQ(count, kPublishers * kEach);
}

}  // namespace
}  // namespace zetta::pubsub
