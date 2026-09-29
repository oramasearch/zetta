# API implementation

Registry declarations generate REST routes, MCP tools and OpenAPI. Pub/sub CRUD
uses the same schemas and handlers on both transports, with coroutine long polls
and no thread per waiter. Successful polling reads share audit sessions and
window totals only when they return data; empty windows add no summary. Writes,
denials and failures stay per-call. The admin-only `purge_audit` action is
offloaded for its segment rewrite and has REST/MCP/OpenAPI parity. See
[CRUD](../../../docs/pubsub/crud.md) and
[REST/OpenAPI](../../../docs/api/rest-openapi.md).

`node_memory` renders the node's memory-accounting registry through the same
REST/MCP declaration. HTTP sessions, cached parsers, active requests, and
long-poll waiters add their counters to the node's registry while the server
lives. See [memory accounting](../../../docs/ops/memory.md).

The run-metric capabilities share this registry and publish exposed metric
topics. MCP receive and browse waits inspect the first governed result directly;
neither path holds a thread or changes non-object request error handling.
See [run metrics](../../../docs/ml/run-metrics.md).

`mcp_docs.cpp` also renders `/cli.md`. Its pub/sub and audit sections document
the dispatch-pool and class-specific retention flags with their environment
variables; the CLI parity test keeps that page synchronized with `src/main.cpp`.
