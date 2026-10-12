# Native tests

GoogleTest exercises the headless core and native loopback transports. Build
with at most six jobs and run one suite at a time. Audit lifecycle regressions
cover idle windows, class-specific retention, explicit purge, and peer trimming
(#1169). The audit proof worker is a
separate executable so each replicated node has independently measured resident
and restart peak memory (#1164). See [the proof runbook](../docs/testing/audit-replication.md).
Study lifecycle tests cover running-study refusal, non-promoted Run and metric
removal, promoted lineage retention, restart persistence, and REST/MCP parity
(#1158).
Server-level Study journeys additionally cover real M-1 dataset and Pipeline
training, restart resumption, wall and cost limits, active cancellation, curves,
analysis, and actual-model promotion (#1205). The full-table monthly-data
regression proves a learnable signal exceeds 0.7 validation AUC without losing
repeated entity observations; split preflight and cost reservation regressions
cover failed data status and cap-safe admission (#1209).
Run metric regressions cover same-step automatic gauges, competing initial and
trainer samples through the owner queue, restart ordering, strict caller
validation, and live-event step agreement (#1205). See
[pipeline tests](pipeline/README.md).
GBDT coverage verifies that out-of-time Study validation derives one global
timestamp cutoff and reports the same holdout across different shard counts.
Project graph and API tests cover denormalized name creation, rename propagation,
legacy startup backfill, page summaries, and REST/MCP list parity (#1184).
Native M-2 coverage includes keyed projection that collapses repeated snapshot
observations while preserving distinct text entity identifiers (#1204).
Dataset tests refuse ragged CSV by line, check lake's name rule, and keep a
table's source format across a restart and in the replicated `tbl:` value (#1221).
`query/test_sql_write_permissions.cpp` proves a reader is refused and a writer
allowed for every data-changing statement, object and column scope, row
security on UPDATE/DELETE, and the `data_changed` audit (#1229).
`connect/test_credential_seal.cpp` and `connect/test_connection_store.cpp` cover
sealed storage, every read surface, legacy re-sealing and key rotation (#1230).

`data/test_object_store.cpp` covers the object capability's contracts, a
conditional-create race, fencing, torn tails after a kill, compaction and restart,
the quota, and rf = 2 on three real nodes with a node lost, repaired and replaced;
`data/test_object_soak.cpp` holds disk and memory to the live set under a
liquid-shaped load (`ZETTA_OBJECT_SOAK_BYTES`); `api/test_object_routes.cpp`
runs the raw byte routes over loopback HTTP, with the six latency budgets as
failing benchmarks (#1005). See [the object capability](../docs/data/object-store.md).
