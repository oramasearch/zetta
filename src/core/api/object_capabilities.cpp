// SPDX-License-Identifier: MIT

// The object capability on the control surface (#1005, docs/data/object-store.md).
//
// The bucket and lease operations are registry capabilities: REST route, MCP tool
// and OpenAPI operation each. They are `blocks` entries: the registry authorizes
// and audits on the owner loop, then runs the handler on the blocking pool,
// because a call can wait on the bucket's owners over the signal plane.
//
// The byte operations (put, put range, get) move raw bodies, so the HTTP adapter
// serves them as routes under /v1/objects/<bucket>/<key> (http_server.cpp); they
// are documented in OpenAPI from object_route_paths() below and share this TU's
// status mapping and authorization (object_routes.hpp). The REST aliases lake
// calls — GET /v1/objects/<bucket>, DELETE …/<key>, POST …:barrier, GET …:usage,
// POST …:acquire|:renew|:release — are thin renderings of these capabilities.

#include <algorithm>
#include <chrono>
#include <utility>

#include "object_routes.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"

namespace zetta::api {

using zetta::data::ObjectOutcome;
using zetta::data::ObjectStatus;

std::string object_error_code(ObjectStatus status) {
    switch (status) {
        case ObjectStatus::Ok: return "ok";
        case ObjectStatus::InvalidInput: return "invalid_input";
        case ObjectStatus::NotFound: return "not_found";
        case ObjectStatus::Conflict: return "conflict";
        case ObjectStatus::Fenced: return "fenced";
        case ObjectStatus::Held: return "held";
        case ObjectStatus::OutOfRange: return "range_not_satisfiable";
        case ObjectStatus::QuotaExceeded: return "quota_exceeded";
        case ObjectStatus::Unavailable: return "busy";
        case ObjectStatus::IncarnationChanged: return "incarnation_changed";
    }
    return "engine_error";
}

namespace {

nlohmann::json lease_value(const zetta::data::ObjectLease& lease, const std::string& incarnation) {
    return {{"bucket", lease.bucket},
            {"holder", lease.holder},
            {"fence", lease.fence},
            {"expires_ms", lease.expires_ms},
            {"incarnation", incarnation},
            {"object_log_prefix_durability", zetta::data::ObjectStore::kPrefixDurability}};
}

}  // namespace

ApiResult object_failure(const ObjectOutcome& outcome) {
    ApiResult result =
        ApiResult::failure(object_error_code(outcome.status), outcome.message, outcome.path);
    nlohmann::json details = nlohmann::json::object();
    switch (outcome.status) {
        case ObjectStatus::Conflict:
        case ObjectStatus::OutOfRange:
            // What a writer retries from: the object's size (0 when absent).
            details["size"] = outcome.object ? outcome.object->size : 0;
            if (outcome.bucket) details["bucket"] = to_json(*outcome.bucket);
            break;
        case ObjectStatus::Fenced: details["fence"] = outcome.fence; break;
        case ObjectStatus::IncarnationChanged: details["incarnation"] = outcome.incarnation; break;
        case ObjectStatus::Held:
            if (outcome.lease) {
                details = {{"bucket", outcome.lease->bucket},
                           {"holder", outcome.lease->holder},
                           {"fence", outcome.lease->fence},
                           {"expires_ms", outcome.lease->expires_ms}};
            }
            break;
        default: break;
    }
    if (!details.empty()) result.error.details = std::move(details);
    return result;
}

nlohmann::json object_write_value(const std::string& bucket, const ObjectOutcome& outcome) {
    nlohmann::json value{{"bucket", bucket},
                         {"incarnation", outcome.incarnation},
                         {"owner", outcome.owner},
                         {"replicas", outcome.replicas},
                         {"replicas_expected", outcome.replicas_expected}};
    if (outcome.object) {
        value["key"] = outcome.object->key;
        value["size"] = outcome.object->size;
        value["generation"] = outcome.object->generation;
        value["updated_ms"] = outcome.object->updated_ms;
    }
    if (outcome.unchanged) value["unchanged"] = true;
    return value;
}

std::optional<ApiResult> authorize_object_route(Node& node, const std::string& acting_key,
                                                bool acting_is_user, std::string_view route,
                                                const zetta::governance::Operation& operation,
                                                const std::string& bucket) {
    bool allowed = true;
    std::string reason;
    if (acting_is_user) {
        // The flat right, or ON BUCKET <prefix> covering this bucket.
        allowed = node.authorize_object(acting_key, /*acting_is_user=*/true, operation,
                                        zetta::governance::Resource::bucket(bucket));
        if (!allowed) {
            reason = "no " + std::string(operation.action == "objects_read"    ? "objects:read"
                                         : operation.action == "objects_write" ? "objects:write"
                                                                               : "objects:manage") +
                     " grant for bucket '" + bucket + "'";
        }
    } else if (!acting_key.empty()) {
        const auto decision = node.authorize(acting_key, operation);
        allowed = static_cast<bool>(decision);
        if (!allowed) reason = decision.reason;
    }
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DataAccess;
    event.actor = acting_key.empty() ? "local" : acting_key;
    event.target = "api:" + std::string(route);
    event.allowed = allowed;
    event.detail = allowed ? "bucket " + bucket : reason;
    node.record_audit_event(std::move(event));
    if (allowed) return std::nullopt;
    return ApiResult::failure("unauthorized", reason);
}

namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

// ---- shared schema pieces ----------------------------------------------------

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required)}};
}

nlohmann::json bucket_property() {
    return {{"type", "string"},
            {"pattern", "^[a-z0-9][a-z0-9_.-]{0,95}$"},
            {"description", "the bucket's name"}};
}

nlohmann::json key_property() {
    return {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 1024},
            {"description", "the object's key: 1 to 1024 bytes, `/` as separator, no control "
                            "characters"}};
}

nlohmann::json fence_property() {
    return {{"type", "integer"},
            {"minimum", 0},
            {"description", "the bucket lease's current fence (the Zetta-Fence header on the "
                            "byte routes); required once the bucket has had a lease holder"}};
}

nlohmann::json holder_property() {
    return {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 256},
            {"description", "who holds the lease, e.g. a process identity"}};
}

nlohmann::json ttl_property() {
    return {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 86400000},
            {"description", "how long the lease lasts without a renewal, in ms"}};
}

nlohmann::json incarnation_property() {
    return {{"type", "string"},
            {"description",
             "the answering node's incarnation for this bucket: a random id chosen at the "
             "node's start, then `.` and how many times its log for the bucket was reopened "
             "after a failed fsync. A change between a write's answer and a barrier's means "
             "the write may be gone (lake#543)"}};
}

nlohmann::json expected_incarnation_property() {
    return {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 128},
            {"description",
             "a precondition (the Zetta-Incarnation header on the routes): the bucket's "
             "incarnation the caller last saw; when it differs the call is refused "
             "`incarnation_changed` (409) and nothing is applied (lake#543)"}};
}

nlohmann::json prefix_durability_property() {
    return {{"type", "integer"},
            {"minimum", 1},
            {"description",
             "the durability format of this node's bucket logs: 1 keeps a prefix of a "
             "bucket's writes, up to the last barrier, after any crash, power loss included "
             "(#1235). Absent on a node from before it"}};
}

nlohmann::json lease_output_schema() {
    return object_schema(
        {{"bucket", bucket_property()},
         {"holder", {{"type", "string"}, {"description", "empty once released"}}},
         {"fence",
          {{"type", "integer"},
           {"minimum", 0},
           {"description", "send this as Zetta-Fence on every mutation of the bucket"}}},
         {"expires_ms", {{"type", "integer"}}},
         {"incarnation", incarnation_property()},
         {"object_log_prefix_durability", prefix_durability_property()}},
        {"bucket", "holder", "fence", "expires_ms", "incarnation",
         "object_log_prefix_durability"});
}

nlohmann::json write_output_schema() {
    return object_schema(
        {{"bucket", bucket_property()},
         {"key", key_property()},
         {"size", {{"type", "integer"}, {"minimum", 0}, {"description", "the object's size now"}}},
         {"generation",
          {{"type", "integer"},
           {"minimum", 0},
           {"description", "rises with every new version of the key"}}},
         {"updated_ms", {{"type", "integer"}}},
         {"incarnation", incarnation_property()},
         {"unchanged",
          {{"type", "boolean"},
           {"description", "a retried append whose bytes were already there"}}},
         {"owner", {{"type", "string"}, {"description", "the node that ordered the write"}}},
         {"replicas",
          {{"type", "integer"}, {"minimum", 0}, {"description", "owners that hold the write"}}},
         {"replicas_expected",
          {{"type", "integer"}, {"minimum", 0}, {"description", "the owners it was placed on"}}}},
        {"bucket", "incarnation", "owner", "replicas", "replicas_expected"});
}

// A capability scoped by bucket: the flat right, or ON BUCKET <prefix>.
void add_bucket_scoped(Registry& registry, Capability capability) {
    capability.authorize_scoped = [operation = capability.required_operation](
                                      const nlohmann::json& input, const ApiContext& context) {
        const auto bucket = input.find("bucket");
        return bucket != input.end() && bucket->is_string() &&
               context.node.authorize_object(context.acting_key, context.acting_is_user,
                                             operation,
                                             zetta::governance::Resource::bucket(
                                                 bucket->get<std::string>()));
    };
    capability.blocks = true;
    registry.add(std::move(capability));
}

ApiResult no_store() {
    return ApiResult::failure("engine_error",
                              "this node has no replicated catalogue to keep buckets in");
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// ---- buckets -----------------------------------------------------------------

const nlohmann::json& create_bucket_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()},
         {"quota_bytes",
          {{"type", "integer"},
           {"minimum", 0},
           {"description", "the most object bytes the bucket may hold; a write past it is "
                           "refused (507)"}}},
         {"rf",
          {{"type", "integer"},
           {"minimum", 1},
           {"maximum", 16},
           {"description", "how many nodes hold each object on a multi-node cluster; "
                           "default min(3, nodes)"}}}},
        {"bucket"});
    return schema;
}

const nlohmann::json& create_bucket_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket",
          object_schema({{"name", bucket_property()},
                         {"quota_bytes", {{"type", "integer"}}},
                         {"rf", {{"type", "integer"}}},
                         {"created_ms", {{"type", "integer"}}}},
                        {"name", "created_ms"})},
         {"created", {{"type", "boolean"},
                      {"description", "false when an identical bucket already existed"}}}},
        {"bucket", "created"});
    return schema;
}

ApiResult handle_create_bucket(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    zetta::data::ObjectBucket bucket;
    bucket.name = input.at("bucket").get<std::string>();
    if (input.contains("quota_bytes")) bucket.quota_bytes = input.at("quota_bytes").get<std::uint64_t>();
    if (input.contains("rf")) bucket.rf = input.at("rf").get<std::uint32_t>();
    const ObjectOutcome outcome = store->create_bucket(bucket);
    if (!outcome.ok()) return object_failure(outcome);
    nlohmann::json definition{{"name", outcome.bucket->name},
                              {"created_ms", outcome.bucket->created_ms}};
    if (outcome.bucket->quota_bytes) definition["quota_bytes"] = *outcome.bucket->quota_bytes;
    if (outcome.bucket->rf) definition["rf"] = *outcome.bucket->rf;
    return ApiResult::success({{"bucket", std::move(definition)}, {"created", outcome.created}});
}

const nlohmann::json& list_objects_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()},
         {"prefix", {{"type", "string"}, {"maxLength", 1024},
                     {"description", "list only keys that start with this"}}},
         {"after", {{"type", "string"}, {"maxLength", 1024},
                    {"description", "the next_after a previous page returned"}}},
         {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}}},
        {"bucket"});
    return schema;
}

const nlohmann::json& list_objects_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"objects",
          {{"type", "array"},
           {"items", object_schema({{"key", key_property()},
                                    {"size", {{"type", "integer"}, {"minimum", 0}}},
                                    {"updated_ms", {{"type", "integer"}}}},
                                   {"key", "size", "updated_ms"})}}},
         {"next_after", {{"type", "string"},
                         {"description", "pass as `after` for the next page; absent when the "
                                         "listing is complete"}}}},
        {"objects"});
    return schema;
}

ApiResult handle_list_objects(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const zetta::data::ObjectPage page =
        store->list(input.at("bucket").get<std::string>(), input.value("prefix", std::string{}),
                    input.value("after", std::string{}), input.value("limit", std::size_t{1000}));
    if (!page.ok()) {
        return ApiResult::failure(object_error_code(page.status), page.message, page.path);
    }
    nlohmann::json objects = nlohmann::json::array();
    for (const zetta::data::ObjectInfo& object : page.objects) {
        objects.push_back(
            {{"key", object.key}, {"size", object.size}, {"updated_ms", object.updated_ms}});
    }
    nlohmann::json output{{"objects", std::move(objects)}};
    if (!page.next_after.empty()) output["next_after"] = page.next_after;
    return ApiResult::success(std::move(output));
}

const nlohmann::json& bucket_only_schema() {
    static const nlohmann::json schema = object_schema({{"bucket", bucket_property()}}, {"bucket"});
    return schema;
}

const nlohmann::json& usage_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"objects", {{"type", "integer"}, {"minimum", 0}}},
         {"bytes", {{"type", "integer"}, {"minimum", 0}}},
         {"quota_bytes", {{"type", "integer"}, {"minimum", 0}}}},
        {"objects", "bytes"});
    return schema;
}

ApiResult handle_object_usage(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const zetta::data::ObjectUsage usage = store->usage(input.at("bucket").get<std::string>());
    if (!usage.ok()) return ApiResult::failure(object_error_code(usage.status), usage.message);
    nlohmann::json output{{"objects", usage.objects}, {"bytes", usage.bytes}};
    if (usage.quota_bytes) output["quota_bytes"] = *usage.quota_bytes;
    return ApiResult::success(std::move(output));
}

const nlohmann::json& barrier_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()}, {"incarnation", expected_incarnation_property()}},
        {"bucket"});
    return schema;
}

const nlohmann::json& barrier_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()},
         {"nodes", {{"type", "integer"}, {"minimum", 1},
                    {"description", "the nodes that synced the bucket"}}},
         {"incarnation", incarnation_property()},
         {"object_log_prefix_durability", prefix_durability_property()}},
        {"bucket", "nodes", "incarnation", "object_log_prefix_durability"});
    return schema;
}

ApiResult handle_object_barrier(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const std::string bucket = input.at("bucket").get<std::string>();
    const ObjectOutcome outcome =
        store->barrier(bucket, input.value("incarnation", std::string()));
    if (!outcome.ok()) return object_failure(outcome);
    return ApiResult::success({{"bucket", bucket},
                               {"nodes", outcome.nodes},
                               {"incarnation", outcome.incarnation},
                               {"object_log_prefix_durability",
                                zetta::data::ObjectStore::kPrefixDurability}});
}

const nlohmann::json& delete_object_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()},
         {"key", key_property()},
         {"fence", fence_property()},
         {"durable", {{"type", "boolean"}, {"description", "fsync before answering"}}},
         {"incarnation", expected_incarnation_property()}},
        {"bucket", "key"});
    return schema;
}

ApiResult handle_delete_object(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const std::string bucket = input.at("bucket").get<std::string>();
    zetta::data::ObjectWrite options;
    if (input.contains("fence")) options.fence = input.at("fence").get<std::uint64_t>();
    options.durable = input.value("durable", false);
    options.incarnation = input.value("incarnation", std::string());
    const ObjectOutcome outcome = store->remove(bucket, input.at("key").get<std::string>(), options);
    if (!outcome.ok()) return object_failure(outcome);
    return ApiResult::success(object_write_value(bucket, outcome));
}

// ---- the owner lease -------------------------------------------------------------

const nlohmann::json& acquire_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()},
         {"holder", holder_property()},
         {"ttl_ms", ttl_property()},
         {"wait_ms",
          {{"type", "integer"},
           {"minimum", 0},
           {"maximum", 600000},
           {"description", "wait this long for the current holder to release, or for its "
                           "lease to expire (0: answer at once)"}}}},
        {"bucket", "holder", "ttl_ms"});
    return schema;
}

ApiResult handle_acquire(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const std::string bucket = input.at("bucket").get<std::string>();
    // Registered before the attempt, so a release between the attempt and the
    // route's wait cannot be missed.
    if (const std::shared_ptr<runtime::async_event> wake = context.wait_event) {
        store->watch_lease(bucket, [wake] { wake->set(); });
    }
    const std::string incarnation = store->incarnation(bucket);
    const ObjectOutcome outcome = store->acquire(bucket, input.at("holder").get<std::string>(),
                                                 input.at("ttl_ms").get<std::int64_t>());
    if (!outcome.ok()) return object_failure(outcome);
    return ApiResult::success(lease_value(*outcome.lease, incarnation));
}

const nlohmann::json& renew_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()},
         {"holder", holder_property()},
         {"fence", fence_property()},
         {"ttl_ms", ttl_property()}},
        {"bucket", "holder", "fence", "ttl_ms"});
    return schema;
}

ApiResult handle_renew(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const std::string bucket = input.at("bucket").get<std::string>();
    const std::string incarnation = store->incarnation(bucket);
    const ObjectOutcome outcome =
        store->renew(bucket, input.at("holder").get<std::string>(),
                     input.at("fence").get<std::uint64_t>(), input.at("ttl_ms").get<std::int64_t>());
    if (!outcome.ok()) return object_failure(outcome);
    return ApiResult::success(lease_value(*outcome.lease, incarnation));
}

const nlohmann::json& release_schema() {
    static const nlohmann::json schema = object_schema(
        {{"bucket", bucket_property()}, {"holder", holder_property()}, {"fence", fence_property()}},
        {"bucket", "holder", "fence"});
    return schema;
}

ApiResult handle_release(const nlohmann::json& input, ApiContext& context) {
    zetta::data::ObjectStore* store = context.node.object_store();
    if (!store) return no_store();
    const std::string bucket = input.at("bucket").get<std::string>();
    const std::string incarnation = store->incarnation(bucket);
    const ObjectOutcome outcome =
        store->release(bucket, input.at("holder").get<std::string>(),
                       input.at("fence").get<std::uint64_t>());
    if (!outcome.ok()) return object_failure(outcome);
    return ApiResult::success(lease_value(*outcome.lease, incarnation));
}

// ---- OpenAPI for the byte routes and the aliases ----------------------------------------

nlohmann::json path_parameter(std::string name, std::string description) {
    return {{"name", std::move(name)},
            {"in", "path"},
            {"required", true},
            {"description", std::move(description)},
            {"schema", {{"type", "string"}}}};
}

nlohmann::json query_parameter(std::string name, nlohmann::json schema, bool required,
                               std::string description) {
    return {{"name", std::move(name)},
            {"in", "query"},
            {"required", required},
            {"description", std::move(description)},
            {"schema", std::move(schema)}};
}

nlohmann::json json_envelope(const nlohmann::json& value_schema) {
    return {{"application/json",
             {{"schema",
               {{"type", "object"},
                {"required", {"ok", "value"}},
                {"properties", {{"ok", {{"type", "boolean"}, {"const", true}}},
                                {"value", value_schema},
                                {"trace_id", {{"type", "string"}}},
                                {"span_id", {{"type", "string"}}},
                                {"duration_ms", {{"type", "number"}}}}}}}}}};
}

nlohmann::json error_reply(std::string description) {
    return {{"description", std::move(description)},
            {"content",
             {{"application/json", {{"schema", {{"$ref", "#/components/schemas/error_envelope"}}}}}}}};
}

nlohmann::json strip_bucket(nlohmann::json schema) {
    schema["properties"].erase("bucket");
    auto& required = schema["required"];
    required.erase(std::remove(required.begin(), required.end(), "bucket"), required.end());
    return schema;
}

}  // namespace

nlohmann::json object_route_paths() {
    const nlohmann::json bucket = path_parameter("bucket", "the bucket's name");
    const nlohmann::json key = path_parameter(
        "key", "the object's key; its `/` separators may be sent literally or percent-encoded");
    const nlohmann::json fence_header{
        {"name", "Zetta-Fence"},
        {"in", "header"},
        {"required", false},
        {"description", "the bucket lease's current fence; required once the bucket has had a "
                        "lease holder (409 `fenced` otherwise)"},
        {"schema", {{"type", "integer"}, {"minimum", 0}}}};
    const nlohmann::json incarnation_header{
        {"name", "Zetta-Incarnation"},
        {"in", "header"},
        {"required", false},
        {"description", "the bucket's incarnation the caller last saw; when the bucket's is "
                        "another, 409 `incarnation_changed` (details.incarnation) and nothing is "
                        "applied (lake#543)"},
        {"schema", {{"type", "string"}}}};
    const nlohmann::json durable = query_parameter(
        "durable", {{"type", "boolean"}}, false,
        "fsync before answering (otherwise the barrier is the durability point)");
    const nlohmann::json octets{
        {"application/octet-stream", {{"schema", {{"type", "string"}, {"format", "binary"}}}}}};
    const auto write_responses = [&](std::string conflict) {
        return nlohmann::json{
            {"200", {{"description", "every owner holds the write"},
                     {"content", json_envelope(write_output_schema())}}},
            {"409", error_reply(std::move(conflict))},
            {"507", error_reply("the write would take the bucket past its quota "
                                "(`quota_exceeded`)")},
            {"4XX", error_reply("the input or authorization is at fault, or no such bucket")},
            {"503", error_reply("an owner did not answer, or not every owner holds the write "
                                "(`busy`): retry")}};
    };
    const auto tagged = [](nlohmann::json operation) {
        operation["tags"] = nlohmann::json::array({"objects"});
        return operation;
    };

    nlohmann::json paths = nlohmann::json::object();
    nlohmann::json& object = paths["/v1/objects/{bucket}/{key}"];
    object["put"] = tagged({
        {"operationId", "objects_put"},
        {"summary", "Put an object"},
        {"description",
         "Atomically replace the whole object with the request body, streamed: readers see the "
         "old bytes or the new ones, never a mix. `if_absent=true` creates it only when the key "
         "is absent (409 conflict otherwise). Requires data:objects_write (objects:write, "
         "object-scopable ON BUCKET)."},
        {"parameters",
         {bucket, key, fence_header, incarnation_header, durable,
          query_parameter("if_absent", {{"type", "boolean"}}, false,
                          "create only when the key is absent")}},
        {"requestBody", {{"required", true}, {"content", octets}}},
        {"responses", write_responses("`conflict`: the key exists (if_absent); `fenced`: a stale "
                                      "or missing Zetta-Fence (details.fence); "
                                      "`incarnation_changed`: see Zetta-Incarnation")}});
    object["patch"] = tagged({
        {"operationId", "objects_put_range"},
        {"summary", "Put a range of an object"},
        {"description",
         "Create, or extend, the object at exactly `offset` with the request body. `offset` equal "
         "to the size appends; a range inside the object equal to the bytes already there "
         "succeeds and changes nothing, so an append can be retried. Requires "
         "data:objects_write."},
        {"parameters",
         {bucket, key, fence_header, incarnation_header, durable,
          query_parameter("offset", {{"type", "integer"}, {"minimum", 0}}, true,
                          "where the bytes go: the object's size to append")}},
        {"requestBody", {{"required", true}, {"content", octets}}},
        {"responses", write_responses("`conflict`: any other range (details.size is the object's "
                                      "size); `fenced`: a stale or missing Zetta-Fence; "
                                      "`incarnation_changed`: see Zetta-Incarnation")}});
    object["get"] = tagged({
        {"operationId", "objects_get"},
        {"summary", "Get an object or a range of it"},
        {"description",
         "Stream the object, or exactly the range a `Range: bytes=a-b` header names (206). "
         "Requires data:objects_read."},
        {"parameters",
         {bucket, key,
          {{"name", "Range"},
           {"in", "header"},
           {"required", false},
           {"description", "`bytes=a-b`, `bytes=a-` or `bytes=-n`; one range"},
           {"schema", {{"type", "string"}}}}}},
        {"responses",
         {{"200", {{"description", "the whole object"}, {"content", octets}}},
          {"206", {{"description", "the range asked for"}, {"content", octets}}},
          {"404", error_reply("no such object or bucket")},
          {"416", error_reply("the range lies outside the object (details.size)")},
          {"4XX", error_reply("the input or authorization is at fault")}}}});
    object["delete"] = tagged({
        {"operationId", "objects_delete"},
        {"summary", "Remove an object"},
        {"description", "The delete_object capability at the object's route. Requires "
                        "data:objects_write."},
        {"parameters", {bucket, key, fence_header, incarnation_header, durable}},
        {"responses", write_responses("`fenced`: a stale or missing Zetta-Fence; "
                                      "`incarnation_changed`: see Zetta-Incarnation")}});

    paths["/v1/objects/{bucket}"]["get"] = tagged({
        {"operationId", "objects_list"},
        {"summary", "List objects"},
        {"description", "The list_objects capability at the bucket's route. Requires "
                        "data:objects_read."},
        {"parameters",
         {bucket,
          query_parameter("prefix", {{"type", "string"}}, false, "keys that start with this"),
          query_parameter("after", {{"type", "string"}}, false, "the previous page's next_after"),
          query_parameter("limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}},
                          false, "1 to 1000 (default 1000)")}},
        {"responses", {{"200", {{"description", "one page"},
                                {"content", json_envelope(list_objects_output_schema())}}},
                       {"4XX", error_reply("the input or authorization is at fault")}}}});
    paths["/v1/objects/{bucket}:usage"]["get"] = tagged({
        {"operationId", "objects_usage"},
        {"summary", "A bucket's usage"},
        {"description", "The object_usage capability at the bucket's route. Requires "
                        "data:objects_read."},
        {"parameters", {bucket}},
        {"responses", {{"200", {{"description", "objects, bytes and the quota"},
                                {"content", json_envelope(usage_output_schema())}}},
                       {"4XX", error_reply("the input or authorization is at fault")}}}});
    paths["/v1/objects/{bucket}:barrier"]["post"] = tagged({
        {"operationId", "objects_barrier"},
        {"summary", "Barrier: make acknowledged writes durable"},
        {"description", "The object_barrier capability at the bucket's route. Requires "
                        "data:objects_write."},
        {"parameters", {bucket, incarnation_header}},
        {"responses", {{"200", {{"description", "every live node synced the bucket"},
                                {"content", json_envelope(barrier_output_schema())}}},
                       {"409", error_reply("`incarnation_changed`: see Zetta-Incarnation")},
                       {"4XX", error_reply("the input or authorization is at fault")},
                       {"503", error_reply("a node did not confirm the barrier: retry")}}}});
    const auto lease_operation = [&](std::string id, std::string summary, std::string capability,
                                     const nlohmann::json& schema) {
        return tagged({{"operationId", std::move(id)},
                       {"summary", std::move(summary)},
                       {"description", "The " + capability +
                                           " capability at the bucket's route; the body is its "
                                           "input without `bucket`. Requires data:objects_write."},
                       {"parameters", {bucket}},
                       {"requestBody",
                        {{"required", true},
                         {"content", {{"application/json", {{"schema", strip_bucket(schema)}}}}}}},
                       {"responses",
                        {{"200", {{"description", "the lease"},
                                  {"content", json_envelope(lease_output_schema())}}},
                         {"409", error_reply("`held`: another holder has the lease (details name "
                                             "it); `fenced`: not the current lease")},
                         {"4XX", error_reply("the input or authorization is at fault")}}}});
    };
    paths["/v1/objects/{bucket}:acquire"]["post"] = lease_operation(
        "objects_acquire", "Acquire the bucket's owner lease", "acquire_object_lease",
        acquire_schema());
    paths["/v1/objects/{bucket}:renew"]["post"] =
        lease_operation("objects_renew", "Renew the owner lease", "renew_object_lease", renew_schema());
    paths["/v1/objects/{bucket}:release"]["post"] = lease_operation(
        "objects_release", "Release the owner lease", "release_object_lease", release_schema());
    return paths;
}

void register_object_capabilities(Registry& registry) {
    add_bucket_scoped(registry,
                      Capability{"create_bucket",
                                 "Create an object bucket",
                                 "Declare an object bucket with an optional byte quota and "
                                 "replication factor; idempotent for an identical definition, a "
                                 "conflict for a different one. Mutates replicated state.",
                                 create_bucket_schema(),
                                 create_bucket_output_schema(),
                                 CapabilityKind::Action,
                                 kObjectsManage,
                                 /*mutates=*/true,
                                 /*requires_consent=*/false,
                                 handle_create_bucket});
    Capability list{"list_objects",
                    "List objects",
                    "List a bucket's objects by key with their sizes, optionally under a prefix, "
                    "a page at a time. No side effects.",
                    list_objects_schema(),
                    list_objects_output_schema(),
                    CapabilityKind::Read,
                    kObjectsRead,
                    /*mutates=*/false,
                    /*requires_consent=*/false,
                    handle_list_objects};
    list.post_alias = true;
    add_bucket_scoped(registry, std::move(list));
    add_bucket_scoped(registry,
                      Capability{"object_usage",
                                 "Bucket usage",
                                 "Count a bucket's objects and bytes and report its quota. No "
                                 "side effects.",
                                 bucket_only_schema(),
                                 usage_output_schema(),
                                 CapabilityKind::Read,
                                 kObjectsRead,
                                 /*mutates=*/false,
                                 /*requires_consent=*/false,
                                 handle_object_usage});
    add_bucket_scoped(registry,
                      Capability{"object_barrier",
                                 "Object barrier",
                                 "Fsync every write to the bucket acknowledged before this call, "
                                 "on every live node; the durability point for writes made "
                                 "without durable. No other side effects.",
                                 barrier_schema(),
                                 barrier_output_schema(),
                                 CapabilityKind::Action,
                                 kObjectsWrite,
                                 /*mutates=*/false,
                                 /*requires_consent=*/false,
                                 handle_object_barrier});
    add_bucket_scoped(registry,
                      Capability{"delete_object",
                                 "Delete an object",
                                 "Remove one object (with the bucket lease's fence once it has a "
                                 "holder); on every owner before it returns. Mutates the bucket.",
                                 delete_object_schema(),
                                 write_output_schema(),
                                 CapabilityKind::Action,
                                 kObjectsWrite,
                                 /*mutates=*/true,
                                 /*requires_consent=*/false,
                                 handle_delete_object});
    Capability acquire{"acquire_object_lease",
                       "Acquire a bucket's owner lease",
                       "Take the bucket's owner lease for ttl_ms, waiting up to wait_ms for the "
                       "holder to release it or for it to expire; returns the fence every "
                       "mutation must then carry (409 held otherwise). Mutates replicated state.",
                       acquire_schema(),
                       lease_output_schema(),
                       CapabilityKind::Action,
                       kObjectsWrite,
                       /*mutates=*/true,
                       /*requires_consent=*/false,
                       handle_acquire};
    acquire.awaitable_read = AwaitableRead{
        [](const nlohmann::json& input) {
            return std::chrono::milliseconds(input.value("wait_ms", std::int64_t{0}));
        },
        [](const ApiResult& result) { return !result.ok && result.error.code == "held"; },
        // Wake at the holder's expiry at the latest; a release wakes sooner.
        [](const ApiResult& result) {
            const std::int64_t expires = result.error.details.is_object()
                                             ? result.error.details.value("expires_ms", std::int64_t{0})
                                             : 0;
            return std::chrono::milliseconds(std::max<std::int64_t>(1, expires - now_ms() + 1));
        }};
    add_bucket_scoped(registry, std::move(acquire));
    add_bucket_scoped(registry,
                      Capability{"renew_object_lease",
                                 "Renew a bucket's owner lease",
                                 "Extend the caller's lease by ttl_ms; 409 fenced when the fence "
                                 "is no longer the bucket's current one. Mutates replicated "
                                 "state.",
                                 renew_schema(),
                                 lease_output_schema(),
                                 CapabilityKind::Action,
                                 kObjectsWrite,
                                 /*mutates=*/true,
                                 /*requires_consent=*/false,
                                 handle_renew});
    add_bucket_scoped(registry,
                      Capability{"release_object_lease",
                                 "Release a bucket's owner lease",
                                 "Give the lease up so a waiting acquirer gets it at once; the "
                                 "fence stays until the next holder raises it. Mutates "
                                 "replicated state.",
                                 release_schema(),
                                 lease_output_schema(),
                                 CapabilityKind::Action,
                                 kObjectsWrite,
                                 /*mutates=*/true,
                                 /*requires_consent=*/false,
                                 handle_release});
}

}  // namespace zetta::api
