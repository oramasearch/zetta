# REST rendering + OpenAPI generation (#618)

The capability registry served as plain HTTP routes for integrations, CI, and
customer systems that speak REST rather than MCP, with an OpenAPI document
generated from the registered input/output schemas — **one registry, two wire
renderings, zero drift**. Realizes the rendering the design settled in
[registry-design.md](registry-design.md) ("One registry, many renderings").

## The route table — derived, never authored

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
the MCP path: bearer token = the acting `AccessKey` id (401 on a transport
mismatch when a token is configured; loopback-no-token = the audited
"unauthenticated local"), then validate → `Node::authorize` + audit → handler
→ span. Inbound `traceparent` is honored. The response body is the same
envelope `structuredContent` carries over MCP:

```json
{ "ok": true, "value": { … }, "trace_id": "…", "span_id": "…", "duration_ms": 1.2 }
```

Domain failures keep the engine's reason verbatim and map onto HTTP statuses
(`rest_status`): `invalid_input` → 400 · `unauthorized` / `consent_required`
→ 403 · `not_found` / `unknown_capability` → 404 · `engine_error` → 422 ·
`busy` → 503.

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
read-back, error statuses, the served document, the bearer gate).
