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

The ten Study capabilities publish the accepted M-1 search space and validate
and normalize a candidate without mutation. `create_study` uses the same path
for executable objectives, datasets, Pipeline references, and canonical search
controls before admission. They also
include `delete_study`, a separately permissioned action that refuses running
studies, removes non-promoted trial Runs and their metrics, and preserves the
promoted Model and producing Run. REST, MCP and OpenAPI share the registry
declarations (#1205, #1207). See
[AutoML studies](../../../docs/ml/studies.md).

The `list_runs`, `list_models`, and `list_studies` reads expose page-ready
display names, metrics and timestamps through one registry definition, keeping
REST, MCP and OpenAPI output in lockstep (#1184).

Dataset-backed scoring and explanation accept text business identifiers and
return them unchanged; their schemas are generated from the registry alongside
the dataset contract (#957).

`mcp_docs.cpp` also renders `/cli.md`. Its pub/sub and audit sections document
the dispatch-pool and class-specific retention flags with their environment
variables; the CLI parity test keeps that page synchronized with `src/main.cpp`.
