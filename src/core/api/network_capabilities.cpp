// SPDX-License-Identifier: MIT

// Workspace network controls on the control surface (#1280/#1281,
// docs/network/). Lake issues and revokes one least-privilege credential per
// app Connection; zetta-tunnel writes validated records and Lake reads decoded
// pages.

#include <utility>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/network/app_credential.hpp"
#include "zetta/network/records.hpp"
#include "zetta/node.hpp"

namespace zetta::api {
namespace {

using zetta::data::RecordStatus;
using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json object_schema(nlohmann::json properties,
                             nlohmann::json required) {
  return {{"type", "object"},
          {"additionalProperties", false},
          {"properties", std::move(properties)},
          {"required", std::move(required)}};
}

std::string error_code(RecordStatus status) {
  switch (status) {
  case RecordStatus::Ok:
    return "ok";
  case RecordStatus::InvalidInput:
    return "invalid_input";
  case RecordStatus::NotFound:
    return "not_found";
  case RecordStatus::Conflict:
    return "conflict";
  case RecordStatus::Unavailable:
    return "busy";
  }
  return "engine_error";
}

ApiResult no_store() {
  return ApiResult::failure(
      "engine_error",
      "this node has no replicated record store for workspace network records");
}

const nlohmann::json &write_output_schema() {
  static const nlohmann::json schema = object_schema(
      {{"type",
        {{"enum", {"connection", "grant", "tunnel_session", "activity"}}}},
       {"key", {{"type", "string"}}},
       {"revision", {{"type", "integer"}, {"minimum", 1}}},
       {"updated_ms", {{"type", "integer"}}},
       {"expires_ms", {{"type", "integer"}}},
       {"owner", {{"type", "string"}}},
       {"replicas", {{"type", "integer"}, {"minimum", 0}}},
       {"replicas_expected", {{"type", "integer"}, {"minimum", 0}}}},
      {"type", "key", "revision", "updated_ms", "expires_ms", "owner",
       "replicas", "replicas_expected"});
  return schema;
}

const nlohmann::json &list_schema() {
  static const nlohmann::json schema = object_schema(
      {{"type",
        {{"enum", {"connection", "grant", "tunnel_session", "activity"}}}},
       {"filter",
        {{"type", "string"},
         {"pattern", "^[a-z][a-z_]{0,63}$"},
         {"description", "a declared index; defaults to all"}}},
       {"value", {{"type", "string"}, {"maxLength", 512}}},
       {"after", {{"type", "string"}}},
       {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
       {"direction", {{"enum", {"asc", "desc"}}}}},
      {"type"});
  return schema;
}

nlohmann::json stored_network_record_schema() {
  return object_schema(
      {{"key", {{"type", "string"}}},
       {"type",
        {{"enum", {"connection", "grant", "tunnel_session", "activity"}}}},
       {"record", zetta::network::network_record_schema()},
       {"revision", {{"type", "integer"}, {"minimum", 1}}},
       {"updated_ms", {{"type", "integer"}}},
       {"expires_ms", {{"type", "integer"}}}},
      {"key", "type", "record", "revision", "updated_ms", "expires_ms"});
}

const nlohmann::json &list_output_schema() {
  static const nlohmann::json schema = object_schema(
      {{"records",
        {{"type", "array"}, {"items", stored_network_record_schema()}}},
       {"next_after", {{"type", "string"}}}},
      {"records"});
  return schema;
}

const nlohmann::json &issue_credential_schema() {
  static const nlohmann::json schema = [] {
    nlohmann::json scope = object_schema(
        {{"access", {{"enum", {"read", "write"}}}},
         {"schema", {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
         {"tables",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 128},
           {"uniqueItems", true},
           {"items",
            {{"type", "string"}, {"minLength", 1}, {"maxLength", 257}}}}},
         {"topics",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 128},
           {"uniqueItems", true},
           {"items",
            {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}}}}},
        {"access"});
    scope["anyOf"] = {{{"required", {"schema"}}},
                      {{"required", {"tables"}}},
                      {{"required", {"topics"}}}};
    return object_schema(
        {{"connection_id",
          {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}},
         {"credential_scope", std::move(scope)}},
        {"connection_id", "credential_scope"});
  }();
  return schema;
}

nlohmann::json credential_scope_output_schema() {
  return object_schema(
      {{"access", {{"enum", {"read", "write"}}}},
       {"schema", {{"type", "string"}}},
       {"tables", {{"type", "array"}, {"items", {{"type", "string"}}}}},
       {"topics", {{"type", "array"}, {"items", {{"type", "string"}}}}}},
      {"access", "schema", "tables", "topics"});
}

const nlohmann::json &issue_credential_output_schema() {
  static const nlohmann::json schema = object_schema(
      {{"connection_id", {{"type", "string"}}},
       {"user_id", {{"type", "string"}}},
       {"role_id", {{"type", "string"}}},
       {"token_id", {{"type", "string"}}},
       {"credential",
        {{"type", "string"},
         {"description", "plaintext bearer returned once and never stored"}}},
       {"credential_scope", credential_scope_output_schema()}},
      {"connection_id", "user_id", "role_id", "token_id", "credential",
       "credential_scope"});
  return schema;
}

const nlohmann::json &revoke_credential_schema() {
  static const nlohmann::json schema = object_schema(
      {{"connection_id",
        {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}}}},
      {"connection_id"});
  return schema;
}

const nlohmann::json &revoke_credential_output_schema() {
  static const nlohmann::json schema =
      object_schema({{"connection_id", {{"type", "string"}}},
                     {"revoked", {{"type", "integer"}, {"minimum", 0}}},
                     {"revoked_token_ids",
                      {{"type", "array"}, {"items", {{"type", "string"}}}}}},
                    {"connection_id", "revoked", "revoked_token_ids"});
  return schema;
}

ApiResult handle_issue_credential(const nlohmann::json &input,
                                  ApiContext &context) {
  const nlohmann::json &scope_input = input.at("credential_scope");
  zetta::network::AppCredentialRequest request;
  request.connection_id = input.at("connection_id").get<std::string>();
  request.scope.access = scope_input.at("access") == "write"
                             ? zetta::network::AppCredentialAccess::Write
                             : zetta::network::AppCredentialAccess::Read;
  request.scope.schema = scope_input.value("schema", std::string{});
  request.scope.tables =
      scope_input.value("tables", std::vector<std::string>{});
  request.scope.topics =
      scope_input.value("topics", std::vector<std::string>{});
  const zetta::network::AppCredentialIssue issue =
      zetta::network::issue_app_credential(context.node, request);
  if (!issue.ok()) {
    return ApiResult::failure(
        std::string(zetta::network::to_string(issue.status)), issue.message,
        issue.path);
  }
  return ApiResult::success({{"connection_id", request.connection_id},
                             {"user_id", issue.user_id},
                             {"role_id", issue.role_id},
                             {"token_id", issue.token_id},
                             {"credential", issue.credential},
                             {"credential_scope",
                              {{"access", std::string(zetta::network::to_string(
                                              request.scope.access))},
                               {"schema", request.scope.schema},
                               {"tables", request.scope.tables},
                               {"topics", request.scope.topics}}}});
}

ApiResult handle_revoke_credential(const nlohmann::json &input,
                                   ApiContext &context) {
  const std::string connection_id =
      input.at("connection_id").get<std::string>();
  const zetta::network::AppCredentialRevocation revocation =
      zetta::network::revoke_app_credential(context.node, connection_id);
  if (!revocation.ok()) {
    return ApiResult::failure(
        std::string(zetta::network::to_string(revocation.status)),
        revocation.message, revocation.path);
  }
  return ApiResult::success(
      {{"connection_id", connection_id},
       {"revoked", revocation.revoked_token_ids.size()},
       {"revoked_token_ids", revocation.revoked_token_ids}});
}

ApiResult handle_write(const nlohmann::json &input, ApiContext &context) {
  zetta::data::RecordStore *store = context.node.record_store();
  if (!store) {
    return no_store();
  }
  const std::optional<zetta::network::NetworkRecordType> type =
      zetta::network::classify_network_record(input);
  const zetta::data::RecordOutcome outcome =
      zetta::network::write_network_record(*store, input);
  if (!outcome.ok()) {
    return ApiResult::failure(error_code(outcome.status), outcome.message,
                              outcome.path);
  }
  const zetta::data::StoredRecord &record = *outcome.record;
  return ApiResult::success(
      {{"type", std::string(zetta::network::to_string(*type))},
       {"key", record.key},
       {"revision", record.revision},
       {"updated_ms", record.updated_ms},
       {"expires_ms", *record.expires_ms},
       {"owner", outcome.owner},
       {"replicas", outcome.replicas},
       {"replicas_expected", outcome.replicas_expected}});
}

ApiResult handle_list(const nlohmann::json &input, ApiContext &context) {
  const zetta::data::RecordStore *store = context.node.record_store();
  if (!store) {
    return no_store();
  }
  const std::optional<zetta::network::NetworkRecordType> type =
      zetta::network::network_record_type_from_string(
          input.at("type").get<std::string>());
  if (!type) {
    return ApiResult::failure("invalid_input", "unknown network record type",
                              "/type");
  }
  zetta::network::NetworkRecordListRequest request;
  request.type = *type;
  request.filter = input.value("filter", std::string{"all"});
  request.value = input.value("value", std::string{});
  request.after = input.value("after", std::string{});
  request.limit = input.value("limit", std::size_t{100});
  request.descending = input.value("direction", std::string{"desc"}) == "desc";
  const zetta::network::NetworkRecordPage page =
      zetta::network::list_network_records(*store, request);
  if (!page.ok()) {
    return ApiResult::failure(error_code(page.status), page.message, page.path);
  }
  nlohmann::json records = nlohmann::json::array();
  for (const zetta::network::StoredNetworkRecord &record : page.records) {
    records.push_back(
        {{"key", record.key},
         {"type", std::string(zetta::network::to_string(record.type))},
         {"record", record.value},
         {"revision", record.revision},
         {"updated_ms", record.updated_ms},
         {"expires_ms", *record.expires_ms}});
  }
  nlohmann::json output{{"records", std::move(records)}};
  if (!page.next_after.empty()) {
    output["next_after"] = page.next_after;
  }
  return ApiResult::success(std::move(output));
}

} // namespace

void register_network_capabilities(Registry &registry) {
  registry.add(Capability{
      "issue_app_credential", "Issue an app connection credential",
      "Issue one revocable bearer credential for an app Connection, limited "
      "to the requested schema/tables and topic prefixes. Returns the "
      "plaintext credential once and mutates replicated governance state.",
      issue_credential_schema(), issue_credential_output_schema(),
      CapabilityKind::Action, Operation{CapabilityDomain::Api, "manage_tokens"},
      /*mutates=*/true,
      /*requires_consent=*/false, handle_issue_credential});

  registry.add(Capability{
      "revoke_app_credential", "Revoke an app connection credential",
      "Revoke every bearer token for one app Connection and remove its "
      "least-privilege role assignment. Idempotent and mutates replicated "
      "governance state.",
      revoke_credential_schema(), revoke_credential_output_schema(),
      CapabilityKind::Action, Operation{CapabilityDomain::Api, "manage_tokens"},
      /*mutates=*/true,
      /*requires_consent=*/false, handle_revoke_credential});

  Capability write{
      "write_network_record",
      "Write a workspace network record",
      "Validate and durably store one Lake connection/grant history record or "
      "one zetta-tunnel session/gateway decision in this workspace's Zetta. "
      "Mutates replicated state.",
      zetta::network::network_record_schema(),
      write_output_schema(),
      CapabilityKind::Action,
      Operation{CapabilityDomain::Data, "network_records_write"},
      /*mutates=*/true,
      /*requires_consent=*/false,
      handle_write};
  write.blocks = true;
  registry.add(std::move(write));

  Capability list{
      "list_network_records",
      "List workspace network records",
      "Read a newest-first page of decoded connection, grant, tunnel-session, "
      "or gateway/mesh records from this workspace's Zetta. No side effects.",
      list_schema(),
      list_output_schema(),
      CapabilityKind::Read,
      Operation{CapabilityDomain::Data, "network_records_read"},
      /*mutates=*/false,
      /*requires_consent=*/false,
      handle_list};
  list.blocks = true;
  list.post_alias = true;
  registry.add(std::move(list));
}

} // namespace zetta::api
