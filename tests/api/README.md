# API tests

Real loopback HTTP tests cover REST and MCP. Pub/sub CRUD parity compares registry,
OpenAPI and tools/list schemas and calls every operation through each transport.
Long-poll tests exercise both transports while owner work continues.
The core registry count and presence assertions include all five CRUD additions.
See [CRUD verification](../../docs/pubsub/crud.md#verification).
