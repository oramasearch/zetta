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
| `get_project` | read | api:query | one project's full detail (metadata + inputs + graphs) by name or uuid |
| `get_run` | read | api:query | one run's validated record (status / logs / outputs / lineage) by uuid |
| `get_model` | read | api:query | one model's validated record (kind / lineage / feature schema / metrics / weights fingerprint) by uuid |
| `get_job` | read | api:query | one compute job's validated record (kind / status / lineage refs / detail) by uuid |
| `get_index` | read | api:query | one index's validated record (table / kind / backend / fields) by uuid |
| `get_dataset` | read | api:query | one dataset's computed summary (schema / rows / size / replication / placement) by name (+ database when ambiguous) |
| `list_runs` | read | api:query | compact run listing (ids feed `get_run`) |
| `list_models` | read | api:query | compact model listing (ids feed `get_model`) |
| `list_jobs` | read | api:query | compact compute-job listing (ids feed `get_job`) |
| `list_indexes` | read | api:query | compact index listing (ids feed `get_index`) |
| `list_datasets` | read | api:query | every dataset's computed summary, structured (columns/partitions as arrays) |
| `list_traces` | read | api:query | recent observability spans |
| `get_trace` | read | api:query | every span of one trace id |
| `submit_feedback` | action | api:query | snapshot a feedback bundle (description + traces + errors + crashes + versions) as a large object; export is explicit (#640) |
| `create_project` | action | orchestration:create_project | create a replicated AI project |

All accept the reserved optional `intent` field (recorded to the span,
stripped before schema validation).

**Read parity (#616).** The typed `get_*` reads return the entity's
**already-validated value JSON** — exactly the `metadata()` shape the boundary
schema validated (the CRDT value / on-disk projection) — with the `id` added;
`get_dataset` / `list_datasets` return the computed `DatasetSummary` with the
schema and partition placement kept structured (the SQL view flattens them for
tabular reading). Every handler is a thin caller of the same live engine
source its `system.*` view computes from (`Node::…()`), so MCP, SQL, and the
GUI cannot drift (RULE-7). Arbitrary reads beyond these go through `sql`.
Action parity: #617.
