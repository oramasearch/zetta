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
admission (#1209). A study whose trials all failed ends failed with the last
trial's reason, `ml/metric_names.hpp` holds the one name per held-out metric
with its input aliases, and Run/Study captures made every training round are
coalesced to one per second (#1217).

Run and Model values also carry denormalized Project/Pipeline names. Node
startup backfills older records, and rename operations keep those names current
for list consumers (#1184).

`Table::source_format()`, `AddDatasetOptions::source_format` and
`DatasetSummary::source_format` record what a dataset was loaded from;
`data/add_dataset.hpp` exposes `dataset_name_problem` and `csv_problem`, the
upload rules lake also applies (#1221). See
[add dataset](../../docs/data/add-dataset.md).

`NodeConfig::connection_root_secret` (main passes the cluster join token) and
`retired_connection_root_secrets` key the sealing of saved connector
credentials; `Node::connection_keyring()` builds the keyring on first use, from
a node-local secret when no root secret is given. `connect/credential_seal.hpp`
seals and opens a credential, and `connect::reseal_connections` re-seals legacy
and old-key entries on start (#1230). `QueryService::acting_as` runs SQL as a
per-call identity (#1229). See
[connector query](../../docs/data/connector-query.md#saved-connections).

`data/object_store.hpp` is the object capability (#1005): `ObjectStore` keeps byte
objects by `(bucket, key)` in node-local logs, placed on `rf` owners, with put, put
range, ranged reads, list, barrier, usage, a quota and an owner lease whose fence
every mutation carries. `Node::object_store()` is null on a standalone node, like
the record store. See [the object capability](../../docs/data/object-store.md).

`data/backup_snapshot.hpp` is the backup snapshot and its restore (zetta#1518):
`SnapshotWriter` writes one consistent archive under `<data_dir>/backups/<id>/`,
`BackupSnapshots` finds, releases and expires them, `SnapshotRestore` applies an
archive fed a slice at a time and verifies it against its manifest, and
`discard_restored` empties a restored restore target. `data/write_gate.hpp` is the
non-blocking, bounded freeze the record and object stores carry (`write_gate()`).
`BackupFreeze` pauses every member's gates for one snapshot, and `BackupPeer` is each
node's `backup.gate` / `backup.body` signal handler, which also serves body slices to the
member taking a snapshot (zetta#1520). See
[backup snapshots](../../docs/data/backup-snapshot.md).
