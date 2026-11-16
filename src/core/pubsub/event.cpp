// SPDX-License-Identifier: MIT

#include "zetta/pubsub/event.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include <nlohmann/json.hpp>

#include "zetta/util/uuid.hpp"

namespace zetta::pubsub {
namespace {

// CRC-32 (IEEE 802.3, reflected, polynomial 0xEDB88320) for the frame check.
std::uint32_t crc32(std::string_view bytes) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> entries{};
        for (std::uint32_t index = 0; index < 256; ++index) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1U) ? (0xEDB88320U ^ (value >> 1)) : (value >> 1);
            }
            entries[index] = value;
        }
        return entries;
    }();
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const char character : bytes) {
        crc = table[(crc ^ static_cast<std::uint8_t>(character)) & 0xFFU] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFU;
}

void append_u32(std::string& out, std::uint32_t value) {
    for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((value >> shift) & 0xFFU));
    }
}

std::uint32_t read_u32(std::string_view bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (int index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(static_cast<std::uint8_t>(bytes[offset + index]))
                 << (8 * index);
    }
    return value;
}

nlohmann::json addresses_to_json(const std::vector<Address>& addresses) {
    nlohmann::json list = nlohmann::json::array();
    for (const auto& address : addresses) list.push_back(address.to_string());
    return list;
}

bool addresses_from_json(const nlohmann::json& value, std::vector<Address>& out) {
    if (value.is_null()) return true;
    if (!value.is_array()) return false;
    for (const auto& item : value) {
        if (!item.is_string()) return false;
        auto parsed = parse_address(item.get<std::string>());
        if (!parsed) return false;
        out.push_back(std::move(*parsed));
    }
    return true;
}

std::vector<Address> without(const std::vector<Address>& addresses,
                             const Address& keep_only_if_equal) {
    std::vector<Address> kept;
    for (const auto& address : addresses) {
        if (address == keep_only_if_equal) kept.push_back(address);
    }
    return kept;
}

}  // namespace

std::string_view to_string(Encoding encoding) {
    switch (encoding) {
        case Encoding::Blob: return "blob";
        case Encoding::Json: return "json";
        case Encoding::Text: return "text";
    }
    return "blob";
}

std::optional<Encoding> encoding_from_string(std::string_view text) {
    if (text == "blob") return Encoding::Blob;
    if (text == "json") return Encoding::Json;
    if (text == "text") return Encoding::Text;
    return std::nullopt;
}

std::uint16_t MimeRegistry::intern(std::string_view mime_type) {
    if (mime_type.empty()) return 0;
    std::lock_guard lock(mutex_);
    const std::string key(mime_type);
    if (auto found = ids_.find(key); found != ids_.end()) return found->second;
    if (names_.size() >= 0xFFFFU) return 0;  // registry full: no id, never a wrong one
    const auto id = static_cast<std::uint16_t>(names_.size());
    names_.push_back(key);
    ids_.emplace(key, id);
    return id;
}

std::string MimeRegistry::lookup(std::uint16_t id) const {
    std::lock_guard lock(mutex_);
    return id < names_.size() ? names_[id] : std::string();
}

Payload Payload::make(std::string bytes, std::size_t inline_threshold) {
    Payload payload;
    if (bytes.size() <= inline_threshold) {
        payload.storage_ = std::move(bytes);
    } else {
        payload.storage_ = std::make_shared<const std::string>(std::move(bytes));
    }
    return payload;
}

std::string_view Payload::bytes() const {
    if (const auto* inline_bytes = std::get_if<std::string>(&storage_)) return *inline_bytes;
    const auto& heap = std::get<std::shared_ptr<const std::string>>(storage_);
    return heap ? std::string_view(*heap) : std::string_view();
}

std::vector<Address> Envelope::recipients() const {
    std::vector<Address> all;
    all.reserve(to.size() + cc.size() + bcc.size());
    all.insert(all.end(), to.begin(), to.end());
    all.insert(all.end(), cc.begin(), cc.end());
    all.insert(all.end(), bcc.begin(), bcc.end());
    return all;
}

std::size_t Event::memory_bytes() const {
    std::size_t envelope_bytes = envelope.correlation_id.size();
    auto count = [&](const std::vector<Address>& list) {
        for (const auto& address : list) envelope_bytes += address.kind.size() + address.name.size() + 2;
    };
    count(envelope.sender);
    count(envelope.to);
    count(envelope.cc);
    count(envelope.bcc);
    count(envelope.reply_to);
    count(envelope.topics_chain);
    for (const auto& [key, value] : headers) envelope_bytes += key.size() + value.size();
    return sizeof(Event) + id.size() + edited_by.size() + envelope_bytes + payload.size();
}

Event copy_for_recipient(const Event& event, const Address& recipient) {
    Event copy = event;
    copy.envelope.bcc = without(event.envelope.bcc, recipient);
    return copy;
}

std::string_view to_string(EnvelopeProblem problem) {
    switch (problem) {
        case EnvelopeProblem::None: return "none";
        case EnvelopeProblem::SenderChainTooDeep: return "sender chain too deep";
        case EnvelopeProblem::TopicLoop: return "topic loop";
        case EnvelopeProblem::TopicsChainTooDeep: return "topics-chain too deep";
    }
    return "none";
}

EnvelopeProblem check_sender_chain(const Envelope& envelope, std::size_t max_depth) {
    const std::size_t limit = std::min(max_depth, kMaxSenderChainDepth);
    return envelope.sender.size() > limit ? EnvelopeProblem::SenderChainTooDeep
                                          : EnvelopeProblem::None;
}

EnvelopeProblem check_topic_hop(const Envelope& envelope, const Address& topic,
                                std::size_t max_depth) {
    if (std::ranges::find(envelope.topics_chain, topic) != envelope.topics_chain.end()) {
        return EnvelopeProblem::TopicLoop;
    }
    if (envelope.topics_chain.size() >= max_depth) return EnvelopeProblem::TopicsChainTooDeep;
    return EnvelopeProblem::None;
}

std::string new_event_id() { return zetta::util::new_uuid(); }

std::string encode_event_frame(const Event& event, std::optional<std::size_t> slot) {
    const auto& envelope = event.envelope;
    nlohmann::json header = {
        {"sequence", event.sequence},
        {"id", event.id},
        {"encoding", to_string(event.encoding)},
        {"mime", event.mime},
        {"priority", event.priority},
        {"correlation_id", envelope.correlation_id},
        {"sender", addresses_to_json(envelope.sender)},
        {"to", addresses_to_json(envelope.to)},
        {"cc", addresses_to_json(envelope.cc)},
        {"bcc", addresses_to_json(envelope.bcc)},
        {"reply_to", addresses_to_json(envelope.reply_to)},
        {"topics_chain", addresses_to_json(envelope.topics_chain)},
    };
    header["revision"] = event.revision;
    if (slot) header["slot"] = *slot;
    if (event.edited_at) {
        header["edited_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
            event.edited_at->time_since_epoch()).count();
        header["edited_by"] = event.edited_by;
    }
    if (envelope.from) header["from"] = envelope.from->to_string();
    if (!event.headers.empty()) header["headers"] = event.headers;
    header["published_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                    event.published_at.time_since_epoch())
                                    .count();
    const auto& delivery = event.delivery;
    nlohmann::json delivery_json = nlohmann::json::object();
    if (delivery.recipients) {
        delivery_json["recipients"] = to_string(delivery.recipients->mode);
        delivery_json["count"] = delivery.recipients->count;
    }
    if (delivery.ordering) delivery_json["ordering"] = to_string(*delivery.ordering);
    if (delivery.priority) delivery_json["priority"] = *delivery.priority;
    if (delivery.guarantee) delivery_json["guarantee"] = to_string(*delivery.guarantee);
    if (delivery.ack_deadline) delivery_json["ack_deadline_ms"] = delivery.ack_deadline->count();
    if (delivery.reply) delivery_json["reply"] = to_string(*delivery.reply);
    if (!delivery_json.empty()) header["delivery"] = delivery_json;
    if (event.expires_at) {
        header["expires_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
                                      event.expires_at->time_since_epoch())
                                      .count();
    }
    const std::string header_text = header.dump();
    std::string body;
    append_u32(body, static_cast<std::uint32_t>(header_text.size()));
    body += header_text;
    body += event.payload.bytes();

    std::string frame;
    append_u32(frame, static_cast<std::uint32_t>(body.size()));
    append_u32(frame, crc32(body));
    frame += body;
    return frame;
}

DecodedFrame decode_event_frame(std::string_view buffer, std::size_t inline_threshold) {
    DecodedFrame decoded;
    if (buffer.size() < 8) {
        decoded.status = FrameStatus::Incomplete;
        return decoded;
    }
    const std::uint32_t length = read_u32(buffer, 0);
    const std::uint32_t checksum = read_u32(buffer, 4);
    if (buffer.size() < 8 + static_cast<std::size_t>(length)) {
        decoded.status = FrameStatus::Incomplete;
        return decoded;
    }
    const std::string_view body = buffer.substr(8, length);
    if (crc32(body) != checksum || body.size() < 4) return decoded;  // Corrupt
    const std::uint32_t header_length = read_u32(body, 0);
    if (4 + static_cast<std::size_t>(header_length) > body.size()) return decoded;

    const auto header = nlohmann::json::parse(body.substr(4, header_length), nullptr,
                                              /*allow_exceptions=*/false);
    if (!header.is_object()) return decoded;

    Event event;
    try {
        event.sequence = header.value("sequence", std::uint64_t{0});
        event.revision = header.value("revision", std::uint64_t{1});
        if (header.contains("slot")) decoded.slot = header.at("slot").get<std::size_t>();
        if (header.contains("edited_at_ms")) {
            event.edited_at = std::chrono::system_clock::time_point(
                std::chrono::milliseconds(header.at("edited_at_ms").get<std::int64_t>()));
            event.edited_by = header.at("edited_by").get<std::string>();
        }
        event.id = header.value("id", std::string());
        event.encoding =
            encoding_from_string(header.value("encoding", std::string("blob"))).value_or(Encoding::Blob);
        event.mime = header.value("mime", std::uint16_t{0});
        event.priority = header.value("priority", std::uint8_t{0});
        event.envelope.correlation_id = header.value("correlation_id", std::string());
    } catch (const nlohmann::json::exception&) {
        return decoded;  // a mistyped field is corruption, never a guess
    }
    auto& envelope = event.envelope;
    if (!addresses_from_json(header.value("sender", nlohmann::json()), envelope.sender) ||
        !addresses_from_json(header.value("to", nlohmann::json()), envelope.to) ||
        !addresses_from_json(header.value("cc", nlohmann::json()), envelope.cc) ||
        !addresses_from_json(header.value("bcc", nlohmann::json()), envelope.bcc) ||
        !addresses_from_json(header.value("reply_to", nlohmann::json()), envelope.reply_to) ||
        !addresses_from_json(header.value("topics_chain", nlohmann::json()), envelope.topics_chain)) {
        return decoded;
    }
    if (header.contains("from")) {
        const auto& from = header["from"];
        if (!from.is_string()) return decoded;
        envelope.from = parse_address(from.get<std::string>());
        if (!envelope.from) return decoded;
    }
    if (header.contains("headers")) {
        const auto& headers = header["headers"];
        if (!headers.is_object()) return decoded;
        for (const auto& [key, value] : headers.items()) {
            if (!value.is_string()) return decoded;
            event.headers.emplace(key, value.get<std::string>());
        }
    }
    if (header.contains("published_at_ms") && header["published_at_ms"].is_number_integer()) {
        event.published_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(header["published_at_ms"].get<long long>()));
    }
    if (header.contains("delivery")) {
        const auto& delivery = header["delivery"];
        if (!delivery.is_object()) return decoded;
        auto& options = event.delivery;
        if (delivery.contains("recipients") && delivery["recipients"].is_string()) {
            const auto mode = delivery["recipients"].get<std::string>();
            Recipients recipients;
            recipients.mode = mode == "one"     ? Recipients::Mode::One
                              : mode == "count" ? Recipients::Mode::Count
                                                : Recipients::Mode::All;
            if (delivery.contains("count") && delivery["count"].is_number_unsigned()) {
                recipients.count = delivery["count"].get<std::uint32_t>();
            }
            options.recipients = recipients;
        }
        if (delivery.contains("ordering") && delivery["ordering"].is_string()) {
            options.ordering = delivery["ordering"].get<std::string>() == "priority" ? Ordering::Priority
                                                                                     : Ordering::Fifo;
        }
        if (delivery.contains("priority") && delivery["priority"].is_number_unsigned()) {
            options.priority = delivery["priority"].get<std::uint8_t>();
        }
        if (delivery.contains("guarantee") && delivery["guarantee"].is_string()) {
            options.guarantee = guarantee_from_string(delivery["guarantee"].get<std::string>());
        }
        if (delivery.contains("ack_deadline_ms") && delivery["ack_deadline_ms"].is_number_integer()) {
            options.ack_deadline = std::chrono::milliseconds(delivery["ack_deadline_ms"].get<long long>());
        }
        if (delivery.contains("reply") && delivery["reply"].is_string()) {
            const auto reply = delivery["reply"].get<std::string>();
            options.reply = reply == "reply_to"    ? ReplyPolicy::ReplyTo
                            : reply == "reply_all" ? ReplyPolicy::ReplyAll
                                                   : ReplyPolicy::None;
        }
    }
    if (header.contains("expires_at_ms") && header["expires_at_ms"].is_number_integer()) {
        event.expires_at = std::chrono::system_clock::time_point(
            std::chrono::milliseconds(header["expires_at_ms"].get<long long>()));
    }
    event.payload = Payload::make(std::string(body.substr(4 + header_length)), inline_threshold);

    decoded.status = FrameStatus::Ok;
    decoded.event = std::move(event);
    decoded.consumed = 8 + length;
    return decoded;
}

}  // namespace zetta::pubsub
