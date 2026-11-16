// SPDX-License-Identifier: MIT

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
namespace zetta::pipeline {
class Project;
class ProjectGraph;
}
namespace zetta::cluster {
class Cluster;
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
    // What a published pipeline's run needs (zetta#1289): the registry whose
    // `run` executes it, the owner of its background work, and the cluster.
    // Unused by predict / search / blob.
    const Registry* registry = nullptr;
    BackgroundActions* background = nullptr;
    const zetta::cluster::Cluster* cluster = nullptr;
};

// --- published pipelines (zetta#1289) ---------------------------------------
//
// expose_pipeline pins a pipeline and publishes its served route on every node:
//
//   POST /v1/pipelines/{name}/run              run it with the caller's input
//   GET  /v1/pipelines/{name}/runs/{run_id}    a run it served: status, outputs
//
// A served run is governed like predict, and more: the caller's own key must
// hold `data:run_pipeline` (flat, or scoped ON PROJECT to the pipeline's
// project) AND read access to every table the pinned pipeline reads, so the
// caller's grants decide, never the exposer's. The body is checked against the
// size limit and the input schema the exposer declared, the call is admitted by
// the per-caller rate limit, audited (`serve:run_pipeline:<id>`), and traced.

// A pipeline found by its {name} path segment: the graph id, or the name of an
// exposed pipeline (unique among exposed pipelines, or `conflict`).
struct PublishedPipeline {
    std::shared_ptr<zetta::pipeline::Project> project;
    std::shared_ptr<zetta::pipeline::ProjectGraph> graph;
};
ApiResult resolve_published_pipeline(Node& node, const std::string& target,
                                     PublishedPipeline& found);

std::string pipeline_endpoint_path(const std::string& graph_id);
std::string pipeline_run_status_path(const std::string& graph_id, const std::string& run_id);

// A served run's answer, and a status read's: { pipeline_id, project, name,
// version, run_id, status, outputs, status_endpoint, error? }. The output
// schema of run_published_pipeline / get_published_pipeline_run and the
// OpenAPI response of the two served routes.
const nlohmann::json& pipeline_run_response_schema();

// The default declared input when the exposer declares none: an object that
// admits no fields, so a caller can pass nothing the exposer did not allow.
const nlohmann::json& default_pipeline_input_schema();

// Serve one run of a published pipeline: `request.input` is the declared input
// (passed to `run` with the pipeline's pinned version). Order: resolve ->
// authorize `data:run_pipeline` (audited either way) -> exposure check -> size
// limit -> the declared input schema -> rate limit -> the caller's read access
// to every table the run reads -> `run`. Returns { pipeline_id, project, name,
// version, run_id, status, outputs, status_endpoint }. Main-thread only;
// non-throwing.
ApiResult serve_pipeline_run(Node& node, const ServeRequest& request);

// Serve one run's status: `request.input` is { run_id }. The run must have been
// served by this pipeline to this same caller, or it is `not_found`. Returns {
// pipeline_id, version, run_id, status, outputs, error? }. Main-thread only.
ApiResult serve_pipeline_run_status(Node& node, const ServeRequest& request);

// True once a run's status is terminal (succeeded or failed): the served
// synchronous mode stops waiting then.
bool pipeline_run_finished(const ApiResult& status);

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
