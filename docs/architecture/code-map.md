# Code map

Issue #1136 adds local navigation READMEs in `include/zetta/{api,governance,pubsub}`,
`src/core/` and `src/core/{api,governance,observability,pubsub}`,
`tests/{api,governance,pubsub}`, and `schemas/`. They link the implemented
contracts to [pub/sub CRUD](../pubsub/crud.md), the new API/example page.
The existing pub/sub stores own sparse payload mutation; the topic layer owns
revisions/redelivery; registry declarations generate both transports and schemas.

Issue #1164 adds navigation READMEs in `include/zetta/`,
`include/zetta/{cluster,compute,pipeline}/`, `src/core/{cluster,query}/`, `tests/`, `tests/cluster/`,
and `.claude/skills/zetta-dev/`. They link to the segment-log replication
contract and [native audit proof](../testing/audit-replication.md).

The annotated, file-by-file map of the zetta codebase — what every header and
translation unit is for, which issue introduced it, and which library it builds
into. This is the highest-fidelity map of the repo that exists: **keep it in
sync in the same commit whenever files are added, moved, or removed** (the
docs-with-the-change discipline). For the subsystem narrative — how these pieces
fit together at runtime — see [architecture.md](architecture.md); for how-to
recipes and gotchas when working in the code, see the `zetta-dev` skill's
[reference.md](../../.claude/skills/zetta-dev/reference.md).

Directory **`CLAUDE.md` rule-cards** carry each tree's scoped rules and are
auto-loaded when working there: [`src/core/`](../../src/core/CLAUDE.md) ·
[`src/gui/`](../../src/gui/CLAUDE.md) ·
[`include/zetta/`](../../include/zetta/CLAUDE.md) ·
[`tests/`](../../tests/CLAUDE.md) · [`docs/`](../CLAUDE.md) ·
[`schemas/`](../../schemas/CLAUDE.md) — the disclosure map is in the root
`CLAUDE.md`.

## Build targets

`CMakeLists.txt` builds four targets, in dependency order: the vendored
**zetta_transport** (independent Corosio + mutual TLS 1.3 framed transport) ·
**imgui** library → **zetta-compute** (the accelerator interface; optional CUDA
backend behind `-DZETTA_ENABLE_CUDA=ON`) → **zetta-core** (the static core
library — **no SDL/ImGui dependency**, the build-level guarantee that a node
runs headless) → the **zetta** executable (links SDL3 + ImGui on top of
zetta-core). The GoogleTest unit tests build behind `ZETTA_BUILD_TESTS`.

## Code layout

```
CMakeLists.txt              build graph: zetta_transport (and ZETTA_TRANSPORT_ONLY consumer mode), imgui lib, zetta-compute lib (+ optional CUDA), zetta-core lib, zetta exe
.dockerignore               build-context filter for the zetta-bridge image Dockerfiles (COPY . /src); keeps a host build/ + its absolute-path CMakeCache.txt out of the image (#839) — see docs/build.md
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
assets/fonts/               embedded Geist + Geist Mono TTFs (SIL OFL 1.1) + README
scripts/install-deps.sh     install system build tools (macOS / Ubuntu)
scripts/gen-parquet-fixture.py  dev-time pyarrow generator for the committed Parquet fixtures (tests/fixtures/parquet/ + the embedded src/core/testkit/parquet_fixture_data.hpp)
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases + projects + deployments + runs + models + solution monitors + indexes + jobs + dataset partitions + principals; the immutable solutions ledger (record_action/record_outcome/ledger_entries, #1107); owns the shared ExperimentManager (#1105); the per-node IndexRegistry (index_registry(), #528); the per-node checkpoint store (checkpoint_store()/checkpoints(), <data_dir>/checkpoints, RULE-5/#468); the per-node telemetry sampler (sample_node_metrics(), #581) + the cluster-wide telemetry snapshot (telemetry_snapshot(), #584); the live near-data pipeline instrumentation registry (record_pipeline_job()/pipeline_jobs(), the bounded source for system.pipeline_jobs, #589); the node's span ring (span_recorder(), the ONE source for system.traces/system.spans + list_traces/get_trace, #640); the computed-at-query-time dataset summaries (datasets(), the source for system.datasets, #585) + the detected-provisioner cache (record_provisioner_config()/provisioner_configs(), ephemeral, the source for system.provisioners); run_graph; the audit sink; cluster_members() (the replicated node:<id> membership, #519); the long-lived compute plane over the real cluster — attach_cluster() + job_coordinator() (#579)
  runtime.hpp               zetta::runtime — async/coroutine interface over Capy (task/io_task/io_result/block_on/spawn/thread_pool + parallel_for data-parallel fan-out + the structured-concurrency pair when_all/timeout, #827)
  transport.hpp             umbrella include for the independent zetta::transport library (#1283)
  transport/                README.md (navigation); credentials.hpp (standard-only TlsCredentials); security.hpp (TlsCredentialFiles, PEM loading + verified mutual TLS 1.3 Corosio context); framed_stream.hpp (FramedStream/HandshakeRole, bounded newline frames over a connected plain or TLS Corosio socket; one reader + one writer)
  memory/
    README.md              navigation for the public memory-accounting interface
    accounting.hpp         zetta::memory::MemoryAccounting — mergeable subsystem probes, allocator/RSS truth, unattributed and excess accounting, log formatting, and optional allocator trim (#1163)
  runtime/net.hpp           engine networking interface (Corosio; included by src/core/cluster/cluster.cpp for listener/dialer work and src/core/api/native_http_transport.cpp for outbound HTTPS); the reusable TLS/framing surface is include/zetta/transport/ (#1283)
  headless.hpp              zetta::run_headless(Node&, Cluster&)
  signals.hpp               install_signal_handlers / stop_requested
  api/                      the programmatic control surface (docs/api/): registry.hpp (Capability/ApiResult/ApiContext/Registry — one entry renders as MCP tool + REST route + palette action; every call a span, authorized + audited), bridge.hpp (MainThreadBridge — coroutine-first marshaling onto the owning loop via runtime::async_event), background.hpp (BackgroundActions — owns the worker handles kind:job capabilities launch, pruned each drain on the owner thread; #617), capabilities.hpp (register_core_capabilities: the reads — server_info · sql · search · the get_*/list_* parity set · list_traces · get_trace · list_ledger · get_experiment · get_monitor · get_roi_readout · the #646 spec-index reads list_specs · get_spec — plus submit_feedback · create_project · create_experiment · assign and the #617 governed actions capture_intent · import_n8n · compile_intent/n8n/python · gate · deploy · run · add_dataset · create_index (the public create path over a loaded table, #825) · testsuite_run · record_action · record_outcome, the #648 serving actions expose_model/unexpose_model/expose_index/unexpose_index, and register_cloud_capabilities: the #621 consent-gated cloud tools cloud_inspect · detect_provisioners · add_nodes + the consent tools list_consent_requests · approve_consent · deny_consent; docs/api/governed-actions.md, docs/api/consent.md, docs/solutions/audit-ledger.md), rest.hpp (the REST rendering, #618 — rest_path/rest_method derived from each entry's kind, query-string coercion by the input schema, the ApiError -> HTTP status map, openapi_document generating the drift-checked OpenAPI 3.1 doc; docs/api/rest-openapi.md), server.hpp (ApiServer — the embedded Beast2 MCP + REST endpoint behind a pimpl interface; POST /mcp, GET/POST /v1/<name>, GET /v1/openapi.json, bearer auth, drain()), llms.hpp (the llms.txt progressive-disclosure pages, #645 — render_llms_index/topic/full, public docs from the registry + build version with no Node access (#1402); docs/api/llms-endpoints.md), specs.hpp (the spec index, #646 — spec_index/find_spec over the registry + zetta::schemas::boundary_schemas(): the OpenAPI document + every capability and boundary schema as one navigable set, served from the C++ accessors; rendered as list_specs/get_spec and GET /v1/specs[/name]; docs/api/specs-index.md), native_http_transport.hpp (install_native_http_transport/shutdown_native_http_transport — the native coroutine HTTPS client that fills connect::HttpTransport, built into zetta-api because zetta-core links no HTTP library; docs/api/native-http-transport.md, #827), serving.hpp (governed HTTP serving of exposed models/indexes, #648 — the drift-checked predict/serve-search request schemas, resolve_served_model, serve_predict/serve_search (data:predict/data:search key scoping, serve:* spans, audited), model/index_endpoint_path; docs/api/serving.md)
    capabilities.hpp       registration entry points, including the decision-policy, audit-ledger, experiment, monitor, ROI-readout, and action-integration capabilities shared by MCP and REST
  schemas/                  registry.hpp — zetta::schemas::boundary_schemas()/find_boundary_schema (#646): the ONE name → accessor → generated-file table over every schema-as-code boundary schema; consumed by tests/test_schemas.cpp (drift check + write mode) and the spec index (docs/architecture/schemas.md)
  solutions/                monitor.hpp — zetta::solutions::Monitor and its specification, observation, breach, calibration, cohort-performance, guardrail, and schema contracts (#1106)
  observability/            trace.hpp — OTel-style TraceContext/Span/SpanRecorder/ScopedSpan + traceparent codec; the four captures (intent · plan · performance · result_shape) on a bounded per-node ring (docs/architecture/observability.md); activity.hpp — ActivityEntry/ActivitySurface/ActivityOutcome + activity_entries(Node) — the merged audit-trail+runs activity feed, computed at call time (#622, docs/gui/activity-feed.md); crash.hpp — crash reports (#640): the fatal-signal handler (install_crash_handler), the validated <data_dir>/crashes/<uuid>.json report (CrashReport + schema), load_crash_reports (the system.crashes source)
  cluster/                  the distributed plane: membership, the signal plane, collectives, CRDT replication, telemetry
    cluster.hpp             zetta::cluster::Cluster — equal-peer membership over TCP (mutual TLS when ClusterConfig::security is enabled) + the signal-plane API (request_each: one request to several peers in one round, #1231)
    security.hpp            zetta::cluster::ClusterSecurity + generate/valid_join_token/derive_cluster_security/load_or_create_cluster_security/cluster_secret_schema — inter-node transport security (CTL-2/TB-3, #466): the join token derives the cluster CA (deterministic Ed25519), each node mints a CA-signed identity, persisted under <data_dir>/security/ with a schema-validated secret file (docs/cluster/security.md)
    signal.hpp              zetta::cluster::Signal/SignalHandler — ephemeral RPC/event/task signals + envelope encode/decode (ADR-28)
    collectives.hpp         zetta::cluster::Collectives — allreduce/broadcast/barrier/gather over the signal plane (#365); wire_bytes() + rounds_completed() live meters feed system.collective_rounds (#589)
    replication.hpp         zetta::cluster::ReplicationManager — Automerge CRDT state; remove() writes a TOMBSTONE (never a map delete) and knows_key() sees through it (#838); update() is an atomic read-check-write (#1231); cheap document/cache/change-journal counters split audit ownership from other replicated state (#1163)
    node_metrics.hpp        zetta::cluster::NodeMetrics/NodeMetricsSampler — live per-node host resource telemetry plus process RSS/allocator/probe totals, sampled from OS counters; ephemeral/observed, the source for system.node_metrics; graceful degradation to null when a counter is absent (#575 area 2, #581, #1163)
    node_telemetry.hpp      zetta::cluster::NodeTelemetryExchange + encode_node_metrics/decode_node_metrics/node_metrics_wire_schema — cluster-wide live telemetry over the signal plane (#584): each node broadcasts its own NodeMetrics as an ephemeral `node.metrics` event, a SignalHandler (the JobCoordinator's fallback) caches peers' by sender id; the schema-validated wire boundary; feeds the animated node-box gauges via Node::telemetry_snapshot(); not replicated
  data/                     the data model: databases, tables, partitioning + placement, large objects, datasets, imports
    database.hpp            zetta::data::Database — owns tables (disk-backed)
    table.hpp               zetta::data::Table — owns a storage engine (its rows); configurable per-table with a Partitioner; partition_only() marks a table whose rows are NEVER captured into the CRDT (docs/data/partition-only-tables.md, #833); release_local_rows() frees such a table's local copy once its partitions are written (#1302)
    partitioner.hpp         zetta::data::Partitioner/SinglePartitioner — the partitioning interface: record→partition + partition→node(s) over a fixed P>>nodes, pluggable per table (ADR-31/ENT-37; #512); + the record→partition strategies HashPartitioner (FNV-1a hash(key columns)%P) / RoundRobinPartitioner (surrogate%P for even balance) + the partitioner_from_json/to_json config boundary (#513)
    fetch_affinity_placement.hpp  zetta::data::FetchAffinityPlacement — fetch-affinity partition→node placement: the node that FETCHED a slice owns the partitions it wrote, so a distributed ingest never shuffles; replicas spread by rendezvous (#833 §6)
    partition_scheduler.hpp zetta::data::PartitionScheduler/DefaultPartitionScheduler/PartitionRequest/PartitionPlan/SourceLayout — the policy ABOVE Partitioner: how many partitions (a 64 MiB target body size with a floor of partitions per node, or a manual override honoured exactly), how big each targets, and which nodes own them (weighted by live NodeTelemetry; an unmeasured node weighs like a typical one, no telemetry at all spreads evenly). The plan has a validated JSON boundary because it TRAVELS — capacity-aware placement is not derivable per node (docs/data/partition-scheduler.md, #833)
    rendezvous_placement.hpp  zetta::data::RendezvousPlacement/rendezvous_owners — rendezvous (highest-random-weight) partition→node placement: a membership change moves only ~1/N of partitions, deterministic + leaderless (ADR-31/ENT-37; #514)
    partition_body.hpp      zetta::data::serialize_partition_body/read_partition_body/sniff_partition_body — the partition body format: Parquet (`_zid` as a column) for every dataset, format sniffed from the bytes so legacy .jsonl bodies still read, JSON Lines fallback for rows Parquet cannot express (docs/data/partition-body.md, #833)
    partition_writer.hpp    zetta::data::write_partitioned/PartitionWriteResult — the partitioned write path: assign each row to a partition (Partitioner), serialize each non-empty partition to a JSON Lines blob, store it as a LargeObject (import_bytes), record a DatasetPartition catalog entry per partition (blob ref + cumulative row range + place_partition owners); N blobs + N catalog rows (ADR-31/ENT-37; #511)
    promote_dataset.hpp     zetta::data::promote_dataset/PromoteDatasetResult/PartitionPromotion — raise a dataset's replication factor (a cache at RF=1 -> the durable 3): add owners to each part: entry and the bodies follow over the existing large-object transport; never removes an owner, clamped to the live roster (docs/data/promote-dataset.md, #833)
    partition_rebalance.hpp  zetta::data::rebalance_partitions/RebalanceResult — between-jobs rebalance on a membership change: recompute rendezvous placement for the sharded partitions preserving rf=owner count, update changed owner_nodes (set_owner_nodes captures → bodies move via #515); skip replicate-to-all; ≈rf/N move on a join. Two triggers: directly from add_nodes(Node&, …) (#519) and auto-triggered in Node::tick off node:<id> membership keys (ADR-31/ENT-37; #517, TC-10/#500)
    dataset_partition.hpp   zetta::data::DatasetPartition — a partition catalog entry: where a table's data slice lives (table/blob_ref/row_range/stats/owner_nodes) + JSON Schema; replicate-to-all; set_owner_nodes is the capturing re-placement mutator the rebalance uses (ENT-36/TBL-13; #517). `stats` also carries the body's own Parquet schema + byte size, so a node can plan and prune without reading a body (#837)
    dataset_columns.hpp     zetta::data::ColumnProfile/DatasetColumn — a dataset's typed columns `[{name, type, nullable, null_fraction}]` (JSON families), profiled from rows or from per-partition `column_stats`; what get_dataset answers (docs/data/column-order.md, #1021)
    dataset_manifest.hpp    zetta::data::DatasetManifestValue/catalog_shortfall — the table-level PARTITION MANIFEST (`mfst:<table-id>`): how many part: entries the finished dataset HAS, so a reader can tell "I can see 6 of 8" from "there are 6" and FAIL the query rather than answer short; written once by the node that completed the write (docs/data/partition-manifest.md, #837)
    dataset.hpp             zetta::data::DatasetSummary/DatasetPlacement — a read-only, computed-at-query-time summary of one dataset (name/format/replication/rows/size/columns + per-partition placement); one dataset == one table, no stored Dataset entity — recomputed from the live Node tree joined with the partition catalog so it never drifts; the engine source behind system.datasets and the GUI dataset views (#585, #575 area 3; docs/data/system-datasets.md)
    large_object.hpp        zetta::data::LargeObjectStore — files as blobs (per database); import_file (from a path) + import_bytes (from in-memory bytes, used by the partitioned write path)
    import.hpp              zetta::data::import_records — JSON/JSONL files → table rows
    dataset_path.hpp        zetta::data::parse_dataset_path/DatasetContract/infer_dataset_contract/sample_dataset/resolve_dataset — THE one `dataset_path` grammar (a filesystem directory OR `database.table`) plus the shared catalog-and-sample read, then the column contract a partitioned table cannot state for itself: which column is the entity (integer-keyed), the label ({0,1}), and the features; every inference reported in `notes`, and the point-in-time cutoff NEVER inferred because it decides the leakage boundary (#947, #1062)
    import_path.hpp         zetta::data::import_root/resolve_import_path/ImportPathResult — the one directory (<data_dir>/imports) a caller may name files under, and the rules that keep every caller-named path inside it (#1391, #1433); caller_path_components, the lexical rules shared with the export root
    export_path.hpp         zetta::data::export_root/write_export_file/ExportResult — the one directory (<data_dir>/exports) a caller may have the node write a file in: the import root's lexical rules, no link on the way, O_CREAT|O_EXCL so nothing existing is overwritten (#1397, #1399)
    categorical_encoding.hpp zetta::data::CategoricalEncoding/fit_categorical_encoding/encode_categorical_row — low-cardinality text columns (at most kCategoricalMaxLevels = 32 levels) one-hot encoded as `<column>=<level>` for the M-1 GBDT: the qualifying rules, label-free fitting on the training rows, the JSON form stored in a Model's feature_schema, and row encoding where an unseen level is all zeros (#1203)
    featurize_plan.hpp      zetta::data::DatasetAnalysis/FeaturePlan/analyze_dataset/plan_features/methodology_rule — read-only semantic and table-shape evidence plus the centralized string-keyed methodology rules that produce explicit feature, exclusion, or required-transform results (#1062)
    add_dataset.hpp         zetta::data::add_dataset_file + AsyncDatasetAdd — "Add dataset" affordance backend: load a parquet/jsonl/json/csv file as a partitioned table (RendezvousPlacement + write_partitioned), so it surfaces in system.datasets with partitions placed across the cluster; CSV type-inferred, no new JSON boundary (#587); the async handle decodes on a worker + materializes in owner-thread pump slices with progress/cancel + a dataset_added audit event (#624); add_dataset_rows lands already-decoded in-memory rows through the same materialize/partition/audit path — the connector-query landing path (#827); DatasetLineDecoder, the line-at-a-time CSV/JSONL decoder every load path shares, AsyncObjectDatasetLoad, load_dataset_object's job body that streams an object from a bucket into a partition-only table, and recover_object_dataset_loads, which fails at start the loads its node left running (#1302)
    replication_strategy.hpp  zetta::data::ReplicationStrategy — per-table/db replication strategy (ReplicateToAll | Partitioned | PartitionedWithReplicas(factor)) + JSON boundary; recorded/persisted/replicated, default ReplicateToAll (ENT-35; #509)
    record_store.hpp        zetta::data::RecordStore — the keyed record store: opaque base64 records by (collection, key); definitions in the replicated catalogue, records in a node-local log; if_absent/if_revision writes ordered by the key's rendezvous owner (record.put forwarding), fsynced, fanned out to every live peer (record.apply) before the acknowledgement; anti-entropy (record.digest/keys/fetch); index lists; retention (#1231; docs/data/record-store.md)
    object_store.hpp        zetta::data::ObjectStore — the object capability: byte objects by (bucket, key); bucket definitions and owner leases in the replicated catalogue, bytes in node-local per-bucket logs; atomic put (staged uploads), put-if-absent, append at an exact offset, ranged reads (ObjectReader), list, barrier, usage, quota; rf owners by rendezvous hash with the primary ordering each key (object.write) and the other owners installing the decided version (object.apply); fenced mutations; anti-entropy and eviction of unowned copies (#1005; docs/data/object-store.md)
    metric_store.hpp        zetta::data::MetricStore — labelled counters and gauges: definitions in the catalogue, per-node bucket records in the record log's `_metric.<name>` collections, range queries summed across nodes, grouped and stepped; retention as bucket expiry (#1231; docs/data/time-series.md)
    virtual_table.hpp       zetta::data::VirtualTable/VirtualColumn/VirtualColumnTier — a user-declared VIRTUAL TABLE over a CRDT key-family (durable KEY + durable/ephemeral columns) + virtual_table_to_json/from_json boundary; durable `vtable:<name>` catalog metadata, rows computed at query time (CREATE VIRTUAL TABLE, #809 slice 3; docs/data/virtual-tables.md)
    blob_store.hpp          zetta::data::BlobStore — large-object body transfer interface
    replicated_blob_store.hpp  zetta::data::ReplicatedBlobStore — BlobStore from CRDT + disk; partition-aware placement: a partition body is fetched/held only by its owner node(s) per the catalog + replication factor, not replicate-to-all (#515)
  connect/                  source connectors (docs/data/connector-query.md)
    distributed_fetch.hpp   zetta::connect::plan_partition_assignments / plan_fetch_assignments / submit_connector_statement(_async) / fetch_result_partitions(_serially) — a warehouse result set is N independently-fetchable partitions, so it is TILED (disjoint slices balanced by rowCount, deterministic so no plan travels) and read CONCURRENTLY in bounded waves through the coroutine transport interface, instead of walked serially on one node (#827)
    distributed_ingest.hpp  zetta::connect::run_distributed_connector_ingest / install_connector_fetch_launch_runner — the CROSS-NODE ingest: submit once, co-launch the roster over `run.launch`, every node derives the same tiling and pulls ONLY its tile, landing it as partition-only partitions IT owns; one allreduce agrees, and a node dying mid-fetch fails the whole ingest (#833 §7)
    connection_store.hpp    zetta::connect::SavedConnection + save/find/list/remove_connection + reseal_connections — named connections stored in the reserved zetta_connect.connections table, whose default ReplicateToAll strategy puts the credential on every node (each node fetches its own stripe straight from the source), sealed (#1230); to_redacted_json() is the only shape any read surface returns, with `credential` saying only whether one is set (#827)
    credential_seal.hpp     zetta::connect::CredentialKeyring + seal_credential / unseal_credential / load_or_create_local_keyring / sealed_credential_schema — AES-256-GCM sealing of connector credentials under a key derived from the join token, never a catalogue key (#1230)
    connector.hpp           zetta::connect::ConnectorConfig/ConnectorRows/ConnectorResult/run_connector_query — run a read-only query against an external system and return typed rows for data::add_dataset_rows to shard across the cluster; Snowflake (SQL API v2) first; read-only single-statement guard at the boundary, token never serialized, no warehouse driver in-engine (ADR-17) (#827)
    http_transport.hpp      zetta::connect::HttpRequest/HttpResponse/HttpTransport + AsyncHttpTransport — the injected outbound-HTTP interface in both a synchronous and a coroutine (io_task) shape, so every connector is unit-testable against a fake with no network/credentials AND a distributed fetch can fan N exchanges out under runtime::when_all; secret_headers marks values no transport may put in a reason/log/argv; set_default_http_transport/default_http_transport hold the pair the zetta-api client installs at startup — zetta-core links no HTTP library (#827)
  storage/
    engine.hpp              zetta::storage::StorageEngine interface — at(i) returns a ParquetLazyPtr handle, not a json& (#833); id_at(i) returns std::string BY VALUE so an engine may derive an id rather than hold one per row (#836); memory_usage reports resident rows and caches (#1163)
    lazy_row.hpp            zetta::storage::ParquetLazyPtr/RowPage/RowKey/RowPageSource — the row handle: a refcounted page that survives the store reaping it, a borrowed (allocation-free) state for always-resident engines, a failed read reported as a reason not null (docs/data/lazy-rows.md, #833)
    json_store.hpp          zetta::storage::JsonStore — in-memory vector<json> (header-only)
    jsonl_store.hpp         zetta::storage::JsonlStore — JSONL on disk, 10k/chunk; maintains a mutation-time resident-row estimate for memory accounting (#1163) and the chunk files' byte count, stored_bytes() (#1195)
    parquet_store.hpp       zetta::storage::ParquetStore/ParquetStoreOptions/ParquetIdMode — disk-backed paging StorageEngine over a Parquet body: footer-only open, a row group per page, byte-budgeted cache, refused mutations, and POSITIONAL ids by default so nothing is held per row (id_index_bytes() reports the Indexed alternative's cost) (docs/data/parquet-store.md, #833, #836)
  query/                    zetta::query — the SQL layer: dialect frontends over a dialect-neutral AST (docs/data/query.md)
    ast.hpp                 the generic, dialect-neutral query AST (CreateDatabase/DropDatabase/CreateTable/DropTable/Insert/Select statements + TableRef/WhereClause/AggregateExpr) every frontend parses into and the executor runs — support a new dialect by writing a frontend, never by touching execution
    dialect.hpp             zetta::query::Dialect/ParseResult — the frontend interface: dialect text → generic statements
    postgres.hpp            zetta::query::PostgresDialect — the PostgreSQL-flavored frontend: a basic subset (CREATE/DROP DATABASE|TABLE, INSERT … VALUES, SELECT … [WHERE col = value]); case-insensitive keywords, `--` comments, `;`-separated statements
    executor.hpp            zetta::query::Executor/QueryResult — run generic statements against a Node (dialect-neutral, only ever sees the AST), current_database for unqualified table names; hosts the read-only system.* observability tables (nodes/datasets/dataset_partitions/node_metrics including pub/sub dispatch sizing/pipeline_jobs/collective_rounds/spans/traces/crashes/provisioners/activity/…) + system_table_names(), the public span over the system-table catalog (one list for system.tables, the explorer, and the error suggestions; #684/#1157)
    predicate.hpp           zetta::query::compare_cell/row_matches — the WHERE comparison primitive, shared so a scattered SELECT filters exactly like a local one (#833)
    distributed_select.hpp  zetta::query::DistributedSelect/PartitionScanRequest/PartitionScanResponse/merge_scan_responses/select_over_partitions — scatter-gather SELECT over a partition-only table: assign each partition to ONE reachable owner, scan near the data over the `sql.scan` signal-plane RPC, merge (count/sum add, avg weights by row count, min/max reduce); an unreachable owner fails the query (docs/data/scatter-gather-select.md, #833)
    service.hpp             zetta::query::QueryService/Outcome — the facade the UI (and tests) call: parse with the Postgres dialect + execute every statement; pass a Cluster to make the nodes system table queryable; swapping/adding a dialect is a one-line change here
  parquet/                  zetta::parquet — native (no Arrow/parquet-cpp) Parquet reader (ADR-32; #481)
    snappy.hpp              zetta::parquet::snappy_decompress/snappy_compress/snappy_compress_literals — Snappy raw-block page codec (Parquet's default codec), non-throwing + bounds-checked (#507 decode; #833 encode + its literals-only reference)
    metadata.hpp            zetta::parquet::parse_parquet_metadata — read a file's Thrift FileMetaData (schema + row-group/column-chunk layout, incl. per-leaf max def/rep levels); Thrift compact decoder, non-throwing, JSON projection + schema boundary (#505)
    column_decoder.hpp      zetta::parquet::decode_column/decode_column_chunk — read a column's data pages into typed values + a validity bitmap: PLAIN (int/float/bool/byte_array) + dictionary (RLE/bit-packed indices) + definition levels; uncompressed + snappy codecs (zstd/gzip rejected); non-throwing, JSON projection + schema boundary (#506)
    reader.hpp              zetta::parquet::read_parquet_rows — assemble metadata+decoders into a whole-file reader: decode every leaf column → one JSON object per row (the inverse of the JsonlStore row shape); the untyped-rows reader the churn loader uses (#508)
    writer.hpp              zetta::parquet::write_parquet_rows/infer_parquet_schema — the native Parquet WRITER (inverse of the reader): Thrift compact footer + v1 data pages, PLAIN values + RLE definition levels, SNAPPY page bodies, schema inferred from JSON rows; non-throwing, round-trips through read_parquet_rows (#833)
  ir/
    step_kind.hpp           zetta::ir::StepKind — connect→…→act taxonomy (header-only)
    execution_graph.hpp     zetta::ir::ExecutionGraph — typed step nodes + edges
    serialization.hpp       zetta::ir::to_json/from_json — the graph's JSON form
  input/
    language_frontend.hpp   zetta::input::LanguageFrontend/FrontendResult — interface: lower a source language to the ExecutionGraph IR (the IR-targeting analogue of query::Dialect); non-throwing (ENT-38/ADR-33; #520)
    python_frontend.hpp     zetta::input::PythonFrontend/python_step_kind — simulated-Python frontend: parse a Python subset (assignments/calls/attribute+method chaining/literals) → IR (calls→typed steps via the engine-faithful verb table — train→model, score/predict/rank→index, explain→serve, add_nodes→act; refs/receivers→data-flow edges); not a Python runtime (#520/#522)
    cluster_bindings.hpp    zetta::input::run_add_nodes_step/add_nodes_count/add_nodes_cost_cap — execute a lowered sim-Python cluster.add_nodes IR node against the permanent-add control action zetta::cloud::add_nodes (provider injected, cloud consent-gated); the Node-aware overload composes the BurstApprovalGate when the script names a cost_cap= and rebalances partitions onto the new members (#522, #502)
    datasets_bindings.hpp   zetta::input::run_load_dataset_step/DatasetStepExecutor/dataset_load_spec/DatasetResolver — the `datasets` library shim: execute a lowered sim-Python load_dataset("<path>", columns=…) IR node by decoding the parquet via the native reader (read_parquet_rows) into a (partitioned) table; file/network IO behind the injectable DatasetResolver interface (offline default = local path); HF datasets is the fidelity reference only (#521)
  pipeline/                 AI projects: inputs → compiled execution graphs → correctness gate → deploy → runs (+ the solution catalog)
    project.hpp             zetta::pipeline::Project — an AI project; owns inputs + execution graphs
    project_input.hpp       zetta::pipeline::ProjectInput — raw intent/artifact input + provenance
    project_graph.hpp       zetta::pipeline::ProjectGraph — a named ExecutionGraph under a project
    intent_compiler.hpp     zetta::pipeline::compile_intent — methodology/solution/keyword resolution → registered ExecutionGraph shape (FEAT-2; #1061)
    n8n_import.hpp          zetta::pipeline::import_n8n_workflow — n8n workflow JSON → artifact ProjectInput (schema-validated)
    n8n_compile.hpp         zetta::pipeline::compile_n8n_workflow — n8n workflow JSON → ExecutionGraph IR (nodes→steps, connections→edges; #308)
    graph_plan.hpp          zetta::pipeline::project_cluster_plan/ClusterPlan/StepPlan/StepPlacement — project how an ExecutionGraph would run over the current cluster (near-data / fan-out / all-nodes / coordinator per step, near-data locality propagated forward along DataFlow edges); pure + deterministic, the engine of the Graph Workspace plan view (#591, #575 area 6); no new JSON boundary
    executor.hpp            zetta::pipeline::GraphExecutor / StepExecutor — orchestrate a graph into a Run; GraphExecutor::run_as runs each step as a principal (#843)
    step_authority.hpp      zetta::pipeline::RunPrincipal / step_operation / decide_step / authorize_step / authorize_graph_steps — deny-by-default per-step authority from the run's principal, each decision audited as data_access (#843)
    run_progress.hpp        zetta::pipeline::project_run_progress/RunProgress/StepRunProgress/StepRunStatus — project a Run's logs+outputs onto an ExecutionGraph (per-step pending/running/succeeded/failed + failing reason + attributed outputs); pure + per-frame cheap, the engine of the Graph Workspace live-run overlay (#626); no new JSON boundary
    validation.hpp          zetta::pipeline::ValidationReport — correctness-gate result (ENT-5/FEAT-3)
    gate.hpp                zetta::pipeline::CorrectnessGate — gate runner: checks → report (RULE-1)
    graph_checks.hpp        zetta::pipeline shared and methodology-specific correctness checks, including snapshot lineage/blackout windows, label gold isolation and privacy budgets (#309/#1103/#1058/#1059)
    run.hpp                 zetta::pipeline::Run — one graph execution (status/logs/outputs/lineage), replicated column-oriented metric series, params, summaries, typed artifacts + Run JSON Schema (#1137)
    run_proposal.hpp        zetta::pipeline::RunProposal + RunProposalStore — a run the chat agent proposed with an impact preview, replicated as `run_proposal:<uuid>`; proposed → approved | rejected (#969)
    deployment_target.hpp   zetta::pipeline::DeploymentTarget — a placement (ENT-6; R1 local)
    deployment.hpp          zetta::pipeline::Deployment — a graph placed onto a target
    deployment_controller.hpp  zetta::pipeline::DeploymentController — reconcile local deployments desired→actual + self-heal (FEAT-4/TC-3)
    solution_catalog.hpp    zetta::pipeline::SolutionCatalog — AI Solution Library (deployable blueprints) + schema-validated parse
  solutions/
    snapshot.hpp            zetta::solutions SnapshotSpec/materialize_snapshot and AsOfJoinSpec/as_of_join — replicated point-in-time tables, row-level nearest-prior joins, time parsing and JSON boundaries (#1103)
  compute/                  zetta::compute — the accelerator interface (ADR-27/ARC-9; no SDL/CUDA in headers)
    data_type.hpp           zetta::compute::DataType — element-type tag + byte_size/data_type_of<T> (header-only)
    compute_status.hpp      zetta::compute::ComputeStatus — the no-throw call result (header-only)
    device_buffer.hpp       zetta::compute::DeviceBuffer — typed, owning, move-only device memory
    compute_device.hpp      zetta::compute::ComputeDevice — the interface: info + memory + the operator set
    host_stub_device.hpp    zetta::compute::HostStubDevice — host-memory transfer stub (ops unsupported)
    cpu_device.hpp          zetta::compute::CpuDevice — the always-built reference backend (every op on host memory, multi-threaded; #360)
    cuda_device.hpp         zetta::compute::CudaDevice — the GPU backend (CUDA-free header; guarded by ZETTA_WITH_CUDA, #363)
    mlx_device.hpp          zetta::compute::MlxDevice — the Apple-Silicon/Metal backend (MLX-free header; guarded by ZETTA_WITH_MLX); unified-memory model + gemm/axpy on GPU; default macOS accelerator, CPU fallback; SYCL deferred (#555)
    mlx_transformer.hpp     zetta::compute::model — the native transformer (MLX-aware, ZETTA_WITH_MLX): attention/MLP/RMSNorm blocks, MlxTransformer (forward/generate/checkpoint), cross_entropy + policy_gradient loss, AdamW, distributed_adamw_step, base_agent_config (FEAT-12; #555)
    mlx_qwen.hpp            zetta::compute::model::MlxQwenInference + MlxQwenModel (MLX-aware, ZETTA_WITH_MLX) — FUSED Qwen3 GPU inference (one MLX graph/decode, KV cache in MLX arrays; 42.9 tok/s), fused training (differentiable full-seq forward + mx::value_and_grad + AdamW in one graph; ~290 ms/step over 0.6B vs ~7-10 s CPU), and a fully-fused GPU grpo_step (KV-cached rollouts + group value_and_grad + AdamW, shared params; learns on real 0.6B at ~2.2 s/step) (FEAT-12; #555)
    safetensors.hpp         zetta::compute::model::serialize/deserialize_safetensors — the HuggingFace safetensors format (8-byte header + JSON + data), F32 write, F32+BF16 read; how the model saves a checkpoint / ingests open weights (FEAT-12; #555)
    device_registry.hpp     zetta::compute::DeviceRegistry — per-node enumerate/report/select devices (ARC-9; #362)
    work_stealing_scheduler.hpp  zetta::compute::WorkStealingScheduler — local per-worker-deque work-stealing task pool (ARC-10/#366)
    distributed_scheduler.hpp  zetta::compute::DistributedScheduler — cross-node steal over the signal plane, elastic mid-job membership (ARC-10/#366/#367)
    job_coordinator.hpp     zetta::compute::JobCoordinator/JobRound — split a ComputeJob into tasks on the scheduler, drive collectives at round boundaries, drive job lifecycle (ARC-11/#372); a running Node owns one over its real Cluster via Node::attach_cluster/job_coordinator (the SPMD spine, #579)
    coordinated_launch.hpp  zetta::compute::CoordinatedLaunchRequest/CoordinatedLaunchListener + encode/decode_coordinated_launch + participant_rank — the cross-process SPMD co-launch (#601): a schema-validated `run.launch` Task fans an interactive run out to the roster; the listener validates+queues on the network thread and Node::tick drains/executes on the owner thread; runner registry per launch kind
    checkpoint.hpp          zetta::compute::JobCheckpoint/CheckpointStore/FileCheckpointStore/run_resumable_rounds — burst-job eviction tolerance: durable sha256-fingerprinted progress snapshot (validated boundary) + a durable store interface + a resume-from-last-checkpoint driver (graceful spot-drain), so an evicted job resumes without recomputing committed rounds (RULE-5/RSK-9; #468)
    compute_job.hpp         zetta::compute::ComputeJob — one unit of accelerated compute (kind/lifecycle) + ComputeJob JSON Schema (ARC-11/ENT-31)
    scaling_policy.hpp      zetta::compute::ScalingPolicy — per-job autoscaler reward objective (cost/runtime/data-shape) + bounds + evaluate() + JSON boundary (ADR-29/ENT-34; #370)
    job_autoscaler.hpp      zetta::compute::JobAutoscaler/AutoscalerConfig — autoscaler control loop: observe a running ComputeJob, score its ScalingPolicy, add/remove nodes via the Provisioner within bounds, hysteresis + cost-cap shed, elastic membership, best-effort on given hardware (ADR-29/ARC-11; #371)
    pipeline_instrumentation.hpp  zetta::compute::PipelineJobInstrumentation/NearDataJobKind — the per-job near-data instrumentation record (kind / partitions+rows read / local_bytes_read vs wire_bytes_moved / status) the near-data runners record into the Node; ephemeral/observed, the source for system.pipeline_jobs; no new JSON boundary (#589, #575 area 5)
  ddf/
    data_frame.hpp          zetta::ddf::DataFrame/Column/DataFrameSchema — typed columnar frame over a table, chunked, ComputeDevice-backed + loader/infer/JSON boundary (ARC-12/ENT-33; #380)
    parquet_reader.hpp      zetta::ddf::load_parquet_data_frame/infer_parquet_schema — native Parquet → DataFrame reader with column projection: decode only the requested leaf columns into DeviceBuffers (drop embedding_0..3071), numeric/dense boundary + parquet_data_frame schema for the golden (ADR-32; #508)
    ops.hpp                 zetta::ddf::scan/filter/project — DDF ops over a DataFrame, built on the ComputeDevice interface (gather-based filter, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (ARC-12; #381)
    join.hpp                zetta::ddf::distributed_join — DDF distributed inner equi-join: repartition by key via collectives (gather+broadcast) then local hash/sort-merge join per shard (ARC-12; #382)
    data_frame_ops.hpp      zetta::ddf::sort/concat/slice — frame-shaping DDF ops: order by a key column (device sort_by_key), row-wise union, contiguous row range (ARC-12; #385)
    distributed_sort.hpp    zetta::ddf::DistributedSorter/PartitionMessage — distributed sort across the cluster (sample-partition + local sort + merge) over collectives + the signal-plane all-to-all shuffle (ARC-12; #385)
    group_by.hpp            zetta::ddf::group_by/distributed_group_by — DDF groupby-aggregate (count/sum/min/max/mean), partial-per-node + gather/merge/broadcast combine + GroupBySpec JSON boundary (ARC-12; #383)
    as_of.hpp               zetta::ddf::as_of_aggregate/AsOfAggregate — DDF window / as-of (point-in-time) op: per-entity reduce of rows in a time window (time <= as_of), the leakage-free featurization primitive + JSON boundary (ARC-12; #384)
    group_by_job.hpp        zetta::ddf::run_group_by_job — run the distributed groupby-aggregate as a recorded ComputeJob (kind ddf) through the job coordinator (drives distributed_group_by on the JobRound; result on every node) (FEAT-8; #387)
    local_partitions.hpp    zetta::ddf::load_local_partitions/LocalShardResult — compute-near-data shard loader: enumerate this node's OWNED partitions of a table (owner_nodes ∋ node_id) from the catalog, read only the locally-held blob bodies, parse the JsonlStore {_zid,doc} lines, load into a local DataFrame; instrumented bytes_read (near data, not moved) (ARC-12/FEAT-8; #516)
    local_group_by_job.hpp  zetta::ddf::run_near_data_group_by_job/NearDataGroupByResult — the first-class compute-near-data op: load the local shard via load_local_partitions → run_group_by_job, reporting local_bytes_read vs wire_bytes_moved (the partials, via Collectives::wire_bytes()); result == single-node oracle (ARC-12/FEAT-8; #516)
    featurize.hpp           zetta::ddf::point_in_time_featurize/FeaturizeSpec/FeatureTable — point-in-time (leakage-free) featurization: churn_features.sql as a DDF graph (as-of + group-by + filter + join) → a feature table for training (time <= as_of) + JSON boundary; also a static carry-through mode (empty event_features → carry engineered columns + label, point-in-time row filter where a time_column is set, #532) (ARC-12/FEAT-6; #388)
    featurize_job.hpp       zetta::ddf::run_featurize_job — run the point-in-time featurization as a recorded `featurize` ComputeJob across the cluster (distributed_join + distributed_group_by on the JobRound; global table on every node) (ARC-12/FEAT-6; #388)
    local_featurize_job.hpp  zetta::ddf::run_near_data_featurize_job/NearDataFeaturizeResult — compute-near-data featurize over a single partitioned table: load this node's owned partitions via load_local_partitions → run_featurize_job over the local shard; the engineered-columns slice of UC-1 at scale, result == single-node oracle (ARC-12/FEAT-8; #532)
  ml/
    entity_resolution.hpp  zetta::ml M-2 records, owned candidate pairs, scoring, conflict-safe entities, metrics, and resolved-table runner (#1041/#1072)
    entity_resolution_job.hpp  zetta::ml::run_entity_resolution_job — distributed candidate ownership, iterative statistics allreduce, component merge, bounded pair streaming, and coordinated-launch contract (#1072)
    segmentation.hpp        zetta::ml — segmentation parameters, trainer, metrics and resolved-dataset runner (#1042; docs/ml/segmentation.md)
    segmentation_methodology.hpp M-3 graph/launch contracts, peer runner installation and typed report reader (#1042)
    methodology.hpp         zetta::ml::Methodology and its shape/ownership/run callback contract; ordered registry lookup and graph resolution (#1061)
    workspace_methodology.hpp  zetta::ml — workspace methodologies (#1261): the replicated `methodology:<W-id>` value schema and rules, WorkspaceMethodologyStore, the listing, the python_methodology shape and graph resolution
    workspace_study.hpp        zetta::ml — experiments over a workspace methodology's declared parameters (lake#689): the published search space, spec normalization, trial admission and Lake's runner work items
    solution.hpp            zetta::ml::Solution catalogue contract, typed defaults, capability levels, methodology availability, and composed execution graph (#1102/#1128)
    study.hpp               zetta::ml::Study replicated record, typed search space, Run-backed trial lifecycle, TPE/grid/random samplers, ASHA/median pruners, budgets, analysis and work-stealing runner (#1138)
    uplift.hpp              zetta::ml native T/X uplift learners, propensity model, held-out Qini/AUUC evaluation, bootstrap interval, and targeting policy (#1045)
    uplift_job.hpp          zetta::ml::run_uplift_job — distributed owned-partition training and gathered/broadcast uplift evaluation (#1045)
    uplift_runner.hpp       zetta::ml::run_uplift_on_table — resolved-table runner that writes model, effect/Qini tables, run state, and lineage (#1045)
    graph.hpp               zetta::ml graph core and resolved-dataset runner: time-bounded features, PageRank, components, label propagation, clustering, embeddings, output tables, Model, and lineage (#1051)
    graph_job.hpp           zetta::ml::run_graph_job — distributed edge-partition statistics and iterative PageRank allreduce through a recorded train ComputeJob (#1051)
    document_extraction.hpp zetta::ml M-11 native PDF/PGM recovery, TrOCR runner, declared-field extraction, review routing, metrics, and resolved-table runner (#1050)
    speech.hpp              M-14 audio decode/resample/log-mel, energy segmentation, native Whisper checkpoint/inference, privacy filtering, WER, transcript runner contract (#1053)
    speech_train_job.hpp    distributed M-14 numeric statistics reduction through JobRound allreduce (#1053)
    bandit.hpp              zetta::ml::BanditPolicy/BanditState/BanditParams and run_bandit_job — native Beta-Bernoulli Thompson sampling, LinUCB, and full-covariance linear Thompson sampling; constrained logged propensities and allreduced per-arm sufficient statistics (M-15; #1054)
    optimization.hpp        zetta::ml native LP/MIP problem, solver result, independent verification, scenario-table parser, distributed reduction callback, and table-running contract (#1055)
    bayesian.hpp            zetta::ml split-conformal regression/classification, hierarchical Gibbs posterior types, distributed allreduce job, and resolved-dataset runner (#1056)
    label_efficiency.hpp    zetta::ml active-learning queue + weak-supervision label model, distributed through JobRound collectives (#1058)
    label_efficiency_runner.hpp  resolved-table M-19 runner and asynchronous action handle; writes queue/weak-label tables with lineage (#1058)
    streaming.hpp           M-21 online logistic model, event-time trainer, distributed JobRound adapter, run options/results, and JSON boundaries (#1060)
    model.hpp               zetta::ml::Model — a trained model (metadata + weights fingerprint) + Model JSON Schema (ENT-14/FEAT-6)
    anomaly.hpp             zetta::ml — native M-7 anomaly types and algorithms: deterministic partition-assigned isolation forest, robust median/MAD contributions, causal KPI decomposition, exact contamination/alert-budget thresholding (#1046)
    anomaly_job.hpp         zetta::ml::run_anomaly_job — distributed tabular anomaly ComputeJob: tree and robust-summary allreduces, compact score gather, exact global budget, result broadcast; multi-member KPI refused in phase A (#1046)
    anomaly_table.hpp       zetta::ml::run_anomaly_on_table — resolved-dataset entry: primary-owned partition reads, `anomaly` Model registration, queryable anomaly/report tables, optional Run outputs and lineage (#1046)
    anomaly_methodology.hpp M-7 registry adapter, validated plan, coordinated-launch registration, and report interface (#1046)
    forecast.hpp            zetta::ml::ForecastParams/ForecastResult/train_forecast_partitions — lagged squared-error forecasting with rolling temporal origins, seasonal-naive FVA, and residual-quantile intervals (#1043)
    forecast_runner.hpp     zetta::ml::run_forecast_job/run_forecast_table — distributed ComputeJob and ResolvedDataset entry points; write a `forecast` Model plus forecast table, RunOutputs, and lineage (#1043)
    forecast_methodology.hpp zetta::ml — M-4 graph contract, registry callbacks and coordinated-launch registration (#1043)
    survival.hpp            zetta::ml discrete-time survival API: subject/person-period types, distributed train collectives, censoring-aware metrics, curves, resolved-dataset runner, and async run handle (#1044)
    next_best_action.hpp    zetta::ml — M-8 propensity GBDTs, implicit-feedback ALS, chronological holdout, constrained policy, and IPS/DR evaluation (#1047)
    next_best_action_job.hpp  zetta::ml::run_next_best_action_job — distributed M-8 lifecycle over JobRound allreduce (#1047)
    next_best_action_methodology.hpp  zetta::ml — M-8 graph contract, registry callbacks, coordinated-launch installation, and run adapter (#1047)
    next_best_action_table.hpp  zetta::ml::run_next_best_action_on_table — load owned resolved-dataset partitions and write recommendation/report tables with lineage (#1047)
    rank.hpp                zetta::ml — M-9 LambdaMART parameters/data/metrics, whole-group split, distributed job, resolved-table runner, and async run handle (#1048)
    language.hpp            zetta::ml — native WordPiece/BERT text encoder, cosine retrieval, citation-enforced grounded answers, and versioned linear classifier head (M-10; #1049)
    language_train_job.hpp  zetta::ml::run_language_classifier_job — allreduce per-partition classifier sufficient statistics through JobRound (#1049)
    language_runner.hpp     zetta::ml::run_language_dataset — resolved-dataset M-10 runner that writes versioned embedding, retrieval, and answer tables with lineage and metrics (#1049)
    ensemble.hpp            zetta::ml::train_ensemble/fit_stacker — deterministic out-of-fold stacking of native GBDT variants with a regularized logistic meta-model, time-ordered folds, refit bases, blended scores, and external holdout lift (#1057)
    ensemble_train_job.hpp  zetta::ml::run_ensemble_train_job — distributed M-18 training over member-owned frames; allreduces base statistics, stacker gradients, counts, and time bounds, then gathers exact metric pairs (#1057)
    ensemble_runner.hpp     zetta::ml::run_ensemble_on_dataset — ResolvedDataset-to-Run adapter that registers the ensemble Model and writes blended-score and meta-weight tables (#1057)
    synthetic_private.hpp   zetta::ml::run_synthetic_private — distributed empirical Gaussian-copula synthesis, DCR/membership/TSTR evaluation, DP histogram GBDT, and per-run privacy accounting (M-20; #1059)
    synthetic_private_job.hpp  zetta::ml::run_synthetic_private_job — owned-partition M-20 ComputeJob plus coordinated-launch registration; only aggregate statistics and DP histograms cross members (#1059)
  workspace/                the Workspace Profile domain (docs/workspace/)
    workspace_profile.hpp   zetta::workspace::WorkspaceProfile — the environment/role/daily-work + approved-source-refs entity behind Discover Opportunities (#886); value + JSON Schema (parse_workspace_profile_value), modeled on Model
  network/                  workspace network access and records (docs/network/)
    app_credential.hpp      zetta::network app-connection credential scope, issue result, and idempotent revocation contract over native governance entities (#1280)
    records.hpp             zetta::network — the four fixed RecordStore collections and retention policies, NetworkRecord schema/classification/write/list contract, decoded record pages, and zetta-tunnel session/activity shapes (#1281)
  billing/                  the metered-usage + plan domain (docs/billing/)
    plan.hpp                zetta::billing::Plan — the quota/tier config the Billing/Usage surface measures live consumption against (Conflict D); value + JSON Schema (parse_plan_value). Usage itself is the live system.usage view (no entity)
  solutions/                cross-methodology solution controls (docs/solutions/)
    solution_templates.hpp  typed experiment, guardrail and measurement defaults plus policy-engine configurations for all twelve S modules and B-1 through B-8, including pricing, workforce, ticket, document-workflow, supplier-risk, healthcare, credit, and bounded IT/security controls (#1118, #1128, #1175, #1176, #1177, #1178, #1179, #1180, #1181, #1182)
    solution_execution.hpp  composed solution creation/run entry points and snapshot-to-decision foundation execution (#1128)
    decision_policy.hpp     zetta::solutions::DecisionPolicyStore — versioned policy and decision records, bounded tuning, evaluation, A1 approval, batch table decisions, and JSON schemas (#1104)
    experiment.hpp          zetta::solutions::ExperimentManager — replicated experiment definitions and per-entity assignment rows, seeded SHA-256 allocation, decide-before-action protection, exemption erosion, balance checks, and JSON boundaries (#1105)
    action_integration.hpp  zetta::solutions::ActionRuntime and ActionConnector — replicated outbox/report records, webhook and CRM request plugins, retry/dead-letter execution, and schemas (#1109)
    roi_readout.hpp         zetta::solutions::RoiReadoutStore and generate_roi_readout — immutable replicated ROI definitions plus correctness, aggregate incrementality, customer economics, guardrail, operations, and redacted-audit output contracts (#1108)
    gbdt.hpp                zetta::ml — histogram GBDT (binary logistic, squared error, or custom LambdaMART derivatives): GbdtParams/GbdtModel/RegressionTree (+ JSON boundaries), evaluate_binary, train_gbdt, and all-reduced gradient/Hessian histograms (FEAT-6/ENT-14; #390/#1048)
    model_import.hpp        zetta::ml — models trained elsewhere read exactly into GbdtModel (ONNX tree ensembles, XGBoost JSON/UBJSON, LightGBM text) and GbdtModel written as ONNX; float_less_bound (lake#681)
    gbdt_train_job.hpp      zetta::ml::run_train_job — run distributed GBDT training as a recorded `train` ComputeJob through the job coordinator and produce a Model (weights body + metrics + ranked drivers, registered on the root); opt-in held-out probability calibration via CalibrationConfig; opt-in per-round checkpoint/restart via TrainCheckpointConfig; optional progress callback and global out-of-time validation inputs for real Study trials (FEAT-6/ENT-14; #390/#391/#1205)
    gbdt_score.hpp          zetta::ml — batch scoring / rank-at-risk: score_gbdt/predict_risk (calibrated risk = predict_proba, ranked desc) + load_gbdt_model (weights body read back through the GbdtModel JSON Schema boundary) (FEAT-6/CMD-10; #392)
    gbdt_score_job.hpp      zetta::ml::run_score_job — run distributed batch scoring as a recorded `score` ComputeJob through the job coordinator: SPMD per-shard predict, gather + device sort_by_key rank + broadcast → identical global at-risk list on every node (FEAT-6/CMD-10; #392)
    local_train_job.hpp     zetta::ml::run_near_data_train_job/NearDataTrainResult — compute-near-data GBDT train over a partitioned feature table (the train sibling of run_near_data_featurize_job): load this node's owned partitions once → train (+calibrate, registers a Model with AUC/log-loss/accuracy + drivers) → explain (per-row TreeSHAP drivers) → rank (at-risk list), each a recorded ComputeJob; SPMD, near-data instrumented, no new ML math (FEAT-6/FEAT-8; #533)
    local_featurize_train_job.hpp  zetta::ml::run_near_data_featurize_train_job/NearDataFeaturizeTrainResult — the UC-1 featurize → train composition over a partitioned table: load the owned partitions once → recorded featurize job (point-in-time, broadcast global FeatureTable) → slice to this node's entities → the shared #533 train/explain/rank phases over the feature table (not the raw engineered columns); SPMD, two pipeline-instrumentation rows (FEAT-6/FEAT-8; #530)
    calibration.hpp         zetta::ml — probability calibration: Calibrator (Platt sigmoid(a·raw+b) / isotonic PAVA step) + fit_calibrator + Brier/ECE metrics + JSON/wire boundaries; attaches to a GbdtModel (predict_proba applies it) via calibrate_model (FEAT-6; #391)
    tree_shap.hpp           zetta::ml — exact path-dependent TreeSHAP over a GbdtModel: tree_shap (per-feature φ + cover-weighted base_value, raw/margin space — efficiency base+Σφ=predict_raw), dominant_driver; pure, uses TreeNode::cover, no new JSON boundary (FEAT-6; #393)
    gbdt_explain_job.hpp    zetta::ml::run_explain_job — run distributed TreeSHAP as a recorded `explain` ComputeJob: per-entity SHAP over a shard (parallel_for) + an allreduced global mean-|φ| driver ranking; single-node == multi-node (FEAT-6; #393)
    golden_oracle.hpp       zetta::ml::GoldenOracle/load_golden_oracle — the committed golden featurize/train/SHAP expectations format (validated boundary), for the native correctness oracle (RSK-8; #395)
    playbook.hpp            zetta::ml — UC-1 retention playbook: generate_playbook (deterministic driver→action→owner via PlaybookPolicy + frozen expected-retained-value priority risk×ARR×save_rate, ranked) behind a constrained PlaybookNarrator interface (default-Claude ConstrainedNarrator over an injected NarratorCompletion, validated at the playbook_entry schema boundary, never invents numbers; RulesEngineNarrator offline fallback) + OutcomeLog (FEAT-6; #397)
  autograd/
    tensor.hpp              zetta::autograd::Node/Tensor/backward + the full op library (matmul, matmul_transposed_b [x·Wᵀ, HF-layout weights, no transpose materialized], sum, add/mul/scale/add_bias, gelu/silu, row_softmax, rms_norm, layer_norm, transpose, embedding, cross_entropy, row_slice, policy_gradient_loss, sequence_policy_gradient_loss, rope, col_slice, concat_cols) — native reverse-mode autodiff; matmul host kernel spreads over the runtime thread pool; no foreign runtime (ADR-17) (FEAT-12; #555/#1049)
    transformer.hpp         zetta::autograd — decoder blocks (causal_self_attention, grouped_query_attention with GQA/QK-norm/RoPE base, feed_forward, swiglu_feed_forward), the Transformer model, Adam, zero_grad (FEAT-12; #555)
    qwen.hpp                zetta::autograd::QwenConfig/QwenModel + parse_qwen_config/load_qwen_model — Qwen3-parity decoder keyed by Hugging Face tensor names, so a safetensors checkpoint loads by name (set_weight); generate/generate_greedy autoregressive decode; load_qwen_model reads a HF checkpoint dir (config.json + model.safetensors) (FEAT-12; #555)
    safetensors.hpp         zetta::autograd — host-float .safetensors reader/writer (parse/serialize + file I/O), decodes F32 + BF16 to host floats; no MLX, the always-compiled checkpoint path (FEAT-12; #555)
  tokenizer/
    byte_encoding.hpp       zetta::tokenizer — UTF-8 codec (utf8_decode/encode, malformed→U+FFFD) + the GPT-2 reversible byte↔unicode map (byte_to_unicode/unicode_to_byte); the byte foundation of the BPE tokenizer (FEAT-12; #555)
    pre_tokenizer.hpp       zetta::tokenizer::pre_tokenize — the Qwen2/Qwen3 pre-tokenization regex as a hand-written code-point scanner (contractions, words w/ optional non-alnum lead, single digits, punctuation runs, whitespace) (FEAT-12; #555)
    bpe.hpp                 zetta::tokenizer::BpeTokenizer — byte-level BPE encode/decode (from_vocab_and_merges / from_gpt2_files / from_tokenizer_json / from_directory / add_special_tokens); integer-id merge loop with packed-uint64 pair keys (no string alloc in the hot path); all-256-byte base vocab → exact round-trip (FEAT-12; #555)
  agent/
    end_state.hpp           zetta::agent::EndState — a rollout's observable end-state as a set of named JSON facts (header-only value type); the input a deterministic grader checks (FEAT-12; #555)
    grader.hpp              zetta::agent::Grade/EndStateGrader — the RLVR reward primitive: score an observed end-state against a task's required facts by exact match, reward in [0,1] + passed; criterion-free task grades nothing (negative control) (FEAT-12; #555)
    task.hpp                zetta::agent::Task — a "game": id + description + initial EndState + available tool names + required EndState (its grader); readable + verifiable, is_trainable() only with a checkable criterion (#554) (FEAT-12; #555)
    tool_call.hpp           zetta::agent::ToolCall — one agent action: tool name + JSON arguments (MCP-shaped), plain data a policy emits and the sandbox applies (FEAT-12; #555)
    sandbox.hpp             zetta::agent::Sandbox/ToolEffect/ToolResult/ApplyOutcome — the world a rollout acts in: register tools (injected effects), apply a ToolCall to the EndState, report unknown-tool/tool-failed rather than silently ignore (FEAT-12; #555)
    policy.hpp              zetta::agent::AgentPolicy/PolicyDecision/RolloutStep — the actor interface a rollout drives (the trained model plugs in here); next(step) emits the next ToolCall or stops (FEAT-12; #555)
    rollout.hpp             zetta::agent::Rollout/RolloutTurn/run_rollout — drive a policy against a task's sandbox for ≤max_turns, capturing the trajectory (call+outcome) + final EndState; the turn budget bounds a non-stopping agent (FEAT-12; #555)
    reward.hpp              zetta::agent::RolloutReward/RubricTerm/RubricWeights/rubric_reward/reward_for_rollout — the RLVR reward: rubric terms (right end-state/tools/args) combined by active-weight renormalization into [0,1] + per-term breakdown (blender-rl shape) (FEAT-12; #555)
    admission_gate.hpp      zetta::agent::AdmissionCase/AdmissionResult/admit_corpus — the corpus integrity gate: every reference passes its grader, every negative control fails, ≥1 negative control exists, every task trainable (FEAT-12; #555/#248)
    softmax_policy.hpp      zetta::agent::SoftmaxPolicy — the CPU reference policy: one logit per action, stable softmax probabilities, deterministic sample(u), log_prob; the training loop is proven on this before a real model plugs in (FEAT-12; #555)
    advantage.hpp           zetta::agent::GroupStats/group_stats/group_advantages — GRPO group-relative advantages (r_i-mean)/(stddev+eps): the RLVR training signal, group as its own baseline, zero when the group agrees (FEAT-12; #555)
    policy_gradient.hpp     zetta::agent::PolicySample/policy_gradient_step — one REINFORCE/GRPO ascent step: logit_j += lr·Σ advantage·(1[j==a]−pi_j); repeated over a task the reward rises (FEAT-12; #555)
    training.hpp            zetta::agent::train_softmax_policy/TrainingConfig/TrainingReport — the RLVR loop: sample group → advantages → gradient step, N steps, reporting the mean-reward learning curve; environment + rng injected (FEAT-12; #555)
    judge.hpp               zetta::agent::Judge/GradeSource/ResolvedGrade/resolve_grade — the LLM-judge fallback (injected Judge): deterministic grader wins where a criterion exists, judge fires only where none does, ungradeable fails closed (FEAT-12; #555)
    sequence_policy.hpp     zetta::agent::SequencePolicy — a fixed-horizon multi-turn policy: one SoftmaxPolicy per turn over a shared action space; sample(turn,u) (FEAT-12; #555)
    sequence_training.hpp   zetta::agent::TrajectoryReward/train_sequence_policy — trajectory-level GRPO: sample a group of full trajectories, one advantage per trajectory applied to every turn's policy (FEAT-12; #555)
    linear_policy.hpp       zetta::agent::LinearPolicy — action logits = W·x computed on an injected ComputeDevice (gemm), softmax over them; the bridge from the abstract policy to real tensor compute (runs on CpuDevice + MlxDevice) (FEAT-12; #555)
    distributed.hpp         zetta::agent::AllReduce/Reduce/distributed_group_advantages/distributed_policy_gradient_step/local_policy_gradient — cluster RLVR via injected allreduce (global GRPO stats + summed gradient); single-node==multi-node (FEAT-12; #555)
    distributed_training.hpp  zetta::agent::RewardTask/train_softmax_policy_distributed — the coroutine loop: rollouts fan out via runtime::when_all (io_task), the step allreduces; returns runtime::task<TrainingReport> (FEAT-12; #555)
    collectives_allreduce.hpp  zetta::agent::collectives_allreduce — adapt cluster::Collectives::allreduce to the injected AllReduce (Reduce→CollectiveOp, empty on failure); the real cross-node combine (FEAT-12; #555)
    neural_training.hpp     zetta::agent::ContextSource/ContextualRewardTask/train_linear_policy_distributed — distributed RLVR of a LinearPolicy's WEIGHTS: forward W·x on the device, weight-gradient outer product, allreduced; learns a contextual policy (FEAT-12; #555)
    corpus_training.hpp     zetta::agent::TaskEnvironment/CorpusTrainingReport/train_over_corpus — curriculum training over a task corpus with per-task before/after reward + improved-count (feat-12 step 5 observability) (FEAT-12; #555)
    qwen_rlvr.hpp           zetta::agent::CompletionReward/GrpoConfig/grpo_step (+ grpo_step_text) — the capstone: GRPO/RLVR over the autograd QwenModel (sample group → verifiable reward → group advantages → sequence PG step), token-level and text-level; device-agnostic, no framework (FEAT-12; #555)
    text_generation.hpp     zetta::agent::generate_text — text in/out: compose the BPE tokenizer + QwenModel (encode prompt → generate greedy/sampled → decode the new tokens) (FEAT-12; #555)
  index/
    index_kind.hpp          zetta::index::IndexKind — the search/vector/feature index-type taxonomy (header-only enum + names) (ENT-39/ENT-40)
    index_backend.hpp       zetta::index::IndexBackend/IndexSpec/IndexStatus/IndexPartition/IndexQuery — the build(over partitions)/query/drop interface (ADR-34; no-throw status)
    index_registry.hpp      zetta::index::IndexRegistry + index_spec_from_node — register backends, resolve an IndexSpec / IR `index` step → a backend (like compute::DeviceRegistry); the Node owns one (Node::index_registry(), #528)
    index_step_executor.hpp  zetta::index::IndexStepExecutor + read_index_partitions — the StepExecutor that runs a deployed IR `index` step: resolve backend → read table rows into partitions → build a real index → record the Index entity → emit an `index` output (queryable via the node registry); non-index kinds delegate to DefaultStepExecutor (#528)
    in_memory_index_backend.hpp  zetta::index::InMemoryIndexBackend — the from-scratch in-memory Search reference backend (inverted field→row-ids over partitions; name "memory_search")
    oramacore_fulltext_index.hpp  zetta::index::OramaCoreFulltextIndex — the first real backend: a full-text (BM25F) Search backend over the native oramacore-cpp engine (name "oramacore_fulltext"); pImpl over the engine, ZETTA_WITH_ORAMACORE-guarded so a checkout without the private submodule still builds (#527)
    distributed_fulltext.hpp  zetta::index::DistributedFulltextIndex/merge_fulltext_hits/FulltextHit — per-partition distributed full-text: build a sub-index per LOCAL owned partition (compute-near-data) into an owned IndexBackend, answer a query by scatter-gather (the fulltext.query signal-plane RPC, schema-validated) + merge/rank across nodes → a global ranking matching the consolidated-index oracle; the SignalHandler chained over a delegate (DistributedSorter pattern) (#529, TC-13 #503)
    search.hpp              zetta::index::search + SearchRequest/SearchResult/SearchHit/SearchPath — THE canonical search entry point (Node::search delegates here): resolve the registered Index (id, else newest Search index over the table) → route distributed-first (placed partitions + attached cluster → the DistributedFulltextIndex scatter-gather) or the consolidated registry backend (degraded) → ranked hits + per-node provenance + ONE trace (root `search` span, search:local/search:peer children) (#641); resolve_search_target exports the resolution rule for the serving surface + expose_index (#648)
    index.hpp               zetta::index::Index/IndexValue — the top-level replicated index entity {table,kind,backend,fields} + Index JSON Schema (ENT-39/ENT-40)
  cloud/                    provisioners, cloud burst, the cloud inspector — CLI control planes behind CommandRunner, no cloud SDK
    node_capability.hpp     zetta::cloud::NodeCapability/Gpu/GpuKind — node compute capability model (cores/GPU/memory) + satisfies (ENT-32)
    provisioner.hpp         zetta::cloud::Provisioner/ProvisionedNode — provision N nodes against a capability spec / teardown / list (ARC-13; #373)
    add_nodes.hpp           zetta::cloud::add_nodes/AddNodesRequest/AddNodesResult — permanent-add control entry point: drive a Provisioner to bring up N nodes that auto-join + persist as members; provision-only (never teardown/revoke), the autoscaler's permanent sibling; validated AddNodesRequest JSON boundary (#518). The add_nodes(Node&, …) overloads also trigger the between-jobs partition rebalance onto the new members (#519)
    burst_provisioner.hpp   zetta::cloud::BurstProvisioner<Config, ControlPlane> — the shared cloud-burst orchestration template (provision+mint / teardown+revoke-on-drain / provisioned); every cloud backend subclasses it so the orchestration lives once (#464)
    local_provisioner.hpp   zetta::cloud::LocalProvisioner — given-hardware Provisioner backend: uses existing cluster nodes best-effort, teardown is a no-op (ARC-13; #374)
    provisioner_fleet.hpp   zetta::cloud::ProvisionerFleet — builds + owns the CLI-backed cloud provisioners from the detected configs (moved from src/gui/cloud_wiring for #621); one holder feeds the headed Add-nodes control AND the MCP surface's consent-gated add_nodes
    azure_provisioner.hpp   zetta::cloud::AzureProvisioner/AzureConfig/AzureControlPlane — reference cloud-burst backend: burst N Azure nodes + mint identities + teardown/revoke, behind an injectable control-plane interface, no cloud SDK (ARC-13/ADR-20; #376)
    azure_cli_control_plane.hpp  zetta::cloud::AzCliControlPlane — reference AzureControlPlane binding: shells out to the `az` CLI (VMSS/Spot shape) via the shared CommandRunner (#376)
    command_runner.hpp      zetta::cloud::CommandRunner/CommandResult/default_command_runner — shared, vendor-neutral exec interface every CLI cloud binding runs through (argv, no shell; fork/execvp; #376)
    secret_file.hpp         zetta::cloud::SecretFile — a mode-0600 file in a fresh 0700 directory, removed on scope exit, that hands a secret (the join token) to a vendor CLI instead of its argv (#770)
    node_bootstrap_script.hpp zetta::cloud::render_node_bootstrap_script — the POSIX sh boot script a burst VM runs as user-data: names the secret holding the join token, fetches it from Key Vault / Secret Manager / OCI Vault / AWS SSM at boot, writes the 0400 token file + /etc/zetta/bootstrap.env (#1316)
    aws_provisioner.hpp     zetta::cloud::AwsProvisioner/AwsConfig/AwsControlPlane — AWS cloud-burst backend mirroring the Azure pattern: burst N nodes + mint identities + teardown/revoke behind an injectable control-plane interface, no cloud SDK (ARC-13/ADR-20; #377)
    aws_cli_control_plane.hpp  zetta::cloud::AwsCliControlPlane — reference AwsControlPlane binding: shells out to the `aws` CLI (EC2 Auto Scaling Group / Spot shape) via the shared CommandRunner (#377)
    gcp_provisioner.hpp     zetta::cloud::GcpProvisioner/GcpConfig/GcpControlPlane — GCP cloud-burst backend mirroring Azure: burst N + mint identities + teardown/revoke behind the control-plane interface, no cloud SDK (ARC-13/ADR-20; #378)
    gcp_cli_control_plane.hpp  zetta::cloud::GcpCliControlPlane — reference GcpControlPlane binding: shells out to `gcloud` (Managed Instance Group/Spot shape) via the shared CommandRunner (#378)
    oci_provisioner.hpp     zetta::cloud::OciProvisioner/OciConfig/OciControlPlane — OCI cloud-burst backend mirroring Azure: burst N + mint identities + teardown/revoke behind the control-plane interface, no cloud SDK (ARC-13/ADR-20; #432)
    oci_cli_control_plane.hpp  zetta::cloud::OciCliControlPlane — reference OciControlPlane binding: shells out to `oci` (Instance Pool/preemptible shape, OCID-resolved by name) via the shared CommandRunner (#432)
    k8s_provisioner.hpp     zetta::cloud::K8sProvisioner/K8sConfig/K8sControlPlane — Kubernetes cloud-burst backend mirroring Azure: burst N pods into an existing cluster + mint identities + teardown/revoke behind the control-plane interface, no client lib (ARC-13/ADR-20; #434)
    k8s_cli_control_plane.hpp  zetta::cloud::K8sCliControlPlane — reference K8sControlPlane binding: shells out to `kubectl` (Deployment/scale shape) via the shared CommandRunner (#434)
    bare_metal_provisioner.hpp  zetta::cloud::BareMetalProvisioner/BareMetalConfig/BareMetalControlPlane — bare-metal (local-host) cloud-burst backend mirroring Azure: burst N local worker containers + mint identities + teardown/revoke behind the control-plane interface, no container SDK; bursts (vs LocalProvisioner which reports) (ARC-13/ADR-20; #436)
    bare_metal_cli_control_plane.hpp  zetta::cloud::BareMetalCliControlPlane — reference BareMetalControlPlane binding: shells out to docker/podman (local-container shape, launch-the-shortfall) via the shared CommandRunner (#436)
    provisioner_config.hpp  zetta::cloud::ProvisionerConfig — one cloud-burst provisioner's deployment context, detected from the operator's own local CLI; node-local observed state (like a CloudInspectReport — not replicated, not persisted), surfaced as system.provisioners; carries NO credentials, only the non-secret targeting (subscription/project/region/…) held as the provider's validated XConfig JSON so the GUI wiring can rebuild a real provisioner from it (docs/cloud/provisioner-detection.md)
    provisioner_detect.hpp  zetta::cloud::detect_provisioner_configs — detect which cloud-burst provisioners this node can drive by asking the local CLIs (az/aws/gcloud/oci/kubectl/docker) for their active deployment context; fast, read-only, non-interactive commands through the shared CommandRunner interface (testable against a fake, never touches a real cloud in CI); the credential-free sibling of the consent-gated Cloud Inspector
    burst_identity.hpp      zetta::cloud::BurstIdentityManager/BurstIdentity — bulk-mint/revoke short-lived service Principal+AccessKey per burst node, audited (ARC-13/RSK-9; #375)
    burst_approval_gate.hpp  zetta::cloud::BurstApprovalGate/BurstCap — large-burst cost-cap approval gate: a Provisioner decorator, block over-cap bursts pending approval + audit (APR-3/RSK-9; #379)
    cloud_provider.hpp      zetta::cloud::CloudProvider/CloudProviderCatalog — pure-metadata catalog of deployment-target backends for the permission-gated Cloud Inspector (no cloud access); each inspect command is an InspectCommand (display + no-shell argv)
    cloud_inspector.hpp     zetta::cloud::CloudInspector/CloudInspectReport — the real read-only inspect: run a provider's inspect_commands via the shared CommandRunner (behind GUI consent), parse each vendor's JSON at a validated boundary → identity/regions/nodes; no creds read (#445)
  pubsub/                   in-memory pub/sub (#1024, docs/pubsub/)
    address.hpp             zetta::pubsub::Address/parse_address/address_matches — `kind:name` addresses, prefix-glob wildcards (#1025)
    event.hpp               zetta::pubsub::Event/Envelope/Payload/MimeRegistry — the envelope (from, sender stack, to/cc/bcc, reply-to, correlation id, topics-chain), inline vs shared payloads, envelope checks, the on-disk frame (#1025)
    store.hpp               zetta::pubsub::EventStore/TopicRingStack/RingBuffer/QueueStore — position-addressed stores; a deque of ring buffers with stack-then-ring locking, free list, spill + read-back, flush + restore; the std::queue work-queue store (#1026)
    delivery.hpp            zetta::pubsub::DeliveryOptions/EffectiveDelivery/DeliveryBounds/apply_bounds — recipients, ordering, priority, guarantees, reply policy; layering message > subscription > topic; clamp or reject (#1028/#1029)
    authorizer.hpp          zetta::pubsub::Authorizer/AllowAllAuthorizer — the governance injection point: rights, delivery bounds, may_act_for per sender link (#1029)
    dispatcher.hpp          zetta::pubsub::Dispatcher — shared topic-sized pool by default; per-key order, round-robin turn cap, resize hysteresis, explicit per-key/auto/inline modes and blocking-job guard (D24/#1157)
    resources.hpp           zetta::pubsub::detect_resources/derive_limits/ResourceProbe — memory/CPU/disk from cgroup v2/v1, Downward API env and host; one dispatch worker per 250 active keys within configured bounds; derived limits logged (#1030/#1157)
    log.hpp                 zetta::pubsub::LogSink — the logging injection point; stderr by default
    pubsub.hpp              zetta::pubsub::PubSub/Subscription/TopicSettings/InboxDrain — the facade: runtime topics, subscriptions, inboxes, routing, drains, dead letters, error topics, limits, shutdown flush + reload (#1027–#1032)
    node_integration.hpp    zetta::pubsub::make_node_authorizer/make_table_drain/pubsub_topic_rows — the one place pub/sub names zetta::Node: governance via users/roles (owner thread), table:<db>.<table> row inserts, system.pubsub_topics rows including dispatch sizing
  governance/               access control (principals, keys, scopes, capabilities) + users/roles/tokens + the audit trail
    principal.hpp           zetta::governance::Principal — an authenticated actor; owns access keys
    access_key.hpp          zetta::governance::AccessKey — a scoped + capability-bearing login credential + authorize()
    scope.hpp               zetta::governance::Scope/Resource — deny-by-default key-scoped access, the resource axis (RULE-3)
    capability.hpp          zetta::governance::Capability/CapabilitySet/Operation — deny-by-default rights, the verb axis, TTL'd (ENT-22)
    user.hpp                zetta::governance::User/UserStatus — an authenticatable identity (a Principal{User} in the new model); replicated user:<uuid> (#744)
    role.hpp                zetta::governance::Role/RolePermissions/builtin_role_permissions — a named permission set (scope + rights); built-in admin/writer/reader + custom (#746); role:<uuid>
    permission.hpp          zetta::governance::permission_vocabulary/permission_operations/capabilities_for_permissions/permissions_of/builtin_role_permission_names — the RBAC permission vocabulary: sql:read/write/ddl · mcp:call · serving:read · users/roles/tokens:manage · cluster:admin, each mapped onto Capability{domain,action} (#746)
    token.hpp               zetta::governance::Token/TokenValue/TokenType — a bearer credential hashed at rest (secret shown once); token:<uuid>
    user_role.hpp           zetta::governance::UserRole — a user↔role assignment; user_role:<uuid>
    policy.hpp              zetta::governance::Policy/PolicyValue/policy_value_from_json/policy_schema — a row-level security policy (Postgres-style RLS): a USING read filter + WITH CHECK write filter per table, evaluated by the SQL WHERE engine (#748); policy:<uuid>
    token_secret.hpp        generate_token_secret/generate_token_salt/hash_token_secret/kdf_hash_token_secret/verify_token_secret/constant_time_equal — the token crypto (OpenSSL)
    audit_event.hpp         zetta::governance::AuditEvent/AuditSink/AuditTrail — immutable values, per-origin segment ranges, high/low watermarks, bounded recent reads + JSON Schema (ENT-23, #1154, #1164)
    consent.hpp             zetta::governance::ConsentAction/ConsentChallenge/ConsentLedger — remote consent for cloud-touching capabilities (#621): one-time challenges naming exactly what would be read/run, consumable exactly once by a distinctly-privileged approval; ephemeral + owner-thread-only, never replicated (docs/api/consent.md)
  solutions/                runtime evidence shared by deployed solutions (docs/solutions/)
    audit_ledger.hpp        zetta::solutions::ActionLedgerValue/OutcomeLedgerValue/LedgerEntry — immutable action and outcome records, enum codecs, JSON Schema boundary, and disk projection (#1107)
  usecases/churn/           UC-1 churn — the first use case, end to end
    churn_step_executor.hpp  zetta::usecases::churn::ChurnStepExecutor — the real churn StepExecutor: connect→load-to-DDF, clean→featurize, model→train+calibrate (registers a Model), index→build a real entity index (#528) + score/rank, serve→explain (TreeSHAP), act→playbook, each heavy step a ComputeJob; behind the interface, orchestrator untouched (FEAT-5/FEAT-6/UC-1; #396)
    churn_solution.hpp      zetta::usecases::churn::run_churn_solution_on_node — THE one public churn runner (#637): wire the catalog churn blueprint to the real engine over the node's live JobCoordinator (the #579 spine, #580): encode a ChurnDataset → numeric DDF shards, build the standard featurize spec + GBDT params, inject a ChurnStepExecutor, run the graph → real Model + ranked at-risk list + drivers + playbook; + is_churn_graph (connect+clean+model predicate) + demo_churn_dataset (no-files demo) (FEAT-4/UC-1; #398/#580/#637)
    churn_dataset.hpp       zetta::usecases::churn::ChurnDataset / load_churn_dataset — synthetic UC-1 churn fixture loader (FEAT-6 / #402) + write_churn_dataset, the loader's schema-validated inverse (the guided demo's no-dialog data load, #623)
    entity_resolution.hpp   zetta::usecases::churn::EntityResolver — resolve raw comms mentions → CRM accounts division-correct (FullName) / coarse Brand trap; emit the entity_resolution annotation the gate verifies (FEAT-6/TC-4; #394)
    entity_resolution_check.hpp  zetta::usecases::churn::EntityResolutionCheck — division-correct resolution (TC-4)
  repl/
    console_model.hpp       zetta::repl::ConsoleModel — the engine-agnostic REPL session (frontend + input + transcript + submit) behind the CMP-6 console; no SDL/ImGui, so it backs the GUI component AND a future CLI (#524)
    governed_console.hpp    zetta::repl::GovernedConsole — the governed run handler behind the SCR-8 Console/REPL screen: capability-scope a statement, lower sim-Python to the IR + run the correctness gate, execute (SQL via QueryService / sim-Python via the injectable graph runner — default run_graph, or the real churn engine over the live cluster for a churn-shaped graph, #580), record one data_access audit event; zetta-core / no-SDL (#525/#580)
  testkit/                  zetta::testkit — the in-app test-suite catalog (headless-runnable; #462)
    test_suite.hpp          TestCase/TestOption/TestOptions/TestResult/TestSuite + run_test_case + builtin() — the catalog model the GUI renders
    async_case_run.hpp      AsyncCaseRun — run one case on a worker with a live Run record as the durable handle (run id at submit, logs + verdict flushed at completion); the MCP testsuite_run entry point (#617)
    harness.hpp             ScratchDir + LoopbackCluster (a real N-node loopback cluster; opt-in replicated mode wires ReplicationManager + ReplicatedBlobStore per node, announce_member publishes a node:<id> key) + the in-code synthetic churn dataset + numeric encoding
    example_script.hpp      zetta::testkit::ExampleScript/ExampleExpectations/ExampleRunReport + run_example_script + example_scripts() — the sim-Python example-script harness: run a script end to end (parse->IR->gate->run) + assert on outputs, surfacing the offending statement on failure (the example programs ARE the integration tests; #523)
  util/                     small shared utilities
    uuid.hpp                zetta::util::new_uuid() — v4 UUID for folder names
    sha256.hpp              zetta::util::sha256_hex — large-object body fingerprint
    timestamp.hpp           zetta::util::now_iso8601() / iso8601_seconds_ago() / parse_iso8601_seconds() — UTC ISO-8601 stamps (created/updated + window cutoffs) and the non-throwing inverse for duration math (#629)
    bytes.hpp / duration.hpp  human-readable size / time formatters
    fuzzy.hpp               zetta::util::fuzzy_score — case-insensitive subsequence scorer (header-only) ranking the command palette's search-as-you-type matches (#625)
src/transport/              independent zetta_transport implementation (#1283): README.md; security.cpp (PEM file loading, key/chain/current-time verification, TLS 1.3-only require-peer context); framed_stream.cpp (persistent newline-frame buffer, 64 MiB default bound, plain full-duplex I/O, serialized OpenSSL reads/writes)
src/core/                   zetta-core impl (api/: registry.cpp (incl. the #621 consent branch + Registry::decide_consent) + bridge.cpp + background.cpp + capabilities.cpp + solution_capabilities.cpp (the #1102/#1128 catalogue schemas and thin composed-solution adapters) + cloud_capabilities.cpp (the #621 consent-gated cloud tools + consent tools, off-thread cloud workers) + pubsub_capabilities.cpp (the #1033 pub/sub tools: create_topic/drop_topic/list_topics/publish_event/receive_events, reaching only exposed topics) + llms.cpp (the llms.txt renderers) + mcp_prompts.cpp (the MCP prompts registry — `build_and_run`, a guided agent_run+get_run workflow behind prompts/list + prompts/get, #872) + rest.cpp (the HTTP-free REST/OpenAPI rendering, #618, incl. the two path-templated #648 serving paths in the OpenAPI doc) + specs.cpp (the spec index renderers, #646) + serving.cpp (the HTTP-free serving layer, #648: authorize + exposure check + schema boundary + engine call per served request) + native_http_transport.cpp — the OUTBOUND coroutine HTTPS client behind connect::HttpTransport (Corosio sockets + OpenSSL + Boost.HTTP framing; one io_context on one thread multiplexes every exchange; per-host tls_context, 512 MiB body limit, runtime::timeout deadline, secrets never in a reason string), builds into zetta-api (#827) + http_server.cpp — the beast2/Boost.JSON INBOUND adapter TU (its web-dashboard half renders each capability as a dialog straight from the registry: capability_form_fields/nested_object_fields turn a NESTED object property into a group of parent.child controls with masked credential inputs, and web_form_input decodes them back into a nested input — #827) (POST /mcp + the /v1/* REST routes + /v1/openapi.json + /v1/specs[/name] + the /llms*.txt GETs + the #648 served routes POST /v1/models/:name/predict and GET /v1/indexes/:name/search + the #1005 object routes under /v1/objects/: raw streamed PUT/PATCH/GET and the bucket aliases), builds into zetta-api; schemas/: registry.cpp (the boundary-schema table, #646); observability/: trace.cpp + activity.cpp + crash.cpp)
  api/run_metrics_capabilities.cpp  log_run_metrics/log_run_params/log_run_artifact/get_run_metrics/compare_runs registry schemas and owner-loop handlers (#1137)
  api/run_proposal_capabilities.cpp  propose_run/approve_run_proposal/reject_run_proposal/get_run_proposal/list_run_proposals: propose creates only the proposal; approve creates exactly the previewed Project + Pipeline, approves the plan, and calls `run` through the registry under the approver's rights (#969)
  api/object_capabilities.cpp  create_bucket/list_objects/object_usage/object_barrier/delete_object and the acquire/renew/release_object_lease capabilities (ON BUCKET scoped, `blocks`; acquire waits as a retrying AwaitableRead), the status mapping, authorize_object_route and object_route_paths() — the OpenAPI rendering of the byte routes that http_server.cpp serves (object_routes.hpp) (#1005)
  api/record_capabilities.cpp  create_collection/put_record/get_record/delete_record/list_records and create_metric/record_metric/query_metric: schemas, ON COLLECTION scoped authorization, `blocks` handlers over RecordStore/MetricStore (#1231)
  api/network_capabilities.cpp  issue_app_credential/revoke_app_credential: schemas and api:manage_tokens handlers for least-privilege native users/roles/tokens (#1280); write_network_record/list_network_records: typed zetta-tunnel/Lake schemas, network:write/network:read authorization, decoded retained pages, and blocking RecordStore handlers (#1281)
  api/study_capabilities.cpp  create/validate/get/list/list-trials/stop/stop-trial/promote/analysis registry schemas, permissions, published M-1 search definitions, shared validation and normalization, lifecycle events and owner-loop handlers (#1138/#1205/#1207)
  api/decision_policy_capabilities.cpp  create/get/update/decide/approve registry entries, owner-loop handlers, and schemas (#1104)
  api/action_capabilities.cpp publish-report, enqueue/deliver-task, and bounded closed-loop registry entries, owner-loop handlers, and schemas (#1109)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  catalog.hpp               flat-key scheme for the replicated catalog (internal)
  json_boundary.hpp         internal: the shared JSON-boundary field readers (string/int/…, required/optional) every cloud-burst Config from_json parser uses, so the boundary checks live once instead of copy-pasted per <x>_provisioner.cpp
  base64.hpp                shared base64 (gossip blobs + signal payloads; internal)
  node.cpp                  load_databases()/load_projects()/load_deployments()/load_runs()/load_jobs()/load_dataset_partitions() + replication capture/reconcile/persist + Node::datasets() (the computed-at-query-time DatasetSummary join, #585)
  runtime.cpp               zetta::runtime::parallel_for / parallel_chunks — data-parallel fan-out over the shared Capy thread pool
  memory/
    README.md              navigation for the platform memory-accounting implementation
    accounting.cpp         allocator/RSS readers for glibc/Linux and malloc zones/Mach, probe aggregation, compact formatting, and Linux malloc_trim (#1163)
  headless.cpp              signal-driven headless loop
  signals.cpp               SIGINT/SIGTERM stop flag (OS signals, not the signal plane)
  cluster/
    cluster.cpp             cluster listener/dialer + signal-plane protocol over zetta::transport::FramedStream; mutual TLS 1.3 handshake precedes every protocol line and credential failure remains fail-closed (#466, #1283)
    security.cpp            zetta::cluster security material (the one OpenSSL-libcrypto user): join-token CSPRNG + deterministic Ed25519 CA derivation + per-node cert minting + PEM/secret-file persistence with the schema-validated boundary (#466)
    signal.cpp              zetta::cluster::Signal — envelope JSON form + JSON-Schema boundary (encode/decode)
    collectives.cpp         zetta::cluster::Collectives — collective rounds (star reduce/broadcast) + message JSON-Schema boundary
    replication.cpp         replication CRDT adapter (the one automerge-cpp user); tombstoned removal + the get_all() conflict scan that makes removal win over a concurrent write (#838); clock() + save_since(), the per-peer gossip delta (#794)
    node_metrics.cpp        zetta::cluster::NodeMetricsSampler — per-platform OS-counter sampling (macOS mach/sysctl/getifaddrs, Linux /proc, POSIX statvfs), CPU/network rates from deltas; the source behind system.node_metrics (#581)
    node_telemetry.cpp      zetta::cluster::NodeTelemetryExchange — the node.metrics wire boundary (embedded draft-07 schema + pboettch validation, encode/decode) + the thread-safe peer cache (receive on the network thread, record local + snapshot on the loop thread, expire stale) (#584)
  data/
    database.cpp
    table.cpp
    lazy_row.cpp            ParquetLazyPtr::materialize/release — the lazy re-read and the give-up-residency half of the row handle (#833)
    parquet_store.cpp       ParquetStore + the internal ParquetPager (RowPageSource): tail-only footer read, per-row-group decode against rebased chunk offsets, LRU page cache with weak_ptr re-adoption + a decode counter, positional_id/positional_row (#833, #836)
    jsonl_store.cpp
    partitioner.cpp         zetta::data::HashPartitioner/RoundRobinPartitioner assign_partition (FNV-1a key hash / surrogate mod) + to_json + partitioner_from_json config boundary (#513)
    fetch_affinity_placement.cpp  zetta::data::FetchAffinityPlacement — owner-first placement + round-robin record→partition over the stamped `_seq` surrogate (#833 §6)
    partition_scheduler.cpp zetta::data::DefaultPartitionScheduler::plan — size-targeted count (ceil(bytes/target), raised to partitions_per_node x |members|, capped, never more than rows), capacity weights (storage as a gate-with-taper, link utilization, mild CPU; unmeasured = the mean of the measured), lowest load-to-weight primary + rotated replica spread, and the PartitionPlan JSON boundary (#833)
    rendezvous_placement.cpp  zetta::data::RendezvousPlacement/rendezvous_owners — rendezvous (HRW) partition→node placement: FNV-1a weight, top-k by weight, ~1/N moved on membership change (#514)
    partition_body.cpp      the Parquet/JSON-Lines partition body writer + reader behind that interface (#833)
    partition_writer.cpp    zetta::data::write_partitioned — the partitioned write path: bucket rows by assign_partition (stamping the round-robin surrogate), serialize each non-empty partition to JSON Lines, store via LargeObjectStore::import_bytes, record a DatasetPartition per partition (cumulative row range + place_partition owners) (#511)
    promote_dataset.cpp     zetta::data::promote_dataset — current owners first, then the rendezvous order until the target factor; set_owner_nodes captures the change and that IS the copy (#833)
    partition_rebalance.cpp  zetta::data::rebalance_partitions — between-jobs rebalance: per placed partition recompute rendezvous_owners(stats.partition, members, rf=owner count) and set_owner_nodes on a change; skip replicate-to-all (vs previous_members); the Node::tick auto-trigger + Node::cluster_members live in node.cpp (#517); the add_nodes direct trigger lives in add_nodes.cpp (#519)
    dataset_partition.cpp   zetta::data::DatasetPartition — value, apply_value, JSON-Schema boundary validation (parse_dataset_partition_value)
    dataset_columns.cpp     zetta::data::ColumnProfile — type families, the merge rule (integer+number widen, else mixed), Parquet type names as families, partition `column_stats` (#1021)
    dataset_manifest.cpp    zetta::data::DatasetManifestValue — the `mfst:<table-id>` value, its embedded draft-07 schema + non-throwing parse, and `catalog_shortfall` (the ONE completeness rule the scatter-gather planner and system.datasets both read) (#837)
    large_object.cpp        zetta::data::LargeObjectStore — blob folders + body I/O (import_file/import_bytes, sha256 fingerprint)
    import.cpp              zetta::data::import_records — JSON/JSONL file → table rows
    record_store.cpp        zetta::data::RecordStore — owner choice, commit/install/fan-out, lists, sweep, anti-entropy, and the record_collection/stored_record/record_signal schemas (#1231)
    record_log.hpp/.cpp     zetta::data::RecordLog (internal) — the node-local Bitcask-style log under <data_dir>/records/: checksummed frames in rolling segments, an in-memory key directory and index sets, purge markers, oldest-first segment retirement and bounded compaction, startup rebuild with torn-tail truncation, anti-entropy bucket digests (#1231)
    object_store.cpp        zetta::data::ObjectStore — placement, primary decisions (fence, quota, condition), owner installs, uploads, local and remote readers, list/usage/barrier across nodes, the lease decided by one node and installed on every live one, anti-entropy, the object.* signal handler, and the object_bucket/object_lease/object_signal schemas (#1005)
    object_log.hpp/.cpp     zetta::data::ObjectLog (internal) — one bucket's node-local segmented log under <data_dir>/objects/<bucket>/: crc32c-checked frames (append, stage, commit, remove, purge) in 64 MiB segments, an in-memory directory of generations and extents, order-independent replay that keeps each key's highest whole generation, torn-tail truncation, read snapshots that pin their segments, barrier fsync, oldest-first retirement and batched compaction, anti-entropy digests (#1005)
    metric_store.cpp        zetta::data::MetricStore — bucket folding, range queries, per-node retention, and the metric_definition/metric_bucket schemas (#1231)
    virtual_table.cpp       zetta::data::VirtualTable — tier names + to_json/from_json (non-throwing boundary) for the durable `vtable:<name>` catalog value (CREATE VIRTUAL TABLE, #809 slice 3)
    dataset_path.cpp        zetta::data::parse_dataset_path/infer_dataset_contract/sample_dataset/resolve_dataset/default_dataset_sampler — separator-first path parsing (a path may contain a dot; a table may not contain a slash), ranked case-insensitive column candidates resolved to the table's own spelling, binary-ness outranking the label name, an integer-valued entity key required with the remedy named, degraded datasets refused rather than typed off a fraction (#837), and one SQL-backed sample read shared by contract inference and analysis (#947, #1062); the contract typed from the partition catalog's per-body column types, reading only the columns the catalog cannot decide (text, a 0/1 label or whole-number entity candidate) through a projected SELECT (#1203)
    import_path.cpp         zetta::data::import_root/resolve_import_path — the import root (#1391, #1433): a caller-named path resolves under <data_dir>/imports only; absolute paths, `~`, `..` and backslashes refused before the filesystem is touched, then an openat(O_NOFOLLOW) walk refuses a symbolic link at any step, then the real path must lie under the real root; used by add_dataset, run (M-1 directory form) and testsuite_run
    export_path.cpp         zetta::data::write_export_file — the export root (#1397, #1399): creates <data_dir>/exports (0700) and each directory on the way with mkdirat + openat(O_NOFOLLOW|O_DIRECTORY), then the file with openat(O_CREAT|O_EXCL|O_NOFOLLOW, 0600), removing it if the write fails; used by generate_dataset (`path`) and submit_feedback (`export_path`)
    unique_descriptor.hpp   internal: detail::UniqueDescriptor, the owned file descriptor import_path.cpp and export_path.cpp walk with
    categorical_encoding.cpp zetta::data::fit_categorical_encoding/encode_categorical_row/categorical_encodings_from_json — category fitting and row encoding behind categorical_encoding.hpp (#1203)
    analyze_dataset.cpp     zetta::data::analyze_sampled_dataset/analyze_dataset — sample-backed semantic inference and evidence, descriptive statistics, key/grain classification, same-database key-overlap joins, and label/time leakage candidates (#1062); analyze_dataset_as reads as the caller — its table grants, row security and column grants, hidden columns omitted, unreadable neighbours never sampled (#1300, #1396)
    featurize_plan.cpp      zetta::data::plan_features/methodology_rule — one methodology rule table; ready plans state every transform/reason and exclusion, while incompatible data shapes return a named correction with supported SQL or a clean step (#1062); plan_features_as plans from what the caller may read (#1396)
    add_dataset.cpp         zetta::data::add_dataset_file + AsyncDatasetAdd — decode parquet/jsonl/json/csv by extension → free-form rows (native Parquet reader + CSV type inference), create a uniquely-named table, write_partitioned with RendezvousPlacement (#587); async decode worker + pump-sliced materialize + cancel + audit (#624); DatasetLineDecoder + AsyncObjectDatasetLoad: an object read a slice at a time on a worker, 10,000-row batches landed on the owner thread, the run settled there (#1302)
    replication_strategy.cpp  zetta::data::ReplicationStrategy — kind names, JSON boundary validation, replication_strategy_from_meta reader (ENT-35; #509)
    replicated_blob_store.cpp  large-object body transfer (CRDT metadata + disk)
  network/app_credential.cpp  zetta::network — connection-owned user/role naming, table/database and topic-prefix grants, tool allowlist, one-time hashed token issuance, and idempotent revocation (#1280)
  network/records.cpp       zetta::network — producer-schema validation, fixed collection installation, retention/index definitions, deterministic keys, session lifecycle replacement, and decoded RecordStore reads (#1281)
  connect/
    distributed_fetch.cpp   the longest-processing-time-first tiling (largest partition to the lightest node; ties on node order so every node computes the same plan) + the wave-bounded runtime::when_all fetch, degrading to a serial walk when no async transport is installed (#827)
    distributed_ingest.cpp  submit → PLAN (PartitionScheduler, computed once here) → fan_out carrying the plan → fetch this node's tile (tiled by the plan's per-node shares, so a flexible source lands in the target shape and needs no rebalance) → land with FetchAffinityPlacement → allreduce {landed, rows, partitions, bytes} → record the partition manifest; a short sum or a timed-out round discards every node's partitions and the initiator's table (#833 §7)
    snowflake_protocol.hpp/.cpp  INTERNAL — the SQL API v2 wire details (statements URL, bearer headers, partition requests, rowType-driven cell typing, partitionInfo row counts) shared by connector.cpp and distributed_fetch.cpp; no I/O, no transport (#827)
    connection_store.cpp    the saved-connection store over data::Table: name validation at the boundary, save-replaces-in-place (no duplicate rows on a credential rotation), the credential stored only sealed (#1230), and reseal_connections (legacy plaintext + retired-key entries, run on start)
    credential_seal.cpp     AES-256-GCM sealing of connector credentials under an HKDF-SHA256 key from a root secret held outside the catalogue (the join token, or a node-local secret); key ids, retired keys, the sealed_credential schema (#1230)
    connector.cpp           the ConnectorConfig boundary + connector_config_schema, the read-only single-statement guard (comment-skipping; a write never reaches the network), and the Snowflake SQL API v2 connector — bearer PAT/OAuth, 202 async statement polling, multi-partition result fetch, rowType-driven cell typing so the landed dataset is featurizable rather than all strings (#827)
    http_transport.cpp      the installed-default transport registry and nothing else — set_default_http_transport/default_http_transport/default_async_http_transport behind one startup-installed-once mutex; the real client is a zetta-api TU (src/core/api/native_http_transport.cpp) because zetta-core must never link Boost.HTTP (#827)
  query/
    postgres.cpp            zetta::query::PostgresDialect — the tokenizer + recursive-descent parser (Postgres subset → the generic AST), incl. CREATE USER / CREATE TOKEN (#744) + CREATE VIRTUAL TABLE … OVER CRDT … (#809 slice 3)
    statement_builders.cpp  zetta::query::make_create_token_statement + the role-management make_*_statement builders (#746) — assemble Statement variants OUT of the parser TU (a pinned-toolchain EH-miscompile workaround; the parser TU is also built at -O1, see CMakeLists)
    executor.cpp            zetta::query::Executor — statement execution against the Node, each data-changing statement authorized (sql:write / sql:ddl at table or database scope, users:manage / tokens:manage, column grants and row security on UPDATE/DELETE) and audited as data_changed (#1229), the zetta_connect store refused (#1230) (incl. CREATE USER / CREATE TOKEN returning the plaintext once) + the read-only system.* observability tables (tables / nodes / deployments / runs / models / workspace_profiles / usage / plans / indexes / jobs / principals / keys / users / roles / tokens / audit / ledger / activity / datasets / dataset_partitions / checkpoints / node_metrics with pub/sub pool sizing / pipeline_jobs / collective_rounds / spans / traces / crashes / cloud_providers / cloud_status / provisioners); kSystemTableCatalog is the one list they all derive from (system.tables + system_table_names() + the unknown-table error, #684/#1157)
    predicate.cpp           zetta::query::compare_cell/row_matches — the one WHERE comparison, used by the executor and by the owner-side partition scan (#833)
    distributed_select.cpp  zetta::query::DistributedSelect + scan_owned_partitions + merge_scan_responses — the `sql.scan` responder (a network-thread-safe snapshot of the owned partition bodies, refreshed from tick), the coordinator's one-owner-per-partition assignment, and the aggregate merge (#833)
    service.cpp             zetta::query::QueryService — parse-with-Postgres + execute-each-statement facade (the Outcome the REPL/GUI render)
  parquet/
    thrift_compact.hpp      zetta::parquet::detail::CompactReader/CompactWriter — the shared Thrift compact-protocol cursor (non-throwing, bounds-checked; used by both the footer and page-header parsers) plus its inverse emitter, which owns the field-id delta + nested-struct id stack (internal header; #833)
    metadata.cpp            zetta::parquet — Thrift compact-protocol decoder + FileMetaData footer decode + to_json + parquet_metadata_schema/validate boundary (zetta-core; #505)
    snappy.cpp              zetta::parquet — Snappy raw-block codec: decoder (preamble varint + literal/copy elements, bounds-checked) for Parquet data pages (#507) + the encoder the Parquet writer emits pages with (#833)
    column_decoder.cpp      zetta::parquet — page-header parse + RLE/bit-packed-hybrid + PLAIN/dictionary value decode + definition levels → DecodedColumn (dense values + validity); parquet_columns_schema/validate boundary (zetta-core; #506)
    reader.cpp              zetta::parquet::read_parquet_rows — decode every leaf column + transpose to one JSON object per row (the untyped whole-file reader; powers load_churn_dataset Parquet) (zetta-core; #508)
    writer.cpp              zetta::parquet::write_parquet_rows/infer_parquet_schema — emit PAR1 + v1 data pages (definition levels + PLAIN values, snappy-compressed) + the Thrift FileMetaData footer; JSON-row schema inference, path-qualified failures (zetta-core; #833)
  ir/
    execution_graph.cpp     ExecutionGraph IR build + traversal
    serialization.cpp       ExecutionGraph IR ↔ JSON (+ boundary validation)
  input/
    python_frontend.cpp     zetta::input::PythonFrontend — non-throwing tokenizer + recursive-descent parser (Python subset) + AST→IR lowering + the engine-faithful python_step_kind verb table (#520/#522)
    cluster_bindings.cpp    zetta::input::run_add_nodes_step/add_nodes_count/add_nodes_cost_cap — read the count + cost cap from a lowered cluster.add_nodes IR node + drive zetta::cloud::add_nodes (cost-cap-gated + Node-aware when given a Node) (#522, #502)
    datasets_bindings.cpp   zetta::input::run_load_dataset_step/DatasetStepExecutor — read the source + column projection from a lowered load_dataset IR node, resolve bytes via the DatasetResolver interface, decode with the native parquet reader, project, and populate a (partitioned) table (#521)
  pipeline/
    project.cpp             zetta::pipeline::Project — persist/rename project.json + input/graph CRUD
    project_input.cpp       zetta::pipeline::ProjectInput — persist input.json + payload body
    project_graph.cpp       zetta::pipeline::ProjectGraph — persist a graph's json projection
    intent_compiler.cpp     compile_intent — deterministic registry resolution, unsupported-methodology refusal, and shape callback dispatch (#1061)
    n8n_import.cpp          zetta::pipeline::import_n8n_workflow — n8n workflow JSON Schema + validate + store as artifact input
    n8n_compile.cpp         zetta::pipeline::compile_n8n_workflow — n8n node-type→StepKind table + connections→edges → ExecutionGraph IR
    graph_plan.cpp          project_cluster_plan — per-step cluster placement projection (kind → placement; near-data owner propagation along DataFlow edges; drop stale owners) for the Graph Workspace plan view (#591)
    executor.cpp            GraphExecutor + topological_order + DefaultStepExecutor (graph → Run); the per-step authority check before each step (#843)
    step_authority.cpp      step kind → right, the user / AccessKey / table-scoped decision, the data_access audit record (#843)
    run_progress.cpp        project_run_progress — derive per-step run status from the GraphExecutor log grammar + attribute outputs to producing steps (#626)
    validation.cpp          ValidationReport verdict + JSON round-trip/validation
    gate.cpp                CorrectnessGate runner + NonEmptyGraphCheck + make_check
    graph_checks.cpp        SchemaTypeCheck + DataLeakageCheck (#309), SnapshotLineageCheck (#1103), M-10 grounding/training-access checks (#1049), and annotation JSON-Schema validation
    forecast_graph_check.cpp
                            zetta::pipeline::ForecastGraphCheck — refuse invalid forecast shapes, non-temporal splits, future features, calibration and missing baselines (#1043)
    run.cpp                 zetta::pipeline::Run — value, mutators, JSON-Schema boundary validation (parse_run_value)
    run_proposal.cpp        RunProposal JSON + non-throwing boundary parser, and the CRDT-backed RunProposalStore (put/get/list newest first) (#969)
    deployment_target.cpp   zetta::pipeline::DeploymentTarget — names + JSON (boundary-validated)
    deployment.cpp          zetta::pipeline::Deployment — persist deployment.json
    deployment_controller.cpp  zetta::pipeline::DeploymentController — reconcile pass: start/self-heal a deployment's run
    solution_catalog.cpp    zetta::pipeline::SolutionCatalog — built-in library + schema/IR boundary parse
  solutions/
    snapshot.cpp            point-in-time table materialization through the Node catalog, partition-aware source reads, row-level as-of joins and schema accessors (#1103)
  compute/
    device_buffer.cpp       DeviceBuffer lifetime (free via device) + ComputeDevice::make_buffer (→ zetta-compute lib)
    host_stub_device.cpp    HostStubDevice — host-memory allocate/transfer; operators unsupported (→ zetta-compute)
    cuda_device.cu          CudaDevice — CUDA runtime memory model + Thrust/custom operator kernels, CpuDevice-conformant (→ zetta-compute; CUDA build only, #363/#361)
    mlx_device.cpp          MlxDevice — MLX/Metal backend: unified-memory allocate/copy + gemm/axpy via mx::matmul/multiply/add on the GPU; other ops unsupported for now (→ zetta-compute; MLX build only, #555)
    mlx_transformer.cpp     zetta::compute::model — rms_norm/gelu/feed_forward/causal_self_attention, MlxTransformer forward/generate + named-parameter checkpoint I/O, losses, AdamW, flatten/unflatten + distributed_adamw_step (→ zetta-compute; MLX build only, #555)
    safetensors.cpp         serialize (F32) + deserialize (F32/BF16→F32) the safetensors bytes via an nlohmann/json header, + file read/write helpers (→ zetta-compute; MLX build only, #555)
    cpu_device.cpp          CpuDevice — every operator on host memory, multi-threaded via runtime::parallel_for (zetta-core; #360)
    device_registry.cpp     DeviceRegistry — enumerate/report/select devices + DeviceInfo JSON boundary (zetta-core; #362)
    work_stealing_scheduler.cpp  WorkStealingScheduler — worker threads, per-worker deques + the steal loop (zetta-core; #366)
    distributed_scheduler.cpp  DistributedScheduler — the cross-node steal protocol + its JSON-Schema boundary + elastic membership re-queue (zetta-core; #366/#367)
    job_coordinator.cpp     JobCoordinator — lifecycle driving (Running→Succeeded|Failed) + the shared signal-handler chain over Collectives + DistributedScheduler (zetta-core; #372)
    coordinated_launch.cpp  CoordinatedLaunchListener — the run.launch envelope (embedded draft-07 schema + pboettch validation) + fan_out over the roster + the validate-on-network-thread / drain-on-owner-thread queue + per-kind runner registry with drain spans (zetta-core; #601)
    checkpoint.cpp          JobCheckpoint value + JSON-Schema + sha256 integrity boundary, FileCheckpointStore (atomic save / latest / clear / job_ids), run_resumable_rounds (resume + checkpoint + graceful drain) (zetta-core; #468)
    compute_job.cpp         zetta::compute::ComputeJob — value, set_status (+ finish audit), JSON-Schema boundary validation (parse_compute_job_value)
    scaling_policy.cpp      zetta::compute::ScalingPolicy — objective names, evaluate() per objective + cost cap, JSON boundary validation (ADR-29/ENT-34)
    job_autoscaler.cpp      zetta::compute::JobAutoscaler — the tick() control loop (observe → evaluate → hysteresis → provision/teardown) + AutoscalerConfig JSON boundary (ADR-29/ARC-11)
  ddf/
    data_frame.cpp          DataFrame loader (table→typed columns, chunked, device-backed), schema inference + DataFrameSchema JSON boundary (zetta-core; #380)
    parquet_reader.cpp      load_parquet_data_frame (decode requested leaf columns → DeviceBuffers, column projection + numeric/dense boundary), infer_parquet_schema, parquet_data_frame_schema/validate boundary (zetta-core; #508)
    ops.cpp                 DDF scan/filter/project ops over a DataFrame (gather-based filter + repack, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (zetta-core; #381)
    join.cpp                distributed_join + to_host_relation + hash/sort-merge join + collective repartition (gather/broadcast all-to-all) + deserialize boundary check (zetta-core; #382)
    data_frame_ops.cpp      zetta::ddf::sort/concat/slice + the internal HostFrame host-staging (data_frame_internal.hpp) shared with the distributed sort (zetta-core; #385)
    distributed_sort.cpp    zetta::ddf::DistributedSorter — sample-sort across the cluster + the ddf.shuffle all-to-all + PartitionMessage JSON-Schema boundary (zetta-core; #385)
    group_by.cpp            group_by/local_partial/GroupByPartial — groupby-aggregate accumulate/merge/finalize, the partial wire form + GroupBySpec JSON boundary (zetta-core; #383)
    as_of.cpp               as_of_aggregate — host-side per-entity windowed/point-in-time reduction (group key + time mask) + AsOfAggregate JSON boundary (zetta-core; #384)
    group_by_job.cpp        run_group_by_job — drive the distributed groupby-aggregate as a recorded ddf ComputeJob through the coordinator (calls distributed_group_by on the JobRound) (zetta-core; #387)
    local_partitions.cpp    load_local_partitions — enumerate this node's owned partitions from Node::dataset_partitions(), read each owned blob body from the local LargeObjectStore, parse the JsonlStore lines, load into a DataFrame; instrumented bytes_read (zetta-core; #516)
    local_group_by_job.cpp  run_near_data_group_by_job — load the local shard via load_local_partitions then drive run_group_by_job, computing wire_bytes_moved from Collectives::wire_bytes() before/after (zetta-core; #516)
    featurize.cpp           point_in_time_featurize — the single-node featurization (as-of + group-by + filter + hash-join composition), FeatureTable::to_data_frame + FeaturizeSpec JSON boundary (zetta-core; #388)
    featurize_job.cpp       run_featurize_job — drive the distributed featurization as a recorded `featurize` ComputeJob (distributed_join enrich → per-feature filter + distributed_group_by; static mode skips the join and carries account_features over the time-filtered shard) through the coordinator (zetta-core; #388/#532)
    local_featurize_job.cpp  run_near_data_featurize_job — load the local shard via load_local_partitions then drive run_featurize_job, reporting local_bytes_read vs wire_bytes_moved (zetta-core; #532)
  ml/
    study.cpp               Study schema, persistence, validation/test-split and DP restrictions, samplers, pruners, analysis, synthetic evaluator support, and the replicated background driver for real Run-backed M-1 trials (#1138/#1205)
    entity_resolution.cpp   M-2 owner-filtered blocking, distributed Fellegi-Sunter EM or labelled-pair GBDT scoring, per-member conflict-safe components, deterministic component merge, metrics, and table outputs (#1041/#1072)
    entity_resolution_job.cpp  M-2 JobRound adapter, collective pair-page transport, output-member streaming, and recorded distributed train job (#1072)
    entity_resolution_methodology.cpp  M-2 registry shape, graph ownership, coordinated peer launch, resolved-dataset run adapter, Run/Model/output-table lineage (#1041/#1072)
    segmentation.cpp        native k-means training, global transforms, deterministic sampling, collective Lloyd rounds, selection and profiles (#1042)
    segmentation_job.cpp    owned-partition loader, JobRound adapter, Model and assignment/profile output tables with Run lineage (#1042)
    segmentation_graph.cpp  M-3 config schema and correctness gate: excluded roles, global transforms, K criterion and connected outputs (#1042)
    segmentation_methodology.cpp M-3 shape/owns/run adapter, unlabeled table resolution, coordinated launch and measured report (#1042)
    methodology.cpp         M-1 reference callbacks: established churn shape, ownership predicate, and asynchronous runner (#1061)
    run_features.cpp        plan_run_features / record_run_features / run_feature_report — common data-first run preflight, existing leakage/causal checks, accepted trainer inputs, and persistent feature/exclusion reasons for all runnable methodologies (#1062); public declarations in include/zetta/ml/run_features.hpp
    graph.cpp               deterministic native graph feature and node2vec-style embedding implementation (#1051)
    graph_job.cpp           SPMD sparse edge-statistics combine and iterative PageRank rank-mass allreduce (#1051)
    graph_methodology.cpp   M-12 graph pipeline shape, ownership predicate, registry metadata, resolved-table run callback (#1051)
    graph_runner.cpp        resolved edge-table reader, node-feature and summary-table writer, graph Model registration, and Run lineage (#1051)
    document_methodology.cpp M-11 registry callbacks: document pipeline shape, ownership predicate, resolved-table run, and asynchronous handoff (#1050)
    document_extraction.cpp M-11 PDF text reader, PGM preprocessing and line segmentation, native TrOCR inference, field extraction/evaluation, output tables, model, and run lineage (#1050)
    methodology_table.cpp   the ordered M-1 through M-21 portfolio table, aliases, primary solutions, keywords, data roles, outputs, metrics, and implementation issues (#1061)
    workspace_methodology.cpp  workspace methodology entries: schema + semantic validation, CRDT store, listing, three-step shape (#1261)
    workspace_study.cpp        a workspace methodology's study: search space from its manifest, spec checks, runner work items (lake#689; admission is advance_workspace_study in study.cpp)
    solution_table.cpp      the ordered, runnable S-1 through S-12 and B-1 through B-8 registry, retained promotion conditions, portfolio-number mapping, typed-template projection, and callback-built composition (#1102/#1128/#1175/#1176/#1177/#1178/#1179/#1180/#1181/#1182)
    uplift.cpp              native T- and X-learner fitting, propensity overlap, held-out selection, Qini/AUUC, bootstrap, and targeting policy (#1045)
    uplift_job.cpp          JobRound allreduce/gather/broadcast adapter for distributed uplift training (#1045)
    uplift_runner.cpp       resolved-table execution, output tables, model weights, report facts, and lineage (#1045)
    uplift_methodology.cpp  M-6 registry shape, ownership, table resolution, and run callback (#1045)
    language_methodology.cpp M-10 registry shape, graph ownership, dataset resolution, column authorization, and language-runner adapter (#1049)
    speech_audio.cpp        native WAV/PCM decoding, 16 kHz resampling, log-mel, energy segmentation, transcript filtering, WER and counter aggregation (#1053)
    speech_whisper.cpp      native Whisper config/checkpoint validation plus encoder-decoder greedy inference and timestamp tokens (#1053)
    speech_train_job.cpp    coordinated allreduce of partition-local M-14 metrics (#1053)
    speech_runner.cpp       consent-first owned-partition transcription, transcript/model outputs, metrics, and run lineage (#1053)
    speech_methodology.cpp  M-14 registry shape, graph ownership, dataset resolution, and run callback (#1053)
    bandit.cpp              zetta::ml — deterministic online bandit policy math, probability constraint projection, state serialization, and propensity logging (M-15; #1054)
    bandit_train_job.cpp    run_bandit_job — allreduce per-arm posterior or ridge-statistic deltas, produce the shared policy and global reward/regret/exploration metrics, retain local decision logs (M-15; #1054)
    bandit_runner.cpp       run_bandit_dataset — restore policy state from a resolved table, apply ordered observations, and materialize the policy, propensity log, Model, Run metrics, and lineage (M-15; #1054)
    bandit_methodology.cpp  M-15 registry shape, ownership predicate, resolved-table adapter, and runnable metadata (M-15; #1054)
    methodology_optimization.cpp  M-16 callbacks: optimization-specific graph shape/ownership, dataset resolution, and native solver dispatch (#1055)
    optimization.cpp        zetta::ml — bounded two-phase revised simplex, best-bound branch-and-bound MIP, scenario-table parser, independent feasibility verifier, and plan/report table writer (#1055)
    bayesian.cpp            split-conformal quantiles/prediction sets, deterministic hierarchical Gibbs sampler, sufficient-statistic allreduce, and posterior table/model/run writer (#1056)
    bayesian_methodology.cpp  M-17 pipeline shape, graph ownership, connect-table resolution, registry metadata, and run callback (#1056)
    streaming.cpp           M-21 event-time windows, prequential SGD, drift metrics, pub/sub score publication, persistence, output table, and lineage (#1060)
    streaming_methodology.cpp  M-21 shape, ownership, table resolution, and asynchronous registry run callback (#1060)
    model.cpp               zetta::ml::Model — value, mutators, weights body I/O, JSON-Schema boundary validation (parse_model_value)
    anomaly.cpp             zetta::ml — isolation-tree training/scoring, partition-summary median/MAD reductions, causal KPI trend/season/residual scoring, exact threshold policy (#1046)
    anomaly_job.cpp         zetta::ml::run_anomaly_job — JobRound collective adapter and compact result wire over existing collectives (#1046)
    anomaly_table.cpp       zetta::ml::run_anomaly_on_table — resolved partition-body loader, anomaly Model/output-table materialization, and Run lineage attachment (#1046)
    anomaly_plan.cpp        M-7 plan parser and AnomalyGraphCheck: executable shape, roles, exact budget, robust scaling, and causal windows (#1046)
    anomaly_methodology.cpp M-7 shape/ownership/run callbacks, label-free catalog resolution, asynchronous coordinated launch, and report assembly (#1046)
    survival.cpp            person-period expansion, time-aware subject split, distributed hazard GBDT, survival curves, concordance/IPCW Brier/calibration, partitioned-table runner, model/table/run materialization (#1044)
    survival_methodology.cpp M-5 survival graph builder, ownership predicate, resolved-table run callback (#1044)
    next_best_action.cpp    native propensity composition, implicit-feedback ALS, chronological hit rate, constrained selection, and IPS/DR evaluation (#1047)
    next_best_action_execution.cpp  asynchronous registry run, graph-owned dataset resolution, coordinated peer launch, and durable Run lifecycle for M-8 (#1047)
    next_best_action_job.cpp  run the M-8 trainer as a recorded distributed job through the coordinator (#1047)
    next_best_action_methodology.cpp  M-8 shape builder, strict graph parser, ownership predicate, metadata, and table/policy parameters (#1047)
    next_best_action_table.cpp  resolved-table adapter and recommendation/report/model materialization with lineage (#1047)
    next_best_action_graph_check.cpp  default-gate enforcement of M-8 chronological evaluation, propensity interpretation and hard policy constraints (#1047)
    rank.cpp                zetta::ml — LambdaMART derivatives, deterministic whole-group split, NDCG@k/yield metrics, and grouped scoring (#1048)
    rank_train_job.cpp      zetta::ml — distributed group-ownership check, all-reduced LambdaMART training, metrics, ranking, and rank Model registration (#1048)
    rank_runner.cpp         zetta::ml — resolved-table near-data load, durable Run, ranked-queue table, outputs, and lineage (#1048)
    rank_methodology.cpp    zetta::ml — M-9 build_shape/owns/run registry adapter (#1048)
    ensemble.cpp            native M-18 fold planning, out-of-fold base fitting, regularized logistic stacker, full-training refit, blended scoring, and holdout comparison (#1057)
    ensemble_train_job.cpp  M-18 ComputeJob adapter from a device-backed member-owned frame to distributed training through JobRound collectives (#1057)
    ensemble_methodology.cpp  M-18 registry shape, ownership, table resolution, and run callback (#1057)
    ensemble_runner.cpp     M-18 GraphExecutor adapter, Model registration, output tables, and Run lineage over a resolved table (#1057)
    synthetic_private.cpp   M-20 empirical Gaussian-copula synthesis, DCR/membership/TSTR evaluation, and formally accounted DP histogram GBDT (#1059)
    synthetic_private_job.cpp  M-20 owned-partition ComputeJob and coordinated-launch runner; combines partition statistics and DP training rounds across members (#1059)
    synthetic_private_methodology.cpp  M-20 registry graph shape, ownership predicate, runner, output-table lineage, model registration, and run metrics (#1059)
  workspace/workspace_profile.cpp  zetta::workspace::WorkspaceProfile — value, set_content/apply_value, JSON-Schema boundary validation (parse_workspace_profile_value); the profile:<uuid> replicated entity (#886)
  solutions/monitor.cpp     zetta::solutions::Monitor — PSI/calibration/cohort/guardrail evaluation, replicated persistence, active breach state, and breach callback (#1106)
  billing/plan.cpp          zetta::billing::Plan — value, set_content/apply_value, JSON-Schema boundary validation (parse_plan_value); the plan:<uuid> replicated entity (Conflict D). Metered usage is the computed system.usage view in executor.cpp
  solutions/decision_policy.cpp  versioned decision-policy storage, catalog reconciliation, evaluation, approval, and batch table adapter (#1104)
  solutions/experiment.cpp zetta::solutions::ExperimentManager — SHA-256 allocation, protected decisions, exemption accounting, report construction, disk projection, and independent experiment:/experiment_assignment: CRDT records (#1105)
  solutions/action_integration.cpp replicated action outbox/report state, connector request builders, bounded delivery, dead letters, policy checks, and ledger recording (#1109)
  solutions/roi_readout.cpp versioned replicated definition storage and live readout assembly from graph gates, experiments, ledger outcomes, policy decisions, and monitors (#1108)
  solutions/solution_templates.cpp executable policy, experiment, guardrail and measurement defaults for S-1 through S-12 and B-1 through B-8 (#1118, #1128, #1175, #1176, #1177, #1178, #1179, #1180, #1181, #1182)
  solutions/solution_execution.cpp durable parent/child Run orchestration over snapshot, methodology and foundation stages, including per-methodology settings and module evidence (#1128, #1175, #1177, #1180)
  solutions/solution_foundation.cpp foundation record creation, snapshot binding, score/evidence joins, governed delivery and monitor updates, including B-1 through B-8 enforcement (#1118, #1128, #1175, #1176, #1177, #1178, #1179, #1180, #1181, #1182)
    gbdt.cpp                zetta::ml — histogram GBDT trainer (binning + split-finding + boosting loop), binary/squared/LambdaMART objective support, globally reduced per-round loss and throughput callbacks + GbdtParams/GbdtMetrics JSON boundaries (gbdt_model via pboettch) (zetta-core; #390, #1137)
    model_import.cpp        zetta::ml — the ONNX, XGBoost and LightGBM readers, split/missing-value conversion to the strict `<`, and the ONNX writer (zetta-core; lake#681)
    protobuf_wire.hpp       zetta::ml::protobuf — hand-written protobuf wire-format reader/writer for the ONNX messages a tree ensemble uses (internal; lake#681)
    forecast.cpp            zetta::ml — refit-at-origin backtesting, recursive multi-step prediction, per-fold/mean MAE·MASE·sMAPE·FVA, and distributed residual intervals (#1043)
    forecast_runner.cpp     zetta::ml — verify per-series ownership, bridge JobRound allreduce into forecasting, load resolved owned partitions, and write root model/table outputs with lineage (#1043)
    forecast_methodology.cpp / forecast_execution.cpp
                            M-4's own shape, validated roles/options, table resolution, coroutine launch and durable Run lifecycle (#1043)
    language.cpp            zetta::ml — WordPiece tokenization, BERT encoder/checkpoint loading, normalized embeddings, deterministic retrieval, grounded-answer enforcement, and classifier math (#1049)
    language_train_job.cpp  distributed classifier-statistics reduction and versioned-head finalization through the compute job coordinator (#1049)
    language_runner.cpp     resolved table read, embedding/retrieval/answer execution, metrics, Model registration, and output-table lineage (#1049)
    gbdt_train_job.cpp      run_train_job — read the device-backed feature frame, drive boosting with JobRound allreduce, compute global metrics, register the Run-linked Model on the root, report per-round progress, and choose either content-hash or global out-of-time holdout rows; opt-in calibration and checkpoint/restart (#390/#391/#468/#1205)
    gbdt_score.cpp          zetta::ml — pure scorer (align columns to the model's feature order, predict_proba, rank desc + top_k) + load_gbdt_model (weights body → GbdtModel JSON Schema boundary) (zetta-core; #392)
    gbdt_score_job.cpp      run_score_job — read device-backed columns, predict the shard, gather (entity,risk) to root, rank on the device (sort_by_key), broadcast the global at-risk list (zetta-core; #392)
    local_train_job.cpp     run_near_data_train_job — load the local shard via load_local_partitions then drive the shared train/explain/rank phases over it; also defines zetta::ml::detail::run_train_explain_rank_phases (zetta-core; #533)
    local_featurize_train_job.cpp  run_near_data_featurize_train_job — load the local shard once, drive run_featurize_job over it, slice the broadcast FeatureTable to this node's entities, then the shared train/explain/rank phases over the slice; records featurize + train pipeline-instrumentation rows (zetta-core; #530)
    near_data_train_pipeline.hpp  zetta::ml::detail::run_train_explain_rank_phases — the #533 train (+calibrate) → explain → rank phase driver over an already-materialized feature frame, shared by local_train_job.cpp and local_featurize_train_job.cpp (zetta-core internal; #533/#530)
    feature_columns.hpp     zetta::ml::detail::read_column_as_double — shared device-download helper (frame column → host doubles, dtype-widening) used by the train + score + explain job bodies (zetta-core internal; #392/#393)
    calibration.cpp         zetta::ml — Platt (robust Newton, Lin et al. 2007) + isotonic (PAVA) fit, Calibrator apply/JSON/wire boundary, Brier/ECE metrics (zetta-core; #391)
    explain_row.cpp         zetta::ml — per-row "why is this one at risk": tree_shap for one entity (near-data) or a supplied row, contributions sorted by |φ|, efficiency residual exposed (zetta-core; #948 step 6)
    tree_shap.cpp           zetta::ml — exact TreeSHAP recursion (extend/unwind/unwound_path_sum), model_base_value (cover-weighted expectation), dominant_driver (zetta-core; #393)
    gbdt_explain_job.cpp    run_explain_job — read the device-backed feature shard, per-entity tree_shap (parallel_for), allreduce the global mean-|φ| driver ranking, drive the `explain` job lifecycle (zetta-core; #393)
    golden_oracle.cpp       load_golden_oracle — parse + schema-validate (pboettch draft-07) the committed golden oracle fixture at the boundary + structural rules, GoldenOracle::to_json (zetta-core; #395)
    playbook.cpp            zetta::ml — playbook engine: PlaybookPolicy churn dictionary + hand-written JSON boundary, expected-retained-value priority + ranking, RulesEngineNarrator + ConstrainedNarrator (pboettch playbook_entry schema validation + semantic checks + fallback), generate_playbook, OutcomeLog (zetta-core; #397)
  autograd/
    tensor.cpp              zetta::autograd — Node/Tensor + backward (topo-order reverse-mode) and the op library (matmul, transpose, add/mul/scale/add_bias, gelu/silu, row_softmax, rms_norm, layer_norm, embedding, cross_entropy); reference host kernels, gradient-checked (zetta-core; #555/#1049)
    transformer.cpp         zetta::autograd::causal_self_attention/grouped_query_attention/feed_forward/swiglu_feed_forward — the decoder blocks composed from the autograd ops (per-head RoPE·QK-norm·scaled SDPA under a causal mask, SwiGLU MLP); backprop flows through via the graph (zetta-core; #555)
    qwen.cpp                zetta::autograd::QwenModel + QwenInference (KV-cached plain-float generation, O(seq)/token, matches forward) + parse_qwen_config — assembles the Qwen3 stack (embed → RMSNorm/GQA+QK-norm/RMSNorm/SwiGLU per layer → norm → lm_head) with HF-named params; fine_tune_qwen_step performs one completion-masked update and reports loss/rate/gradient/token metrics; to_safetensors/load_safetensors round-trip a checkpoint by name; generate_greedy/generate autoregressively decode; parse_qwen_config reads a HF config.json; load_qwen_model reads config.json + model.safetensors (single-file or sharded via model.safetensors.index.json) from a directory; save_qwen_model/serialize_qwen_config write a checkpoint dir back out (ship a post-trained checkpoint) (zetta-core; #555, #1137)
    safetensors.cpp         zetta::autograd — host safetensors parse/serialize (8-byte LE header length + JSON header + contiguous LE data), F32/BF16 decode, non-throwing bounds checks; no foreign runtime (zetta-core; #555)
  tokenizer/
    byte_encoding.cpp       zetta::tokenizer — non-throwing UTF-8 decode/encode + the GPT-2 bytes_to_unicode table (printable bytes→self, rest→U+0100+) and its inverse (zetta-core; #555)
    pre_tokenizer.cpp       zetta::tokenizer::pre_tokenize — decode-with-byte-offsets + seven ordered alternative matchers (Perl leftmost-first) reproducing the Qwen pattern; ASCII-exact letter/number classification + common scripts (zetta-core; #555)
    bpe.cpp                 zetta::tokenizer::BpeTokenizer — builds byte→id + packed-uint64 merge tables; apply_merges (lowest-rank pair first, merge all occurrences); encode splits special tokens then encode_ordinary per span; loaders parse vocab.json+merges.txt / tokenizer.json (both merge formats + added_tokens) / a directory (zetta-core; #555)
  agent/
    grader.cpp              zetta::agent::EndStateGrader::grade — deterministic reward: count required facts satisfied in the observed end-state, score = matched/total in [0,1], passed = all matched with ≥1 required (zetta-core; #555)
    sandbox.cpp             zetta::agent::Sandbox — register_tool/has_tool/apply: look up the tool, run its effect against the world, map to Ok/UnknownTool/ToolFailed (unknown leaves the world untouched) (zetta-core; #555)
    rollout.cpp             zetta::agent::run_rollout — the turn loop: ask the policy for the next decision, apply non-stop calls to the sandbox and record each turn, stop on a policy stop or the turn budget, return trajectory + final state (zetta-core; #555)
    reward.cpp              zetta::agent::rubric_reward/reward_for_rollout — renormalized weighted average of active rubric terms; build end_state (grader) + right_tools (known/total calls) + right_args (ok/known calls) from a rollout (zetta-core; #555)
    admission_gate.cpp      zetta::agent::admit_corpus/to_string — check each case's reference passes + controls fail via the grader, require ≥1 negative control corpus-wide, reject untrainable tasks; collect every failure (zetta-core; #555/#248)
    softmax_policy.cpp      zetta::agent::SoftmaxPolicy — stable softmax (subtract max), inverse-CDF sample with a rounding guard, log_prob = log(p[action]) (zetta-core; #555)
    advantage.cpp           zetta::agent::group_stats/group_advantages — mean + population stddev of a reward group, then (r-mean)/(stddev+eps) per rollout (zetta-core; #555)
    policy_gradient.cpp     zetta::agent::policy_gradient_step — probs at the sampling policy, accumulate advantage·(indicator−p) per logit over the batch, ascend by learning_rate (zetta-core; #555)
    training.cpp            zetta::agent::train_softmax_policy — per step: sample group_size actions via the injected uniform, score via the injected reward, GRPO advantages, gradient step, record the mean reward (zetta-core; #555)
    judge.cpp               zetta::agent::resolve_grade — deterministic grader when the task is_trainable, else the injected judge, else a failing Deterministic grade (fail closed) (zetta-core; #555)
    sequence_training.cpp   zetta::agent::train_sequence_policy — per step: sample group trajectories, GRPO advantages over trajectory rewards, apply each trajectory's advantage to every turn's policy_gradient_step (zetta-core; #555)
    corpus_training.cpp     zetta::agent::train_over_corpus/CorpusTrainingReport — train one SoftmaxPolicy per task, record first/last mean reward + improved; improved_count/mean_reward_after aggregates (zetta-core; #555)
    linear_policy.cpp       zetta::agent::LinearPolicy — upload W + features, gemm (action_count×feature_count · feature_count×1), download logits; probabilities reuse the SoftmaxPolicy softmax (zetta-core; #555)
    distributed.cpp         zetta::agent — allreduce local {sum,sqsum,count} → global GRPO mean/std; allreduce-Sum local gradients → global update; identity_allreduce = single-node (zetta-core; #555)
    distributed_training.cpp  zetta::agent::train_softmax_policy_distributed — per step: sample local group, co_await when_all over the reward io_tasks, distributed advantages + gradient step, allreduce the global mean reward; step-unique collective tags (zetta-core; #555)
    collectives_allreduce.cpp  zetta::agent::collectives_allreduce — Reduce→CollectiveOp map, forward to Collectives::allreduce with a timeout, value_or({}) so a failed round is a collective failure (zetta-core; #555)
    neural_training.cpp     zetta::agent::train_linear_policy_distributed — per step: forward W·x on the device per context, sample, when_all rewards, GRPO advantages, accumulate + allreduce dW = A·(1[a]-pi)⊗x, update W (zetta-core; #555)
    qwen_rlvr.cpp           zetta::agent::grpo_step (+ grpo_step_text) — one GRPO/RLVR update over the autograd QwenModel: sample a group of completions, score, group_advantages, sum each completion's sequence_policy_gradient_loss (advantage over its tokens), backward + Adam; grpo_step_text wraps it with the tokenizer (zetta-core; #555)
    text_generation.cpp     zetta::agent::generate_text — encode(prompt) → model.generate(_greedy) → decode(new tokens); greedy + sampling overloads (zetta-core; #555)
    action.cpp              zetta::agent::StudioAction {message, tool_calls, execution_graph} — the Studio auto-AI agent's ACTION (to_json/action_from_json/studio_action_schema): the pipeline the agent emits, which the engine RUNS and the gate scores (mirrors blender-rl's Blender-Python action; the reshape from the facts output, #873) (zetta-core; #873)
    base_prompt.cpp         zetta::agent::base_agent_system_prompt (the Orama Studio auto-AI agent's canonical system prompt: turn an ask into a production pipeline, per-use-case, correct-by-construction, emit {message,tool_calls,execution_graph}, pipeline/project/solution vocab) + agent_prompt_format (system + ask; the model completes with the JSON action) — the SINGLE source of truth shared by the SFT render and agent::respond (zetta-core; #555/#873)
    solution_graph.cpp      zetta::agent::build_solution_graph/graph_solution_facts (the generic facts↔pipeline bridge: any solution facts → a real gradeable ir::ExecutionGraph and back — faithful by construction, so a graph built from a task's reference facts passes its grader) + reference_action_from_facts/attach_reference_action (the facts→StudioAction bridge, abstain special-cased to decline + build nothing) + render_action_example (the SFT (prompt,target): Studio system prompt + ask → the reference action JSON) — the whole 565→actions reshape without a per-use-case builder (zetta-core; #873)
    interaction.cpp         zetta::agent::InteractionKind (action|answer|clarify|abstain - the mode a response DECLARES, so the harness switches instead of guessing and a wrong mode is a graded failure) + render_interaction_prompt - THE one renderer shared by the corpus (render_action_example) and serving (build_chat_prompt); grounding is placed here, not at either call site, so a training example is byte-identical to a served turn (zetta-core; #918)
    response_facts.cpp      zetta::agent::response_solution_facts - projects a whole response envelope into gradeable facts, because answer/clarify/abstain carry no pipeline: kind, built_nothing, asked_one_question, called_<capability>, and the anti-fabrication check numbers_from_context (every numeric literal in the message must appear in the retrieved context, compared AS WRITTEN so rounding 0.880 to 0.88 fails) plus entity/value grounding (top_driver, top_account, measure_auc, table, row_count, node_count, run_status) (zetta-core; #918)
  dataset/                  native, in-Zetta training-dataset generation (#873/#878/#879) — the native replacement for the offline Python corpus generators (ADR-17); what a per-customer fork (#853) grows its own corpus with. See docs/dataset/README.md
    dataset_generator_factory.cpp  zetta::dataset::DatasetGeneratorFactory — registers every DatasetGenerator (base-agent, auto-suggest); create-by-kind; the generate_dataset/list_dataset_generators capabilities + the GUI/web "Data generators" view all drive builtin() so a new generator shows up in API+MCP+web+GUI at once (zetta-core; #873)
    base_agent_generator.cpp  zetta::dataset::BaseAgentDatasetGenerator — the base agent's intent→pipeline corpus (churn + search hand-built rich graphs, plus every plan subject via plan_solution_templates), each an admission-gated case with its reference {message,tool_calls,execution_graph} action attached (zetta-core; #878)
    plan_templates.cpp      zetta::dataset::plan_solution_templates/case_from_template — the solutions plan encoded as gated task templates (a subject → the facts its correct pipeline must satisfy, mined from the plan's per-method graphs + per-solution gate checks); coverage grows as one row per subject (24 use cases · 21 methods · 12 solutions · DSC · autonomy · abstain) (zetta-core; #878)
    interaction_families.cpp  zetta::dataset::interaction_families/synthetic_project_contexts/render_grounding - the interaction corpus: 10 families covering the turns a user actually has (grounded QA over results/data/platform, capability discovery, feature engineering, Python/SQL in a node, pipeline modification, governance, abstention, clarification). Volume is a deterministic cross product of contexts x asks x phrasings; the multiplier that matters is CONTEXT variation, which makes a grounded-QA case unanswerable without reading the supplied facts (zetta-core; #918)
    auto_suggest_generator.cpp  zetta::dataset::AutoSuggestDatasetGenerator — the Auto-Suggest corpus (#554): gated data-state scenarios (suggest the present use cases, withhold the absent distractors) (zetta-core; #879)
  index/
    index.cpp               zetta::index::Index — IndexValue to_json/parse + Index JSON Schema (pboettch) + persist/apply (parse_index_value) (ENT-39/ENT-40)
    index_registry.cpp      zetta::index::IndexRegistry resolve + IndexSpec::key() + index_spec_from_node (IR `index` step → IndexSpec boundary)
    index_step_executor.cpp  zetta::index::IndexStepExecutor — run an IR `index` step: resolve backend (node registry) → read table rows into partitions → build → record Index entity → `index` output; + read_index_partitions (shared) (#528)
    in_memory_index_backend.cpp  zetta::index::InMemoryIndexBackend — inverted-index build over partitions / query / idempotent drop (the Search reference backend)
    oramacore_fulltext_index.cpp  zetta::index::OramaCoreFulltextIndex — full-text BM25F build over partitions / ranked query (now carrying per-hit scores) / drop via the oramacore-cpp MemoryIndex (pImpl); ZETTA_WITH_ORAMACORE-guarded engine-absent stub (#527)
    distributed_fulltext.cpp  zetta::index::DistributedFulltextIndex — per-partition build from owned partitions, scatter-gather query over the fulltext.query RPC (now with per-node NodeContribution provenance + optional per-peer spans), merge_fulltext_hits + the request/response JSON-Schema boundary (#529, #641)
    search.cpp              zetta::index::search — the canonical entry point's resolution/routing/provenance/trace implementation (#641)
  cloud/
    node_capability.cpp     zetta::cloud::NodeCapability — GpuKind names, satisfies(), JSON boundary validation (ENT-32)
    provisioner.cpp         zetta::cloud::ProvisionedNode — JSON boundary validation for the Provisioner DTO
    provisioner_detect.cpp  zetta::cloud::detect_provisioner_configs — the per-provider detectors: run each local CLI's fast read-only context queries through the CommandRunner, parse the output, build the provider's validated XConfig JSON + one-line summary → ProvisionerConfig
    add_nodes.cpp           zetta::cloud::add_nodes — permanent-add control action over the Provisioner interface (provision + report, no teardown) + AddNodesRequest JSON boundary (#518); the Node& overloads drive rebalance_partitions onto the new members (#519)
    local_provisioner.cpp   zetta::cloud::LocalProvisioner — given-hardware backend: best-effort select over the live cluster fleet, no-op teardown (#374)
    provisioner_fleet.cpp   zetta::cloud::ProvisionerFleet — per-provider wire_one from the validated XConfig (seed injected), stable-address optional slots, provisioner_for lookup (#621)
    azure_provisioner.cpp   zetta::cloud::AzureProvisioner — burst lifecycle (provision+mint / teardown+revoke) over the AzureControlPlane interface + AzureConfig JSON boundary (#376)
    azure_cli_control_plane.cpp  zetta::cloud::AzCliControlPlane — `az vmss` argv construction + JSON parsing at the ProvisionedNode boundary (#376)
    command_runner.cpp      zetta::cloud::default_command_runner — the shared fork/execvp/pipe exec runner (argv, no shell; #376)
    secret_file.cpp         zetta::cloud::SecretFile — mkdir 0700 + open O_EXCL|O_NOFOLLOW 0600, write, remove_all on destruction (#770)
    node_bootstrap_script.cpp the boot-script renderer: shell_quote, per-vendor fetch_token, retry + validate + write (#1316)
    aws_provisioner.cpp     zetta::cloud::AwsProvisioner — burst lifecycle (provision+mint / teardown+revoke) over the AwsControlPlane interface + AwsConfig JSON boundary (#377)
    aws_cli_control_plane.cpp  zetta::cloud::AwsCliControlPlane — `aws autoscaling`/`aws ec2` argv construction + JSON parsing at the ProvisionedNode boundary (#377)
    gcp_provisioner.cpp     zetta::cloud::GcpProvisioner — GCP burst lifecycle (provision+mint / teardown+revoke) over the GcpControlPlane interface + GcpConfig JSON boundary (#378)
    gcp_cli_control_plane.cpp  zetta::cloud::GcpCliControlPlane — `gcloud` MIG argv construction (template/create/resize/list/delete-instances) + JSON parse at the ProvisionedNode boundary (#378)
    oci_provisioner.cpp     zetta::cloud::OciProvisioner — OCI burst lifecycle (provision+mint / teardown+revoke) over the OciControlPlane interface + OciConfig JSON boundary (#432)
    oci_cli_control_plane.cpp  zetta::cloud::OciCliControlPlane — `oci` Instance Pool argv (resolve-by-name/instance-config/create-or-resize/list-instances/detach) + JSON parse at the ProvisionedNode boundary (#432)
    k8s_provisioner.cpp     zetta::cloud::K8sProvisioner — Kubernetes burst lifecycle (provision+mint / teardown+revoke) over the K8sControlPlane interface + K8sConfig JSON boundary (#434)
    k8s_cli_control_plane.cpp  zetta::cloud::K8sCliControlPlane — `kubectl` Deployment argv (create/set env+sa/patch spot/scale/get pods/delete pod) + JSON parse at the ProvisionedNode boundary (#434)
    bare_metal_provisioner.cpp  zetta::cloud::BareMetalProvisioner — bare-metal burst lifecycle (provision+mint / teardown+revoke) over the BareMetalControlPlane interface + BareMetalConfig JSON boundary (#436)
    bare_metal_cli_control_plane.cpp  zetta::cloud::BareMetalCliControlPlane — docker/podman argv (ps --filter label/run -d shortfall/rm -f) + NDJSON parse at the ProvisionedNode boundary (#436)
    burst_identity.cpp      zetta::cloud::BurstIdentityManager — bulk mint/revoke burst identities over the Node's principal CRUD + audit sink (#375)
    burst_approval_gate.cpp  zetta::cloud::BurstApprovalGate — cost-cap gate logic: estimate, default cost, audit the decision, BurstCap JSON boundary (APR-3)
    cloud_provider.cpp      zetta::cloud::CloudProviderCatalog — built-in provider metadata (local/bare-metal/k8s/aws/gcp/azure/oci); no cloud access
    cloud_inspector.cpp     zetta::cloud::CloudInspector — run a provider's read-only commands, per-vendor JSON parse + boundary validation → CloudInspectReport, report JSON boundary (#445)
  pubsub/
    address.cpp             zetta::pubsub address parsing + wildcard matching (#1025)
    event.cpp               zetta::pubsub payloads, MIME interning, bcc stripping, envelope checks, frame encode/decode with crc32 (#1025)
    store.cpp               zetta::pubsub ring buffers, TopicRingStack, QueueStore (#1026)
    delivery.cpp            zetta::pubsub delivery resolution + apply_bounds (#1028/#1029)
    dispatcher.cpp          zetta::pubsub::Dispatcher — dynamic shared-pool growth/shrink and between-job key rebalance (D24/#1157)
    resources.cpp           zetta::pubsub resource detection (cgroup/mountinfo parsing, sysctl/meminfo, statvfs) + topic-sized dispatch bounds + derive_limits + the stderr sink (#1030/#1157)
    topic.hpp / topic.cpp   internal: one topic or inbox — priority levels of stores, subscriptions + groups + claims, at-least/exactly-once, overflow enforcement, spill logging, snapshot/restore; the Subscription handle (#1027/#1028/#1030)
    pubsub.cpp              zetta::pubsub::PubSub — routing (to/cc/bcc, wildcards via a cache), governance, inboxes, drains + replies, dead letters, error topics, auto-creation, definitions on disk, shutdown flush + start-up reload (#1027–#1032)
    node_integration.cpp    the node adapters above (#1029/#1032/#1034)
  governance/
    principal.cpp           zetta::governance::Principal — persist/rename principal.json + key CRUD
    access_key.cpp          zetta::governance::AccessKey — persist key.json + authorize() decisions (resource + operation)
    scope.cpp               zetta::governance::Scope — permits() + JSON boundary validation
    capability.cpp          zetta::governance::CapabilitySet — permits()/TTL + JSON boundary validation (ENT-22)
    user.cpp                zetta::governance::User — persist user.json + status/schema (#744)
    role.cpp                zetta::governance::Role — persist role.json + built-in permission sets (from the vocabulary) + schema + JSON boundary validation
    permission.cpp          the RBAC permission vocabulary table + the atoms→Capability{domain,action} mapping + expand/reverse + built-in role permission-name sets (#746)
    token.cpp               zetta::governance::Token — persist token.json + validates()/expired() + TokenValue JSON boundary validation
    user_role.cpp           zetta::governance::UserRole — persist user_role.json + schema
    policy.cpp              zetta::governance::Policy — persist policy.json + PolicyValue JSON boundary validation + policy schema (RLS, #748)
    token_secret.cpp        the token crypto (the 2nd OpenSSL-libcrypto user): RAND_bytes secret/salt + SHA-256(salt||secret) + PBKDF2-HMAC-SHA256 for the bootstrap owner (#769) + CRYPTO_memcmp constant-time compare (#744)
    audit_event.cpp         zetta::governance::AuditEvent/AuditTrail — value/schema boundary, compressed segment frames, durable seek index/watermarks, legacy migration, range validation and retention
    credential_redaction.cpp the one rule for which input fields are credentials (`is_credential_field`) + `redact_credentials`, used by the consent challenge JSON and the web form's masked inputs (#74)
    consent.cpp             zetta::governance::ConsentLedger — challenge lifecycle (pending → denied/approved → executed/failed, lazy TTL expiry, capacity that drops decided entries first), challenge JSON + embedded schema (#621)
  solutions/
    audit_ledger.cpp        zetta::solutions ledger JSON codecs/schema validation plus immutable `<data_dir>/ledger/<uuid>.json` persistence (#1107)
    action_integration.cpp  action task/report codecs, catalog persistence, connectors, and delivery runtime (#1109)
  usecases/churn/
    churn_step_executor.cpp  ChurnStepExecutor — the per-kind real churn compute, each heavy step a ComputeJob (composes featurize/train/score/explain); index step also builds a real entity index via the node registry (#396/#528)
    churn_solution.cpp      run_churn_solution_on_node — the ChurnDataset→numeric encoder + standard churn pipeline config + the one public runner (resolve Node::job_coordinator → inject ChurnStepExecutor into a GraphExecutor over the live spine; run_churn_solution_with_coordinator is the file-local isolated-coordinator detail) + is_churn_graph + demo_churn_dataset (#398/#580/#637) + the coordinated fan-out (snapshot the roster, ship the encoded shards in a run.launch payload, run this node's entity shard) + churn_coordinated_run_schema + install_churn_launch_runner (the peer-side co-run: validate → re-gate → shard → run) (#601)
    churn_dataset.cpp       load_churn_dataset + write_churn_dataset — JSONL/CSV churn fixture reader and its canonical-JSONL writer, both over the same 3 embedded draft-07 schemas (boundary validation both directions, #402/#623)
    entity_resolution.cpp   EntityResolver — normalized full-name / brand matching + entity_resolution_annotation builder (FEAT-6/TC-4; #394)
    entity_resolution_check.cpp  EntityResolutionCheck — conflation/fragmentation + schema boundary
  repl/
    console_model.cpp       zetta::repl::ConsoleModel — submit/clear logic + Frontend names + format_result/format_transcript text projections (CMP-6; #524)
    governed_console.cpp    zetta::repl::GovernedConsole — the governance pipeline (capability check → sim-Python lower+gate → execute via the injectable graph runner → data_access audit) + required_operation + the SQL Outcome→Result grid mapping (SCR-8; #525/#580)
  testkit/                  zetta::testkit — the in-app test-suite catalog (no SDL; #462)
    test_suite.cpp          TestCase/TestOption/TestOptions/TestResult/TestSuite + run_test_case + builtin()
    harness.cpp             ScratchDir + LoopbackCluster (real N-node loopback; optional replicated wiring + announce_member + replicated_document, and attached mode = announce + Node::attach_cluster, which installs the run.launch listener every co-launched job kind needs) + sample_churn_dataset + numeric encoding
    cases.hpp               internal: the per-category register_*_cases registrars TestSuite::builtin() assembles (one TU per category keeps includes local)
    cases_*.cpp             the canned cases per category (compute/ddf/parquet/ml/distributed/connect/fulltext/governance/pipeline/uc1_scale/examples; cases_fulltext.cpp = the TC-13 cluster.fulltext_index distributed case, #503)
    cases_connect.cpp       the source-connector cases, both TestCategory::Distributed and both against an injected fake transport: cluster.connections (replicate-to-all credential, read-only guard before egress, no token on any read surface, typed cells; #827) + cluster.distributed_import (the co-launched ingest: fetched once cluster-wide, landed where pulled, zero row: entries, read back by scatter-gather; #833)
    parquet_fixture_data.hpp  GENERATED by scripts/gen-parquet-fixture.py — the embedded (base64) pyarrow saas_sample.parquet fixture + its golden DataFrame projection, so the in-app TC-9 Parquet parity case is self-contained (same golden as tests/data/test_parquet_reader.cpp; #462)
    example_script.cpp      run_example_script — parse->IR->gate->run + expectation checks + offending-statement reconstruction (#523)
    cases_examples.cpp      example_scripts() built-in catalog + register_example_cases (each sim-Python example a TestCase under TestCategory::Examples)
  util/
    private_directory.hpp / private_directory.cpp  internal zetta::util::PrivateDirectory: 0700 directories, exclusive 0600 staging files, checked reads and atomic replacement for cluster and connector secrets (#1413)
    uuid.cpp
    bytes.cpp / duration.cpp  the size / time formatter impls
    sha256.cpp              self-contained SHA-256 (no crypto dependency)
    timestamp.cpp           zetta::util::now_iso8601() / iso8601_seconds_ago() / parse_iso8601_seconds() — UTC ISO-8601 stamps + the non-throwing parse
src/gui/                    headed UI (links SDL3 + ImGui)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  app.hpp                   zetta::gui::App — the one class declaring every panel method (forward-declares SDL/ImGui types)
  app.cpp                   lifecycle: ctor/dtor, init/run/render_frame, apply_display_scale, the menu bar + view switcher, file ingestion, the g_mono_font definition + the two load-bearing rendering bits
  app_internal.hpp          shared GUI-only helpers: the Orama design tokens + the canvas ImU32 palette (kCanvasBox*/kCanvasGauge*/kCanvasEdge* — every colour's one home, #630), Tone + tone_color/tone_dot_color/tone_background + the status widget forms status_label (rows) / status_pill (headers) / status_dot(tone) (#636), the widget grammar (page_header/header_action/section_card/stat_tile/stat_tile_link/empty_state/banner/callout, #630/#629), the table style (kDataTableFlags zebra-off + table_numeric right-aligned mono cells, #630), the master-detail scaffold (MasterRow/detail_header/master_detail, #634), wash/eyebrow/mono_line/primary_button, term_hint (hover hint naming a technical term, #635), activity_surface_label (#622), extern g_mono_font/g_heading_font (not in the public include/zetta surface)
  tones.hpp                 the ONE mapping site from engine statuses to visual tones (#636): tone_of(RunStatus/StepRunStatus/JobStatus/ScalingAction/CheckStatus/TestOutcome/ActivityOutcome/cluster::NodeStatus/StepPlacement) + tone_of_log_level + tone_of_utilization (the shared gauge thresholds), with static_asserts locking the cross-lifecycle agreements
  labels.hpp                the ONE mapping site from engine enums / kind tokens to product-language labels (#635, docs/gui/copy-audit.md): label_of(JobKind/JobStatus/RunStatus/StepRunStatus/StepKind/EdgeKind/SearchPath/DeploymentKind/StepPlacement/CheckStatus/TestOutcome/ScalingObjective/ScalingAction/ActivityOutcome) + humanize_kind (free-text wire tokens, graceful fallback), with static_asserts locking the engine-speak wire names; SQL/system.* output stays raw by design
  app_views.cpp             the view registry (App::view_registry, one ViewInfo row per view) + navigate_to/take_pending_selection + entity_link + the sidebar rail (#628) + sidebar badges
  app_palette.cpp           the Cmd+K command palette (#625, docs/gui/command-palette.md): fuzzy view navigation (the registry's fourth consumer) + entity jump (projects/datasets/runs/models/jobs/nodes via navigate_to) + the capability registry's actions rendered for humans — a schema-driven argument form per entry, executed through Registry::call on the owning loop (validation/authorize/audit/spans/consent all in the one path)
  cluster_telemetry.hpp     zetta::gui::NodeGaugeState/Sparkline/Gauge — pure-data (no ImGui) animation state for the cluster node-box gauges (#584): per-gauge eased value + sparkline history ring, keyed by node id; a thin renderer's working copy of Node::telemetry_snapshot(); the drawing lives in app_cluster.cpp
  cluster_pipeline.hpp      zetta::gui::PipelineFlow/ClusterPipelineState/PipelineInputs — pure-data (no ImGui) animation state for the distributed-algorithm-at-work overlay on the cluster node-box canvas (#590, #575 area 5): short-lived eased "flows" (partition placement / near-data load / scatter-gather collective rounds / rebalance moves) diffed each frame from the live instrumentation read views (system.dataset_partitions / system.pipeline_jobs / system.collective_rounds); owns no numbers, only the animation; the diff (update_cluster_pipeline) lives in app_cluster.cpp, the drawing (draw_pipeline_flow) in app_operations_internal.hpp, and the Search view replays a search's provenance through the same flows (#643)
  app_home.cpp              the Home dashboard (SCR-1, #629; docs/gui/home-dashboard.md): cluster health strip (stat_tile_link tiles over Cluster::nodes() + telemetry_snapshot()), the intent→deployed velocity KPI, needs-attention (failed runs/jobs, denials, lagging peers), recent runs, projects with last activity, the recent-activity strip, the guided demo / guided-start empty branch — plus draw_dashboard (the persistent node strip) and the About modal
  app_onboarding.cpp        first-run onboarding (#623, docs/gui/onboarding.md): App::draw_guided_demo_card — the five-step guided UC-1 churn demo on the empty Home (write_churn_dataset + add_dataset_file demo load → pre-filled New Project modal → compile_intent_to_graph + gate + deploy → the shared AsyncChurnRun over the live coordinator → at-risk accounts + drivers summary); step progress derived from live engine state each frame, dismiss session-only, replayable from Help · Guided demo
  app_explorer.cpp / app_governance.cpp (app_governance additionally carries the #621 consent approval surface: draw_consent_requests — pending challenges named in the Cloud Inspector grammar, Approve/Deny via Registry::decide_consent as the trusted headed surface)
                            the explorer and admin+audit App::draw_* panels (draw_admin + draw_audit_log — audit targets render as entity links routed by action, #633) — each an App member TU over app.hpp + app_internal.hpp (#464); the explorer's `system` table list renders zetta-core's query::system_table_names(), never a GUI copy (#684, docs/gui/explorer-system-tables.md)
  app_operations_internal.hpp
                            helpers shared by the per-view operations TUs below (#638) and the Search view: entity_short_id, join_refs, the render_model_card declaration (defined in app_models.cpp; also used by the Runs detail), and the #590 flow-animation grammar (kFlow* palette, with_alpha, flow_color, draw_pipeline_flow — shared by the Cluster view's overlay and the Search view's cluster strip, #643) — internal to src/gui, not in the public include/zetta surface; the canvas node-box palette moved to app_internal.hpp (kCanvasBox*, #630)
  app_graph_workspace.cpp   the Graph Workspace view (open_graph + draw_graph_workspace + draw_graph_canvas + draw_graph_editor), its graph-over-cluster plan view (draw_graph_plan + build_graph_plan + the canvas placement overlay; #591), and the live-run overlay (latest_run_for_open_graph + draw_graph_run_status + run_open_graph + the canvas status dots/pulse/pinned failure; #626)
  app_deploy.cpp            the correctness gate (draw_gate) + Deploy panel (draw_deploy — deployments table with graph + latest-run entity links and the deep-link row highlight, #633) + the async churn demo run (run_churn_demo → AsyncChurnRun, live progress + cancel, #624)
  app_activity.cpp          the Activity view (draw_activity; #622): the merged audit-trail+runs feed via observability::activity_entries, time-window + surface filters, entity_link click-through per entry (docs/gui/activity-feed.md)
  app_traces.cpp            the Traces view (#640, docs/gui/traces.md): the per-operation trace pane over Node::span_recorder() (draw_traces — recent traces, per-span four-capture detail; the GUI form of system.traces / system.spans)
  app_runs.cpp              the Runs / outputs / lineage view (draw_runs + draw_run_detail — an App member so the detail pane's project/graph/deployment/job refs render as entity links, #633)
  app_models.cpp            the Models registry view (draw_models + draw_add_model_form) + render_model_card (shared with the Runs detail via app_operations_internal.hpp)
  app_agents.cpp            the Agents registry view (draw_agents + draw_add_agent_form) — served agent:<id> records via agent::AgentStore (#849)
  app_chat.cpp              the Chat view (draw_chat + ChatOp/poll_chat_op) — talk to an agent off the UI thread via agent::respond (#849)
  app_datasets.cpp          the Datasets view (draw_datasets + the partition-placement overlay draw_dataset_placement; #585)
  app_search.cpp            the Search view (draw_search + draw_search_cluster_strip): the GUI rendering of Node::search — target picker over Search-kind indexes, the provenance strip (path / nodes / sub-indexes / time), the cluster strip replaying the scatter-gather as #590 flows, master-detail ranked hits with the source row's fields + entity_links (dataset, trace), and the per-node provenance table (#641, #643)
  app_jobs.cpp              the Compute-jobs view (draw_jobs + the objective/scaling preview draw_job_scaling_controls)
  app_cluster.cpp           the Cluster view: the spatial node-box canvas (draw_cluster; #582), the animated per-node resource gauges + sparklines (#584) over the telemetry snapshot, the distributed-algorithm-at-work overlay (update_cluster_pipeline + draw_pipeline_activity; #590 — the flow drawing itself is shared via app_operations_internal.hpp) over the live instrumentation read views, the node detail pane, and the consent-gated Add-nodes control (draw_add_nodes_control + run/poll_add_nodes; #583)
  app_cloud.cpp             the Cloud Inspector view (draw_cloud + draw_cloud_inspect_result + the consent-gated run_cloud_inspect — async CLI via CloudInspectOp spawn/poll, #624)
  app_test_suite.cpp        the in-app Test-suite panel (draw_test_suite + start_test; #462) — a thin renderer over the headless zetta::testkit catalog
  repl_console.hpp/.cpp     zetta::gui::ReplConsole — the thin Dear ImGui renderer over repl::ConsoleModel: frontend selector, multiline editor, run/clear, scrollback transcript, result grid (CMP-6; #524). draw_console (in app_explorer.cpp) hosts it over the live node
src/main.cpp                arg parsing + mode dispatch, including pub/sub pool mode/topics-per-thread/minimum/maximum CLI and environment settings (#1157)
schemas/                    generated, drift-checked JSON schemas (the C++ embedded schema is the source of truth), including the M-2 coordinated `entity_resolution_launch` boundary (#1072), roi_readout_definition.schema.json and roi_readout.schema.json (#1108)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
docs/                       domain-organized system docs (see docs/README.md)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  ops/                      operator diagnostics and tuning, including per-subsystem memory accounting (#1163)
  solutions/                runtime solution foundations and complete S-1 through S-12 plus B-1 through B-8 module contracts; policy, holdouts, actions, monitoring, and ROI readouts
    B-1.md                  dynamic pricing data, levels, templates, approvals, correctness refusals, execution, and planted-effect proof (#1175)
    B-2.md                  aggregate-first employee attrition data, levels, templates, approvals, correctness refusals, execution, and planted-effect proof (#1176)
    B-3.md                  ticket triage data, levels, abstention, taxonomy, closure approval, drift controls, execution, and planted-effect proof (#1177)
    B-4.md                  document workflow data, field confidence, review queues, validated handoff, execution, and planted-effect proof (#1178)
    B-5.md                  supplier risk data, evidence, bounded procurement actions, execution, and planted-effect proof (#1179)
    B-6.md                  healthcare operations data, equity, privacy, PHI controls, execution, and planted-effect proof (#1180)
    B-7.md                  credit underwriting data, reasons, validation, fairness, human authority, execution, and planted-effect proof (#1181)
    integration.md          module merge order, shared-code resolutions, scope, and native proof (#1122, #1128)
modules/                    vendored deps as git submodules (capy, corosio)
tests/                      GoogleTest unit tests (CTest); link zetta-core
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  CMakeLists.txt            the test target — every test TU is listed here, grouped by domain
  util/test_private_directory.cpp permissions before writing, failed-publication cleanup and filename validation (#1413)
  cluster/test_cluster_setup.cpp TLS/HELLO setup deadline, shared admission cap, slot recovery, and accept recovery after subprocess-local descriptor exhaustion (#1401)
  transport/test_security.cpp reusable credential-file loading, valid context creation, and malformed/mismatched PEM refusal (#1283)
  memory/README.md          navigation for memory-accounting registry and node-log tests
  memory/test_accounting.cpp probe aggregation, fixture explanation ratio, unattributed accounting, unregister, and periodic log formatting (#1163)
  ml/test_entity_resolution.cpp  M-2 blocking, scoring, conflict-safe clustering, metric, and threshold tests (#1041/#1072)
  ml/test_entity_resolution_partitions.cpp  three-member owned-pair, EM allreduce, bounded-stream, component-merge oracle equivalence, and malformed-launch refusal (#1072)
  ml/test_segmentation.cpp  segmentation recovery, preprocessing, selection, cluster equivalence and persisted output acceptance (#1042)
  ml/test_segmentation_methodology.cpp M-3/S-9/intent shape and graph-gate refusal tests (#1042)
  ml/test_segmentation_launch.cpp off-root registry submission over three owned partitions and single-node metric equivalence (#1042)
  ml/test_model_bounds.cpp  crafted models refused at every boundary (splits past the feature list, backward children, caller-shortened XGBoost names); predict and TreeSHAP stay in bounds (#1390)
  solutions/test_experiment.cpp deterministic balance and restart stability, protected actions, attributed exemptions, malformed boundaries, and concurrent peer convergence (#1105)
  temp_dir.hpp              per-test isolated data_dir helper
  cluster/test_refused_start_exit.cpp  process test: an occupied --listen port exits 1 without an abort (#1078)
  cluster/test_startup_log_redaction.cpp  process test: no startup line carries the join token (flag, env or generated) (#1393)
  cluster/test_gossip_bandwidth.cpp  steady-state gossip rounds over mutual TLS ship deltas, plus the ZETTA_GOSSIP_BENCH=1 bytes-per-round and convergence measurement (#794)
  cluster/test_peer_gateway.cpp  two dial-out-only Zetta nodes with overlapping private endpoints replicate through two opaque TCP gateway fixtures while retaining end-to-end mutual TLS (#1282)
  api/journey.hpp           the shared MCP-journey driver (#619): Journey/JourneyStep{tool, input, expect} DATA tables + saved-value flow + patient Poll, run by one transport-agnostic driver (layer 1 in-process / layer 2 JSON-RPC); failures name the offending step + assertion
  api/journeys_canonical.hpp  the canonical journey tables (#619): UC-1 happy path (+ near-data trace read-back), gate-blocked deploy, dataset ingest + placement inspection — shared verbatim by test_action_journeys.cpp (layer 1) and test_mcp_server.cpp (layer 2); and the #969 chat loop (propose → approve → run → report → explain, plus its refusals), replayed at both layers by test_chat_loop.cpp
  api/test_chat_loop.cpp     the #969 loop end to end: chat_loop_happy_path / chat_loop_refusals at layer 1 and over MCP, the chat legs with a scripted model (byte-level tokenizer + installed inference runner), POST /v1/chat == MCP (#975), chat_sandbox live tools, RunProposalStore
  api/journeys_canonical.hpp  the three canonical journey tables (#619): UC-1 happy path (+ near-data trace read-back), gate-blocked deploy, dataset ingest + placement inspection — shared verbatim by test_action_journeys.cpp (layer 1) and test_mcp_server.cpp (layer 2)
  api/test_analysis_access.cpp  analyze_dataset / plan_features read as the caller: hidden columns omitted, an unreadable table answers as missing and is never joined, row security applies to the sample (#1300, #1396)
  api/test_study_driver.cpp  server-level real Study journeys: dataset and Pipeline trials, published-space round trips, validation/create agreement, restart recovery, stop propagation, budgets, curves, analysis and actual-model promotion (#1205/#1207)
  api/test_web_ui_studio.cpp  the imported Studio design contract over the pure web renderers (#874, docs/product/experience/design-sync.md): tokens (IBM Plex / Blue 60 / paired dark), the five-group shell, the vocabulary contract, icon semantics
  api/test_web_blob_authz.cpp  the web UI's cookie blob routes check the session user's sql:write / blob:read grants and audit every decision (#1389)
  data/test_featurize_plan.cpp semantic inference, entity/event/time-series/snapshot grain, key-overlap joins, leakage detection, methodology-specific plans, and required dataset corrections (#1062)
  solutions/test_snapshot.cpp snapshot cutoff/idempotence/replication, as-of join future-row exclusion, and boundary refusal tests (#1103)
  ml/test_anomaly.cpp       tabular/KPI anomaly correctness: planted outliers, exact budgets, label-only evaluation, seasonal spike handling, and future-point invariance (#1046)
  ml/test_anomaly_partitions.cpp  real multi-member isolation-forest collectives compared row-for-row with the single-member partition oracle (#1046)
  ml/test_anomaly_table.cpp  resolved label-free table in, `anomaly` Model plus queryable anomaly/report tables and Run lineage out (#1046)
  ml/test_anomaly_methodology.cpp  runnable own shape and hard refusals for labels, budgets, scaling, future fitting, malformed parameters, grouping, and unsupported operations (#1046)
  fixtures/                 committed test fixtures (churn/ golden/ parquet/ churn_workflow.json)
  test_node.cpp / test_integrity.cpp / test_schemas.cpp / test_runtime.cpp
                            cross-domain root tests (Node CRUD, integrity, schema drift-check, runtime interface)
  cluster/test_solution_replication.cpp  one-tick incremental solution projections, scoped removals, late parents, direct CRDT reads, and table-backed state (#1128)
  api/ util/ cluster/ data/ ddf/ ml/ solutions/ compute/ index/ pipeline/ cloud/ governance/ observability/ pubsub/ repl/ testkit/ usecases/churn/
                            one test directory per domain, mirroring include/zetta + src/core (each test file lives with its domain)
  solutions/test_audit_ledger.cpp  action/outcome chain reconstruction, append-only behavior, persistence, CRDT replication, SQL queryability, and schema rejection (#1107)
  governance/audit_proof.cpp native process-isolated million-event replication and restart RSS worker, with opt-in allocation-profile pause (#1164)
  governance/test_audit.cpp  segment reads/retention, malformed ranges, legacy catalog compaction, incompatible-peer refusal and two-process convergence/RSS assertions (#1164)
  governance/test_sandbox_escape.cpp  TC-5 jailbreak attempts by a low-privilege analyst over Registry::call: each refused and audited, no existence oracle, open gaps as KNOWN_DEFECT probes (#252)
  ml/test_forecast.cpp / ml/test_forecast_runner.cpp
                            temporal correctness, FVA, intervals, multiple series, horizon bounds, output lineage, and single-member == multi-member forecasting (#1043)
  ml/test_forecast_methodology.cpp
                            runnable M-4 ownership and negative forecast graph checks; API journey in api/test_action_journeys.cpp (#1043)
  ml/test_forecast_launch.cpp
                            non-root forecast submission, peer launch validation, replicated artifacts and single-member equivalence (#1043)
  ml/test_methodology.cpp   registry order/lookup/graph ownership and generated methodology-table drift check (#1061)
  solutions/test_decision_policy.cpp policy constraints, versioning, replication/restart, A1 approval, kill-switch, table output, capability journey, and request p99 (#1104)
  solutions/test_monitor.cpp shifted-input PSI, guardrail bounds, score/calibration/cohort metrics, pub/sub events, persistence, replication, and malformed boundaries (#1106)
  ml/test_solution.cpp      20-entry runnable solution catalogue, module domain refusals including B-1 through B-8, portfolio mapping, template schema validation and methodology availability, level selection, and callback composition (#1102/#1110/#1121/#1128/#1175/#1176/#1177/#1178/#1179/#1180/#1181/#1182)
  solutions/test_action_integration.cpp loopback webhook retries/idempotency, dead letters, CRM plugins, replicated outbox, holdout suppression, kill-switch stop, report publication, and ledger evidence (#1109)
  api/test_action_journeys.cpp synthetic solution journeys, including B-1 through B-8 outcomes, through build, run, decision, action, holdout, ledger, outcomes, and ROI readout (#1118, #1175, #1176, #1177, #1178, #1179, #1180, #1181, #1182)
  solutions/test_roi_readout.cpp planted-effect confidence interval, no-holdout refusal, policy approval and override accounting, version selection, ROI arithmetic, guardrails, and schema rejection (#1108)
  ml/test_survival.cpp      survival censor labels, known-hazard ranking, monotone curves, determinism, and multi-member partition equivalence (#1044)
  ml/test_uplift.cpp        planted-effect, binary-role, positivity, and deterministic-bootstrap tests (#1045)
  ml/test_uplift_partitions.cpp  owned-partition distributed uplift versus single-node oracle (#1045)
  ml/test_next_best_action_methodology.cpp  M-8 ownership and graph-check refusals for unsafe evaluation, column, policy and structure changes (#1047)
  ml/test_graph.cpp / ml/test_graph_partitions.cpp  graph reference values, cutoff and seed rules, partition equivalence, and real three-member allreduce coverage (#1051)
  ml/test_document_extraction.cpp generated PDF, deterministic tiny TrOCR, review routing, resolved-table outputs, and opt-in real-checkpoint coverage (#1050)
  ml/test_language.cpp      tiny native BERT determinism/normalization, retrieval/RAG rules, distributed classifier statistics, runner outputs, and skipped-by-default real-checkpoint coverage (#1049)
  ml/test_speech.cpp        WAV/resample, log-mel reference, random Whisper determinism, privacy filtering, WER, and checkpoint test contract (#1053)
  ml/test_speech_train_partitions.cpp  multi-member allreduce equivalence for owned-partition speech statistics (#1053)
  ml/test_bandit.cpp        M-15 convergence, normalized/capped propensity, restart-state, contextual-policy, and multi-member allreduce tests (#1054)
  ml/test_optimization.cpp  revised-simplex LP, branch-and-bound MIP, table-contract, and independent-feasibility correctness tests (#1055)
  ml/test_bayesian.cpp      conformal coverage/disjointness, hierarchical shrinkage, and two-member posterior equivalence (#1056)
  ml/test_ensemble.cpp      planted lift, in-sample refusal, determinism, and time-ordered-fold correctness for M-18 (#1057)
  ml/test_ensemble_train_partitions.cpp  two-member owned-partition convergence for the M-18 ComputeJob adapter (#1057)
  ml/test_synthetic_private.cpp  M-20 copula fidelity, no-copy privacy evaluation, budget refusal, accounting, and DP utility tests (#1059)
  ml/test_synthetic_private_partitions.cpp  M-20 multi-member owned-partition statistics/model equivalence and near-data instrumentation test (#1059)
  ml/test_streaming.cpp     M-21 convergence, event/label ordering, prequential metrics, persistence, pub/sub readback, score table, and lineage (#1060)
  ml/test_streaming_train_partitions.cpp  multi-member gradient allreduce and identical-model proof for owned event partitions (#1060)
build/generated/            embedded font header (generated; git-ignored)
```

`src/gui/app.hpp` forward-declares SDL/ImGui types so only `app.cpp` pulls in
those heavy headers — keep it that way.

## On-disk layout

```
data_dir/
  node_id                    ← this node's stable UUID (cluster id + actor id)
  replication.automerge      ← the replicated CRDT document (source of truth)
  <project-uuid>/            ← one folder per project (UUIDv4 = its id)
    project.json             ← { id, name, created, updated, owner }
    inputs/
      <input-uuid>/          ← one folder per project input (UUIDv4 = its id)
        input.json           ← { id, project, kind, source, filename,
                                 content_hash, size, created_by, created_at }
        payload              ← raw bytes (NOT in the CRDT; on-disk like an LO body)
    graphs/                  ← this project's execution graphs
      <graph-uuid>.json      ← { id, project, name, graph: { nodes, edges } }
  <deployment-uuid>/         ← one folder per deployment (UUIDv4 = its id)
    deployment.json          ← { id, graph_name, target, node, node_count, … }
  <run-uuid>/                ← one folder per run (UUIDv4 = its id)
    run.json                 ← { id, project, graph, status, created, updated,
                                  logs, outputs, lineage }
  <model-uuid>/              ← one folder per model (UUIDv4 = its id)
    model.json               ← { id, name, kind, project, run, feature_schema,
                                  metrics, created, weights:{ size, sha256 } }
    weights                  ← raw serialized bytes (NOT in the CRDT; on-disk
                                 like an LO body — transfers peer-to-peer)
  experiments/
    <experiment-uuid>/       ← one folder per experiment
      experiment.json        ← { id, name, seed, allocations, created }
      assignments/
        <entity-digest>.json ← stable allocation + propensity + exemption log
  <index-uuid>/              ← one folder per index (UUIDv4 = its id)
    index.json               ← { id, table, kind, backend, fields, created }
                                 (metadata only — no body)
  <job-uuid>/                ← one folder per compute job (UUIDv4 = its id)
    job.json                 ← { id, kind, status, project, graph, run,
                                  scaling_policy, created, updated, detail }
  <part-uuid>/               ← one folder per dataset partition (UUIDv4 = its id)
    partition.json           ← { id, table, blob_ref, row_range:{start,end},
                                  stats, owner_nodes, created }
  <principal-uuid>/          ← one folder per principal (UUIDv4 = its id)
    principal.json           ← { id, kind, name, created }
    keys/
      <key-uuid>/            ← one folder per access key (UUIDv4 = its id)
        key.json             ← { id, principal, scope:{ unrestricted, grants },
                                 capabilities:[ { domain, action, expires_at? } ],
                                 created }
  audit/                     ← the immutable-record audit projection
    segment-<number>.audit.gz ← checksummed compressed event frames; legacy
                                 <audit-uuid>.json files migrate on startup
    index.bin                ← checksummed fingerprint + origin/sequence seek records
    watermarks.bin           ← durable per-origin high and retention low sequences
  <db-uuid>/                 ← one folder per database (UUIDv4 = its id)
    database.json            ← { id, name, replication? }  (replication = the
                                ReplicationStrategy, ENT-35; omitted when default)
    <table-uuid>/            ← one folder per table (UUIDv4 = its id)
      table.json             ← { id, name, replication? }  (replication strategy,
                                omitted when the default ReplicateToAll)
      000000.jsonl           ← rows 0..9999    ({ "_zid": row-uuid, "doc": row })
      000001.jsonl           ← rows 10000..19999
      ...                    ← JsonlStore::kRecordsPerChunk = 10k per chunk
```

The folder name *is* the id; the human name lives only in `database.json` /
`table.json` (so rename rewrites that file, never moves the folder). Each JSONL
line is `{ "_zid": <row-uuid>, "doc": <row> }` — `_zid` is the row's CRDT key
(same id on every node); `doc` is what `SELECT` returns. (Legacy bare-row lines
are still read, with a fresh id assigned.) When replicated, the folder tree is a
projection of `replication.automerge`; standalone nodes read/write it directly.
`add_row` appends a line to the active chunk immediately (append-open per row —
simple and crash-resilient; can hold the stream open later). `data_dir` defaults
to `./zetta-data` and is git-ignored.
    app_reports.cpp         zetta::gui — the Reports view: runs that emitted a report, and one run's report drawn from get_run_report (metrics, weight bars, at-risk + evidence, caveats) (#960 item 4)
