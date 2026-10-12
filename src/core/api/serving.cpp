#include "zetta/api/serving.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include <nlohmann/json-schema.hpp>

#include "../base64.hpp"  // zetta::detail::base64_encode_bytes (src/core/base64.hpp)
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/governance/capability.hpp"
#include "zetta/index/index.hpp"
#include "zetta/index/search.hpp"
#include "zetta/ml/gbdt_score.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"

namespace zetta::api {

namespace {

// Collects the first validation failure (the registry.cpp / model.cpp
// pattern): the error-handler overload keeps validation failures from
// crossing the validator-library boundary as exceptions.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&, const std::string& message) override {
        if (failed_) return;  // keep the first, most specific failure
        failed_ = true;
        const std::string at = pointer.to_string();
        reason_ = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed() const { return failed_; }
    const std::string& reason() const { return reason_; }

private:
    bool failed_ = false;
    std::string reason_;
};

bool validate_request(const nlohmann::json& schema, const nlohmann::json& input,
                      std::string* reason) {
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(schema);
    collecting_error_handler handler;
    validator.validate(input, handler);
    if (handler.failed() && reason) *reason = handler.reason();
    return !handler.failed();
}

// Every served request lands on the audit trail under the acting key — the
// audit_call convention, with a `serve:` target instead of `api:`.
void audit_serve(Node& node, const std::string& surface,
                 const std::string& target, const std::string& acting_key,
                 bool allowed, const std::string& detail) {
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DataAccess;
    event.actor = acting_key.empty() ? "local" : acting_key;
    event.target = "serve:" + surface + ":" + target;
    event.allowed = allowed;
    event.detail = detail;
    node.record_audit_event(std::move(event));
}

// One served request = one span with the four captures, and the trace ids
// ride back on the ApiResult — the Registry::call convention, so a customer
// (or an agent) can read the whole request back via get_trace.
struct ServeSpan {
    ServeSpan(const ServeRequest& request, std::string name)
        : span(request.recorder, request.trace, std::move(name)),
          trace_id(request.trace.trace_id) {
        span.set_attribute("target", request.target);
        span.set_attribute("actor", request.acting_key.empty()
                                        ? "local"
                                        : request.acting_key);
    }

    ApiResult finish(ApiResult result) {
        result.trace_id = trace_id;
        result.span_id = span.span_id();
        result.duration_ms = span.elapsed_ms();
        if (result.ok) {
            span.finish(/*ok=*/true);
        } else {
            span.finish(/*ok=*/false,
                        result.error.code + ": " + result.error.message);
        }
        return result;
    }

    zetta::observability::ScopedSpan span;
    std::string trace_id;
};

// The reserved cross-capability `intent` field: recorded to the span,
// stripped before schema validation (the Registry::call convention).
nlohmann::json strip_intent(const nlohmann::json& input,
                            zetta::observability::ScopedSpan& span) {
    if (!input.is_object() || !input.contains("intent")) return input;
    nlohmann::json effective = input;
    span.set_attribute("intent", effective["intent"]);
    effective.erase("intent");
    return effective;
}

}  // namespace

const nlohmann::json& predict_request_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = {
            {"$schema", "http://json-schema.org/draft-07/schema#"},
            {"title", "zetta.PredictRequest"},
            {"type", "object"},
            {"required", {"rows"}},
            {"additionalProperties", false}};
        document["properties"]["rows"] = {
            {"type", "array"},
            {"minItems", 1},
            {"description",
             "the batch to score: one object per entity mapping feature name "
             "to its numeric or boolean value; every feature the model was trained on "
             "must be present (extra features are ignored)"},
            {"items", {{"type", "object"}}}};
        document["properties"]["rows"]["items"]["additionalProperties"] = {
            {"oneOf", {{{"type", "number"}}, {{"type", "boolean"}}}}};
        document["properties"]["entities"] = {
            {"type", "array"},
            {"description",
             "optional entity key per row (defaults to the row index); must "
             "match rows in length"}};
        document["properties"]["entities"]["items"] = {
            {"oneOf", {{{"type", "integer"}}, {{"type", "string"}}}}};
        document["properties"]["top_k"] = {
            {"type", "integer"},
            {"minimum", 1},
            {"description",
             "cap the ranked at-risk list to the k highest-risk entities "
             "(absent = all rows)"}};
        return document;
    }();
    return schema;
}

const nlohmann::json& predict_response_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"model_id", "rows", "ranked"}},
        {"additionalProperties", false},
        {"properties",
         {{"model_id", {{"type", "string"}}},
          {"name", {{"type", "string"}}},
          {"rows",
           {{"type", "integer"},
            {"description", "how many rows were scored"}}},
          {"ranked",
           {{"type", "array"},
            {"description",
             "the scored entities, highest calibrated risk first (ties break "
             "by entity ascending — deterministic)"},
            {"items",
             {{"type", "object"},
              {"required", {"entity", "risk"}},
              {"additionalProperties", false},
              {"properties",
               {{"entity",
                 {{"oneOf",
                   {{{"type", "integer"}}, {{"type", "string"}}}}}},
                {"risk",
                 {{"type", "number"},
                  {"description",
                   "the model's calibrated probability in [0, 1]"}}}}}}}}}}}};
    return schema;
}

const nlohmann::json& serve_search_request_schema() {
    static const nlohmann::json schema = {
        {"$schema", "http://json-schema.org/draft-07/schema#"},
        {"title", "zetta.ServeSearchRequest"},
        {"type", "object"},
        {"required", {"query"}},
        {"additionalProperties", false},
        {"properties",
         {{"query",
           {{"type", "string"},
            {"minLength", 1},
            {"description", "the free-text query"}}},
          {"field",
           {{"type", "string"},
            {"description",
             "restrict matching to one indexed field (optional)"}}},
          {"fields",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description",
             "the row fields each hit carries back (default: the index's own "
             "fields; [] = ids and scores only). Each must be one of the "
             "index's fields."}}},
          {"top_k",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 1000},
            {"description", "cap on the global ranking (default 10)"}}}}}};
    return schema;
}

const nlohmann::json& search_response_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required",
         {"index_id", "table", "path", "hits", "nodes", "wire_bytes_moved",
          "duration_ms"}},
        {"additionalProperties", false},
        {"properties",
         {{"index_id", {{"type", "string"}}},
          {"table", {{"type", "string"}}},
          {"path",
           {{"type", "string"},
            {"description", "how the query executed: consolidated or "
                            "per-partition scatter-gather"}}},
          {"hits",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"required", {"id", "score", "fields"}},
              {"additionalProperties", false},
              {"properties",
               {{"id",
                 {{"type", "string"},
                  {"description", "the engine's internal row id"}}},
                {"score", {{"type", "number"}}},
                {"fields",
                 {{"type", "object"},
                  {"description",
                   "the matched row's values for the requested fields "
                   "(default: the index's own fields)"}}}}}}}}},
          {"nodes",
           {{"type", "array"},
            {"description", "per-node provenance: which member answered, at "
                            "what cost (#641)"},
            {"items",
             {{"type", "object"},
              {"required",
               {"node_id", "local", "hits", "sub_indexes", "latency_ms",
                "wire_bytes"}},
              {"additionalProperties", false},
              {"properties",
               {{"node_id", {{"type", "string"}}},
                {"local", {{"type", "boolean"}}},
                {"hits", {{"type", "integer"}}},
                {"sub_indexes", {{"type", "integer"}}},
                {"latency_ms", {{"type", "number"}}},
                {"wire_bytes", {{"type", "integer"}}}}}}}}},
          {"wire_bytes_moved", {{"type", "integer"}}},
          {"duration_ms", {{"type", "number"}}}}}};
    return schema;
}

nlohmann::json search_result_to_json(const zetta::index::SearchResult& result) {
    nlohmann::json hits = nlohmann::json::array();
    for (const zetta::index::SearchHit& hit : result.hits) {
        hits.push_back(
            {{"id", hit.row_id}, {"score", hit.score}, {"fields", hit.fields}});
    }
    // Per-node provenance: which node answered, and at what cost (#641).
    nlohmann::json nodes = nlohmann::json::array();
    for (const zetta::index::NodeContribution& contribution : result.nodes) {
        nodes.push_back({{"node_id", contribution.node_id},
                         {"local", contribution.local},
                         {"hits", contribution.hits},
                         {"sub_indexes", contribution.sub_indexes},
                         {"latency_ms", contribution.latency_ms},
                         {"wire_bytes", contribution.wire_bytes}});
    }
    nlohmann::json output;
    output["index_id"] = result.index_id;
    output["table"] = result.spec.table;
    output["path"] = std::string(zetta::index::to_string(result.path));
    output["hits"] = std::move(hits);
    output["nodes"] = std::move(nodes);
    output["wire_bytes_moved"] = result.wire_bytes_moved;
    output["duration_ms"] = result.duration_ms;
    return output;
}

std::shared_ptr<zetta::ml::Model> resolve_served_model(
    Node& node, const std::string& target) {
    if (std::shared_ptr<zetta::ml::Model> by_id = node.find_model(target)) {
        return by_id;
    }
    std::shared_ptr<zetta::ml::Model> newest;
    for (const std::shared_ptr<zetta::ml::Model>& model : node.models()) {
        if (model && !model->name().empty() && model->name() == target) {
            newest = model;  // models() is (created, id)-sorted: last = newest
        }
    }
    return newest;
}

std::string model_endpoint_path(const std::string& model_id) {
    return "/v1/models/" + model_id + "/predict";
}

std::string index_endpoint_path(const std::string& index_id) {
    return "/v1/indexes/" + index_id + "/search";
}

ApiResult serve_predict(Node& node, const ServeRequest& request) {
    ServeSpan serve(request, "serve:predict");

    // Endpoint access is deny-by-default. Under the default-deny gate (#745) a
    // token-resolved user must hold data:predict through one of its roles;
    // otherwise the legacy key path holds — a named AccessKey must carry it,
    // and the empty key is the audited "unauthenticated local" session (only
    // reachable with the gate off — the loopback default).
    const zetta::governance::Operation predict_op{
        zetta::governance::CapabilityDomain::Data, "predict"};
    if (request.acting_is_user) {
        auto decision = node.authorize_user(request.acting_key, predict_op);
        if (!decision) {
            audit_serve(node, "predict", request.target, request.acting_key,
                        /*allowed=*/false, decision.reason);
            return serve.finish(
                ApiResult::failure("unauthorized", decision.reason));
        }
    } else if (!request.acting_key.empty()) {
        auto decision = node.authorize(request.acting_key, predict_op);
        if (!decision) {
            audit_serve(node, "predict", request.target, request.acting_key,
                        /*allowed=*/false, decision.reason);
            return serve.finish(
                ApiResult::failure("unauthorized", decision.reason));
        }
    }
    audit_serve(node, "predict", request.target, request.acting_key,
                /*allowed=*/true, "served predict request");

    const std::shared_ptr<zetta::ml::Model> model =
        resolve_served_model(node, request.target);
    if (!model) {
        return serve.finish(ApiResult::failure(
            "not_found", "no model named '" + request.target + "'"));
    }
    // The exposure gate: the flag is replicated state, so every node of the
    // cluster serves — or refuses — this route consistently, and un-exposing
    // kills it everywhere without touching any route table.
    if (!model->exposed()) {
        return serve.finish(ApiResult::failure(
            "not_found", "model '" + request.target +
                             "' is not exposed; expose_model publishes " +
                             model_endpoint_path(model->id())));
    }

    const nlohmann::json input = strip_intent(request.input, serve.span);
    std::string reason;
    if (!validate_request(predict_request_schema(), input, &reason)) {
        return serve.finish(ApiResult::failure("invalid_input", reason));
    }

    // The weights body is read back through the GBDT model schema boundary
    // (it may still be in transfer to this node, or be malformed — both fail
    // with the boundary's own reason, never a crash or a corrupt score).
    std::string load_error;
    std::optional<zetta::ml::GbdtModel> weights =
        zetta::ml::load_gbdt_model(*model, &load_error);
    if (!weights) {
        return serve.finish(ApiResult::failure(
            "engine_error", "model '" + request.target +
                                "' has no servable weights on this node: " +
                                load_error));
    }

    // Build the design matrix in the model's own feature order. Extra features
    // in a row are ignored; a feature the model needs must be present in every
    // row (the schema already guaranteed rows are objects of numbers).
    const nlohmann::json& rows = input.at("rows");
    const std::size_t row_count = rows.size();
    std::vector<std::vector<double>> columns(weights->feature_count());
    for (std::size_t feature = 0; feature < weights->feature_count();
         ++feature) {
        const std::string& feature_name = weights->feature_names[feature];
        columns[feature].resize(row_count);
        for (std::size_t row = 0; row < row_count; ++row) {
            const auto value = rows[row].find(feature_name);
            if (value == rows[row].end()) {
                return serve.finish(ApiResult::failure(
                    "invalid_input",
                    "missing feature '" + feature_name +
                        "' the model requires",
                    "/rows/" + std::to_string(row)));
            }
            columns[feature][row] = value->is_boolean()
                                        ? (value->get<bool>() ? 1.0 : 0.0)
                                        : value->get<double>();
        }
    }
    std::shared_ptr<const zetta::data::EntityKeyMapping> entity_keys;
    if (const auto mapping = model->feature_schema().find("entity_key_mapping");
        mapping != model->feature_schema().end()) {
        std::string mapping_error;
        entity_keys = zetta::data::entity_key_mapping_from_json(
            *mapping, &mapping_error);
        if (!entity_keys) {
            return serve.finish(ApiResult::failure(
                "engine_error",
                "model has an invalid entity key mapping: " + mapping_error));
        }
    }
    std::vector<std::int64_t> entities;
    if (const auto it = input.find("entities"); it != input.end()) {
        entities.reserve(it->size());
        for (const nlohmann::json& entity : *it) {
            if (entity.is_number_integer()) {
                entities.push_back(entity.get<std::int64_t>());
                continue;
            }
            if (!entity.is_string() || !entity_keys) {
                return serve.finish(ApiResult::failure(
                    "invalid_input",
                    "text entities require a model trained with text entity ids",
                    "/entities"));
            }
            const std::optional<std::int64_t> derived =
                entity_keys->derived(entity.get_ref<const std::string&>());
            if (!derived) {
                return serve.finish(ApiResult::failure(
                    "invalid_input",
                    "unknown text entity '" + entity.get<std::string>() + "'",
                    "/entities"));
            }
            entities.push_back(*derived);
        }
        if (entities.size() != row_count) {
            return serve.finish(ApiResult::failure(
                "invalid_input",
                "entities (" + std::to_string(entities.size()) +
                    ") must match rows (" + std::to_string(row_count) + ")",
                "/entities"));
        }
    } else {
        entities.resize(row_count);
        for (std::size_t row = 0; row < row_count; ++row) {
            entities[row] = static_cast<std::int64_t>(row);
        }
    }
    const std::size_t top_k = input.value("top_k", std::size_t{0});

    serve.span.set_attribute(
        "plan", nlohmann::json{{"model", model->id()},
                               {"scorer", "gbdt host-side reference"},
                               {"features", weights->feature_count()}});

    const zetta::ml::ScoreResult scored = zetta::ml::score_gbdt(
        *weights, columns, weights->feature_names, entities, top_k);
    if (!scored.ok()) {
        const bool caller_fault = scored.status.code ==
                                  zetta::compute::ComputeStatusCode::InvalidArgument;
        return serve.finish(ApiResult::failure(
            caller_fault ? "invalid_input" : "engine_error",
            scored.status.message));
    }

    nlohmann::json ranked = nlohmann::json::array();
    for (const zetta::ml::EntityScore& score : scored.ranked) {
        ranked.push_back(
            {{"entity", entity_keys
                            ? entity_keys->original(score.entity)
                            : nlohmann::json(score.entity)},
             {"risk", score.risk}});
    }
    serve.span.set_attribute(
        "result_shape", nlohmann::json{{"rows", row_count},
                                       {"ranked", scored.ranked.size()}});
    nlohmann::json output;
    output["model_id"] = model->id();
    output["name"] = model->name();
    output["rows"] = row_count;
    output["ranked"] = std::move(ranked);
    return serve.finish(ApiResult::success(std::move(output)));
}

ApiResult serve_search(Node& node, const ServeRequest& request) {
    ServeSpan serve(request, "serve:search");

    const zetta::governance::Operation search_op{
        zetta::governance::CapabilityDomain::Data, "search"};
    if (request.acting_is_user) {
        auto decision = node.authorize_user(request.acting_key, search_op);
        if (!decision) {
            audit_serve(node, "search", request.target, request.acting_key,
                        /*allowed=*/false, decision.reason);
            return serve.finish(
                ApiResult::failure("unauthorized", decision.reason));
        }
    } else if (!request.acting_key.empty()) {
        auto decision = node.authorize(request.acting_key, search_op);
        if (!decision) {
            audit_serve(node, "search", request.target, request.acting_key,
                        /*allowed=*/false, decision.reason);
            return serve.finish(
                ApiResult::failure("unauthorized", decision.reason));
        }
    }
    audit_serve(node, "search", request.target, request.acting_key,
                /*allowed=*/true, "served search request");

    // THE search resolution rule (resolve_search_target — #641): the route
    // accepts an index id or an indexed table's name, exactly like `search`.
    const std::shared_ptr<zetta::index::Index> index =
        zetta::index::resolve_search_target(node, request.target);
    if (!index) {
        return serve.finish(ApiResult::failure(
            "not_found", "no index named '" + request.target + "'"));
    }
    if (!index->exposed()) {
        return serve.finish(ApiResult::failure(
            "not_found", "index '" + request.target +
                             "' is not exposed; expose_index publishes " +
                             index_endpoint_path(index->id())));
    }

    const nlohmann::json input = strip_intent(request.input, serve.span);
    std::string reason;
    if (!validate_request(serve_search_request_schema(), input, &reason)) {
        return serve.finish(ApiResult::failure("invalid_input", reason));
    }

    serve.span.set_attribute("plan",
                             nlohmann::json{{"index", index->id()},
                                            {"table", index->table()}});

    zetta::index::SearchRequest search_request;
    search_request.target = index->id();  // already resolved: exact
    search_request.query = input.at("query").get<std::string>();
    search_request.field = input.value("field", std::string{});
    search_request.top_k = input.value("top_k", std::size_t{10});
    if (input.contains("fields")) {
        search_request.fields = input.at("fields").get<std::vector<std::string>>();
    }

    // The search records its own `search` span tree (root + local + per-peer,
    // #641) as a child of this serve span — one trace for the whole request.
    const zetta::observability::TraceContext parent{serve.trace_id,
                                                    serve.span.span_id()};
    const zetta::index::SearchResult result =
        node.search(search_request, request.recorder, &parent);
    if (!result.ok()) {
        return serve.finish(
            ApiResult::failure("engine_error", result.status.message));
    }
    serve.span.set_attribute(
        "result_shape", nlohmann::json{{"hits", result.hits.size()},
                                       {"nodes", result.nodes.size()}});
    return serve.finish(ApiResult::success(search_result_to_json(result)));
}

ApiResult serve_blob(Node& node, const ServeRequest& request) {
    ServeSpan serve(request, "serve:blob");

    // Object-scoped access (#747). A token-resolved user must hold `blob:read`
    // covering this blob id — either a flat blob:read (admin/global) or a prefix
    // grant whose prefix the id starts with (authorize_blob folds both). A legacy
    // AccessKey checks the coarse blob:read right; the empty key is the audited
    // "unauthenticated local" session. Deny-by-default.
    bool allowed = true;
    std::string reason;
    if (request.acting_is_user) {
        allowed = node.authorize_blob(request.acting_key, /*acting_is_user=*/true,
                                      request.target);
        if (!allowed) {
            reason = "no blob:read grant for blob '" + request.target + "'";
        }
    } else if (!request.acting_key.empty()) {
        const zetta::governance::Operation blob_op{
            zetta::governance::CapabilityDomain::Data, "blob_read"};
        auto decision = node.authorize(request.acting_key, blob_op);
        allowed = static_cast<bool>(decision);
        if (!allowed) reason = decision.reason;
    }
    if (!allowed) {
        audit_serve(node, "blob", request.target, request.acting_key,
                    /*allowed=*/false, reason);
        return serve.finish(ApiResult::failure("unauthorized", reason));
    }
    audit_serve(node, "blob", request.target, request.acting_key,
                /*allowed=*/true, "served blob request");

    // Resolve the object across the node's databases (blob ids are globally
    // unique) and read its body.
    for (const std::string& db_name : node.database_names()) {
        auto db = node.find_database(db_name);
        if (!db) continue;
        std::optional<std::string> body = db->large_objects().read(request.target);
        if (!body) continue;
        serve.span.set_attribute("blob_size", body->size());
        return serve.finish(ApiResult::success(nlohmann::json{
            {"id", request.target},
            {"size", body->size()},
            {"body_base64", zetta::detail::base64_encode_bytes(*body)}}));
    }
    return serve.finish(ApiResult::failure(
        "not_found",
        "no blob '" + request.target + "' with a local body on this node"));
}

}  // namespace zetta::api
