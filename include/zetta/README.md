# Core interfaces

Public headers mirror their implementation domains. `Node` owns the audit
segment log and exposes bounded recent reads, per-origin class watermarks and
memory accounting. Cluster interfaces exchange versioned audit ranges
separately from the CRDT catalog (#1164). Audit interfaces also expose
independent summary retention and the admin purge action (#1169). See
[audit replication](../../docs/governance/audit-trail.md).

The public Study lifecycle includes guarded deletion: running studies are
refused, non-promoted trial Runs and their embedded metrics are removed, and
the promoted Model's producing Run is retained (#1158). See
[AutoML studies](../../docs/ml/studies.md).
