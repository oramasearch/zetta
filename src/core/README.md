# Core implementation

The node wires its pub/sub service to owner-thread governance, table drains and
the audit trail. High-frequency authorised reads are counted in five-minute
actor/capability/scope windows; audit persistence uses bounded-memory segments
and configurable retention. Pub/sub payloads and topic/message CRUD remain node-local.
The default shared dispatcher grows at one worker per 250 active keys.
See [architecture](../../docs/architecture/README.md) and
[pub/sub dispatch](../../docs/pubsub/dispatch.md).
