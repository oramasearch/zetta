# Governance interfaces

The closed audit taxonomy includes topic settings updates, event edits and event
deletions. Payload mutation records carry hashes and sequence metadata, not
payload copies. See [audit trail](../../../docs/governance/audit-trail.md).

`AuditTrail` stores validated events in checksummed gzip segments, keeps a
bounded recent window, migrates legacy JSON records, and applies independent
ordinary and summary retention, including explicit purge (#1154, #1169). Its
bounded `memory_usage()` counters
separate the indexes, segment cache, recent window, and pending persistence for `system.memory`
(#1163).
Its origin/watermark/range interface ships audit independently of the catalog;
ordinary and summary low-water marks prevent expired rows from returning, and
reserved `legacy` records never ship (#1164).
`data_changed` records every SQL statement that changes data, schema or
governance — its actor, object and outcome, refused or not (#1229). See
[access control](../../../docs/governance/access-control.md#per-statement-enforcement).

`ResourceKind::Bucket` (`Resource::bucket`) names an object bucket by name prefix,
so `objects:read`, `objects:write` and `objects:manage` are object-scopable
`ON BUCKET <prefix>` (#1005). See
[the object capability](../../../docs/data/object-store.md#rights).

`credential_redaction.hpp` is the one rule for which input fields hold a
credential (`is_credential_field`: `token`, `secret`, `password`, `api_key`,
`*_token`, `*_secret`, `*_password`) and `redact_credentials`, which the consent
challenge JSON and the web form use so an inline credential is never rendered
back out (#74). See
[secret references](../../../docs/governance/secret-references.md).
