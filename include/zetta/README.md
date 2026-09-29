# Core interfaces

Public headers mirror their implementation domains. `Node` owns the audit
segment log and exposes bounded recent reads, per-origin watermarks and memory
accounting. Cluster interfaces exchange versioned audit ranges separately from
the CRDT catalog (#1164). See [audit replication](../../docs/governance/audit-trail.md).
