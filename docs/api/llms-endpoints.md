# llms.txt progressive-disclosure endpoints

The node serves its own agent-readable documentation over plain HTTP
([#645](https://github.com/oramasearch/zetta/issues/645); the AX contract is
[../product/experience/ax.md](../product/experience/ax.md)): an agent with no
MCP client orients with one GET, learns the first three reads, and works its
way down to the full capability table. Follows the community
[llms.txt](https://llmstxt.org) convention (markdown served as plain text),
custom wherever AX demands it.

## The endpoints

Served by the same embedded Beast2 server as `POST /mcp` (#615) — same bind,
same flags (`--mcp-listen`, `--no-mcp`), same bearer policy: the always-on
auth gate (#799) fronts these pages too, so every request needs a bearer that
resolves to a user (the legacy `--mcp-token` shared secret was removed from the
CLI):

| Route | Serves |
|---|---|
| `GET /llms.txt` | orientation: what this node is (live facts), the **first three reads** (`server_info` → `sql` → `list_traces`), how to call a capability over `POST /mcp`, the topic index |
| `GET /llms/<topic>.txt` | one topic page (the bare name without `.txt` works too); unknown topic → 404 listing the valid topics |
| `GET /llms-full.txt` | the index plus every topic page as one document |

Topics: `capabilities` (every registry entry with kind, required operation,
description, input schema) · `cluster` (node id, version, live membership) ·
`data` (live databases, tables, the dataset-partition catalog) · `indexes`
(live registered indexes + how to search them) · `projects` (live AI projects,
runs/models/jobs) · `observability` (traces/spans and how to read them back).

## The no-drift rule (the design)

Every page is **generated at request time** from the capability registry's own
descriptions (`Registry::list()` / `find()`) plus live `Node` reads (node id,
`cluster_members()`, `database_names()`, `indexes()`, `dataset_partitions()`,
`project_names()`, the span ring) — **never a second prose source**. The
renderers add structure (headings, ordering, the curated first-three-reads
sequence); every capability description on a page is the registry entry's
`description` verbatim. Register a capability, create an index, join a peer —
the pages change on the next GET, nothing to sync.

## Placement

The renderers are HTTP-free zetta-core (`include/zetta/api/llms.hpp` +
`src/core/api/llms.cpp`); the three GET routes live in the one Beast2 adapter
TU (`src/core/api/http_server.cpp`) beside `/mcp`, marshal onto the owning
loop via the `MainThreadBridge` (rendering reads the main-thread-only `Node`
tree), and record an `llms:<page>` span (every operation is observable).

## Boundaries

- **No new JSON format** — the pages are `text/plain; charset=utf-8` output
  only; nothing is parsed.
- Rendering is read-only: no state change, no audit event, no consent surface.

The pages point agents at the REST rendering (#618,
[rest-openapi.md](rest-openapi.md)): the index names the `GET/POST /v1/<name>`
convention and `/v1/openapi.json`, and each capability page carries its REST
route and output schema — still generated from the registry, never a second
prose source. The spec index (`/v1/specs` + `list_specs`/`get_spec`, #646 —
[specs-index.md](specs-index.md)) completes the discovery story: every schema
in the system, one navigable set.

Tests: `tests/api/test_llms.cpp` (renderer content + liveness, plain GET over
a real socket, the bearer gate).
