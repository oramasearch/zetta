# Core interfaces

Public headers mirror their implementation domains. `Node` owns the audit
segment log and exposes bounded recent reads, per-origin class watermarks and
memory accounting. Cluster interfaces exchange versioned audit ranges
separately from the CRDT catalog (#1164). Audit interfaces also expose
independent summary retention and the admin purge action (#1169). See
[audit replication](../../docs/governance/audit-trail.md).
