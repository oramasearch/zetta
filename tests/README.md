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
`data/test_export_path.cpp` proves a caller-named export (`generate_dataset`'s `path`,
`submit_feedback`'s `export_path`) is created new under `<data_dir>/exports` and never
truncates a sentinel outside it, through an absolute path, `..`, or a link (#1397, #1399).

`connect/test_destination_policy.cpp` classifies addresses and proves the native
client refuses a public-only request to loopback, by literal, by name and on a
redirect hop to the metadata address, before connecting (#1394).

`connect/test_egress_policy.cpp` covers the egress allowlist (every entry kind,
ports, loopback, ranges, malformed entries), and the egress cases in
`connect/test_native_http_transport.cpp` prove the dialler refuses an outside
address by default, an unlisted name before DNS, and a redirect to an outside
destination (#75). `connect/test_egress_gateway.cpp` covers the gateway's address and
names-only route parser, the `ZTEG1` preface and the re-read gateway files (#1517); the gateway cases in
`connect/test_native_http_transport.cpp` drive a stand-in gateway on loopback:
the preface, SNI on 443, the allowlist first, unrouted names direct, and redirect
hops (#1331).

`data/test_object_store.cpp` covers the object capability's contracts, a
conditional-create race, fencing, torn tails after a kill, compaction and restart,
the quota, and rf = 2 on three real nodes with a node lost, repaired and replaced;
`data/test_object_soak.cpp` holds disk and memory to the live set under a
liquid-shaped load (`ZETTA_OBJECT_SOAK_BYTES`); `api/test_object_routes.cpp`
runs the raw byte routes over loopback HTTP, with the six latency budgets as
failing benchmarks (#1005). See [the object capability](../docs/data/object-store.md).

`connect/test_connector_destinations.cpp` covers exact connector authorities,
resolved-address approval, and native HTTP, Postgres, MySQL, TiDB and the lake
SQL types refusing an unapproved private target before a connection, reaching an
exact approved `host:port`, never reaching metadata or link-local addresses, and
re-validating a saved connection whose type changed (#1414). The MySQL driver
tests approve their loopback fake server's exact `host:port`; `RegistryFixture.ConnectorDiscoveryRequiresApprovalForInlineDestinations`
covers saved discovery and privileged inline discovery for both identity systems.

`api/test_dataset_scope.cpp` covers the per-call dataset scope (#1515): the
header parser, the registry decision (a `SELECT` inside and outside the scope, an
empty scope, every non-`SELECT` statement, a capability that declares no reads,
the audited refusal, `list_datasets` and `get_dataset` under a scope and the
capabilities that stay refused, #1519), and over REST the header confining `/v1/sql`, a malformed
or repeated header (400), only capability routes answering (403 for `/mcp`, the
object routes and `/v1/specs`), and a 401 that stays a 401.
