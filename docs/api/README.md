# API docs

The programmatic control surface: the capability registry rendered as **MCP
tools** and (later) **REST routes** — drive the cluster without the GUI, with
the same governance as every other surface. Product framing:
[../product/control-surfaces.md](../product/control-surfaces.md); execution
plan: [../plans/mcp-service.md](../plans/mcp-service.md) (epic
[#614](https://github.com/oramasearch/zetta/issues/614)). Read here before any
MCP/HTTP work.

- [registry-design.md](registry-design.md) — **read first**: the settled design
  — one registry/many renderings, the main-thread bridge, auth = AccessKey +
  audit-on-authorize, long work = Run/ComputeJob handles (no new job infra),
  the error mapping, the consent challenge.
- [adding-a-capability.md](adding-a-capability.md) — the recipe + review
  self-check for every new tool/route.

## The endpoint (shipped with #615)

`POST /mcp` — JSON-RPC 2.0 (`initialize`, `notifications/initialized`, `ping`,
`tools/list`, `tools/call`), served by Beast2 on the vendored capy/corosio
stack, embedded in BOTH run modes. Defaults: `127.0.0.1:4800`, the audited
unauthenticated-local session; flags: `--mcp-listen HOST:PORT`,
`--mcp-token T` (the bearer token doubles as the acting AccessKey id),
`--no-mcp`. Every `tools/call` result carries `structuredContent` with
`ok/value|error`, `trace_id`, `span_id`, `duration_ms`; inbound `traceparent`
is honored. The handlers marshal onto the owning loop (headless tick / headed
frame) via the bridge — the loops drain it beside `Node::tick()`.

## Capabilities (v1)

| Name | Kind | Requires | Does |
|---|---|---|---|
| `server_info` | read | api:query | node identity, version, entity counts, membership |
| `sql` | read | api:query | one governed SQL statement — every `system.*` view |
| `search` | read | api:query | ranked fulltext search via [`Node::search`](../indexing/node-search.md) · distributed when partitioned · per-node provenance |
| `list_projects` | read | api:query | project names |
| `list_traces` | read | api:query | recent observability spans |
| `get_trace` | read | api:query | every span of one trace id |
| `submit_feedback` | action | api:query | snapshot a feedback bundle (description + traces + errors + crashes + versions) as a large object; export is explicit (#640) |
| `create_project` | action | orchestration:create_project | create a replicated AI project |

All accept the reserved optional `intent` field (recorded to the span,
stripped before schema validation). Wider parity: #616/#617.
