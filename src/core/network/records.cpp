// SPDX-License-Identifier: MIT

#include "zetta/network/records.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <ranges>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/util/sha256.hpp"

#include "../base64.hpp"

namespace zetta::network {
namespace {

constexpr std::int64_t kDayMs = 24LL * 60 * 60 * 1000;
constexpr std::int64_t kConnectionRetentionMs = 365LL * kDayMs;
constexpr std::int64_t kGrantRetentionMs = 365LL * kDayMs;
constexpr std::int64_t kTunnelSessionRetentionMs = 90LL * kDayMs;
constexpr std::int64_t kActivityRetentionMs = 30LL * kDayMs;
constexpr std::int64_t kNetworkCollectionsCreatedMs = 1'791'936'000'000LL;

const std::string kTimestampPattern =
    R"(^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}(\.[0-9]+)?Z$)";
const std::string kUuidPattern =
    R"(^[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$)";

nlohmann::json string_property(std::size_t maximum = 1024) {
  return {{"type", "string"}, {"maxLength", maximum}};
}

nlohmann::json non_empty_string(std::size_t maximum = 1024) {
  return {{"type", "string"}, {"minLength", 1}, {"maxLength", maximum}};
}

nlohmann::json timestamp_property() {
  return {{"type", "string"}, {"pattern", kTimestampPattern}};
}

nlohmann::json uuid_property() {
  return {{"type", "string"}, {"pattern", kUuidPattern}};
}

nlohmann::json object_schema(nlohmann::json properties,
                             nlohmann::json required) {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", std::move(properties)},
          {"required", std::move(required)}};
}

nlohmann::json connection_schema() {
  return object_schema(
      {{"schema", {{"const", 1}}},
       {"event",
        {{"enum",
          {"connection_declared", "connection_updated",
           "connection_removed"}}}},
       {"timestamp", timestamp_property()},
       {"workspace", non_empty_string(253)},
       {"connection_id", uuid_property()},
       {"actor_id", non_empty_string(512)},
       {"source_kind", {{"enum", {"app", "service", "agent", "tunnel"}}}},
       {"source_id", uuid_property()},
       {"target_kind", {{"enum", {"service", "app"}}}},
       {"target_id", uuid_property()},
       {"ports",
        {{"type", "array"},
         {"uniqueItems", true},
         {"items", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}}}},
       {"credential_scope", {{"type", {"object", "null"}}}}},
      {"schema", "event", "timestamp", "workspace", "connection_id", "actor_id",
       "source_kind", "source_id", "target_kind", "target_id", "ports"});
}

nlohmann::json grant_schema() {
  return object_schema(
      {{"schema", {{"const", 1}}},
       {"event",
        {{"enum", {"grant_created", "grant_revoked", "grant_expired"}}}},
       {"timestamp", timestamp_property()},
       {"workspace", non_empty_string(253)},
       {"grant_id", uuid_property()},
       {"actor_id", non_empty_string(512)},
       {"user_id", non_empty_string(512)},
       {"target_kind", {{"enum", {"service", "app"}}}},
       {"target_id", uuid_property()},
       {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
       {"expires_at", timestamp_property()},
       {"revoked_at",
        {{"type", {"string", "null"}}, {"pattern", kTimestampPattern}}}},
      {"schema", "event", "timestamp", "workspace", "grant_id", "actor_id",
       "user_id", "target_kind", "target_id", "port", "expires_at"});
}

nlohmann::json tunnel_session_schema() {
  return object_schema(
      {{"schema", {{"const", 1}}},
       {"event", {{"enum", {"session_start", "session_end", "refusal"}}}},
       {"role", {{"const", "relay"}}},
       {"session_id", {{"type", "string"}, {"pattern", "^[0-9a-f]{32}$"}}},
       {"timestamp", timestamp_property()},
       {"started_at", timestamp_property()},
       {"grant_id", uuid_property()},
       {"workspace", non_empty_string(253)},
       {"peer", non_empty_string(512)},
       {"endpoint", non_empty_string(512)},
       {"reason", string_property(128)},
       {"bytes_up", {{"type", "integer"}, {"minimum", 0}}},
       {"bytes_down", {{"type", "integer"}, {"minimum", 0}}}},
      {"schema", "event", "role", "session_id", "timestamp", "started_at",
       "grant_id", "workspace", "peer", "endpoint", "reason", "bytes_up",
       "bytes_down"});
}

nlohmann::json egress_decision_schema() {
  return object_schema(
      {{"schema", {{"const", 1}}},
       {"event", {{"const", "egress_decision"}}},
       {"role", {{"const", "egress"}}},
       {"timestamp", timestamp_property()},
       {"started_at", timestamp_property()},
       {"decision", {{"enum", {"allow", "deny"}}}},
       {"reason", non_empty_string(128)},
       {"rule_id",
        {{"type", "string"},
         {"pattern", "^$|[0-9a-f]{8}-[0-9a-f]{4}-[1-5][0-9a-f]{3}-[89ab][0-9a-"
                     "f]{3}-[0-9a-f]{12}$"}}},
       {"workspace", non_empty_string(253)},
       {"source", non_empty_string(512)},
       {"dns_name", non_empty_string(253)},
       {"port", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65535}}},
       {"protocol", {{"enum", {"https_sni", "explicit"}}}},
       {"resolved_address", string_property(512)}},
      {"schema", "event", "role", "timestamp", "started_at", "decision",
       "reason", "rule_id", "workspace", "source", "dns_name", "port",
       "protocol", "resolved_address"});
}

nlohmann::json gateway_activity_schema() {
  return object_schema(
      {{"schema", {{"const", 1}}},
       {"event",
        {{"enum", {"peer_decision", "ingress_decision", "mesh_decision"}}}},
       {"role", {{"enum", {"peer_gateway", "ingress_gateway", "mesh"}}}},
       {"timestamp", timestamp_property()},
       {"started_at", timestamp_property()},
       {"decision", {{"enum", {"allow", "deny"}}}},
       {"reason", non_empty_string(128)},
       {"workspace", non_empty_string(253)},
       {"source", non_empty_string(512)},
       {"destination", non_empty_string(512)},
       {"protocol", non_empty_string(64)},
       {"bytes_up", {{"type", "integer"}, {"minimum", 0}}},
       {"bytes_down", {{"type", "integer"}, {"minimum", 0}}}},
      {"schema", "event", "role", "timestamp", "started_at", "decision",
       "reason", "workspace", "source", "destination", "protocol", "bytes_up",
       "bytes_down"});
}

class CollectingErrorHandler : public nlohmann::json_schema::error_handler {
public:
  void error(const nlohmann::json::json_pointer &pointer,
             const nlohmann::json &, const std::string &message) override {
    if (failed_) {
      return;
    }
    failed_ = true;
    const std::string location = pointer.to_string();
    reason_ = (location.empty() ? "/" : location) + ": " + message;
  }

  bool failed() const { return failed_; }
  const std::string &reason() const { return reason_; }

private:
  bool failed_ = false;
  std::string reason_;
};

nlohmann::json_schema::json_validator &record_validator() {
  static nlohmann::json_schema::json_validator validator(
      network_record_schema());
  return validator;
}

zetta::data::RecordOutcome failure(zetta::data::RecordStatus status,
                                   std::string message, std::string path) {
  zetta::data::RecordOutcome outcome;
  outcome.status = status;
  outcome.message = std::move(message);
  outcome.path = std::move(path);
  return outcome;
}

std::string collection_name(NetworkRecordType type) {
  switch (type) {
  case NetworkRecordType::Connection:
    return "network_connections";
  case NetworkRecordType::Grant:
    return "network_grants";
  case NetworkRecordType::TunnelSession:
    return "network_tunnel_sessions";
  case NetworkRecordType::Activity:
    return "network_activity";
  }
  return {};
}

const zetta::data::RecordCollection *definition_for(NetworkRecordType type) {
  const std::string name = collection_name(type);
  const auto &definitions = network_record_collections();
  const auto found = std::ranges::find(definitions, name,
                                       &zetta::data::RecordCollection::name);
  return found == definitions.end() ? nullptr : &*found;
}

std::string record_key(NetworkRecordType type, const nlohmann::json &value) {
  if (type == NetworkRecordType::TunnelSession) {
    return value.at("role").get<std::string>() + ":" +
           value.at("session_id").get<std::string>();
  }
  return zetta::util::sha256_hex(value.dump());
}

std::map<std::string, std::string> record_indexes(NetworkRecordType type,
                                                  const nlohmann::json &value) {
  std::map<std::string, std::string> indexes{
      {"all", "all"},
      {"event", value.at("event").get<std::string>()},
      {"workspace", value.at("workspace").get<std::string>()},
  };
  const auto add = [&](std::string_view name) {
    const auto found = value.find(name);
    if (found != value.end() && found->is_string()) {
      indexes.emplace(name, found->get<std::string>());
    }
  };
  switch (type) {
  case NetworkRecordType::Connection:
    add("connection_id");
    add("actor_id");
    add("source_id");
    add("target_id");
    break;
  case NetworkRecordType::Grant:
    add("grant_id");
    add("user_id");
    add("actor_id");
    break;
  case NetworkRecordType::TunnelSession:
    add("session_id");
    add("grant_id");
    add("role");
    break;
  case NetworkRecordType::Activity:
    add("role");
    add("decision");
    add("rule_id");
    break;
  }
  return indexes;
}

} // namespace

std::string_view to_string(NetworkRecordType type) {
  switch (type) {
  case NetworkRecordType::Connection:
    return "connection";
  case NetworkRecordType::Grant:
    return "grant";
  case NetworkRecordType::TunnelSession:
    return "tunnel_session";
  case NetworkRecordType::Activity:
    return "activity";
  }
  return "activity";
}

std::optional<NetworkRecordType>
network_record_type_from_string(std::string_view text) {
  if (text == "connection")
    return NetworkRecordType::Connection;
  if (text == "grant")
    return NetworkRecordType::Grant;
  if (text == "tunnel_session")
    return NetworkRecordType::TunnelSession;
  if (text == "activity")
    return NetworkRecordType::Activity;
  return std::nullopt;
}

const std::vector<zetta::data::RecordCollection> &network_record_collections() {
  static const std::vector<zetta::data::RecordCollection> collections{
      {"network_connections",
       {"all", "event", "workspace", "connection_id", "actor_id", "source_id",
        "target_id"},
       kConnectionRetentionMs,
       kNetworkCollectionsCreatedMs},
      {"network_grants",
       {"all", "event", "workspace", "grant_id", "user_id", "actor_id"},
       kGrantRetentionMs,
       kNetworkCollectionsCreatedMs},
      {"network_tunnel_sessions",
       {"all", "event", "workspace", "session_id", "grant_id", "role"},
       kTunnelSessionRetentionMs,
       kNetworkCollectionsCreatedMs},
      {"network_activity",
       {"all", "event", "workspace", "role", "decision", "rule_id"},
       kActivityRetentionMs,
       kNetworkCollectionsCreatedMs},
  };
  return collections;
}

bool initialize_network_record_collections(zetta::data::RecordStore &store,
                                           std::string *reason) {
  for (const zetta::data::RecordCollection &definition :
       network_record_collections()) {
    const zetta::data::RecordOutcome outcome =
        store.create_collection(definition);
    if (!outcome.ok()) {
      if (reason) {
        *reason = outcome.message;
      }
      return false;
    }
  }
  return true;
}

const nlohmann::json &network_record_schema() {
  static const nlohmann::json schema{
      {"$schema", "http://json-schema.org/draft-07/schema#"},
      {"title", "zetta.NetworkRecord"},
      {"description",
       "A workspace network record (#1281): Lake connection/grant history or "
       "a zetta-tunnel relay session/gateway decision."},
      {"oneOf",
       {connection_schema(), grant_schema(), tunnel_session_schema(),
        egress_decision_schema(), gateway_activity_schema()}},
  };
  return schema;
}

bool validate_network_record(const nlohmann::json &value, std::string *reason) {
  CollectingErrorHandler handler;
  record_validator().validate(value, handler);
  if (!handler.failed()) {
    return true;
  }
  if (reason) {
    *reason = handler.reason();
  }
  return false;
}

std::optional<NetworkRecordType>
classify_network_record(const nlohmann::json &value) {
  const auto event = value.find("event");
  if (event == value.end() || !event->is_string()) {
    return std::nullopt;
  }
  const std::string name = event->get<std::string>();
  if (name.starts_with("connection_"))
    return NetworkRecordType::Connection;
  if (name.starts_with("grant_"))
    return NetworkRecordType::Grant;
  if (name == "session_start" || name == "session_end" || name == "refusal") {
    return NetworkRecordType::TunnelSession;
  }
  if (name == "egress_decision" || name == "peer_decision" ||
      name == "ingress_decision" || name == "mesh_decision") {
    return NetworkRecordType::Activity;
  }
  return std::nullopt;
}

zetta::data::RecordOutcome write_network_record(zetta::data::RecordStore &store,
                                                const nlohmann::json &value) {
  const auto workspace = value.find("workspace");
  if (workspace == value.end() || !workspace->is_string() ||
      workspace->get_ref<const std::string &>().empty()) {
    return failure(zetta::data::RecordStatus::InvalidInput,
                   "/workspace: a non-empty workspace is required",
                   "/workspace");
  }
  std::string validation_error;
  if (!validate_network_record(value, &validation_error)) {
    const std::size_t colon = validation_error.find(':');
    return failure(
        zetta::data::RecordStatus::InvalidInput, validation_error,
        colon == std::string::npos ? "/" : validation_error.substr(0, colon));
  }
  const std::optional<NetworkRecordType> type = classify_network_record(value);
  if (!type) {
    return failure(zetta::data::RecordStatus::InvalidInput,
                   "record event does not select a network record type",
                   "/event");
  }
  zetta::data::RecordWrite write;
  write.collection = collection_name(*type);
  write.key = record_key(*type, value);
  write.value = zetta::detail::base64_encode_bytes(value.dump());
  write.index = record_indexes(*type, value);
  write.order = value.at("timestamp").get<std::string>() + ":" + write.key;
  return store.write(write);
}

NetworkRecordPage
list_network_records(const zetta::data::RecordStore &store,
                     const NetworkRecordListRequest &request) {
  NetworkRecordPage page;
  const zetta::data::RecordCollection *definition =
      definition_for(request.type);
  if (!definition) {
    page.status = zetta::data::RecordStatus::InvalidInput;
    page.message = "unknown network record type";
    page.path = "/type";
    return page;
  }
  if (std::ranges::find(definition->indexes, request.filter) ==
      definition->indexes.end()) {
    page.status = zetta::data::RecordStatus::InvalidInput;
    page.message = "filter '" + request.filter + "' is not available for " +
                   std::string(to_string(request.type)) + " records";
    page.path = "/filter";
    return page;
  }
  if (request.filter != "all" && request.value.empty()) {
    page.status = zetta::data::RecordStatus::InvalidInput;
    page.message = "a filtered network-record list requires value";
    page.path = "/value";
    return page;
  }

  zetta::data::RecordListRequest list;
  list.collection = definition->name;
  list.index = request.filter;
  list.value = request.filter == "all" ? "all" : request.value;
  list.after = request.after;
  list.limit = request.limit;
  list.descending = request.descending;
  const zetta::data::RecordPage stored = store.list(list);
  if (!stored.ok()) {
    page.status = stored.status;
    page.message = stored.message;
    page.path = stored.path;
    return page;
  }
  page.next_after = stored.next_after;
  for (const zetta::data::StoredRecord &record : stored.records) {
    nlohmann::json value = nlohmann::json::parse(
        zetta::detail::base64_decode_to_string(record.value), nullptr, false);
    if (value.is_discarded() || !validate_network_record(value)) {
      page.status = zetta::data::RecordStatus::Unavailable;
      page.message =
          "stored network record '" + record.key + "' failed its schema";
      page.path = "/records";
      page.records.clear();
      page.next_after.clear();
      return page;
    }
    page.records.push_back(StoredNetworkRecord{
        record.key, request.type, std::move(value), record.revision,
        record.updated_ms, record.expires_ms});
  }
  return page;
}

} // namespace zetta::network
