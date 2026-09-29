# Cluster implementation

Peer hello messages advertise the audit protocol. Compatible peers exchange
bounded status/range messages; incompatible peers retain ordinary catalog
replication but refuse audit. Only the CRDT adapter uses backend codecs to
compact/filter legacy audit history (#1164).
Catalog rebuilds and legacy-audit retirement remeasure the replication memory
counters from the current encoded-value cache; probe subtraction is clamped so
removed catalog history cannot wrap bytes or items (#1167).
See [audit replication](../../../docs/governance/audit-trail.md).
