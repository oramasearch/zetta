#pragma once

#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/api/registry.hpp"
#include "zetta/observability/trace.hpp"

// Governed HTTP serving of models and indexes (#648, docs/api/serving.md).
//
// Every trained Model and built Index is cluster-internal by default; the
// expose_model / expose_index capabilities flip a REPLICATED `exposed` flag on
// the entity's value, which publishes a per-entity endpoint on EVERY node:
//
//   POST /v1/models/{name}/predict    (batch scoring — score_gbdt underneath)
//   GET  /v1/indexes/{name}/search    (the distributed scatter-gather
//                                      Node::search path underneath, #641)
//
// This header is the HTTP-free half (zetta-core): the Beast2 adapter TU
// (src/core/api/http_server.cpp) forwards the matched route here and renders
// the returned ApiResult with the same envelope + status mapping as the REST
// rendering (#618). Each served request is governed and observable like every
// other operation: the bearer key is authorized against a specific data-plane
// right (`data:predict` / `data:search`, deny-by-default — a customer key can
// be minted with exactly serving access and nothing else), the decision is
// audited, the request body is schema-validated at the boundary, and the whole
// request is a `serve:predict` / `serve:search` span carrying the four
// captures. Un-exposing withdraws the route everywhere: the exposure check
// reads the replicated flag at request time, so a node that has reconciled the
// change refuses with `not_found` — no route table to tear down.
namespace zetta {
class Node;
}
namespace zetta::ml {
class Model;
}
namespace zetta::index {
struct SearchResult;
}

namespace zetta::api {

// The predict request body (draft-07, schema-as-code): `rows` is the batch to
// score — one object per entity mapping feature name -> numeric value (extra
// features are ignored; a feature the model needs must be present in every
// row). `entities` optionally names each row (defaults to the row index);
// `top_k` caps the ranked at-risk list (absent = all rows). The reserved
// optional `intent` string is recorded to the span and stripped before
// validation, like every capability input. Drift-checked artifact:
// schemas/predict_request.schema.json.
const nlohmann::json& predict_request_schema();

// The predict success value: the model's identity plus the ranked at-risk list
// (`ranked`, highest calibrated risk first — the batch-scoring path of #392).
// Documentation asserted in tests, rendered into the OpenAPI document.
const nlohmann::json& predict_response_schema();

// The served search input (draft-07): the `search` capability's input minus
// `target` — the path segment names the index. Rendered as query parameters.
// Drift-checked artifact: schemas/serve_search_request.schema.json.
const nlohmann::json& serve_search_request_schema();

// The search success value — ONE shape for the `search` capability's output
// and the served route's response, declared here so both render from the same
// source and cannot drift.
const nlohmann::json& search_response_schema();

// A SearchResult rendered as that documented shape (hits + per-node
// provenance + path). Shared by the `search` capability handler and
// serve_search — no second rendering.
nlohmann::json search_result_to_json(const zetta::index::SearchResult& result);

// Resolve the {name} path segment (or an expose_model input) to a registered
// model: an exact model id wins; otherwise the most recently created model
// whose display label equals `target` (models() is sorted (created, id), so
// the last match is the newest — the resolve_search_target convention).
// Empty handle when nothing matches.
std::shared_ptr<zetta::ml::Model> resolve_served_model(
    Node& node, const std::string& target);

// The served route for an entity, by id — the unambiguous spelling (a display
// name / table name also resolves, but can be shadowed by a newer entity with
// the same label). What expose_* returns and the system tables surface.
std::string model_endpoint_path(const std::string& model_id);
std::string index_endpoint_path(const std::string& index_id);

// One served request, as the adapter hands it over: the raw path segment, the
// (already JSON-parsed) input — the POST body for predict, the coerced query
// parameters for search — the acting identity (a user id when the default-deny
// gate resolved a token, else an AccessKey id, empty = the audited
// "unauthenticated local" session), and the trace to record into.
struct ServeRequest {
    std::string target;    // the {name} path segment: an entity id, a model
                           // display label, or an indexed table's name
    nlohmann::json input;  // schema-validated here, at the boundary
    std::string acting_key;
    zetta::observability::TraceContext trace;
    zetta::observability::SpanRecorder* recorder = nullptr;
    // The gate (#745) resolved a token to a user, so `acting_key` is a user id:
    // authorize against the user's effective permissions, not an AccessKey.
    bool acting_is_user = false;
};

// Serve one batch-predict request over an exposed model. Main-thread only
// (reads the Node tree — the adapter marshals via MainThreadBridge). Order:
// authorize (`data:predict`, audited either way) -> resolve -> exposure check
// -> validate the body -> load the weights through the GBDT schema boundary ->
// score + rank. Non-throwing; domain failures carry the engine's reason
// verbatim (`not_found` for an unknown or un-exposed target).
ApiResult serve_predict(Node& node, const ServeRequest& request);

// Serve one search request over an exposed index — the same resolution rule
// and distributed scatter-gather execution as the `search` capability
// (Node::search), gated on the index's replicated `exposed` flag. Main-thread
// only; non-throwing.
ApiResult serve_search(Node& node, const ServeRequest& request);

// Serve one large-object (blob) body over the object-scoped access model (#747).
// `request.target` is the blob id. Order: authorize (`blob:read`, scoped by
// id/prefix — a flat blob:read or a covering prefix grant admits it; the local
// session + legacy keys follow the same bypass as predict/search), audited either
// way -> resolve the object across the node's databases -> return { id, size,
// body_base64 }. Non-throwing; `not_found` for an unknown / body-absent object,
// `unauthorized` when no grant covers the id. Main-thread only (reads the tree).
ApiResult serve_blob(Node& node, const ServeRequest& request);

}  // namespace zetta::api
