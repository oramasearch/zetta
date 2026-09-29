# Core implementation

The node wires its pub/sub service to owner-thread governance, table drains and
the audit trail. High-frequency authorised reads are counted in five-minute
actor/capability/scope windows only when a window returned data. Audit
persistence uses bounded-memory segments, independent ordinary/summary
retention, and an admin-only explicit purge. Per-origin audit ranges replicate outside the CRDT;
startup migrates and compacts legacy catalog audit history (#1164).
Flat audit fingerprints and reserved segment/base64 buffers limit retained
allocation overhead; bulk phase boundaries request allocator pressure relief.
Pub/sub payloads and topic/message CRUD remain node-local.
The default shared dispatcher grows at one worker per 250 active keys.
The node memory registry combines cheap ownership probes with platform allocator
and RSS readings, then feeds SQL, telemetry, API, logs, and status output.
Study deletion is an owner-thread Node operation that tombstones the Study and
non-promoted trial Runs while retaining promoted Model lineage (#1158).
See [architecture](../../docs/architecture/README.md) and
[pub/sub dispatch](../../docs/pubsub/dispatch.md), plus the
[memory operations guide](../../docs/ops/memory.md).
