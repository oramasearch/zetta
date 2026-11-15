// SPDX-License-Identifier: MIT

#include "zetta/pubsub/delivery.hpp"

#include <algorithm>

namespace zetta::pubsub {
namespace {

template <class Value>
Value first_set(const std::optional<Value>& message, const std::optional<Value>& subscription,
                const std::optional<Value>& topic, Value fallback) {
    if (message) return *message;
    if (subscription) return *subscription;
    if (topic) return *topic;
    return fallback;
}

}  // namespace

std::uint32_t Recipients::claims() const {
    switch (mode) {
        case Mode::All: return 0;
        case Mode::One: return 1;
        case Mode::Count: return std::max<std::uint32_t>(count, 1);
    }
    return 0;
}

std::string_view to_string(Recipients::Mode mode) {
    switch (mode) {
        case Recipients::Mode::All: return "all";
        case Recipients::Mode::One: return "one";
        case Recipients::Mode::Count: return "count";
    }
    return "all";
}

std::string_view to_string(Ordering ordering) {
    return ordering == Ordering::Priority ? "priority" : "fifo";
}

std::string_view to_string(Guarantee guarantee) {
    switch (guarantee) {
        case Guarantee::AtMostOnce: return "at_most_once";
        case Guarantee::AtLeastOnce: return "at_least_once";
        case Guarantee::ExactlyOnce: return "exactly_once";
    }
    return "at_most_once";
}

std::optional<Guarantee> guarantee_from_string(std::string_view text) {
    if (text == "at_most_once") return Guarantee::AtMostOnce;
    if (text == "at_least_once") return Guarantee::AtLeastOnce;
    if (text == "exactly_once") return Guarantee::ExactlyOnce;
    return std::nullopt;
}

std::string_view to_string(ReplyPolicy policy) {
    switch (policy) {
        case ReplyPolicy::None: return "none";
        case ReplyPolicy::ReplyTo: return "reply_to";
        case ReplyPolicy::ReplyAll: return "reply_all";
    }
    return "none";
}

EffectiveDelivery resolve_delivery(const DeliveryOptions& message,
                                   const DeliveryOptions& subscription,
                                   const DeliveryOptions& topic) {
    const EffectiveDelivery defaults;
    EffectiveDelivery effective;
    effective.recipients =
        first_set(message.recipients, subscription.recipients, topic.recipients, defaults.recipients);
    effective.ordering =
        first_set(message.ordering, subscription.ordering, topic.ordering, defaults.ordering);
    effective.priority =
        first_set(message.priority, subscription.priority, topic.priority, defaults.priority);
    effective.guarantee =
        first_set(message.guarantee, subscription.guarantee, topic.guarantee, defaults.guarantee);
    effective.ack_deadline = first_set(message.ack_deadline, subscription.ack_deadline,
                                       topic.ack_deadline, defaults.ack_deadline);
    return effective;
}

BoundedDelivery apply_bounds(const EffectiveDelivery& requested, const DeliveryBounds& bounds,
                             OverBoundPolicy policy) {
    BoundedDelivery result;
    result.effective = requested;
    auto over = [&](std::string field, std::string rule) {
        if (policy == OverBoundPolicy::Reject) {
            if (!result.rejected) {
                result.rejected = true;
                result.reason = std::move(rule);
            }
            return false;
        }
        result.clamped.push_back(std::move(field));
        return true;
    };

    if (bounds.max_priority && requested.priority > *bounds.max_priority) {
        if (over("priority", "priority " + std::to_string(requested.priority) +
                                 " exceeds the bound " + std::to_string(*bounds.max_priority))) {
            result.effective.priority = *bounds.max_priority;
        }
    }
    if (!bounds.allowed_recipient_modes.empty() &&
        std::ranges::find(bounds.allowed_recipient_modes, requested.recipients.mode) ==
            bounds.allowed_recipient_modes.end()) {
        if (over("recipients", "recipient mode '" + std::string(to_string(requested.recipients.mode)) +
                                   "' is not allowed")) {
            // Clamp to the narrowest allowed mode: One, then Count, then All.
            const auto& allowed = bounds.allowed_recipient_modes;
            auto has = [&](Recipients::Mode mode) {
                return std::ranges::find(allowed, mode) != allowed.end();
            };
            if (has(Recipients::Mode::One)) {
                result.effective.recipients = {Recipients::Mode::One, 1};
            } else if (has(Recipients::Mode::Count)) {
                result.effective.recipients = {Recipients::Mode::Count,
                                               bounds.max_recipients.value_or(1)};
            } else {
                result.effective.recipients = {Recipients::Mode::All, 1};
            }
        }
    }
    if (bounds.max_recipients && result.effective.recipients.mode == Recipients::Mode::Count &&
        result.effective.recipients.count > *bounds.max_recipients) {
        if (over("recipients", "recipient count " + std::to_string(result.effective.recipients.count) +
                                   " exceeds the bound " + std::to_string(*bounds.max_recipients))) {
            result.effective.recipients.count = *bounds.max_recipients;
        }
    }
    if (bounds.max_guarantee && requested.guarantee > *bounds.max_guarantee) {
        if (over("guarantee", "guarantee '" + std::string(to_string(requested.guarantee)) +
                                  "' exceeds the bound '" +
                                  std::string(to_string(*bounds.max_guarantee)) + "'")) {
            result.effective.guarantee = *bounds.max_guarantee;
        }
    }
    if (result.rejected) result.effective = requested;
    return result;
}

}  // namespace zetta::pubsub
