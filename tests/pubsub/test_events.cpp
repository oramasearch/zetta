// SPDX-License-Identifier: MIT

// Pub/sub addresses, envelope and events (#1025).
#include <gtest/gtest.h>

#include "zetta/pubsub/address.hpp"
#include "zetta/pubsub/event.hpp"

namespace zetta::pubsub {
namespace {

Address address(std::string_view text) { return *parse_address(text); }

TEST(PubSubAddress, ParsesKindAndName) {
    auto parsed = parse_address("topic:orders");
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->kind, "topic");
    EXPECT_EQ(parsed->name, "orders");
    EXPECT_EQ(parsed->to_string(), "topic:orders");
    EXPECT_FALSE(parsed->is_wildcard());
    EXPECT_EQ(parse_address("table:db.logs")->name, "db.logs");
}

TEST(PubSubAddress, RejectsMalformedWithAReason) {
    std::string reason;
    EXPECT_FALSE(parse_address("orders", &reason));
    EXPECT_NE(reason.find("no ':'"), std::string::npos);
    EXPECT_FALSE(parse_address("Topic:orders", &reason));
    EXPECT_FALSE(parse_address("topic:", &reason));
    EXPECT_FALSE(parse_address("topic:a b", &reason));
    EXPECT_FALSE(parse_address("topic:a:b", &reason));
    EXPECT_FALSE(parse_address("agent:*security", &reason));
    EXPECT_NE(reason.find("last character"), std::string::npos);
}

TEST(PubSubAddress, WildcardsArePrefixGlobsWithinTheName) {
    const auto all_agents = address("agent:*");
    const auto security = address("agent:security-*");
    EXPECT_TRUE(all_agents.is_wildcard());
    EXPECT_TRUE(address_matches(all_agents, address("agent:405")));
    EXPECT_TRUE(address_matches(security, address("agent:security-scan")));
    EXPECT_FALSE(address_matches(security, address("agent:billing")));
    EXPECT_FALSE(address_matches(all_agents, address("topic:405")));
    EXPECT_TRUE(address_matches(address("agent:405"), address("agent:405")));
    EXPECT_FALSE(address_matches(address("agent:405"), address("agent:4050")));
}

TEST(PubSubEvent, SmallPayloadsInlineLargeOnesShared) {
    const auto small = Payload::make("hello", 256);
    EXPECT_TRUE(small.is_inline());
    EXPECT_EQ(small.bytes(), "hello");
    const auto large = Payload::make(std::string(1000, 'x'), 256);
    EXPECT_FALSE(large.is_inline());
    const auto copy = large;  // a heap payload is shared, not copied
    EXPECT_EQ(copy.bytes().data(), large.bytes().data());
}

TEST(PubSubEvent, MimeTypesAreInterned) {
    MimeRegistry registry;
    const auto png = registry.intern("image/png");
    EXPECT_NE(png, 0);
    EXPECT_EQ(registry.intern("image/png"), png);
    EXPECT_NE(registry.intern("application/pdf"), png);
    EXPECT_EQ(registry.lookup(png), "image/png");
    EXPECT_EQ(registry.intern(""), 0);
    EXPECT_EQ(registry.lookup(0), "");
}

TEST(PubSubEvent, BccIsStrippedExceptForTheBccRecipient) {
    Event event;
    event.envelope.to = {address("agent:1")};
    event.envelope.bcc = {address("agent:2"), address("agent:3")};
    EXPECT_TRUE(copy_for_recipient(event, address("agent:1")).envelope.bcc.empty());
    const auto for_two = copy_for_recipient(event, address("agent:2"));
    ASSERT_EQ(for_two.envelope.bcc.size(), 1u);
    EXPECT_EQ(for_two.envelope.bcc[0], address("agent:2"));
}

TEST(PubSubEvent, SenderChainLimitIsTenThousand) {
    Envelope envelope;
    envelope.sender.assign(kMaxSenderChainDepth, address("agent:a"));
    EXPECT_EQ(check_sender_chain(envelope), EnvelopeProblem::None);
    envelope.sender.push_back(address("agent:b"));
    EXPECT_EQ(check_sender_chain(envelope), EnvelopeProblem::SenderChainTooDeep);
    // A lower configured limit applies; a higher one is capped at 10,000.
    envelope.sender.resize(5);
    EXPECT_EQ(check_sender_chain(envelope, 4), EnvelopeProblem::SenderChainTooDeep);
    envelope.sender.assign(kMaxSenderChainDepth + 1, address("agent:a"));
    EXPECT_EQ(check_sender_chain(envelope, 50'000), EnvelopeProblem::SenderChainTooDeep);
}

TEST(PubSubEvent, TopicsChainStopsLoopsAndDepth) {
    Envelope envelope;
    envelope.topics_chain = {address("topic:a"), address("topic:b")};
    EXPECT_EQ(check_topic_hop(envelope, address("topic:a")), EnvelopeProblem::TopicLoop);
    EXPECT_EQ(check_topic_hop(envelope, address("topic:c")), EnvelopeProblem::None);
    EXPECT_EQ(check_topic_hop(envelope, address("topic:c"), 2),
              EnvelopeProblem::TopicsChainTooDeep);
}

TEST(PubSubEvent, FrameRoundTripsEveryField) {
    Event event;
    event.sequence = 42;
    event.id = new_event_id();
    event.encoding = Encoding::Json;
    event.mime = 7;
    event.priority = 200;
    event.envelope.from = address("user:riley");
    event.envelope.sender = {address("agent:ask-orama"), address("agent:security-scan")};
    event.envelope.to = {address("topic:orders")};
    event.envelope.cc = {address("agent:405")};
    event.envelope.bcc = {address("agent:audit")};
    event.envelope.reply_to = {address("agent:ask-orama")};
    event.envelope.correlation_id = "req-1";
    event.envelope.topics_chain = {address("topic:inbound")};
    event.payload = Payload::make(R"({"a":1})", 256);
    event.headers = {{"reason", "no match"}};
    event.published_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'600'000'000'000));
    event.revision = 3;
    event.edited_at = event.published_at + std::chrono::seconds(1);
    event.edited_by = "user:editor";
    event.delivery.recipients = Recipients{Recipients::Mode::Count, 3};
    event.delivery.guarantee = Guarantee::ExactlyOnce;
    event.delivery.reply = ReplyPolicy::ReplyTo;
    event.expires_at = std::chrono::system_clock::time_point(std::chrono::milliseconds(1'700'000'000'000));

    const auto frame = encode_event_frame(event, 7);
    const auto decoded = decode_event_frame(frame);
    ASSERT_EQ(decoded.status, FrameStatus::Ok);
    EXPECT_EQ(decoded.consumed, frame.size());
    const auto& round = decoded.event;
    EXPECT_EQ(round.sequence, 42u);
    EXPECT_EQ(round.id, event.id);
    EXPECT_EQ(round.encoding, Encoding::Json);
    EXPECT_EQ(round.mime, 7);
    EXPECT_EQ(round.priority, 200);
    EXPECT_EQ(round.envelope.from, event.envelope.from);
    EXPECT_EQ(round.envelope.sender, event.envelope.sender);
    EXPECT_EQ(round.envelope.to, event.envelope.to);
    EXPECT_EQ(round.envelope.cc, event.envelope.cc);
    EXPECT_EQ(round.envelope.bcc, event.envelope.bcc);
    EXPECT_EQ(round.envelope.reply_to, event.envelope.reply_to);
    EXPECT_EQ(round.envelope.correlation_id, "req-1");
    EXPECT_EQ(round.envelope.topics_chain, event.envelope.topics_chain);
    EXPECT_EQ(round.payload.bytes(), R"({"a":1})");
    EXPECT_EQ(round.expires_at, event.expires_at);
    EXPECT_EQ(round.headers, event.headers);
    EXPECT_EQ(round.published_at, event.published_at);
    EXPECT_EQ(round.revision, event.revision);
    EXPECT_EQ(round.edited_at, event.edited_at);
    EXPECT_EQ(round.edited_by, event.edited_by);
    EXPECT_EQ(decoded.slot, 7u);
    EXPECT_EQ(round.delivery.recipients, event.delivery.recipients);
    EXPECT_EQ(round.delivery.guarantee, event.delivery.guarantee);
    EXPECT_EQ(round.delivery.reply, event.delivery.reply);
}

TEST(PubSubEvent, TornAndCorruptFramesAreDetected) {
    Event event;
    event.payload = Payload::make("payload", 256);
    auto frame = encode_event_frame(event);
    EXPECT_EQ(decode_event_frame(std::string_view(frame).substr(0, frame.size() - 2)).status,
              FrameStatus::Incomplete);
    EXPECT_EQ(decode_event_frame(std::string_view(frame).substr(0, 3)).status,
              FrameStatus::Incomplete);
    frame[frame.size() - 1] ^= 0x5A;
    EXPECT_EQ(decode_event_frame(frame).status, FrameStatus::Corrupt);
}

}  // namespace
}  // namespace zetta::pubsub
