// SPDX-License-Identifier: MIT

// The keyed record store and labelled time series on the control surface (#1231;
// docs/data/record-store.md, docs/data/time-series.md). Each entry is one
// registry capability: the REST route, the MCP tool and the OpenAPI operation.
//
// Every handler is a thin caller of zetta::data::RecordStore / MetricStore. They
// are `blocks` capabilities: the registry authorizes and audits on the owner
// loop, then runs the handler on the blocking pool, because a write waits for the
// key's owner and for every live peer to install it.

#include <utility>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/data/metric_store.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/governance/scope.hpp"
#include "zetta/node.hpp"

namespace zetta::api {
namespace {

using zetta::data::MetricStatus;
using zetta::data::RecordStatus;
using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

// ---- shared schema pieces ----------------------------------------------------

nlohmann::json object_schema(nlohmann::json properties, nlohmann::json required) {
    return {{"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required)}};
}

nlohmann::json collection_property() {
    return {{"type", "string"},
            {"pattern", "^[a-z0-9][a-z0-9_.-]{0,95}$"},
            {"description", "the record collection's name"}};
}

nlohmann::json key_property() {
    return {{"type", "string"},
            {"minLength", 1},
            {"maxLength", 512},
            {"description", "the record's key: 1 to 512 bytes, no control characters"}};
}

nlohmann::json index_name_property() {
    return {{"type", "string"}, {"pattern", "^[a-z0-9][a-z0-9_]{0,63}$"}};
}

nlohmann::json revision_property(std::string description) {
    return {{"type", "integer"}, {"minimum", 1}, {"description", std::move(description)}};
}

nlohmann::json index_values_property() {
    return {{"type", "object"},
            {"maxProperties", 8},
            {"propertyNames", index_name_property()},
            {"additionalProperties", {{"type", "string"}, {"maxLength", 512}}},
            {"description", "this record's value in each declared index it belongs to"}};
}

nlohmann::json collection_definition_schema() {
    return object_schema(
        {{"name", collection_property()},
         {"indexes", {{"type", "array"}, {"items", index_name_property()}}},
         {"retention_ms", {{"type", "integer"}, {"minimum", 1}}},
         {"created_ms", {{"type", "integer"}}}},
        {"name", "indexes", "created_ms"});
}

nlohmann::json record_output_schema(bool with_value) {
    nlohmann::json properties{
        {"collection", collection_property()},
        {"key", key_property()},
        {"revision", revision_property("the record's revision; pass it as if_revision")},
        {"index", index_values_property()},
        {"order", {{"type", "string"}}},
        {"updated_ms", {{"type", "integer"}}},
        {"expires_ms", {{"type", "integer"}}}};
    nlohmann::json required{"collection", "key", "revision", "index", "order", "updated_ms"};
    if (with_value) {
        properties["value"] = {{"type", "string"},
                               {"description", "the record's bytes, base64"}};
        required.push_back("value");
    }
    return object_schema(std::move(properties), std::move(required));
}

nlohmann::json write_output_schema() {
    return object_schema(
        {{"collection", collection_property()},
         {"key", key_property()},
         {"revision", revision_property("the revision this write created")},
         {"updated_ms", {{"type", "integer"}}},
         {"expires_ms", {{"type", "integer"}}},
         {"owner", {{"type", "string"},
                    {"description", "the node that ordered this write"}}},
         {"replicas", {{"type", "integer"}, {"minimum", 0},
                       {"description", "live peers that installed the write before it "
                                       "was acknowledged"}}},
         {"replicas_expected", {{"type", "integer"}, {"minimum", 0},
                                {"description", "live peers when it was written"}}}},
        {"collection", "key", "revision", "updated_ms", "owner", "replicas",
         "replicas_expected"});
}

// ---- governance --------------------------------------------------------------

// A records/metrics right held only for some collections: ON COLLECTION <prefix>.
void add_collection_scoped(Registry& registry, Capability capability) {
    capability.authorize_scoped = [operation = capability.required_operation](
                                      const nlohmann::json& input,
                                      const ApiContext& context) {
        const auto collection = input.find("collection");
        return collection != input.end() && collection->is_string() &&
               context.node.authorize_object(
                   context.acting_key, context.acting_is_user, operation,
                   zetta::governance::Resource::collection(collection->get<std::string>()));
    };
    capability.blocks = true;
    registry.add(std::move(capability));
}

// ---- results -----------------------------------------------------------------

std::string code_of(RecordStatus status) {
    switch (status) {
        case RecordStatus::Ok: return "ok";
        case RecordStatus::InvalidInput: return "invalid_input";
        case RecordStatus::NotFound: return "not_found";
        case RecordStatus::Conflict: return "conflict";
        case RecordStatus::Unavailable: return "busy";
    }
    return "engine_error";
}

ApiResult record_failure(const zetta::data::RecordOutcome& outcome) {
    ApiResult result = ApiResult::failure(code_of(outcome.status), outcome.message, outcome.path);
    if (outcome.status == RecordStatus::Conflict) {
        // What the caller needs to retry: the live revision, when there is one.
        nlohmann::json details = nlohmann::json::object();
        if (outcome.record) details["revision"] = outcome.record->revision;
        if (outcome.collection) details["collection"] = to_json(*outcome.collection);
        result.error.details = std::move(details);
    }
    return result;
}

ApiResult no_store() {
    return ApiResult::failure("engine_error",
                              "this node has no replicated catalogue to keep records in");
}

nlohmann::json written(const zetta::data::RecordOutcome& outcome) {
    const zetta::data::StoredRecord& record = *outcome.record;
    nlohmann::json value{{"collection", record.collection},
                         {"key", record.key},
                         {"revision", record.revision},
                         {"updated_ms", record.updated_ms},
                         {"owner", outcome.owner},
                         {"replicas", outcome.replicas},
                         {"replicas_expected", outcome.replicas_expected}};
    if (record.expires_ms) value["expires_ms"] = *record.expires_ms;
    return value;
}

// ---- records -----------------------------------------------------------------

const nlohmann::json& create_collection_schema() {
    static const nlohmann::json schema = object_schema(
        {{"collection", collection_property()},
         {"indexes",
          {{"type", "array"},
           {"maxItems", 8},
           {"uniqueItems", true},
           {"items", index_name_property()},
           {"description", "secondary indexes a record may carry a value for; "
                           "list_records lists one index value"}}},
         {"retention_ms",
          {{"type", "integer"},
           {"minimum", 1},
           {"description", "expire a record this long after its last write"}}}},
        {"collection"});
    return schema;
}

const nlohmann::json& create_collection_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"collection", collection_definition_schema()},
         {"created", {{"type", "boolean"},
                      {"description", "false when an identical collection already existed"}}}},
        {"collection", "created"});
    return schema;
}

ApiResult handle_create_collection(const nlohmann::json& input, ApiContext& context) {
    zetta::data::RecordStore* store = context.node.record_store();
    if (!store) return no_store();
    zetta::data::RecordCollection collection;
    collection.name = input.at("collection").get<std::string>();
    collection.indexes = input.value("indexes", std::vector<std::string>{});
    if (input.contains("retention_ms")) {
        collection.retention_ms = input.at("retention_ms").get<std::int64_t>();
    }
    const zetta::data::RecordOutcome outcome = store->create_collection(collection);
    if (!outcome.ok()) return record_failure(outcome);
    nlohmann::json definition = to_json(*outcome.collection);
    definition["created_ms"] = definition.at("created");
    definition.erase("created");
    return ApiResult::success({{"collection", std::move(definition)},
                               {"created", outcome.created}});
}

const nlohmann::json& put_record_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json value = object_schema(
            {{"collection", collection_property()},
             {"key", key_property()},
             {"value",
              {{"type", "string"},
               {"maxLength", 1048576},
               {"description", "the record's bytes, base64; the engine never reads them"}}},
             {"index", index_values_property()},
             {"order",
              {{"type", "string"},
               {"maxLength", 256},
               {"description", "orders records within an index value (then by key); "
                               "for example a zero-padded creation time"}}},
             {"if_absent",
              {{"type", "boolean"},
               {"description", "write only when the key has no live record"}}},
             {"if_revision",
              revision_property("write only when the live record is at this revision")}},
            {"collection", "key", "value"});
        value["not"] = {{"required", {"if_absent", "if_revision"}}};
        return value;
    }();
    return schema;
}

ApiResult handle_put_record(const nlohmann::json& input, ApiContext& context) {
    zetta::data::RecordStore* store = context.node.record_store();
    if (!store) return no_store();
    zetta::data::RecordWrite write;
    write.collection = input.at("collection").get<std::string>();
    write.key = input.at("key").get<std::string>();
    write.value = input.at("value").get<std::string>();
    write.order = input.value("order", std::string{});
    if (input.contains("index")) {
        for (const auto& [name, value] : input.at("index").items()) {
            write.index[name] = value.get<std::string>();
        }
    }
    if (input.value("if_absent", false)) write.condition = zetta::data::RecordCondition::IfAbsent;
    if (input.contains("if_revision")) {
        write.condition = zetta::data::RecordCondition::IfRevision;
        write.revision = input.at("if_revision").get<std::uint64_t>();
    }
    const zetta::data::RecordOutcome outcome = store->write(write);
    if (!outcome.ok()) return record_failure(outcome);
    return ApiResult::success(written(outcome));
}

const nlohmann::json& get_record_schema() {
    static const nlohmann::json schema =
        object_schema({{"collection", collection_property()}, {"key", key_property()}},
                      {"collection", "key"});
    return schema;
}

const nlohmann::json& get_record_output_schema() {
    static const nlohmann::json schema =
        object_schema({{"record", record_output_schema(true)}}, {"record"});
    return schema;
}

ApiResult handle_get_record(const nlohmann::json& input, ApiContext& context) {
    zetta::data::RecordStore* store = context.node.record_store();
    if (!store) return no_store();
    const zetta::data::RecordOutcome outcome = store->get(
        input.at("collection").get<std::string>(), input.at("key").get<std::string>());
    if (!outcome.ok()) return record_failure(outcome);
    return ApiResult::success({{"record", to_json(*outcome.record)}});
}

const nlohmann::json& delete_record_schema() {
    static const nlohmann::json schema = object_schema(
        {{"collection", collection_property()},
         {"key", key_property()},
         {"if_revision",
          revision_property("delete only when the live record is at this revision")}},
        {"collection", "key"});
    return schema;
}

ApiResult handle_delete_record(const nlohmann::json& input, ApiContext& context) {
    zetta::data::RecordStore* store = context.node.record_store();
    if (!store) return no_store();
    zetta::data::RecordWrite write;
    write.collection = input.at("collection").get<std::string>();
    write.key = input.at("key").get<std::string>();
    write.remove = true;
    if (input.contains("if_revision")) {
        write.condition = zetta::data::RecordCondition::IfRevision;
        write.revision = input.at("if_revision").get<std::uint64_t>();
    }
    const zetta::data::RecordOutcome outcome = store->write(write);
    if (!outcome.ok()) return record_failure(outcome);
    return ApiResult::success(written(outcome));
}

const nlohmann::json& list_records_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json value = object_schema(
            {{"collection", collection_property()},
             {"index",
              {{"type", "string"},
               {"pattern", "^[a-z0-9][a-z0-9_]{0,63}$"},
               {"description", "a declared index; omit to list every record by key"}}},
             {"value",
              {{"type", "string"},
               {"maxLength", 512},
               {"description", "the indexed value to list (required with index)"}}},
             {"after",
              {{"type", "string"},
               {"maxLength", 4096},
               {"description", "the next_after a previous page returned"}}},
             {"limit", {{"type", "integer"}, {"minimum", 1}, {"maximum", 1000}}},
             {"direction", {{"enum", {"asc", "desc"}}}}},
            {"collection"});
        value["dependencies"] = {{"index", {"value"}}, {"value", {"index"}}};
        return value;
    }();
    return schema;
}

const nlohmann::json& list_records_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"records", {{"type", "array"}, {"items", record_output_schema(true)}}},
         {"next_after", {{"type", "string"},
                         {"description", "pass as `after` for the next page; absent "
                                         "when the listing is complete"}}}},
        {"records"});
    return schema;
}

ApiResult handle_list_records(const nlohmann::json& input, ApiContext& context) {
    zetta::data::RecordStore* store = context.node.record_store();
    if (!store) return no_store();
    zetta::data::RecordListRequest request;
    request.collection = input.at("collection").get<std::string>();
    request.index = input.value("index", std::string{});
    request.value = input.value("value", std::string{});
    request.after = input.value("after", std::string{});
    request.limit = input.value("limit", 100);
    request.descending = input.value("direction", std::string("asc")) == "desc";
    const zetta::data::RecordPage page = store->list(request);
    if (!page.ok()) return ApiResult::failure(code_of(page.status), page.message, page.path);
    nlohmann::json records = nlohmann::json::array();
    for (const zetta::data::StoredRecord& record : page.records) records.push_back(to_json(record));
    nlohmann::json output{{"records", std::move(records)}};
    if (!page.next_after.empty()) output["next_after"] = page.next_after;
    return ApiResult::success(std::move(output));
}

// ---- time series -------------------------------------------------------------

nlohmann::json metric_property() {
    return {{"type", "string"},
            {"pattern", "^[a-z0-9][a-z0-9_.-]{0,127}$"},
            {"description", "the metric's name"}};
}

nlohmann::json labels_property() {
    return {{"type", "object"},
            {"maxProperties", 16},
            {"propertyNames", {{"type", "string"}, {"pattern", "^[a-z_][a-z0-9_]{0,63}$"}}},
            {"additionalProperties", {{"type", "string"}, {"maxLength", 256}}}};
}

nlohmann::json metric_definition_output() {
    return object_schema(
        {{"metric", metric_property()},
         {"kind", {{"enum", {"counter", "gauge"}}}},
         {"resolution_ms", {{"type", "integer"}}},
         {"retention_ms", {{"type", "integer"}}},
         {"created_ms", {{"type", "integer"}}}},
        {"metric", "kind", "resolution_ms", "created_ms"});
}

nlohmann::json definition_output(const zetta::data::MetricDefinition& definition) {
    nlohmann::json value{{"metric", definition.name},
                         {"kind", to_string(definition.kind)},
                         {"resolution_ms", definition.resolution_ms},
                         {"created_ms", definition.created_ms}};
    if (definition.retention_ms) value["retention_ms"] = *definition.retention_ms;
    return value;
}

std::string code_of(MetricStatus status) {
    switch (status) {
        case MetricStatus::Ok: return "ok";
        case MetricStatus::InvalidInput: return "invalid_input";
        case MetricStatus::NotFound: return "not_found";
        case MetricStatus::Conflict: return "conflict";
    }
    return "engine_error";
}

ApiResult metric_failure(const zetta::data::MetricOutcome& outcome) {
    return ApiResult::failure(code_of(outcome.status), outcome.message, outcome.path);
}

const nlohmann::json& create_metric_schema() {
    static const nlohmann::json schema = object_schema(
        {{"metric", metric_property()},
         {"kind",
          {{"enum", {"counter", "gauge"}},
           {"description", "counter: non-negative increments, summed; gauge: readings"}}},
         {"resolution_ms",
          {{"type", "integer"},
           {"minimum", 1000},
           {"maximum", 86400000},
           {"description", "bucket width (default 60000)"}}},
         {"retention_ms",
          {{"type", "integer"},
           {"minimum", 1},
           {"description", "remove buckets older than this"}}}},
        {"metric", "kind"});
    return schema;
}

const nlohmann::json& create_metric_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"metric", metric_definition_output()}, {"created", {{"type", "boolean"}}}},
        {"metric", "created"});
    return schema;
}

ApiResult handle_create_metric(const nlohmann::json& input, ApiContext& context) {
    zetta::data::MetricStore* store = context.node.metric_store();
    if (!store) return no_store();
    zetta::data::MetricDefinition definition;
    definition.name = input.at("metric").get<std::string>();
    definition.kind = *zetta::data::metric_kind_from_string(input.at("kind").get<std::string>());
    definition.resolution_ms = input.value("resolution_ms", std::int64_t{60'000});
    if (input.contains("retention_ms")) {
        definition.retention_ms = input.at("retention_ms").get<std::int64_t>();
    }
    const zetta::data::MetricOutcome outcome = store->create(definition);
    if (!outcome.ok()) return metric_failure(outcome);
    return ApiResult::success({{"metric", definition_output(*outcome.definition)},
                               {"created", outcome.created}});
}

const nlohmann::json& record_metric_schema() {
    static const nlohmann::json schema = object_schema(
        {{"metric", metric_property()},
         {"points",
          {{"type", "array"},
           {"minItems", 1},
           {"maxItems", 1000},
           {"items",
            object_schema({{"labels", labels_property()},
                           {"value", {{"type", "number"}}},
                           {"at_ms", {{"type", "integer"},
                                      {"minimum", 0},
                                      {"description", "default: now"}}}},
                          {"value"})}}}},
        {"metric", "points"});
    return schema;
}

const nlohmann::json& record_metric_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"metric", metric_property()}, {"recorded", {{"type", "integer"}, {"minimum", 0}}}},
        {"metric", "recorded"});
    return schema;
}

ApiResult handle_record_metric(const nlohmann::json& input, ApiContext& context) {
    zetta::data::MetricStore* store = context.node.metric_store();
    if (!store) return no_store();
    std::vector<zetta::data::MetricPoint> points;
    for (const nlohmann::json& entry : input.at("points")) {
        zetta::data::MetricPoint point;
        point.value = entry.at("value").get<double>();
        if (entry.contains("labels")) {
            for (const auto& [name, value] : entry.at("labels").items()) {
                point.labels[name] = value.get<std::string>();
            }
        }
        if (entry.contains("at_ms")) point.at_ms = entry.at("at_ms").get<std::int64_t>();
        points.push_back(std::move(point));
    }
    const std::string metric = input.at("metric").get<std::string>();
    const zetta::data::MetricOutcome outcome = store->record(metric, points);
    if (!outcome.ok()) return metric_failure(outcome);
    return ApiResult::success({{"metric", metric}, {"recorded", outcome.recorded}});
}

const nlohmann::json& query_metric_schema() {
    static const nlohmann::json schema = object_schema(
        {{"metric", metric_property()},
         {"from_ms", {{"type", "integer"}, {"minimum", 0},
                      {"description", "inclusive; rounded down to a bucket"}}},
         {"to_ms", {{"type", "integer"}, {"minimum", 1}, {"description", "exclusive"}}},
         {"labels", labels_property()},
         {"group_by",
          {{"type", "array"},
           {"maxItems", 16},
           {"uniqueItems", true},
           {"items", {{"type", "string"}, {"pattern", "^[a-z_][a-z0-9_]{0,63}$"}}}}},
         {"step_ms",
          {{"type", "integer"},
           {"minimum", 1000},
           {"description", "a multiple of the resolution; default: one step for the range"}}},
         {"aggregate",
          {{"enum", {"sum", "last", "avg", "min", "max"}},
           {"description", "counter: sum; gauge: last (default), sum, avg, min or max"}}}},
        {"metric", "from_ms", "to_ms"});
    return schema;
}

const nlohmann::json& query_metric_output_schema() {
    static const nlohmann::json schema = object_schema(
        {{"metric", metric_definition_output()},
         {"aggregate", {{"type", "string"}}},
         {"series",
          {{"type", "array"},
           {"items",
            object_schema(
                {{"labels", labels_property()},
                 {"total", {{"type", "number"}}},
                 {"points",
                  {{"type", "array"},
                   {"items", object_schema({{"at_ms", {{"type", "integer"}}},
                                            {"value", {{"type", "number"}}}},
                                           {"at_ms", "value"})}}}},
                {"labels", "total", "points"})}}}},
        {"metric", "aggregate", "series"});
    return schema;
}

ApiResult handle_query_metric(const nlohmann::json& input, ApiContext& context) {
    zetta::data::MetricStore* store = context.node.metric_store();
    if (!store) return no_store();
    zetta::data::MetricQuery query;
    query.metric = input.at("metric").get<std::string>();
    query.from_ms = input.at("from_ms").get<std::int64_t>();
    query.to_ms = input.at("to_ms").get<std::int64_t>();
    if (input.contains("labels")) {
        for (const auto& [name, value] : input.at("labels").items()) {
            query.labels[name] = value.get<std::string>();
        }
    }
    query.group_by = input.value("group_by", std::vector<std::string>{});
    if (input.contains("step_ms")) query.step_ms = input.at("step_ms").get<std::int64_t>();
    query.aggregate = input.value("aggregate", std::string{});
    const zetta::data::MetricOutcome outcome = store->query(query);
    if (!outcome.ok()) return metric_failure(outcome);
    nlohmann::json series = nlohmann::json::array();
    for (const zetta::data::MetricSeriesResult& result : outcome.series) {
        nlohmann::json points = nlohmann::json::array();
        for (const auto& [at, value] : result.points) {
            points.push_back({{"at_ms", at}, {"value", value}});
        }
        series.push_back(
            {{"labels", result.labels}, {"total", result.total}, {"points", std::move(points)}});
    }
    std::string aggregate = query.aggregate;
    if (aggregate.empty()) {
        aggregate = outcome.definition->kind == zetta::data::MetricKind::Counter ? "sum" : "last";
    }
    return ApiResult::success({{"metric", definition_output(*outcome.definition)},
                               {"aggregate", aggregate},
                               {"series", std::move(series)}});
}

}  // namespace

void register_record_capabilities(Registry& registry) {
    add_collection_scoped(
        registry,
        Capability{"create_collection",
                   "Create a record collection",
                   "Declare a keyed record collection with its secondary indexes and "
                   "optional retention; idempotent for an identical definition, a "
                   "conflict for a different one. Mutates replicated state.",
                   create_collection_schema(),
                   create_collection_output_schema(),
                   CapabilityKind::Action,
                   Operation{CapabilityDomain::Orchestration, "create_collection"},
                   /*mutates=*/true,
                   /*requires_consent=*/false,
                   handle_create_collection});
    add_collection_scoped(
        registry,
        Capability{"put_record",
                   "Put a record",
                   "Write one record's opaque base64 value under (collection, key), "
                   "optionally only if absent or only at a revision (409 conflict "
                   "otherwise); durable and on every live node before it returns. "
                   "Mutates replicated state.",
                   put_record_schema(),
                   write_output_schema(),
                   CapabilityKind::Action,
                   Operation{CapabilityDomain::Data, "records_write"},
                   /*mutates=*/true,
                   /*requires_consent=*/false,
                   handle_put_record});
    Capability get{"get_record",
                   "Get a record",
                   "Read one record's value and revision by (collection, key) from "
                   "this node. No side effects.",
                   get_record_schema(),
                   get_record_output_schema(),
                   CapabilityKind::Read,
                   Operation{CapabilityDomain::Data, "records_read"},
                   /*mutates=*/false,
                   /*requires_consent=*/false,
                   handle_get_record};
    get.post_alias = true;
    add_collection_scoped(registry, std::move(get));
    add_collection_scoped(
        registry,
        Capability{"delete_record",
                   "Delete a record",
                   "Delete one record, optionally only at a revision (409 conflict "
                   "otherwise); durable and on every live node before it returns. "
                   "Mutates replicated state.",
                   delete_record_schema(),
                   write_output_schema(),
                   CapabilityKind::Action,
                   Operation{CapabilityDomain::Data, "records_write"},
                   /*mutates=*/true,
                   /*requires_consent=*/false,
                   handle_delete_record});
    Capability list{"list_records",
                    "List records",
                    "List one collection's records with one value of a declared index, "
                    "ordered by the record's order then key, a page at a time; without "
                    "an index, every record by key. No side effects.",
                    list_records_schema(),
                    list_records_output_schema(),
                    CapabilityKind::Read,
                    Operation{CapabilityDomain::Data, "records_read"},
                    /*mutates=*/false,
                    /*requires_consent=*/false,
                    handle_list_records};
    list.post_alias = true;
    add_collection_scoped(registry, std::move(list));

    Capability create_metric{"create_metric",
                             "Create a metric",
                             "Declare a labelled counter or gauge with its bucket "
                             "resolution and optional retention; idempotent for an "
                             "identical definition. Mutates replicated state.",
                             create_metric_schema(),
                             create_metric_output_schema(),
                             CapabilityKind::Action,
                             Operation{CapabilityDomain::Orchestration, "create_metric"},
                             /*mutates=*/true,
                             /*requires_consent=*/false,
                             handle_create_metric};
    create_metric.blocks = true;
    registry.add(std::move(create_metric));
    Capability record_metric{"record_metric",
                             "Record metric points",
                             "Append up to 1000 labelled points to a counter or gauge; "
                             "all are validated before any is written. Mutates "
                             "replicated state.",
                             record_metric_schema(),
                             record_metric_output_schema(),
                             CapabilityKind::Action,
                             Operation{CapabilityDomain::Data, "metrics_write"},
                             /*mutates=*/true,
                             /*requires_consent=*/false,
                             handle_record_metric};
    record_metric.blocks = true;
    registry.add(std::move(record_metric));
    Capability query_metric{"query_metric",
                            "Query a metric",
                            "Sum a counter, or aggregate a gauge, over a time range, "
                            "filtered by labels, grouped by labels and split into steps. "
                            "No side effects.",
                            query_metric_schema(),
                            query_metric_output_schema(),
                            CapabilityKind::Read,
                            Operation{CapabilityDomain::Data, "metrics_read"},
                            /*mutates=*/false,
                            /*requires_consent=*/false,
                            handle_query_metric};
    query_metric.blocks = true;
    query_metric.post_alias = true;
    registry.add(std::move(query_metric));
}

}  // namespace zetta::api
