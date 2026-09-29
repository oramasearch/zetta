# Governance tests

Role tests keep the permission vocabulary and built-in mappings synchronized.
Pub/sub capability tests additionally exercise the topic/event/subscription scope
matrix. See [governance](../../docs/governance/README.md) and
[CRUD](../../docs/pubsub/crud.md).
Audit tests cover segment migration, index-only membership, retention of
summaries, schema round trips, and the million-record RAM/disk budgets (#1154).
That million-record test also proves the audit probe moves after 100,000 events
(#1163).
