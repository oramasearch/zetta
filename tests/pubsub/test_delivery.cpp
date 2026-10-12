// Delivery option layering and governance bounds (#1028, #1029).
#include <gtest/gtest.h>

#include "zetta/pubsub/delivery.hpp"

namespace zetta::pubsub {
namespace {

TEST(PubSubDelivery, MostSpecificLayerWins) {
    DeliveryOptions topic;
    topic.priority = 1;
    topic.guarantee = Guarantee::AtLeastOnce;
    topic.recipients = Recipients{Recipients::Mode::All, 1};
    DeliveryOptions subscription;
    subscription.priority = 2;
    DeliveryOptions message;
    message.priority = 3;

    auto effective = resolve_delivery(message, subscription, topic);
    EXPECT_EQ(effective.priority, 3);
    EXPECT_EQ(effective.guarantee, Guarantee::AtLeastOnce);  // only the topic set it
    effective = resolve_delivery({}, subscription, topic);
    EXPECT_EQ(effective.priority, 2);
    effective = resolve_delivery({}, {}, {});
    EXPECT_EQ(effective, EffectiveDelivery{});  // the built-in defaults
}

TEST(PubSubDelivery, ClampLowersEveryOverBoundField) {
    EffectiveDelivery requested;
    requested.priority = 9;
    requested.guarantee = Guarantee::ExactlyOnce;
    requested.recipients = {Recipients::Mode::All, 1};
    DeliveryBounds bounds;
    bounds.max_priority = 5;
    bounds.max_guarantee = Guarantee::AtLeastOnce;
    bounds.allowed_recipient_modes = {Recipients::Mode::One};

    const auto bounded = apply_bounds(requested, bounds, OverBoundPolicy::Clamp);
    EXPECT_FALSE(bounded.rejected);
    EXPECT_EQ(bounded.effective.priority, 5);
    EXPECT_EQ(bounded.effective.guarantee, Guarantee::AtLeastOnce);
    EXPECT_EQ(bounded.effective.recipients.mode, Recipients::Mode::One);
    EXPECT_EQ(bounded.clamped.size(), 3u);
}

TEST(PubSubDelivery, RejectNamesTheRule) {
    EffectiveDelivery requested;
    requested.priority = 9;
    DeliveryBounds bounds;
    bounds.max_priority = 5;
    const auto bounded = apply_bounds(requested, bounds, OverBoundPolicy::Reject);
    EXPECT_TRUE(bounded.rejected);
    EXPECT_NE(bounded.reason.find("priority 9 exceeds the bound 5"), std::string::npos);
    EXPECT_EQ(bounded.effective.priority, 9);  // untouched when rejected
}

TEST(PubSubDelivery, CountIsCappedByMaxRecipients) {
    EffectiveDelivery requested;
    requested.recipients = {Recipients::Mode::Count, 10};
    DeliveryBounds bounds;
    bounds.max_recipients = 3;
    const auto bounded = apply_bounds(requested, bounds, OverBoundPolicy::Clamp);
    EXPECT_EQ(bounded.effective.recipients.count, 3u);
    EXPECT_EQ(Recipients({Recipients::Mode::All, 1}).claims(), 0u);
    EXPECT_EQ(Recipients({Recipients::Mode::One, 7}).claims(), 1u);
    EXPECT_EQ(Recipients({Recipients::Mode::Count, 4}).claims(), 4u);
}

TEST(PubSubDelivery, WithinBoundsIsUnchanged) {
    EffectiveDelivery requested;
    requested.priority = 2;
    DeliveryBounds bounds;
    bounds.max_priority = 5;
    const auto bounded = apply_bounds(requested, bounds, OverBoundPolicy::Reject);
    EXPECT_FALSE(bounded.rejected);
    EXPECT_TRUE(bounded.clamped.empty());
    EXPECT_EQ(bounded.effective, requested);
}

}  // namespace
}  // namespace zetta::pubsub
