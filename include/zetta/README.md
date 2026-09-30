# Core interfaces

Public headers mirror their implementation domains. `Node` owns the audit
segment log and exposes bounded recent reads, per-origin class watermarks and
memory accounting. Cluster interfaces exchange versioned audit ranges
separately from the CRDT catalog (#1164). Audit interfaces also expose
independent summary retention and the admin purge action (#1169). See
[audit replication](../../docs/governance/audit-trail.md).

The public Study lifecycle includes a replicated background driver, retryable
trial Runs, cooperative training progress, and guarded deletion: running
studies are refused, non-promoted trial Runs and their embedded metrics are
removed, and the promoted Model's producing Run is retained (#1158, #1205). See
[AutoML studies](../../docs/ml/studies.md).
The interface also exposes full-table split validation and a failed Study state
with a durable data reason; the driver reserves estimated trial-seconds before
admission (#1209).

Run and Model values also carry denormalized Project/Pipeline names. Node
startup backfills older records, and rename operations keep those names current
for list consumers (#1184).
