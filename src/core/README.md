# Core implementation

The node wires its pub/sub service to owner-thread governance, table drains and
the audit trail. High-frequency authorised reads are counted in five-minute
actor/capability/scope windows only when a window returned data. Audit
persistence uses bounded-memory segments, independent ordinary/summary
retention, and an admin-only explicit purge. Per-origin audit ranges replicate outside the CRDT;
startup migrates and compacts legacy catalog audit history (#1164).
Flat audit fingerprints and reserved segment/base64 buffers limit retained
allocation overhead; bulk phase boundaries request allocator pressure relief.
Pub/sub payloads and topic/message CRUD remain node-local.
The default shared dispatcher grows at one worker per 250 active keys.
The node memory registry combines cheap ownership probes with platform allocator
and RSS readings, then feeds SQL, telemetry, API, logs, and status output.
Study reconciliation selects one replicated owner to run real M-1 trial graphs
on background schedulers. Owner-thread handoffs persist Runs, curves, budgets,
restart recovery, terminal events, and actual model promotion; deletion still
tombstones non-promoted trials while retaining promoted lineage (#1158, #1205).
Dataset trials keep all point-in-time rows after contract inference, temporal
class preflight records a failed data status before admission, and trial-second
reservations keep new batches within their cost cap (#1209).
Automatic Run gauges allocate increasing steps per metric on the owner thread;
caller metrics remain strict, and live batches report the stored steps. See
[Run metrics](../../docs/ml/run-metrics.md) (#1205).
The tick writes coalesced trainer captures once their second has passed, and
stop writes every pending one; reconciliation leaves a Run or Study with a
pending local change alone until it is written (#1217).
The owner thread also resolves display names at Run/Model creation, propagates
Project/Pipeline renames, and performs idempotent startup backfill (#1184).
Tables record their source format in `table.json` and the replicated `tbl:`
value; reconciliation, migration and the connector ingest's table adoption carry
it, and the CSV decoder refuses ragged rows with their line (#1221). See
[add dataset](../../docs/data/add-dataset.md).
See [architecture](../../docs/architecture/README.md) and
[pub/sub dispatch](../../docs/pubsub/dispatch.md), plus the
[memory operations guide](../../docs/ops/memory.md).
Node startup re-seals saved connector credentials that are plaintext (pre-#1230)
or sealed under a retired join token, and builds the sealing keyring from the
join token main passes, or from a node-local secret (#1230).

The node owns the keyed record store and the labelled time series when it has a
replicated catalogue (#1231). Startup replays `records.journal` after restoring
the document; the persist takes the journal's size, fsyncs the saved document and
only then trims the journal. The tick leaves record and metric keys out of
reconcile work and sweeps retention every ten seconds. The record store joins the
signal plane last and leaves it first. See
[the record store](../../docs/data/record-store.md).
