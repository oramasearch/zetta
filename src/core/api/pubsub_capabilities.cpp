// Pub/sub on the control surface (#1033; decision D22 in
// docs/plans/pubsub/decisions.md; docs/pubsub/exposure.md).
//
// Topics are engine-internal by default. These capabilities exist on every
// node (one registry entry renders as the REST route, the MCP tool and the
// OpenAPI entry), but publishing, reading, dropping and listing only ever see
// topics whose `exposed` setting is on; an unexposed topic answers not_found,
// exactly as if it did not exist. Topic reads/writes and event writes have
// separate permissions; receive_events retains pubsub:subscribe.

#include <algorithm>
#include <chrono>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/node.hpp"
#include "zetta/governance/role.hpp"
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
    if (address && ((address->kind != "topic" && address->kind != "inbox") || address->is_wildcard())) {
        reason = "want a concrete topic:<name> or inbox:<owner kind>.<owner name>, with no wildcard";
        return std::nullopt;
    }
    return address;
}

void add_pubsub_capability(Registry& registry, Capability capability) {
    capability.authorize_scoped = [operation = capability.required_operation](
        const nlohmann::json& input, const ApiContext& context) {
        if (input.contains("topic")) {
            std::string reason;
            const auto topic = topic_address(input, reason);
            return topic && context.node.authorize_object(context.acting_key, true, operation,
                zetta::governance::Resource::topic(topic->name));
        }
        // A listing needs a topic grant, then filters each concrete result.
        for (const auto& role_id : context.node.roles_for_user(context.acting_key)) {
            const auto role = context.node.find_role_by_id(role_id);
            if (!role) continue;
            for (const auto& grant : role->permissions().scoped_grants) {
                if (grant.operation == operation && grant.object.kind == zetta::governance::ResourceKind::Topic) return true;
            }
        }
        return false;
    };
    registry.add(std::move(capability));
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
    return {{"type", "string"}, {"minLength", 1}, {"description", "topic:<name>, <name>, or inbox:<owner kind>.<owner name>"}};
}

const nlohmann::json& settings_schema() {
    static const auto schema = nlohmann::json::parse(R"JSON({
      "type": "object", "additionalProperties": false,
      "properties": {
        "lifecycle": {"type":"string","enum":["default","ephemeral"]},
        "store": {"type":"string","enum":["ring_stack","queue"]},
        "ring_capacity": {"type":"integer","minimum":1,"maximum":1048576},
        "inline_threshold": {"type":"integer","minimum":0,"maximum":1048576},
        "priority_levels": {"type":"integer","minimum":1,"maximum":256},
        "priority_aging_ms": {"type":"integer","minimum":0},
        "memory_cap_bytes": {"type":["integer","null"],"minimum":0},
        "disk_cap_bytes": {"type":["integer","null"],"minimum":0},
        "overflow": {"type":["string","null"],"enum":[null,"unbounded","skip_ahead","disconnect","hold_publisher","reject_new","spill_to_disk"]},
        "over_bound": {"type":"string","enum":["clamp","reject"]},
        "producer_queue": {"type":"boolean"},
        "dispatch": {"type":["string","null"],"enum":[null,"inline","per_key","auto","pool"]},
        "exposed": {"type":"boolean"},
        "topics_chain_max": {"type":"integer","minimum":1},
        "default_ttl_ms": {"type":["integer","null"],"minimum":1},
        "retention_ms": {"type":["integer","null"],"minimum":0},
        "max_events": {"type":["integer","null"],"minimum":0},
        "delivery": {
          "type":"object", "additionalProperties":false,
          "properties": {
            "recipients":{"type":["string","null"],"enum":[null,"all","one","count"]},
            "count":{"type":"integer","minimum":1,"maximum":4294967295},
            "ordering":{"type":["string","null"],"enum":[null,"fifo","priority"]},
            "priority":{"type":["integer","null"],"minimum":0,"maximum":255},
            "guarantee":{"type":["string","null"],"enum":[null,"at_most_once","at_least_once","exactly_once"]},
            "ack_deadline_ms":{"type":["integer","null"],"minimum":1},
            "reply":{"type":["string","null"],"enum":[null,"none","reply_to","reply_all"]}
          }
        },
        "bounds": {
          "type":"object", "additionalProperties":false,
          "properties": {
            "max_priority":{"type":["integer","null"],"minimum":0,"maximum":255},
            "max_recipients":{"type":["integer","null"],"minimum":1,"maximum":4294967295},
            "max_guarantee":{"type":["string","null"],"enum":[null,"at_most_once","at_least_once","exactly_once"]},
            "allowed_recipient_modes":{"type":"array","uniqueItems":true,"items":{"type":"string","enum":["all","one","count"]}}
          }
        }
      }
    })JSON");
    return schema;
}

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
    return {{"type", "object"}, {"additionalProperties", false},
        {"properties", std::move(properties)}, {"required", std::move(required)}};
}

nlohmann::json get_topic_output_schema() {
    const nlohmann::json integer = {{"type", "integer"}, {"minimum", 0}};
    const nlohmann::json text = {{"type", "string"}};
    auto subscription = object_schema({{"id", integer}, {"subscriber", text},
        {"positions", {{"type", "array"}, {"items", integer}}}, {"next_sequence", integer},
        {"lag", integer}, {"in_flight", integer}, {"redeliveries", integer},
        {"connected", {{"type", "boolean"}}}, {"group", text}},
        {"id", "subscriber", "positions", "next_sequence", "lag", "in_flight", "redeliveries", "connected", "group"});
    return object_schema({{"topic", text}, {"settings", settings_schema()}, {"subscriber_count", integer},
        {"subscriptions", {{"type", "array"}, {"items", subscription}}}, {"held_events", integer},
        {"earliest_sequence", integer}, {"next_sequence", integer}},
        {"topic", "settings", "subscriber_count", "subscriptions", "held_events", "earliest_sequence", "next_sequence"});
}

nlohmann::json browse_output_schema() {
    const nlohmann::json integer = {{"type", "integer"}, {"minimum", 0}};
    const nlohmann::json text = {{"type", "string"}};
    const auto header = object_schema({{"name", text}, {"value", text}}, {"name", "value"});
    const auto event = object_schema({{"sequence", integer}, {"id", text}, {"revision", integer},
        {"from", text}, {"published_at", integer}, {"payload", text}, {"size", integer},
        {"encoding", text}, {"headers", {{"type", "array"}, {"items", header}}},
        {"edited_at", {{"type", {"integer", "null"}}}}, {"edited_by", text}},
        {"sequence", "id", "revision", "from", "published_at", "payload", "size", "encoding", "headers", "edited_at", "edited_by"});
    return object_schema({{"topic", text}, {"events", {{"type", "array"}, {"items", event}}},
        {"earliest_sequence", integer}, {"next_sequence", integer}},
        {"topic", "events", "earliest_sequence", "next_sequence"});
}

const nlohmann::json& create_topic_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"topic"}},
        {"additionalProperties", false},
        {"properties",
         {{"topic", topic_property()},
          {"settings", settings_schema()},
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
              {"additionalProperties", false},
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
              {"additionalProperties", false},
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
    auto configured = input.value("settings", nlohmann::json::object());
    for (const auto* field : {"exposed", "lifecycle"}) {
        if (!input.contains(field)) continue;
        if (configured.contains(field)) return ApiResult::failure("invalid_input", std::string(field) + " supplied twice");
        configured[field] = input[field];
    }
    if (input.contains("guarantee")) {
        if (configured.contains("delivery") && configured["delivery"].contains("guarantee")) {
            return ApiResult::failure("invalid_input", "guarantee supplied twice");
        }
        configured["delivery"]["guarantee"] = input["guarantee"];
    }
    const auto settings = zetta::pubsub::parse_topic_settings(configured);
    if (!pubsub->create_topic(identity_of(context), *topic, settings, &reason)) {
        const bool exists = reason.find("already exists") != std::string::npos;
        const bool refused = reason.find("may not") != std::string::npos;
        return ApiResult::failure(refused ? "unauthorized" : exists ? "invalid_input" : "engine_error", reason,
                                  "/topic");
    }
    return ApiResult::success({{"topic", topic->to_string()}, {"exposed", settings.exposed},
        {"settings", zetta::pubsub::topic_settings_json(*pubsub->topic_settings(*topic))}});
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
            const auto address = zetta::pubsub::parse_address(stats.address);
            if (!address || !pubsub->get_topic(identity_of(context), *address)) continue;
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
    const auto wait = std::chrono::milliseconds(
        std::min(input.value("wait_ms", 0LL), kMaxWaitMs));
    const auto waiter = wait.count() > 0 ? context.wait_event : nullptr;
    // REST and MCP register before this no-wait read, then suspend outside
    // the registry when the result is empty. Direct callers without an event
    // receive the immediate read (#1076, #1136, #1137).
    auto events = pubsub->read_events(
        identity_of(context), *topic, from, max, std::chrono::milliseconds(0),
        &reason, waiter);
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

ApiResult crud_failure(const std::string& reason) {
    const auto code = reason.find("may not") != std::string::npos ? "unauthorized" :
        reason.find("not exposed") != std::string::npos || reason == "event is not held" ? "not_found" :
        reason == "stale expected_revision" ? "conflict" : "invalid_input";
    return ApiResult::failure(code, reason);
}

ApiResult handle_get_topic(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (!pubsub) return ApiResult::failure("engine_error", "pub/sub is disabled");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    const auto result = pubsub->get_topic(identity_of(context), *topic, &reason);
    return result ? ApiResult::success(*result) : crud_failure(reason);
}

ApiResult handle_browse_topic(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (!pubsub) return ApiResult::failure("engine_error", "pub/sub is disabled");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    std::optional<std::uint64_t> from;
    if (input.contains("from_sequence")) from = input.at("from_sequence").get<std::uint64_t>();
    const auto result = pubsub->browse_topic(identity_of(context), *topic, from,
        input.value("limit", std::size_t{100}), input.value("direction", "forward") == "backward",
        &reason, input.value("wait_ms", 0LL) > 0 ? context.wait_event : nullptr);
    return result ? ApiResult::success(*result) : crud_failure(reason);
}

ApiResult handle_update_topic(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (!pubsub) return ApiResult::failure("engine_error", "pub/sub is disabled");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    const auto result = pubsub->update_topic(identity_of(context), *topic, input.at("settings"));
    if (!result.ok) return crud_failure(result.reason);
    return ApiResult::success({{"topic", topic->to_string()}, {"trimmed", result.changes.size()},
        {"settings", zetta::pubsub::topic_settings_json(*pubsub->topic_settings(*topic))}});
}

ApiResult handle_update_event(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (!pubsub) return ApiResult::failure("engine_error", "pub/sub is disabled");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    const auto result = pubsub->update_event(identity_of(context), *topic, input.at("sequence").get<std::uint64_t>(),
        input.at("payload").get<std::string>(), input.at("expected_revision").get<std::uint64_t>(), input.value("redeliver", false));
    if (!result.ok) return crud_failure(result.reason);
    const auto& event = *result.changes.front().after;
    return ApiResult::success({{"topic", topic->to_string()}, {"sequence", event.sequence},
        {"revision", event.revision}, {"edited_by", event.edited_by},
        {"edited_at", std::chrono::duration_cast<std::chrono::milliseconds>(event.edited_at->time_since_epoch()).count()}});
}

ApiResult handle_delete_events(const nlohmann::json& input, ApiContext& context) {
    auto* pubsub = pubsub_of(context);
    if (!pubsub) return ApiResult::failure("engine_error", "pub/sub is disabled");
    std::string reason;
    const auto topic = topic_address(input, reason);
    if (!topic) return ApiResult::failure("invalid_input", reason, "/topic");
    std::optional<std::uint64_t> before;
    if (input.contains("before_sequence")) before = input.at("before_sequence").get<std::uint64_t>();
    const auto result = pubsub->delete_events(identity_of(context), *topic,
        input.value("sequences", std::vector<std::uint64_t>{}), before);
    if (!result.ok) return crud_failure(result.reason);
    return ApiResult::success({{"topic", topic->to_string()}, {"removed", result.changes.size()}});
}

AwaitableRead pubsub_wait() {
    return {[](const nlohmann::json& input) {
        return std::chrono::milliseconds(std::min(input.value("wait_ms", 0LL), kMaxWaitMs));
    }, [](const ApiResult& result) {
        return result.ok && result.value.at("events").empty();
    }};
}

void register_crud_capabilities(Registry& registry) {
    const nlohmann::json sequence = {{"type", "integer"}, {"minimum", 1}};
    const nlohmann::json text = {{"type", "string"}};
    Capability get{"get_topic", "Get topic", "Inspect an exposed node-local topic without consuming events.",
        topic_only_schema(), get_topic_output_schema(), CapabilityKind::Read,
        {CapabilityDomain::Data, "topics_read"}, false, false, handle_get_topic};
    get.post_alias = true;
    get.audit_policy = AuditPolicy::SummarizeAuthorizedReads;
    get.audit_scope_field = "topic";
    add_pubsub_capability(registry, std::move(get));
    auto browse_input = receive_events_schema();
    browse_input["properties"].erase("max");
    browse_input["properties"]["limit"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxReceive}};
    browse_input["properties"]["direction"] = {{"type", "string"}, {"enum", {"forward", "backward"}}};
    Capability browse{"browse_topic", "Browse topic", "Browse held events without creating or advancing a subscription; wait_ms is bounded on REST and MCP.",
        browse_input, browse_output_schema(), CapabilityKind::Read,
        {CapabilityDomain::Data, "topics_read"}, false, false, handle_browse_topic};
    browse.awaitable_read = pubsub_wait();
    browse.post_alias = true;
    browse.audit_policy = AuditPolicy::SummarizeAuthorizedReads;
    browse.audit_scope_field = "topic";
    add_pubsub_capability(registry, std::move(browse));
    add_pubsub_capability(registry, Capability{"update_topic", "Update topic", "Update mutable node-local topic settings and trim history on retention shrink; immutable settings are refused.",
        object_schema({{"topic", topic_property()}, {"settings", settings_schema()}}, {"topic", "settings"}),
        object_schema({{"topic", text}, {"settings", settings_schema()}, {"trimmed", {{"type", "integer"}}}}, {"topic", "settings", "trimmed"}),
        CapabilityKind::Action, {CapabilityDomain::Orchestration, "pubsub_manage"}, true, false, handle_update_topic});
    add_pubsub_capability(registry, Capability{"update_event", "Update event", "Replace a held payload with revision checking; redeliver explicitly requeues it for subscribers that received it.",
        object_schema({{"topic", topic_property()}, {"sequence", sequence}, {"expected_revision", sequence},
            {"payload", text}, {"redeliver", {{"type", "boolean"}}}}, {"topic", "sequence", "payload", "expected_revision"}),
        object_schema({{"topic", text}, {"sequence", sequence}, {"revision", sequence}, {"edited_by", text},
            {"edited_at", {{"type", "integer"}}}}, {"topic", "sequence", "revision", "edited_by", "edited_at"}),
        CapabilityKind::Action, {CapabilityDomain::Data, "pubsub_publish"}, true, false, handle_update_event});
    auto deletion = object_schema({{"topic", topic_property()}, {"before_sequence", sequence},
        {"sequences", {{"type", "array"}, {"minItems", 1}, {"maxItems", 1000}, {"uniqueItems", true}, {"items", sequence}}}}, {"topic"});
    deletion["oneOf"] = {{{"required", {"sequences"}}}, {{"required", {"before_sequence"}}}};
    add_pubsub_capability(registry, Capability{"delete_events", "Delete events", "Remove selected held events or all sequences strictly below before_sequence; pending subscribers skip them.",
        deletion, object_schema({{"topic", text}, {"removed", {{"type", "integer"}}}}, {"topic", "removed"}),
        CapabilityKind::Action, {CapabilityDomain::Data, "pubsub_publish"}, true, false, handle_delete_events});
}

}  // namespace

void register_pubsub_capabilities(Registry& registry) {
    register_crud_capabilities(registry);
    auto create_output = topic_output_schema();
    create_output["properties"]["settings"] = settings_schema();
    create_output["required"].push_back("settings");
    add_pubsub_capability(registry, Capability{
        "create_topic",
        "Create topic",
        "Create a node-local topic or concrete inbox with full settings. Only "
        "explicitly exposed topics are reachable through topic/message CRUD.",
        create_topic_schema(),
        create_output,
        CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "pubsub_create"},
        /*mutates=*/true,
        /*requires_consent=*/false,
        handle_create_topic});

    add_pubsub_capability(registry, Capability{
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

    Capability list{
        "list_topics",
        "List topics",
        "The exposed pub/sub topics on this node with their subscriber counts, held "
        "events and next sequence number.",
        empty_schema(),
        list_topics_output_schema(),
        CapabilityKind::Read,
        Operation{CapabilityDomain::Data, "topics_read"},
        /*mutates=*/false,
        /*requires_consent=*/false,
        handle_list_topics};
    list.post_alias = true;
    list.audit_policy = AuditPolicy::SummarizeAuthorizedReads;
    add_pubsub_capability(registry, std::move(list));

    add_pubsub_capability(registry, Capability{
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
    receive.awaitable_read = AwaitableRead{
        [](const nlohmann::json& input) {
            return std::chrono::milliseconds(
                std::min(input.value("wait_ms", 0LL), kMaxWaitMs));
        },
        [](const ApiResult& result) {
            return result.ok && result.value.contains("events") &&
                   result.value.at("events").empty();
        }};
    receive.audit_policy = AuditPolicy::SummarizeAuthorizedReads;
    receive.audit_scope_field = "topic";
    add_pubsub_capability(registry, std::move(receive));
}

}  // namespace zetta::api
