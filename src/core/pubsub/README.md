# Pub/sub implementation

Stores preserve sparse positions across payload edits and deletes. Topics own
revision checks, retained delivery state and retention; the facade authorizes
concrete addresses and emits hash-only audit records. Node integration connects
the audit sink on the owner thread. See [CRUD](../../../docs/pubsub/crud.md)
and [stores](../../../docs/pubsub/stores.md).
