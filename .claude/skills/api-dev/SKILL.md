---
name: api-dev
description: Add or change capabilities on zetta's programmatic control surface — MCP tools, REST routes, the zetta::api registry, the Beast2 HTTP server, auth/consent, the main-thread bridge. Enforces the registry design (docs/api/registry-design.md) — one entry renders everywhere, engine entry points first, AccessKey auth + audit-on-authorize, Run/ComputeJob handles for long work, consent challenges the caller cannot self-satisfy, Beast2/Boost.JSON confined to the adapter TU. Use for any include/zetta/api, src/core/api, or MCP/HTTP work.
---

# api-dev — control-surface work checklist

Read first: **[docs/api/registry-design.md](../../../docs/api/registry-design.md)**
(the settled design) and **[docs/api/adding-a-capability.md](../../../docs/api/adding-a-capability.md)**
(the full recipe + review self-check). The experience contract this work serves is
[docs/product/experience/ax.md](../../../docs/product/experience/ax.md). This is the working distillation.

## Adding a capability

1. **Engine entry point first** — the handler is a thin caller of zetta-core
   (`Node::…`, `QueryService`, the canonical runners). Missing entry point?
   Build it in ITS domain first.
2. **One registry entry**: snake_case name (verb-first actions, noun reads) ·
   agent-actionable one-sentence description stating side effects ·
   schema-as-code input schema (pboettch-validated at the boundary; reject
   with the path-qualified reason) · output schema documenting the success
   value (entity reads compose it from the entity's `*_value_schema()`) ·
   `kind` read|action|job · `required_operation` (reads `api:query`; actions
   `orchestration:<name>`) · `mutates` (fires the activity hook) ·
   `requires_consent` for anything cloud-touching. The entry renders
   everywhere by itself — MCP tool, `GET/POST /v1/<name>` route, OpenAPI path
   (docs/api/rest-openapi.md); then regenerate the drift-checked artifact:
   `ZETTA_WRITE_SCHEMAS=1 ctest --test-dir build -R OpenApi`.
3. **Threading**: the handler body runs on the owning loop via
   `MainThreadBridge` — it may touch the Node but must be quick (it's inside
   a tick). Heavy work = `kind: job`, return the `run_id`/`job_id`
   immediately (the engine's durable handles — never invent async protocol,
   never block the request).
4. **Errors**: non-throwing `ApiResult`; domain failures are `isError: true`
   results carrying the engine's reason verbatim — only malformed
   JSON-RPC/unknown tool/schema-invalid input are protocol errors.
5. **Consent**: a consent-flagged tool NEVER executes on first call — it
   returns the named challenge; approval requires a distinctly-privileged
   principal. No exceptions (this is the cloud hard rule over the wire).

## Boundaries (hard rules)

- beast2 / cppalliance-http / Boost.JSON names appear ONLY in
  `src/core/api/http_server.cpp` (the adapter TU); Boost.JSON ↔ nlohmann
  converts once there. Registry/bridge/handlers stay HTTP-free in zetta-core.
- Default bind loopback; non-loopback requires a bearer token (an AccessKey
  id). Every call goes through `Node::authorize` (capability check + audit in
  one motion); loopback-no-token = the audited "unauthenticated local".
- Non-registry routes are a sanctioned exception ONLY for non-capability
  surfaces: llms.txt pages and the served entity endpoints
  (docs/api/serving.md — per-entity data-plane routes gated on the entity's
  replicated `exposed` flag, `data:predict`/`data:search` scoped, logic in
  the HTTP-free serving.cpp). A control capability is NEVER a bespoke route.

## Before shipping

- A **journey** exercising the capability (in-process registry layer, with a
  read-back assertion for anything mutating) + protocol coverage if the wire
  behavior is new — [docs/testing/mcp-journeys.md](../../../docs/testing/mcp-journeys.md).
- Docs same commit: tool table in docs/api/README.md, code-map for new files.
- `ctest` green incl. tests/api.
