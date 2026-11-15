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
same flags (`--mcp-listen`, `--no-mcp`). These discovery pages are registered
before the auth gate, so every caller gets a page, and they answer **per caller**
(#1357): an authenticated caller reads every capability; an anonymous caller (or
one whose credential does not resolve) reads only the publicly callable ones,
`publicly_callable()` in `rest.hpp`, which is none today. They describe the
build's API and contain no live workspace inventory (#1402, LSR-169). Live reads
use the authenticated MCP or REST capability routes and their authorization
checks:

| Route | Serves |
|---|---|
| `GET /llms.txt` | orientation: product and build version, the **first three reads** (`server_info` → `sql` → `list_traces`), how to call a capability over `POST /mcp`, the topic index |
| `GET /llms/<topic>.txt` | one topic page (the bare name without `.txt` works too); unknown topic → 404 listing the valid topics |
| `GET /llms-full.txt` | the index plus every topic page as one document |

Topics: `capabilities` (every registry entry with kind, required operation,
description, input schema) · `cluster` (where to read membership) · `data`
(where to query databases, tables and partitions) · `indexes` (how to search) ·
`projects` (project capabilities and related system views) · `observability`
(how to read traces and spans).

## The no-drift rule (the design)

Every page is **generated at request time** from the capability registry's own
descriptions (`Registry::list()` / `find()`) and the build version. The
renderers add structure (headings, ordering, the curated first-three-reads
sequence); every capability description on a page is the registry entry's
`description` verbatim. Register a capability and the pages change on the next
GET. Creating resources, joining peers, and recording spans do not change the
pages: the renderer API accepts only the registry, with no `Node` access.

Node ids, cluster membership, database/table/partition names, index fields,
projects, and resource or span counts are never included, for anonymous or
authenticated callers. Clients that consumed those facts from the documentation
must use the corresponding authenticated capabilities.

## Who sees the capability catalogue (#1357)

The pages accept the same credentials as the gate (`Authorization: Bearer`, else
the `zetta_session` cookie from a trusted origin) and resolve them the same way,
spending the same per-source failed-authentication budget (#1417). They never
refuse: a missing or failed credential only selects the anonymous rendering.

| Caller | `/llms.txt` | `/llms/<topic>`, `/llms-full.txt` | `/v1/openapi.json` |
|---|---|---|---|
| anonymous, or a credential that does not resolve | product, version, the first three reads **by name only**, how to authenticate and call, the topic index | the topic prose with no capability lines; `capabilities` says to authenticate | the document over no capability: `info`, `servers`, the security scheme and the shared components, no `/v1` path |
| any authenticated user | the same with each first read's description | every capability, description and schema | every capability (the drift-checked `schemas/openapi.json`) |

Why this split and not per-tool filtering: the catalogue is the build's API shape,
identical on every node of a version, and holds no workspace data, so hiding parts
of it from a user who holds one tool protects nothing (#766's reasoning); the
per-tool views stay in `tools/list` and the `/v1/specs` index. Hiding it from
callers with no credential removes the one thing an anonymous scanner gained from
the node: a ready map of every capability, its inputs and its required rights.
The anonymous pages still teach an agent how to authenticate, which is why they
stay before the gate. Responses carry `Vary: Authorization, Cookie` and
`Cache-Control: private, no-cache` so no shared cache serves one caller's
rendering to another.

## Placement

The renderers are HTTP-free zetta-core (`include/zetta/api/llms.hpp` +
`src/core/api/llms.cpp`); the three GET routes live in the one Beast2 adapter
TU (`src/core/api/http_server.cpp`) beside `/mcp`, marshal onto the owning
loop via the `MainThreadBridge` (for the registry and span recorder), and
record an `llms:<page>` span (every operation is observable).

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

Tests: `tests/api/test_llms.cpp` (registry-derived content, plain GET over
a real socket, and inventory independence across every route with anonymous,
invalid, unprivileged and owner credentials).
