# Governed HTTP serving of models, indexes and pipelines (#648, zetta#1289)

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
    the weights body), returning the ranked at-risk list. Boolean features load
    as 0/1, and a model trained with text entity ids accepts and returns those
    original values. A model trained with text categories takes each one by its
    original column and text value (`{"region": "south"}`), encoded with the
    levels stored in `feature_schema.categorical_encoding`; an unseen level or a
    null encodes as all zeros, a missing column or a number is `invalid_input`
    ([categorical features](../data/categorical-features.md), #1203). The weights
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
  [../governance/access-control.md](../governance/access-control.md)). Like every
  gated route, a served request needs a bearer that resolves to a user (the
  always-on gate, #799) — there is no empty-bearer path over HTTP.
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
  entities), plus boolean features and reversible text entity ids; read-back
  through `get_model` / `get_index` and the system
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

## Published pipelines (zetta#1289)

Steve, 2026-10-14: *"I need some way to call a pipeline externally (if I publish
it)"*. A **pipeline** is published the way a model is, through a replicated
exposure on the pipeline itself, and served at:

- `POST /v1/pipelines/{name}/run` — run it with the caller's input (the JSON
  body);
- `GET /v1/pipelines/{name}/runs/{run_id}` — a run it served to the same
  caller: its status, outputs, and why it failed.

`{name}` is the pipeline's id (what `expose_pipeline` answers as `endpoint`), or
its name while exactly one exposed pipeline has it (two is `conflict`).

### Exposing pins a version

`expose_pipeline` (`orchestration:expose_pipeline`, in `serving:manage`) takes
`{project, graph}` and optionally `input_schema`, `rate_limit_per_minute`
(default 60), `max_input_bytes` (default 65,536, at most 1 MiB) and
`sync_timeout_ms` (default 30,000). It refuses:

- a pipeline that is proposed, not approved (`approve_plan` first, #885), or
  that fails its correctness gate (RULE-1);
- a pipeline with a connect step that reads anything but a table this node
  holds (a `uri`, a `path`, or a `source.table` that is not landed). A served
  call therefore never reaches a remote source or a cloud, which only the
  consent gate (`docs/api/consent.md`) may allow; land the data first with
  `add_dataset` or `create_dataset_from_query`, which are consent-gated;
- an `input_schema` that is not a usable draft-07 object schema.

The exposure is stored on the pipeline's replicated record (`graph:<id>` →
`exposure`, `zetta::pipeline::PipelineExposure`): `exposed`, `version`, the
**pinned** graph IR, the declared `input_schema`, the limits, and who exposed it
and when. Every node reconciles it, so every node serves or refuses the route
the same way; `unexpose_pipeline` flips `exposed` and the route answers 404
everywhere, while runs it already started keep going. A record that has never
been exposed carries no `exposure` field. A malformed exposure from a peer or a
file is not served.

**Pinning**: the served route runs the pinned graph, never the stored one. An
edit to the pipeline (`save_graph`, which also re-proposes it) changes nothing
callers get until the pipeline is exposed again, which pins it as it is then as
the next `version` (1, 2, …). The `run` capability takes the pinned graph from
`ApiContext::served_graph` for that one call. Each served run records
`published: {version, caller}` on the Run (`Run::set_published`).

### A served call

`serve_pipeline_run` (`src/core/api/serving.cpp`), in this order:

1. **Resolve** `{name}`.
2. **Authorize** the caller's own credential: `data:run_pipeline` (the
   `serving:run` permission), flat or object-scoped `ON PROJECT <name>` to the
   pipeline's project. Over HTTP the caller is always a token-resolved user (the
   always-on gate, #799); there is no anonymous path. A legacy access key needs
   the flat right.
3. **Exposure** check: not exposed is `not_found`.
4. **Size**: the input over `max_input_bytes` is `payload_too_large` (413).
5. **Declared input**: the body must match `input_schema`. The default schema
   is an object with no properties, so a caller can pass nothing the exposer did
   not declare. The declared fields are `run`'s inputs (`dataset_path`, `top_k`,
   `model_name`, …); a declared field `run` does not take is refused.
6. **Rate limit**: `rate_limit_per_minute` calls per caller per pipeline in a
   sliding minute (`governance::RateLimiter`), else `rate_limited` (429) with
   `details.retry_after_seconds`. The count is per node: a caller spreading calls
   over N nodes is admitted up to N times the limit.
7. **The caller's data grants**: the caller must be able to read every table the
   pinned pipeline's connect steps read, and a `database.table` `dataset_path`
   it passes (`sql:read`, flat or scoped to the table; a legacy key also needs
   the database in its scope). The caller's grants decide what a served run
   reads, never the exposer's.
8. **Run**: the `run` capability's handler under the caller's identity with the
   pinned graph, and `{pipeline_id, project, name, version, run_id, status,
   outputs, status_endpoint}` back. Each step runs as the caller
   ([step authority](../governance/step-authority.md), #843), so the caller also
   needs the right every step exercises (a clean step:
   `orchestration:create_dataset_from_query`; a model step: `train_model`; …). A
   step the caller lacks the right for fails the run with `status: "failed"` and
   the step named in its logs.

Every step that refuses is audited (`serve:run_pipeline:<pipeline id>`,
`allowed: false`, the reason); an admitted call is audited once with its version.
The whole call is a `serve:run_pipeline` span, and the inner `run` nests under
it.

**Synchronous and asynchronous.** The HTTP route waits for the run to finish, up
to the pipeline's `sync_timeout_ms`, on a `runtime::delay` timer that holds no
thread, then answers its status and outputs (200). A run still going when the
time is up answers 202 with `run_id` and `status_endpoint`; `?wait=false` answers
at once. A run that finishes inside `run` itself (a structural pipeline) answers
200 straight away.

**The same over MCP and REST capabilities**: `run_published_pipeline` (`data:run_pipeline`,
job) and `get_published_pipeline_run` (read) are the same served run and status
read without the HTTP route, for an agent or for Lake. As registry calls they
also need `mcp:call` for the tool, like every capability; the served routes do
not.

**Outputs** are the run's `{kind, name, ref}` outputs (a dataset table, a model,
a report), the same as `get_run` shows; the route does not return the rows of an
output table.

### Not in this slice

- **Posting rows** for the pipeline to run on (the issue's "rows, a document"):
  the declared input today is `run`'s parameters, including a `dataset_path` the
  caller may read. Landing a per-call dataset from the request body is its own
  slice.
- A cluster-wide rate limit (a replicated counter per call), and per-key limits
  set on the key rather than on the pipeline.
- A callback when a long run finishes; the caller polls the status route.
- Lake's half (the ingress address, keys scoped to one pipeline, the UI,
  metering): [lake#747](https://github.com/oramasearch/lake/issues/747).

### Acceptance

- [`tests/api/test_pipeline_serving.cpp`](../../tests/api/test_pipeline_serving.cpp):
  expose → a key with the rights runs it (the Run records `published`) → the
  status reads back to that caller only, not to another caller or for a run
  started another way → the registry capabilities answer the same → unexpose
  withdraws the route; the caller's grants decide (no `data:run_pipeline`, no read
  on the table, a user scoped to another project, a `dataset_path` the caller may
  not read: each refused and audited); the declared schema, the size limit and
  the per-caller rate limit; pinning across an edit until republished as
  version 2; what `expose_pipeline` refuses; the exposure across a restart;
  `RateLimiter`'s sliding minute.
- [`tests/api/test_rest_server.cpp`](../../tests/api/test_rest_server.cpp)
  (`PublishedPipelineEndpointsOverHttp`): real loopback HTTP — 404 before
  exposure, 200 with outputs, the status route, 400, 413, a user without the
  right 403, 429 after the limit, 404 after unexpose.

## Later slices

- Scoring **cluster-resident** datasets through a served endpoint (a dataset
  reference in the predict body → the distributed `score` ComputeJob) — the
  posted-rows body is the first slice.
- Serving kinds beyond GBDT weights bodies (the loader is the
  `load_gbdt_model` boundary; other model kinds extend behind the same route).
- TLS on the served ingress rides the same #466 beast2 `https_server` slice
  as the rest of the API surface; per-entity serving keys minted from the
  GUI.

Search also requires the caller's SQL read rights on the source table and every
indexed field, and applies SELECT row policies to current consolidated rows (#1403).
Distributed searches subject to row security are refused.
This includes named-field and IDs-only requests. Exposure publishes the route;
it does not bypass source-data authorization. See
[search authorization](../indexing/node-search.md#caller-authorization-1403).
