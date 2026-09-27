#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Delivery options and their resolution (#1028, #1029; decisions D3, D6, D7,
// D8, D9 in docs/plans/pubsub/decisions.md). See docs/pubsub/delivery.md.
//
// The same options can be set on a topic (its default), a subscription, and a
// single publish. Resolution layers them most specific first (message, then
// subscription, then topic, then the built-in default), and the result is then
// bounded by governance: over its bounds a request is clamped (the default) or
// rejected, per topic.
namespace zetta::pubsub {

// Who receives an event.
struct Recipients {
    enum class Mode : std::uint8_t {
        All,    // every subscriber (broadcast)
        One,    // exactly one subscriber claims it (a work queue)
        Count,  // up to `count` subscribers claim it
    };
    Mode mode = Mode::All;
    std::uint32_t count = 1;  // for Count

    // How many claims an event allows; 0 = unlimited (All).
    std::uint32_t claims() const;
    bool operator==(const Recipients&) const = default;
};

enum class Ordering : std::uint8_t { Fifo, Priority };

enum class Guarantee : std::uint8_t { AtMostOnce, AtLeastOnce, ExactlyOnce };

// Where a drain's result goes (D36): nowhere, the event's reply-to list, or
// email-style reply-all (reply-to or from, plus every to and cc; never bcc).
enum class ReplyPolicy : std::uint8_t { None, ReplyTo, ReplyAll };

std::string_view to_string(Recipients::Mode mode);
std::string_view to_string(Ordering ordering);
std::string_view to_string(Guarantee guarantee);
std::optional<Guarantee> guarantee_from_string(std::string_view text);
std::string_view to_string(ReplyPolicy policy);

// One layer: every field optional; an unset field defers to the next layer.
struct DeliveryOptions {
    std::optional<Recipients> recipients;
    std::optional<Ordering> ordering;
    std::optional<std::uint8_t> priority;
    std::optional<Guarantee> guarantee;
    std::optional<std::chrono::milliseconds> ack_deadline;  // at-least-once redelivery
    std::optional<ReplyPolicy> reply;                        // where a drain's result goes
};

// Every field set: what actually applies.
struct EffectiveDelivery {
    Recipients recipients;
    Ordering ordering = Ordering::Fifo;
    std::uint8_t priority = 0;
    Guarantee guarantee = Guarantee::AtMostOnce;
    std::chrono::milliseconds ack_deadline{30'000};

    bool operator==(const EffectiveDelivery&) const = default;
};

// Layer message over subscription over topic over the built-in defaults.
EffectiveDelivery resolve_delivery(const DeliveryOptions& message,
                                   const DeliveryOptions& subscription,
                                   const DeliveryOptions& topic);

// What governance allows a principal to ask for on one target (D7). Empty
// optionals mean "no bound".
struct DeliveryBounds {
    std::optional<std::uint8_t> max_priority;
    // Allowed recipient modes; empty = any. A Count above `max_recipients` is over bound.
    std::vector<Recipients::Mode> allowed_recipient_modes;
    std::optional<std::uint32_t> max_recipients;
    std::optional<Guarantee> max_guarantee;  // AtMostOnce < AtLeastOnce < ExactlyOnce
};

enum class OverBoundPolicy : std::uint8_t { Clamp, Reject };  // D8; Clamp is the default

struct BoundedDelivery {
    bool rejected = false;
    EffectiveDelivery effective;       // what applies (clamped when needed)
    std::vector<std::string> clamped;  // which fields were lowered, for the log + audit
    std::string reason;                // why it was rejected, naming the rule
};

// Apply `bounds` to `requested` under `policy`. Clamp lowers each over-bound
// field to its bound and records it; Reject refuses on the first one.
BoundedDelivery apply_bounds(const EffectiveDelivery& requested, const DeliveryBounds& bounds,
                             OverBoundPolicy policy);

}  // namespace zetta::pubsub
