# REST rendering + OpenAPI generation (#618)

The capability registry served as plain HTTP routes for integrations, CI, and
customer systems that speak REST rather than MCP, with an OpenAPI document
generated from the registered input/output schemas — **one registry, two wire
renderings, zero drift**. Realizes the rendering the design settled in
[registry-design.md](registry-design.md) ("One registry, many renderings").

## The route table — derived, never authored

Read capabilities may declare `post_alias` to also accept a JSON-body
`POST /v1/<name>` without changing read classification or removing GET.
The router and OpenAPI derive both from that registry flag. Pub/sub get/list/browse
declare it (#1136), and so do `sql`, `get_record`, `list_records` and `query_metric`
(#1231); aliases use `<name>_post` as their distinct OpenAPI operation id. A `sql`
statement in a body is not bound by the ~4 KiB request line the GET form travels in;
whichever form it arrives in, the executor authorizes each statement and audits a
data-changing one as `data_changed` (#1229).
Revision conflicts use HTTP 409. See [pub/sub CRUD](../pubsub/crud.md).

The object capability (#1005) adds the routes the registry rendering cannot
express: `PUT`, `PATCH` and `GET /v1/objects/<bucket>/<key>` move raw bodies
(`application/octet-stream`, streamed a slice at a time), and the bucket routes
`GET /v1/objects/<bucket>`, `GET …:usage`, `POST …:barrier`, `POST
…:acquire|:renew|:release` and `DELETE …/<key>` are aliases of the
`list_objects`, `object_usage`, `object_barrier`, lease and `delete_object`
capabilities. Their OpenAPI operations come from `object_route_paths()`, beside
the capabilities. The error codes they add map to HTTP as `fenced`, `held` and
`incarnation_changed` (lake#543) 409, `range_not_satisfiable` 416 and
`quota_exceeded` 507. See
[the object capability](../data/object-store.md).

`zetta::api` (`include/zetta/api/rest.hpp`, `src/core/api/rest.cpp` — HTTP-free
zetta-core) derives everything mechanically from each registry entry:

- **reads** → `GET /v1/<name>` — input in the **query string**, each value
  coerced by its declared type in the entry's input schema (integer/number/
  boolean literals parse; array/object values are JSON-encoded; anything that
  does not parse as its declared type stays a string so the schema boundary
  rejects it with its own path-qualified reason).
- **actions and jobs** → `POST /v1/<name>` — input as the **JSON body**
  (empty body = `{}`).
- `GET /v1/openapi.json` — the OpenAPI 3.1 document, generated from the same
  schemas (below).

The Beast2 adapter TU (`src/core/api/http_server.cpp` — still the ONE
wire-facing TU) registers the routes at `start()` by iterating the registry;
adding a capability entry is the whole job — it appears as an MCP tool, a REST
route, and an OpenAPI path with no further wiring.

## Same auth, same audit, same observability

A REST call is `Registry::call` on the owning loop via the bridge — exactly
the MCP path: the always-on auth gate (#799) resolves the bearer to a **user**
via the token store (else 401 — there is no unauthenticated path), then
validate → `Node::authorize_user` + audit → handler → span. Inbound `traceparent` is honored. The response body is the same
envelope `structuredContent` carries over MCP:

```json
{ "ok": true, "value": { … }, "trace_id": "…", "span_id": "…", "duration_ms": 1.2 }
```

Domain failures keep the engine's reason verbatim and map onto HTTP statuses
(`rest_status`): `invalid_input` → 400 · `unauthorized` / `consent_required`
→ 403 · `not_found` / `unknown_capability` → 404 · `conflict` → 409 (a taken
name, e.g. `upload_dataset` without `if_exists: "rename"`, #1221) · `engine_error`
→ 422 · `busy` → 503.

`upload_dataset` takes a file's text in the JSON body, at most 16 MiB of it. A larger
file is uploaded to a bucket with the raw `PATCH /v1/objects/<bucket>/<key>?offset=<n>`
route a chunk at a time, then loaded with the `load_dataset_object` job
(`POST /v1/load_dataset_object`): it answers 200 with a `run_id` at once, refuses a
taken name with the same 409 and words as `upload_dataset`, and reports a failed load
on the run (`get_run`), not in the response (#1302,
[add-dataset.md](../data/add-dataset.md#load-from-a-stored-object-1302)).

**Every request gets an answer — never silence (#1007).** Three ways a request
used to get *no* response, each of which reads exactly like a hung node (curl: 0
bytes until its own timeout; through a proxy, "Empty reply from server"):

- **No route matched.** The router fell through and the worker wrote nothing. A
  terminal handler (`serve_unrouted`, the last registration) now answers:
  `/v1/<name>` for a capability called with the wrong method → **405**
  `method_not_allowed` with an `Allow` header and the method it takes (reads are
  `GET`, actions/jobs `POST` — the misreading #954 was filed on); `/v1/<name>` for
  no capability → **404** `unknown_capability` (a node image older than the
  capability you asked for answers this way); any other path → **404**
  `not_found`. The connection closes after it, since the request body was not
  read.
- **A handler threw.** `Registry::call` runs the handler inside
  `invoke_contained`, which turns an exception into an `engine_error` (422) naming
  the capability — a handler reading a column of an unexpected type from
  replicated state answers instead of dying. `MainThreadBridge::drain` also
  contains a throw from any owner-loop work item and still resumes its awaiter,
  so nothing is parked forever and the rest of the batch runs.
- **The body could not be serialized.** Response bodies are written with
  `wire_dump`, which replaces invalid UTF-8 (a customer's Latin-1 column) with
  U+FFFD rather than throwing out of the route.

```bash
curl 'http://127.0.0.1:4800/v1/server_info'
curl 'http://127.0.0.1:4800/v1/sql?statement=SELECT%20*%20FROM%20system.datasets;'
curl -X POST http://127.0.0.1:4800/v1/create_project -d '{"name":"churn-q3"}'
curl 'http://127.0.0.1:4800/v1/openapi.json'
```

## Output schemas — the other half of zero drift

`Capability` gained the `output_schema` field the design always documented:
a draft-07 schema of the handler's success value, declared next to the input
schema (schema-as-code). All 35 registered capabilities carry one (the six
#621 cloud/consent tools included); the typed entity
reads (`get_run`/`get_model`/`get_job`/`get_index`) **compose theirs from the
same embedded `*_value_schema()` the boundary validates**, so the tool
contract and the entity record cannot drift. It renders three ways: the MCP
tool's `outputSchema` (`tools/list`), the OpenAPI 200 response (the envelope
with `value` typed by it), and the llms.txt capability pages. It is
documentation asserted by tests/journeys, not a runtime output filter.

## The OpenAPI document is schema-as-code

`openapi_document(registry)` is the source of truth;
[`schemas/openapi.json`](../../schemas/openapi.json) is its generated,
drift-checked artifact — exactly the `schemas/*.json` discipline
([../architecture/schemas.md](../architecture/schemas.md)). After any
capability or schema change:

```bash
ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R OpenApi
```

The served `/v1/openapi.json` is generated from the live registry at
`start()` (after any embedding-added entries), so the wire document, the
committed artifact, and the registry are one source. Per capability the
document carries: `operationId` = name, the governance line (required
operation, job/consent notes) appended to the description mechanically, query
parameters or a request body from the input schema **plus the reserved
`intent` field**, the 200 envelope with `value` = the output schema, and
`4XX`/`5XX` referencing the shared `error_envelope` component.

## Boundaries

- **No new JSON format is parsed**: REST inputs land in the same
  pboettch-validated input schemas `Registry::call` already enforces; the
  OpenAPI document is generated output, never read back.
- `rest.hpp`/`rest.cpp` stay HTTP-free; beast2/Boost.JSON stay confined to
  `http_server.cpp`.
- The spec index (`/v1/specs`, #646) shipped —
  [specs-index.md](specs-index.md); SSE/streaming stays with the MCP surface.

Tests: `tests/api/test_openapi.cpp` (route derivation, query coercion, status
mapping, document ↔ registry, the added-once rule, the drift check) and
`tests/api/test_rest_server.cpp` (loopback HTTP: the GET read journey, POST →
read-back, error statuses, the served document, the bearer gate, unrouted
requests answered with 404/405, and #1007's dataset-less run → `get_run_report`
path end to end). Its test client bounds every read at 10 s, so a server that
never answers fails the test instead of hanging the suite.
