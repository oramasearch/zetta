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
