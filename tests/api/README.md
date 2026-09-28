# API tests

Real loopback HTTP tests cover REST and MCP. Pub/sub CRUD parity compares registry,
OpenAPI and tools/list schemas and calls every operation through each transport.
Long-poll tests exercise both transports while owner work continues.
The core registry contains 118 capabilities: the shared 108 plus five CRUD and
five run-metric additions. Both parity suites remain required, alongside the
MCP malformed-request and receive/browse long-poll regressions.
See [CRUD verification](../../docs/pubsub/crud.md#verification).
