# Governance implementation

Permission vocabulary includes topics:read, topics:write and events:write;
subscription rights remain separate. Legacy pub/sub write atoms retain their
existing operation mappings. Mutation audit actions are schema-validated.
The audit projection uses checksummed compressed segments, a compact
constant-time id fingerprint index, batched legacy-file migration, and separate
ordinary/summary retention windows (#1154, #1169). Checksummed sidecars support
index-only startup; origin/sequence ranges and durable class watermarks replace
CRDT audit entries (#1164). `audit:purge` gives only admins the explicit rewrite
action, which records itself after completion. See [audit replication](../../../docs/governance/audit-trail.md)
and [pub/sub CRUD](../../../docs/pubsub/crud.md).
The projection also reports flat/seek index, segment-cache, recent-window, and pending-persist ownership
to the node memory registry (#1163).
The `data_changed` action records SQL statements that change data, schema or
governance (#1229).
