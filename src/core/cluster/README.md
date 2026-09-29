# Cluster implementation

Peer hello messages advertise the audit protocol. Compatible peers exchange
bounded status/range messages; incompatible peers retain ordinary catalog
replication but refuse audit. Only the CRDT adapter uses backend codecs to
compact/filter legacy audit history (#1164).
See [audit replication](../../../docs/governance/audit-trail.md).
