// Pub/sub on the control surface (#1033; decision D22 in
// docs/plans/pubsub/decisions.md; docs/pubsub/exposure.md).
//
// Topics are engine-internal by default. These capabilities exist on every
// node (one registry entry renders as the REST route, the MCP tool and the
// OpenAPI entry), but publishing, reading, dropping and listing only ever see
// topics whose `exposed` setting is on; an unexposed topic answers not_found,
// exactly as if it did not exist. Rights come from the pubsub:* permissions.

#include <algorithm>
#include <chrono>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/pubsub/pubsub.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;
using zetta::pubsub::Address;

constexpr std::size_t kMaxReceive = 1'000;
constexpr long long kMaxWaitMs = 30'000;

zetta::pubsub::Identity identity_of(const ApiContext& context) {
    if (context.acting_key.empty()) return {Address{"service", "local"}, {}};  // the trusted local session
    return {Address{context.acting_is_user ? "user" : "key", context.acting_key}, {}};
}

// "topic:orders" or just "orders".
std::optional<Address> topic_address(const nlohmann::json& input, std::string& reason) {
    const std::string text = input.value("topic", std::string());
    auto address = zetta::pubsub::parse_address(text.find(':') == std::string::npos ? "topic:" + text : text,
                                                &reason);
    if (address && (address->kind != "topic" || address->is_wildcard())) {
        reason = "want a topic, topic:<name> with no wildcard";
        return std::nullopt;
    }
    return address;
}

// The topic when it exists and is exposed; a failure otherwise.
std::optional<ApiResult> require_exposed(zetta::pubsub::PubSub& pubsub, const Address& topic) {
    const auto settings = pubsub.topic_settings(topic);
    if (!settings || !settings->exposed) {
        return ApiResult::failure("not_found", topic.to_string() + " does not exist or is not exposed", "/topic");
    }
    return std::nullopt;
}

zetta::pubsub::PubSub* pubsub_of(ApiContext& context) { return context.node.pubsub(); }

nlohmann::json topic_property() {
    return {{"type", "string"}, {"minLength", 1}, {"description", "topic:<name> or <name>"}};
}

const nlohmann::json& create_topic_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topic"}},
        {"additionalProperties", false},
        {"properties",
         {{"topic", topic_property()},
          {"exposed",
           {{"type", "boolean"},
            {"description", "reachable through this control surface (default false: engine-internal)"}}},
          {"lifecycle",
           {{"type", "string"},
            {"enum", {"default", "ephemeral"}},
            {"description", "default: flushed on shutdown and reloaded; ephemeral: never touches disk"}}},
          {"guarantee", {{"type", "string"}, {"enum", {"at_most_once", "at_least_once", "exactly_once"}}}}}},
    };
    return schema;
}

const nlohmann::json& topic_only_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topic"}},
        {"additionalProperties", false},
        {"properties", {{"topic", topic_property()}}},
    };
    return schema;
}

const nlohmann::json& topic_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topic", "exposed"}},
        {"additionalProperties", false},
        {"properties", {{"topic", {{"type", "string"}}}, {"exposed", {{"type", "boolean"}}}}},
    };
    return schema;
}

const nlohmann::json& empty_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_topics_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topics"}},
        {"additionalProperties", false},
        {"properties",
         {{"topics",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"topic", "subscribers", "held_events", "next_sequence"}},
              {"properties",
               {{"topic", {{"type", "string"}}},
                {"subscribers", {{"type", "integer"}}},
                {"held_events", {{"type", "integer"}}},
                {"next_sequence", {{"type", "integer"}}}}}}}}}}},
    };
    return schema;
}

const nlohmann::json& publish_event_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topic", "payload"}},
        {"additionalProperties", false},
        {"properties",
         {{"topic", topic_property()},
          {"payload", {{"type", "string"}, {"description", "the event's bytes, as text"}}},
          {"encoding", {{"type", "string"}, {"enum", {"text", "json", "blob"}}}},
          {"mime_type", {{"type", "string"}}},
          {"correlation_id", {{"type", "string"}}},
          {"reply_to", {{"type", "array"}, {"items", {{"type", "string"}}}}},
          {"priority", {{"type", "integer"}, {"minimum", 0}, {"maximum", 255}}},
          {"ttl_ms", {{"type", "integer"}, {"minimum", 1}}}}},
    };
    return schema;
}

const nlohmann::json& publish_event_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"event_id", "accepted", "clamped"}},
        {"additionalProperties", false},
        {"properties",
         {{"event_id", {{"type", "string"}}},
          {"accepted", {{"type", "boolean"}}},
          {"clamped", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
    };
    return schema;
}

const nlohmann::json& receive_events_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topic"}},
        {"additionalProperties", false},
        {"properties",
         {{"topic", topic_property()},
          {"from_sequence",
           {{"type", "integer"}, {"minimum", 1}, {"description", "first sequence to return (default 1)"}}},
          {"max", {{"type", "integer"}, {"minimum", 1}, {"maximum", static_cast<int>(kMaxReceive)}}},
          {"wait_ms",
           {{"type", "integer"},
            {"minimum", 0},
            {"maximum", kMaxWaitMs},
            {"description", "long-poll: wait up to this long when nothing is there yet"}}}}},
    };
    return schema;
}

const nlohmann::json& receive_events_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"events", "next_sequence"}},
        {"additionalProperties", false},
        {"properties",
         {{"events",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"sequence", "id", "encoding", "payload"}},
              {"properties",
               {{"sequence", {{"type", "integer"}}},
                {"id", {{"type", "string"}}},
                {"from", {{"type", "string"}}},
                {"correlation_id", {{"type", "string"}}},
                {"encoding", {{"type", "string"}}},
                {"payload", {{"type", "string"}}}}}}}}},
          {"next_sequence", {{"type", "integer"}}}}},
    };
    return schema;
}

ApiResult handle_create_topic(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (pubsub == nullptr) return ApiResult::failure("engine_error", "pub/sub is disabled on this node");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    zetta::pubsub::TopicSettings settings;
    settings.exposed = input.value("exposed", false);
    if (input.value("lifecycle", std::string("default")) == "ephemeral") {
        settings.lifecycle = zetta::pubsub::TopicLifecycle::Ephemeral;
    }
    if (input.contains("guarantee")) {
        settings.delivery.guarantee = zetta::pubsub::guarantee_from_string(input["guarantee"].get<std::string>());
    }
    if (!pubsub->create_topic(identity_of(context), *topic, settings, &reason)) {
        const bool exists = reason.find("already exists") != std::string::npos;
        const bool refused = reason.find("may not") != std::string::npos;
        return ApiResult::failure(refused ? "unauthorized" : exists ? "invalid_input" : "engine_error", reason,
                                  "/topic");
    }
    return ApiResult::success({{"topic", topic->to_string()}, {"exposed", settings.exposed}});
}

ApiResult handle_drop_topic(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (pubsub == nullptr) return ApiResult::failure("engine_error", "pub/sub is disabled on this node");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    if (auto missing = require_exposed(*pubsub, *topic)) return *missing;
    if (!pubsub->drop_topic(identity_of(context), *topic, &reason)) {
        return ApiResult::failure(reason.find("may not") != std::string::npos ? "unauthorized" : "engine_error",
                                  reason, "/topic");
    }
    return ApiResult::success({{"topic", topic->to_string()}, {"exposed", true}});
}

ApiResult handle_list_topics(const nlohmann::json&, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    nlohmann::json topics = nlohmann::json::array();
    if (pubsub != nullptr) {
        for (const auto& stats : pubsub->topic_stats()) {
            if (!stats.exposed) continue;
            topics.push_back({{"topic", stats.address},
                              {"subscribers", stats.subscribers},
                              {"held_events", stats.held_events},
                              {"next_sequence", stats.next_sequence}});
        }
    }
    return ApiResult::success({{"topics", topics}});
}

ApiResult handle_publish_event(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (pubsub == nullptr) return ApiResult::failure("engine_error", "pub/sub is disabled on this node");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    if (auto missing = require_exposed(*pubsub, *topic)) return *missing;

    zetta::pubsub::PublishRequest request;
    request.to = {*topic};
    request.payload = input.value("payload", std::string());
    const auto encoding = input.value("encoding", std::string("text"));
    request.encoding = zetta::pubsub::encoding_from_string(encoding).value_or(zetta::pubsub::Encoding::Text);
    request.mime_type = input.value("mime_type", std::string());
    request.correlation_id = input.value("correlation_id", std::string());
    if (input.contains("reply_to")) {
        for (const auto& item : input["reply_to"]) {
            auto address = zetta::pubsub::parse_address(item.get<std::string>(), &reason);
            if (!address) return ApiResult::failure("invalid_input", reason, "/reply_to");
            request.reply_to.push_back(std::move(*address));
        }
    }
    if (input.contains("priority")) request.delivery.priority = input["priority"].get<std::uint8_t>();
    if (input.contains("ttl_ms")) request.ttl = std::chrono::milliseconds(input["ttl_ms"].get<long long>());
    const auto result = pubsub->publish(identity_of(context), std::move(request));
    if (!result.accepted) {
        const bool refused = result.error.find("no pubsub:") != std::string::npos ||
                             result.error.find("may not") != std::string::npos;
        return ApiResult::failure(refused ? "unauthorized" : "engine_error", result.error, "/topic");
    }
    return ApiResult::success({{"event_id", result.event_id}, {"accepted", true}, {"clamped", result.clamped}});
}

ApiResult handle_receive_events(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (pubsub == nullptr) return ApiResult::failure("engine_error", "pub/sub is disabled on this node");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    if (auto missing = require_exposed(*pubsub, *topic)) return *missing;
    const auto from = input.value("from_sequence", std::uint64_t{1});
    const auto max = std::min<std::size_t>(input.value("max", std::size_t{100}), kMaxReceive);
    auto wait = std::chrono::milliseconds(std::min(input.value("wait_ms", 0LL), kMaxWaitMs));
    // A long poll never blocks the owner loop: a surface that runs this on the
    // owner thread (MCP tools/call) gets an immediate answer, as with wait_ms 0
    // (#1063). REST runs it on the blocking pool, where it waits.
    if (context.node.on_owner_thread()) wait = std::chrono::milliseconds(0);
    auto events = pubsub->read_events(identity_of(context), *topic, from, max, wait, &reason);
    if (!events) return ApiResult::failure("unauthorized", reason, "/topic");
    nlohmann::json rows = nlohmann::json::array();
    std::uint64_t next = from;
    for (const auto& event : *events) {
        nlohmann::json row = {{"sequence", event.sequence},
                              {"id", event.id},
                              {"correlation_id", event.envelope.correlation_id},
                              {"encoding", std::string(zetta::pubsub::to_string(event.encoding))},
                              {"payload", std::string(event.payload.bytes())}};
        if (event.envelope.from) row["from"] = event.envelope.from->to_string();
        rows.push_back(std::move(row));
        next = std::max(next, event.sequence + 1);
    }
    return ApiResult::success({{"events", rows}, {"next_sequence", next}});
}

}  // namespace

void register_pubsub_capabilities(Registry& registry) {
    registry.add(Capability{
        "create_topic",
        "Create topic",
        "Create a pub/sub topic. Topics are engine-internal unless `exposed` is "
        "true; only exposed topics are reachable through publish_event, "
        "receive_events, list_topics and drop_topic.",
        create_topic_schema(),
        topic_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "pubsub_create"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_topic});

    registry.add(Capability{
        "drop_topic",
        "Drop topic",
        "Drop an exposed pub/sub topic: its subscriptions close and its events are discarded.",
        topic_only_schema(),
        topic_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "pubsub_manage"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_drop_topic});

    registry.add(Capability{
        "list_topics",
        "List topics",
        "The exposed pub/sub topics on this node with their subscriber counts, held "
        "events and next sequence number.",
        empty_schema(),
        list_topics_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "pubsub_subscribe"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_topics});

    registry.add(Capability{
        "publish_event",
        "Publish event",
        "Publish one event to an exposed topic. `from` is the caller; delivery "
        "options above the caller's governance bounds are clamped and reported.",
        publish_event_schema(),
        publish_event_output_schema(),
        CapabilityKind::Action,
        Operation{CapabilityDomain::Data, "pubsub_publish"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_publish_event});

    Capability receive{
        "receive_events",
        "Receive events",
        "Read an exposed topic's held events from a sequence number, oldest "
        "first; with wait_ms, wait for new ones (long poll). Stateless: pass the "
        "returned next_sequence to continue.",
        receive_events_schema(),
        receive_events_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "pubsub_subscribe"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_receive_events};
    // A long poll must not hold the owner loop: it runs on a pool worker, and
    // the authorizer marshals its rights check onto the owner thread.
    receive.offloadable = true;
    receive.blocks = true;
    registry.add(std::move(receive));
}

}  // namespace zetta::api
