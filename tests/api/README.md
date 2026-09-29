# API tests

Real loopback HTTP tests cover REST and MCP. Pub/sub CRUD parity compares registry,
OpenAPI and tools/list schemas and calls every operation through each transport.
Long-poll tests exercise both transports while owner work continues and assert
that their reads share an audit session instead of creating one row per return.
The SQL parity path also compares pub/sub dispatch metrics from `system.pubsub_topics`
and `system.node_metrics` over REST and MCP.
The core registry contains 127 capabilities, including the `node_memory` read.
Its parity test validates both REST and MCP values against one output schema and
compares their subsystem ordering. Both parity suites remain
required, alongside the MCP malformed-request and receive/browse long-poll
regressions.
See [CRUD verification](../../docs/pubsub/crud.md#verification).
