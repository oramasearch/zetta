# Core implementation

The node wires its pub/sub service to owner-thread governance, table drains and
the audit trail. Pub/sub payloads and topic/message CRUD remain node-local.
See [architecture](../../docs/architecture/README.md) and
[pub/sub CRUD](../../docs/pubsub/crud.md).
