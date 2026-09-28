# Core implementation

The node wires its pub/sub service to owner-thread governance, table drains and
the audit trail. Pub/sub payloads and topic/message CRUD remain node-local; its
default shared dispatcher grows at one worker per 250 active keys.
See [architecture](../../docs/architecture/README.md) and
[pub/sub dispatch](../../docs/pubsub/dispatch.md).
