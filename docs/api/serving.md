# Governed HTTP serving of models and indexes (#648)

Every trained **Model** and built **Index** is **cluster-internal by default**
and can be **exposed as HTTP** as a governed, per-entity choice — the full
meaning of the pipeline's `serve` step: artifacts stand up as endpoints the
customer's own systems call, with the data and the model never leaving the
cluster. Realizes [../product/experience/serving.md](../product/experience/serving.md).

## The shape

- **Exposure is replicated entity state.** `expose_model` / `expose_index`
  flip an `exposed` boolean on the entity's CRDT value (`model:<uuid>` /
  `index:<uuid>`, `Model::set_exposed` / `Index::set_exposed`) — no route
  table, no second copy. Every node reconciles the flag and therefore serves
  — or refuses — the entity's endpoint **consistently across the cluster**;
  un-exposing is the same one action and kills the route everywhere.
- **The served routes** (registered once in the Beast2 adapter, answered per
  request from the replicated flag):
  - `POST /v1/models/{name}/predict` — batch scoring: the body's `rows` run
    through the model's weights via the [batch-scoring path](../ml/gbdt-score.md)
    (`load_gbdt_model` + `score_gbdt`, the GBDT schema boundary re-validating
    the weights body), returning the ranked at-risk list. The weights
    replicate, so **any node answers**.
  - `GET /v1/indexes/{name}/search?query=…` — [THE search entry point](../indexing/node-search.md)
    (`Node::search`, #641) underneath: the distributed per-partition
    scatter-gather when the table is placed, with per-node provenance in the
    response.
  - `{name}` resolves like everything else: an entity **id** always works
    (the unambiguous spelling the `endpoint` columns surface); a model's
    display label or an indexed table's name also resolves —
    `resolve_served_model` mirrors `resolve_search_target` (#641, newest
    match wins), so the route and `search` can never disagree about what a
    table name means.

## Governance — the serving keys

- **Exposure is an `orchestration:<action>` capability** (`expose_model`,
  `unexpose_model`, `expose_index`, `unexpose_index`) — registry actions like
  every mutation: authorized, audited, `mutates` firing the activity hook.
- **Endpoint access is key-scoped with data-plane rights**: a served request's
  bearer key must hold **`data:predict`** (models) or **`data:search`**
  (indexes) — deliberately NOT `api:query`, so a customer-facing serving key
  can be minted with exactly serving access and nothing else (deny-by-default,
  [../governance/access-control.md](../governance/access-control.md)). The
  empty-bearer loopback call remains the audited "unauthenticated local"
  session, the same convention as every surface.
- **Every request lands on the audit trail** (`serve:predict:<target>` /
  `serve:search:<target>`, allow and deny), and **every request is a trace
  span** — `serve:predict` / `serve:search` with the four captures (intent ·
  plan · performance · result shape); the served search's own `search` span
  tree (root + local + per-peer) nests under it, and the response envelope
  carries `trace_id` so a customer or agent reads the whole request back via
  `get_trace`.

## Boundaries

- **Two new JSON formats, schema-validated at the boundary**: the predict
  request body (`predict_request_schema()` →
  [`schemas/predict_request.schema.json`](../../schemas/predict_request.schema.json))
  and the served search input (`serve_search_request_schema()` →
  [`schemas/serve_search_request.schema.json`](../../schemas/serve_search_request.schema.json))
  — schema-as-code, drift-checked, pboettch-validated with path-qualified
  rejections. The search **response** shape is declared once
  (`search_response_schema()` / `search_result_to_json`) and shared with the
  `search` capability — no second rendering.
- **The logic is HTTP-free zetta-core** (`include/zetta/api/serving.hpp`,
  `src/core/api/serving.cpp`): the adapter TU only matches the route, hands
  over `ServeRequest`, and renders the returned `ApiResult` with the same
  envelope + status mapping as the REST rendering (#618). Beast2 stays
  confined to `http_server.cpp`.
- **The OpenAPI document carries the served paths**: `/v1/models/{name}/predict`
  and `/v1/indexes/{name}/search` are the two path-templated entries the
  registry rendering cannot derive, declared in `openapi_document` from the
  same serving schemas (drift-checked into `schemas/openapi.json`). They are
  the sanctioned exception to "routes are registry-derived", justified the way
  llms.txt was: a per-entity data-plane surface, not a control capability.
- **RULE-7**: exposure state is visible in `system.models` / `system.indexes`
  (`exposed` + `endpoint` columns, computed live from the same entities the
  routes check) and in the typed `get_model` / `get_index` reads. The entity
  value emits `exposed` only when set, so never-exposed records stay
  byte-identical to their pre-#648 shape.

## Acceptance (shipped tests)

- [`tests/api/test_serving.cpp`](../../tests/api/test_serving.cpp) — layer-1
  journeys: expose a trained model → predict with a key holding exactly
  `data:predict` (ranking equals the `score_gbdt` oracle) → a key without it
  is refused AND audited → un-expose kills the route; the same for an index
  (served hits equal the `search` capability's); schema-boundary rejections
  (missing rows / missing model feature with the `/rows/N` pointer / ragged
  entities); read-back through `get_model` / `get_index` and the system
  views; idempotent expose/unexpose; `serve:*` spans in the ring.
- [`tests/api/test_rest_server.cpp`](../../tests/api/test_rest_server.cpp)
  (`ServedModelAndIndexEndpointsOverHttp`) — the wire acceptance: real
  loopback HTTP, expose over `POST /v1/expose_model`, `POST
  /v1/models/churn/predict` with a bearer key → 200 + ranked list, the
  read-only key → 403, un-expose → 404; same for `GET
  /v1/indexes/articles/search?query=…`.
- Entity-level: exposure round-trip + CRDT persistence in
  [`tests/ml/test_model.cpp`](../../tests/ml/test_model.cpp) and
  [`tests/index/test_index.cpp`](../../tests/index/test_index.cpp); boundary
  rejection of a non-boolean `exposed`.

## Later slices

- Scoring **cluster-resident** datasets through a served endpoint (a dataset
  reference in the predict body → the distributed `score` ComputeJob) — the
  posted-rows body is the first slice.
- Serving kinds beyond GBDT weights bodies (the loader is the
  `load_gbdt_model` boundary; other model kinds extend behind the same route).
- TLS on the served ingress rides the same #466 beast2 `https_server` slice
  as the rest of the API surface; per-entity serving keys minted from the
  GUI.
