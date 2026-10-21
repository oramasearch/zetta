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

`records:read`, `records:write` and `records:manage` are object-scopable by
collection name prefix (`ResourceKind::Collection`); `metrics:read` and
`metrics:write` cover labelled time series (#1231). The role and access-key
schemas now list the `topic` and `collection` resource kinds the parser accepts.

`objects:read`, `objects:write` and `objects:manage` are object-scopable by
bucket name prefix (`ResourceKind::Bucket`, #1005); the role and access-key schemas
list the `bucket` kind.

`credential_redaction.cpp` implements the credential-field rule and
`redact_credentials`; `consent.cpp` renders a challenge's frozen input and
result through it, while the ledger keeps the real input to execute (#74). See
[secret references](../../../docs/governance/secret-references.md).
