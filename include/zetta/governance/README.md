# Governance interfaces

The closed audit taxonomy includes topic settings updates, event edits and event
deletions. Payload mutation records carry hashes and sequence metadata, not
payload copies. See [audit trail](../../../docs/governance/audit-trail.md).

`AuditTrail` stores validated events in checksummed gzip segments, keeps a
bounded recent window, migrates legacy JSON records, and retains read summaries
when ordinary detail expires (#1154). Its constant-time `memory_usage()` counters
separate the index, recent window, and pending persistence for `system.memory`
(#1163).
