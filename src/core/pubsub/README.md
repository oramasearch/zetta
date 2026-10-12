# Pub/sub implementation

Stores preserve sparse positions across payload edits and deletes. Topics own
revision checks, retained delivery state and retention; the facade authorizes
concrete addresses and emits hash-only audit records. Node integration connects
the audit sink on the owner thread. The dispatcher preserves per-key order and
round-robin fairness while resizing its shared pool between jobs. See
[dispatch](../../../docs/pubsub/dispatch.md), [CRUD](../../../docs/pubsub/crud.md)
and [stores](../../../docs/pubsub/stores.md).
