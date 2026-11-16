// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include "zetta/pubsub/address.hpp"
#include "zetta/pubsub/delivery.hpp"

// Pub/sub events and their envelope (#1025; decisions D4, D26, D27, D29, D33,
// D34 in docs/plans/pubsub/decisions.md). See docs/pubsub/events.md.
namespace zetta::pubsub {

// What the payload bytes are. Extensible: new encodings append.
enum class Encoding : std::uint8_t { Blob, Json, Text };

std::string_view to_string(Encoding encoding);
std::optional<Encoding> encoding_from_string(std::string_view text);

// Interns MIME types to a small id so an event header stays small (Q15). Id 0
// means "no MIME type". Thread-safe; ids are stable for the registry's life.
class MimeRegistry {
public:
    std::uint16_t intern(std::string_view mime_type);
    // The MIME type for `id`, or "" for 0 / an unknown id.
    std::string lookup(std::uint16_t id) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint16_t> ids_;
    std::vector<std::string> names_{""};
};

// Payload bytes. Small payloads are held inline (owned by each copy); larger
// ones live once on the message heap as an immutable, reference-counted buffer
// so fan-out and queue hops never copy them (D4, Q14).
class Payload {
public:
    Payload() = default;
    // Inline when bytes.size() <= inline_threshold, otherwise on the heap.
    static Payload make(std::string bytes, std::size_t inline_threshold);

    std::string_view bytes() const;
    std::size_t size() const { return bytes().size(); }
    bool is_inline() const { return std::holds_alternative<std::string>(storage_); }

private:
    std::variant<std::string, std::shared_ptr<const std::string>> storage_;
};

// Hard limits that are not settings.
inline constexpr std::size_t kMaxSenderChainDepth = 10'000;  // D34
inline constexpr std::size_t kDefaultTopicsChainDepth = 32;  // D29 (proposal)
inline constexpr std::size_t kDefaultInlineThreshold = 256;  // Q14 (proposal)

// The email-style envelope (D27, D29, D33, D34).
struct Envelope {
    std::optional<Address> from;           // the principal the event is for
    std::vector<Address> sender;           // delegation stack, oldest first
    std::vector<Address> to;
    std::vector<Address> cc;
    std::vector<Address> bcc;
    std::vector<Address> reply_to;
    std::string correlation_id;
    std::vector<Address> topics_chain;     // topics passed through, newest last

    // Every recipient address, in to, cc, bcc order.
    std::vector<Address> recipients() const;
};

struct Event {
    std::uint64_t sequence = 0;  // assigned by the topic on append
    std::uint64_t revision = 1;
    std::optional<std::chrono::system_clock::time_point> edited_at;
    std::string edited_by;
    std::string id;              // unique per publish; exactly-once dedupe key
    Envelope envelope;
    Encoding encoding = Encoding::Blob;
    std::uint16_t mime = 0;      // MimeRegistry id; 0 = none
    std::uint8_t priority = 0;   // 0 lowest .. 255 highest
    Payload payload;
    std::optional<std::chrono::system_clock::time_point> expires_at;  // time to live
    std::chrono::system_clock::time_point published_at{};  // priority aging, observability
    // The message's own delivery layer (D6): overrides the subscription and topic.
    DeliveryOptions delivery;
    // Small text annotations the engine adds: a dead letter's `reason`, an
    // error event's `error`, a gap notice's `missed` count.
    std::map<std::string, std::string> headers;

    // Bytes this event holds in memory (payload + envelope estimate). Used
    // against the memory limits (D10).
    std::size_t memory_bytes() const;
};

// The copy of `event` a given recipient receives: every other recipient's
// bcc address is removed (D27). `recipient` must be one of the event's
// addresses (or a topic subscriber, which sees no bcc at all).
Event copy_for_recipient(const Event& event, const Address& recipient);

// Envelope checks that stop an event before delivery.
enum class EnvelopeProblem {
    None,
    SenderChainTooDeep,   // D34: > kMaxSenderChainDepth (or a lower configured limit)
    TopicLoop,            // D29: the target topic is already on the topics-chain
    TopicsChainTooDeep,   // D29: topics-chain at its maximum depth
};

std::string_view to_string(EnvelopeProblem problem);

EnvelopeProblem check_sender_chain(const Envelope& envelope,
                                   std::size_t max_depth = kMaxSenderChainDepth);

// May `event` be delivered into `topic`? Checks for a loop and the depth limit.
EnvelopeProblem check_topic_hop(const Envelope& envelope, const Address& topic,
                                std::size_t max_depth = kDefaultTopicsChainDepth);

// A fresh unique event id (UUID).
std::string new_event_id();

// --- On-disk framing (spill + shutdown flush, #1030/#1031) ------------------
// One framed record: [u32 length][u32 crc32][body]. The body carries the
// envelope as JSON plus the raw payload. A torn or corrupt tail is detected by
// the length / crc and reported, never parsed.
std::string encode_event_frame(const Event& event, std::optional<std::size_t> slot = std::nullopt);

enum class FrameStatus { Ok, Incomplete, Corrupt };

struct DecodedFrame {
    FrameStatus status = FrameStatus::Corrupt;
    Event event;
    std::size_t consumed = 0;  // bytes of `buffer` this frame occupied
    std::optional<std::size_t> slot;  // sparse spill position; absent in older frames
};

DecodedFrame decode_event_frame(std::string_view buffer,
                                std::size_t inline_threshold = kDefaultInlineThreshold);

}  // namespace zetta::pubsub
