# Code map

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
**imgui** library → **zetta-compute** (the accelerator seam; optional CUDA
backend behind `-DZETTA_ENABLE_CUDA=ON`) → **zetta-core** (the static core
library — **no SDL/ImGui dependency**, the build-level guarantee that a node
runs headless) → the **zetta** executable (links SDL3 + ImGui on top of
zetta-core). The GoogleTest unit tests build behind `ZETTA_BUILD_TESTS`.

## Code layout

```
CMakeLists.txt              build graph: imgui lib, zetta-compute lib (+ optional CUDA), zetta-core lib, zetta exe
cmake/embed_font.cmake      bakes a .ttf into a C++ header at configure time
assets/fonts/               embedded Geist + Geist Mono TTFs (SIL OFL 1.1) + README
scripts/install-deps.sh     install system build tools (macOS / Ubuntu)
scripts/gen-parquet-fixture.py  dev-time pyarrow generator for the committed Parquet fixtures (tests/fixtures/parquet/ + the embedded src/core/testkit/parquet_fixture_data.hpp)
include/zetta/              zetta-core PUBLIC API (no SDL/ImGui here)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  version.hpp               kVersion (keep in sync with project() VERSION)
  node.hpp                  zetta::Node — the node; CRUD over databases + projects + deployments + runs + models + indexes + jobs + dataset partitions + principals; the per-node IndexRegistry (index_registry(), #528); the per-node checkpoint store (checkpoint_store()/checkpoints(), <data_dir>/checkpoints, RULE-5/#468); the per-node telemetry sampler (sample_node_metrics(), #581) + the cluster-wide telemetry snapshot (telemetry_snapshot(), #584); the live near-data pipeline instrumentation registry (record_pipeline_job()/pipeline_jobs(), the bounded source for system.pipeline_jobs, #589); the node's span ring (span_recorder(), the ONE source for system.traces/system.spans + list_traces/get_trace, #640); the computed-at-query-time dataset summaries (datasets(), the source for system.datasets, #585) + the detected-provisioner cache (record_provisioner_config()/provisioner_configs(), ephemeral, the source for system.provisioners); run_graph; the audit sink; cluster_members() (the replicated node:<id> membership, #519); the long-lived compute plane over the real cluster — attach_cluster() + job_coordinator() (#579)
  runtime.hpp               zetta::runtime — async/coroutine seam over Capy (task/block_on/thread_pool + parallel_for data-parallel fan-out)
  runtime/net.hpp           networking slice of the seam (Corosio; adapter-only) incl. the TLS surface (tls_context/openssl_stream/any_stream, #466)
  headless.hpp              zetta::run_headless(Node&, Cluster&)
  signals.hpp               install_signal_handlers / stop_requested
  api/                      the programmatic control surface (docs/api/): registry.hpp (Capability/ApiResult/ApiContext/Registry — one entry renders as MCP tool + REST route + palette action; every call a span, authorized + audited), bridge.hpp (MainThreadBridge — coroutine-first marshaling onto the owning loop via runtime::async_event), capabilities.hpp (register_core_capabilities: server_info · sql · list_projects · list_traces · get_trace · submit_feedback · create_project), server.hpp (ApiServer — the embedded Beast2 MCP endpoint behind a pimpl seam; POST /mcp, bearer auth, drain())
  observability/            trace.hpp — OTel-style TraceContext/Span/SpanRecorder/ScopedSpan + traceparent codec; the four captures (intent · plan · performance · result_shape) on a bounded per-node ring (docs/architecture/observability.md); activity.hpp — ActivityEntry/ActivitySurface/ActivityOutcome + activity_entries(Node) — the merged audit-trail+runs activity feed, computed at call time (#622, docs/gui/activity-feed.md); crash.hpp — crash reports (#640): the fatal-signal handler (install_crash_handler), the validated <data_dir>/crashes/<uuid>.json report (CrashReport + schema), load_crash_reports (the system.crashes source)
  cluster/                  the distributed plane: membership, the signal plane, collectives, CRDT replication, telemetry
    cluster.hpp             zetta::cluster::Cluster — equal-peer membership over TCP (mutual TLS when ClusterConfig::security is enabled) + the signal-plane API
    security.hpp            zetta::cluster::ClusterSecurity + generate/valid_join_token/derive_cluster_security/load_or_create_cluster_security/cluster_secret_schema — inter-node transport security (CTL-2/TB-3, #466): the join token derives the cluster CA (deterministic Ed25519), each node mints a CA-signed identity, persisted under <data_dir>/security/ with a schema-validated secret file (docs/cluster/security.md)
    signal.hpp              zetta::cluster::Signal/SignalHandler — ephemeral RPC/event/task signals + envelope encode/decode (ADR-28)
    collectives.hpp         zetta::cluster::Collectives — allreduce/broadcast/barrier/gather over the signal plane (#365); wire_bytes() + rounds_completed() live meters feed system.collective_rounds (#589)
    replication.hpp         zetta::cluster::ReplicationManager — Automerge CRDT state
    node_metrics.hpp        zetta::cluster::NodeMetrics/NodeMetricsSampler — live per-node host resource telemetry (CPU/memory/GPU+VRAM/storage/network), sampled from OS counters; ephemeral/observed, the source for system.node_metrics; graceful degradation to null when a counter is absent (#575 area 2, #581)
    node_telemetry.hpp      zetta::cluster::NodeTelemetryExchange + encode_node_metrics/decode_node_metrics/node_metrics_wire_schema — cluster-wide live telemetry over the signal plane (#584): each node broadcasts its own NodeMetrics as an ephemeral `node.metrics` event, a SignalHandler (the JobCoordinator's fallback) caches peers' by sender id; the schema-validated wire boundary; feeds the animated node-box gauges via Node::telemetry_snapshot(); not replicated
  data/                     the data model: databases, tables, partitioning + placement, large objects, datasets, imports
    database.hpp            zetta::data::Database — owns tables (disk-backed)
    table.hpp               zetta::data::Table — owns a storage engine (its rows); configurable per-table with a Partitioner
    partitioner.hpp         zetta::data::Partitioner/SinglePartitioner — the partitioning seam: record→partition + partition→node(s) over a fixed P>>nodes, pluggable per table (ADR-31/ENT-37; #512); + the record→partition strategies HashPartitioner (FNV-1a hash(key columns)%P) / RoundRobinPartitioner (surrogate%P for even balance) + the partitioner_from_json/to_json config boundary (#513)
    rendezvous_placement.hpp  zetta::data::RendezvousPlacement/rendezvous_owners — rendezvous (highest-random-weight) partition→node placement: a membership change moves only ~1/N of partitions, deterministic + leaderless (ADR-31/ENT-37; #514)
    partition_writer.hpp    zetta::data::write_partitioned/PartitionWriteResult — the partitioned write path: assign each row to a partition (Partitioner), serialize each non-empty partition to a JSON Lines blob, store it as a LargeObject (import_bytes), record a DatasetPartition catalog entry per partition (blob ref + cumulative row range + place_partition owners); N blobs + N catalog rows (ADR-31/ENT-37; #511)
    partition_rebalance.hpp  zetta::data::rebalance_partitions/RebalanceResult — between-jobs rebalance on a membership change: recompute rendezvous placement for the sharded partitions preserving rf=owner count, update changed owner_nodes (set_owner_nodes captures → bodies move via #515); skip replicate-to-all; ≈rf/N move on a join. Two triggers: directly from add_nodes(Node&, …) (#519) and auto-triggered in Node::tick off node:<id> membership keys (ADR-31/ENT-37; #517, TC-10/#500)
    dataset_partition.hpp   zetta::data::DatasetPartition — a partition catalog entry: where a table's data slice lives (table/blob_ref/row_range/stats/owner_nodes) + JSON Schema; replicate-to-all; set_owner_nodes is the capturing re-placement mutator the rebalance uses (ENT-36/TBL-13; #517)
    dataset.hpp             zetta::data::DatasetSummary/DatasetPlacement — a read-only, computed-at-query-time summary of one dataset (name/format/replication/rows/size/columns + per-partition placement); one dataset == one table, no stored Dataset entity — recomputed from the live Node tree joined with the partition catalog so it never drifts; the engine source behind system.datasets and the GUI dataset views (#585, #575 area 3; docs/data/system-datasets.md)
    large_object.hpp        zetta::data::LargeObjectStore — files as blobs (per database); import_file (from a path) + import_bytes (from in-memory bytes, used by the partitioned write path)
    import.hpp              zetta::data::import_records — JSON/JSONL files → table rows
    add_dataset.hpp         zetta::data::add_dataset_file — "Add dataset" affordance backend: load a parquet/jsonl/json/csv file as a partitioned table (RendezvousPlacement + write_partitioned), so it surfaces in system.datasets with partitions placed across the cluster; CSV type-inferred, no new JSON boundary (#587)
    replication_strategy.hpp  zetta::data::ReplicationStrategy — per-table/db replication strategy (ReplicateToAll | Partitioned | PartitionedWithReplicas(factor)) + JSON boundary; recorded/persisted/replicated, default ReplicateToAll (ENT-35; #509)
    blob_store.hpp          zetta::data::BlobStore — large-object body transfer seam
    replicated_blob_store.hpp  zetta::data::ReplicatedBlobStore — BlobStore from CRDT + disk; partition-aware placement: a partition body is fetched/held only by its owner node(s) per the catalog + replication factor, not replicate-to-all (#515)
  storage/
    engine.hpp              zetta::storage::StorageEngine interface
    json_store.hpp          zetta::storage::JsonStore — in-memory vector<json> (header-only)
    jsonl_store.hpp         zetta::storage::JsonlStore — JSONL on disk, 10k/chunk
  query/                    zetta::query — the SQL layer: dialect frontends over a dialect-neutral AST (docs/data/query.md)
    ast.hpp                 the generic, dialect-neutral query AST (CreateDatabase/DropDatabase/CreateTable/DropTable/Insert/Select statements + TableRef/WhereClause/AggregateExpr) every frontend parses into and the executor runs — support a new dialect by writing a frontend, never by touching execution
    dialect.hpp             zetta::query::Dialect/ParseResult — the frontend interface: dialect text → generic statements
    postgres.hpp            zetta::query::PostgresDialect — the PostgreSQL-flavored frontend: a basic subset (CREATE/DROP DATABASE|TABLE, INSERT … VALUES, SELECT … [WHERE col = value]); case-insensitive keywords, `--` comments, `;`-separated statements
    executor.hpp            zetta::query::Executor/QueryResult — run generic statements against a Node (dialect-neutral, only ever sees the AST), current_database for unqualified table names; hosts the read-only system.* observability tables (nodes/datasets/dataset_partitions/node_metrics/pipeline_jobs/collective_rounds/spans/traces/crashes/provisioners/activity/…)
    service.hpp             zetta::query::QueryService/Outcome — the facade the UI (and tests) call: parse with the Postgres dialect + execute every statement; pass a Cluster to make the nodes system table queryable; swapping/adding a dialect is a one-line change here
  parquet/                  zetta::parquet — native (no Arrow/parquet-cpp) Parquet reader (ADR-32; #481)
    snappy.hpp              zetta::parquet::snappy_decompress — Snappy raw-block page decoder (Parquet's default codec), non-throwing + bounds-checked (#507)
    metadata.hpp            zetta::parquet::parse_parquet_metadata — read a file's Thrift FileMetaData (schema + row-group/column-chunk layout, incl. per-leaf max def/rep levels); Thrift compact decoder, non-throwing, JSON projection + schema boundary (#505)
    column_decoder.hpp      zetta::parquet::decode_column/decode_column_chunk — read a column's data pages into typed values + a validity bitmap: PLAIN (int/float/bool/byte_array) + dictionary (RLE/bit-packed indices) + definition levels; uncompressed + snappy codecs (zstd/gzip rejected); non-throwing, JSON projection + schema boundary (#506)
    reader.hpp              zetta::parquet::read_parquet_rows — assemble metadata+decoders into a whole-file reader: decode every leaf column → one JSON object per row (the inverse of the JsonlStore row shape); the untyped-rows reader the churn loader uses (#508)
  ir/
    step_kind.hpp           zetta::ir::StepKind — connect→…→act taxonomy (header-only)
    execution_graph.hpp     zetta::ir::ExecutionGraph — typed step nodes + edges
    serialization.hpp       zetta::ir::to_json/from_json — the graph's JSON form
  input/
    language_frontend.hpp   zetta::input::LanguageFrontend/FrontendResult — interface: lower a source language to the ExecutionGraph IR (the IR-targeting analogue of query::Dialect); non-throwing (ENT-38/ADR-33; #520)
    python_frontend.hpp     zetta::input::PythonFrontend/python_step_kind — simulated-Python frontend: parse a Python subset (assignments/calls/attribute+method chaining/literals) → IR (calls→typed steps via the engine-faithful verb table — train→model, score/predict/rank→index, explain→serve, add_nodes→act; refs/receivers→data-flow edges); not a Python runtime (#520/#522)
    cluster_bindings.hpp    zetta::input::run_add_nodes_step/add_nodes_count — execute a lowered sim-Python cluster.add_nodes IR node against the permanent-add control action zetta::cloud::add_nodes (provider injected, cloud consent-gated) (#522)
    datasets_bindings.hpp   zetta::input::run_load_dataset_step/DatasetStepExecutor/dataset_load_spec/DatasetResolver — the `datasets` library shim: execute a lowered sim-Python load_dataset("<path>", columns=…) IR node by decoding the parquet via the native reader (read_parquet_rows) into a (partitioned) table; file/network IO behind the injectable DatasetResolver seam (offline default = local path); HF datasets is the fidelity reference only (#521)
  pipeline/                 AI projects: inputs → compiled execution graphs → correctness gate → deploy → runs (+ the solution catalog)
    project.hpp             zetta::pipeline::Project — an AI project; owns inputs + execution graphs
    project_input.hpp       zetta::pipeline::ProjectInput — raw intent/artifact input + provenance
    project_graph.hpp       zetta::pipeline::ProjectGraph — a named ExecutionGraph under a project
    intent_compiler.hpp     zetta::pipeline::compile_intent — NL intent → templated ExecutionGraph skeleton (FEAT-2)
    n8n_import.hpp          zetta::pipeline::import_n8n_workflow — n8n workflow JSON → artifact ProjectInput (schema-validated)
    n8n_compile.hpp         zetta::pipeline::compile_n8n_workflow — n8n workflow JSON → ExecutionGraph IR (nodes→steps, connections→edges; #308)
    graph_plan.hpp          zetta::pipeline::project_cluster_plan/ClusterPlan/StepPlan/StepPlacement — project how an ExecutionGraph would run over the current cluster (near-data / fan-out / all-nodes / coordinator per step, near-data locality propagated forward along DataFlow edges); pure + deterministic, the engine of the Graph Workspace plan view (#591, #575 area 6); no new JSON boundary
    executor.hpp            zetta::pipeline::GraphExecutor / StepExecutor — orchestrate a graph into a Run
    run_progress.hpp        zetta::pipeline::project_run_progress/RunProgress/StepRunProgress/StepRunStatus — project a Run's logs+outputs onto an ExecutionGraph (per-step pending/running/succeeded/failed + failing reason + attributed outputs); pure + per-frame cheap, the engine of the Graph Workspace live-run overlay (#626); no new JSON boundary
    validation.hpp          zetta::pipeline::ValidationReport — correctness-gate result (ENT-5/FEAT-3)
    gate.hpp                zetta::pipeline::CorrectnessGate — gate runner: checks → report (RULE-1)
    graph_checks.hpp        zetta::pipeline::SchemaTypeCheck + DataLeakageCheck — the #309 concrete checks
    run.hpp                 zetta::pipeline::Run — one graph execution (status/logs/outputs/lineage) + Run JSON Schema
    deployment_target.hpp   zetta::pipeline::DeploymentTarget — a placement (ENT-6; R1 local)
    deployment.hpp          zetta::pipeline::Deployment — a graph placed onto a target
    deployment_controller.hpp  zetta::pipeline::DeploymentController — reconcile local deployments desired→actual + self-heal (FEAT-4/TC-3)
    solution_catalog.hpp    zetta::pipeline::SolutionCatalog — AI Solution Library (deployable blueprints) + schema-validated parse
  compute/                  zetta::compute — the accelerator seam (ADR-27/ARC-9; no SDL/CUDA in headers)
    data_type.hpp           zetta::compute::DataType — element-type tag + byte_size/data_type_of<T> (header-only)
    compute_status.hpp      zetta::compute::ComputeStatus — the no-throw call result (header-only)
    device_buffer.hpp       zetta::compute::DeviceBuffer — typed, owning, move-only device memory
    compute_device.hpp      zetta::compute::ComputeDevice — the seam: info + memory + the operator set
    host_stub_device.hpp    zetta::compute::HostStubDevice — host-memory transfer stub (ops unsupported)
    cpu_device.hpp          zetta::compute::CpuDevice — the always-built reference backend (every op on host memory, multi-threaded; #360)
    cuda_device.hpp         zetta::compute::CudaDevice — the GPU backend (CUDA-free header; guarded by ZETTA_WITH_CUDA, #363)
    device_registry.hpp     zetta::compute::DeviceRegistry — per-node enumerate/report/select devices (ARC-9; #362)
    work_stealing_scheduler.hpp  zetta::compute::WorkStealingScheduler — local per-worker-deque work-stealing task pool (ARC-10/#366)
    distributed_scheduler.hpp  zetta::compute::DistributedScheduler — cross-node steal over the signal plane, elastic mid-job membership (ARC-10/#366/#367)
    job_coordinator.hpp     zetta::compute::JobCoordinator/JobRound — split a ComputeJob into tasks on the scheduler, drive collectives at round boundaries, drive job lifecycle (ARC-11/#372); a running Node owns one over its real Cluster via Node::attach_cluster/job_coordinator (the SPMD spine, #579)
    checkpoint.hpp          zetta::compute::JobCheckpoint/CheckpointStore/FileCheckpointStore/run_resumable_rounds — burst-job eviction tolerance: durable sha256-fingerprinted progress snapshot (validated boundary) + a durable store seam + a resume-from-last-checkpoint driver (graceful spot-drain), so an evicted job resumes without recomputing committed rounds (RULE-5/RSK-9; #468)
    compute_job.hpp         zetta::compute::ComputeJob — one unit of accelerated compute (kind/lifecycle) + ComputeJob JSON Schema (ARC-11/ENT-31)
    scaling_policy.hpp      zetta::compute::ScalingPolicy — per-job autoscaler reward objective (cost/runtime/data-shape) + bounds + evaluate() + JSON boundary (ADR-29/ENT-34; #370)
    job_autoscaler.hpp      zetta::compute::JobAutoscaler/AutoscalerConfig — autoscaler control loop: observe a running ComputeJob, score its ScalingPolicy, add/remove nodes via the Provisioner within bounds, hysteresis + cost-cap shed, elastic membership, best-effort on given hardware (ADR-29/ARC-11; #371)
    pipeline_instrumentation.hpp  zetta::compute::PipelineJobInstrumentation/NearDataJobKind — the per-job near-data instrumentation record (kind / partitions+rows read / local_bytes_read vs wire_bytes_moved / status) the near-data runners record into the Node; ephemeral/observed, the source for system.pipeline_jobs; no new JSON boundary (#589, #575 area 5)
  ddf/
    data_frame.hpp          zetta::ddf::DataFrame/Column/DataFrameSchema — typed columnar frame over a table, chunked, ComputeDevice-backed + loader/infer/JSON boundary (ARC-12/ENT-33; #380)
    parquet_reader.hpp      zetta::ddf::load_parquet_data_frame/infer_parquet_schema — native Parquet → DataFrame reader with column projection: decode only the requested leaf columns into DeviceBuffers (drop embedding_0..3071), numeric/dense boundary + parquet_data_frame schema for the golden (ADR-32; #508)
    ops.hpp                 zetta::ddf::scan/filter/project — DDF ops over a DataFrame, built on the ComputeDevice seam (gather-based filter, map/elementwise computed columns) + FilterPredicate/Projection JSON boundaries (ARC-12; #381)
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
    model.hpp               zetta::ml::Model — a trained model (metadata + weights fingerprint) + Model JSON Schema (ENT-14/FEAT-6)
    gbdt.hpp                zetta::ml — histogram GBDT (binary logistic): GbdtParams/GbdtModel/RegressionTree (+ JSON boundaries), evaluate_binary (AUC/log-loss), train_gbdt (host-side reference, gradient/Hessian histograms allreduced via an injected AllReduce — single-node == multi-node) (FEAT-6/ENT-14; #390)
    gbdt_train_job.hpp      zetta::ml::run_train_job — run distributed GBDT training as a recorded `train` ComputeJob through the job coordinator and produce a Model (weights body + metrics + ranked drivers, registered on the root); opt-in held-out probability calibration via CalibrationConfig; opt-in per-round checkpoint/restart via TrainCheckpointConfig (resume the partial model from the last checkpoint after a spot eviction, cluster-agreed resume round; RULE-5/#468) (FEAT-6/ENT-14; #390/#391)
    gbdt_score.hpp          zetta::ml — batch scoring / rank-at-risk: score_gbdt/predict_risk (calibrated risk = predict_proba, ranked desc) + load_gbdt_model (weights body read back through the GbdtModel JSON Schema boundary) (FEAT-6/CMD-10; #392)
    gbdt_score_job.hpp      zetta::ml::run_score_job — run distributed batch scoring as a recorded `score` ComputeJob through the job coordinator: SPMD per-shard predict, gather + device sort_by_key rank + broadcast → identical global at-risk list on every node (FEAT-6/CMD-10; #392)
    local_train_job.hpp     zetta::ml::run_near_data_train_job/NearDataTrainResult — compute-near-data GBDT train over a partitioned feature table (the train sibling of run_near_data_featurize_job): load this node's owned partitions once → train (+calibrate, registers a Model with AUC/log-loss/accuracy + drivers) → explain (per-row TreeSHAP drivers) → rank (at-risk list), each a recorded ComputeJob; SPMD, near-data instrumented, no new ML math (FEAT-6/FEAT-8; #533)
    calibration.hpp         zetta::ml — probability calibration: Calibrator (Platt sigmoid(a·raw+b) / isotonic PAVA step) + fit_calibrator + Brier/ECE metrics + JSON/wire boundaries; attaches to a GbdtModel (predict_proba applies it) via calibrate_model (FEAT-6; #391)
    tree_shap.hpp           zetta::ml — exact path-dependent TreeSHAP over a GbdtModel: tree_shap (per-feature φ + cover-weighted base_value, raw/margin space — efficiency base+Σφ=predict_raw), dominant_driver; pure, uses TreeNode::cover, no new JSON boundary (FEAT-6; #393)
    gbdt_explain_job.hpp    zetta::ml::run_explain_job — run distributed TreeSHAP as a recorded `explain` ComputeJob: per-entity SHAP over a shard (parallel_for) + an allreduced global mean-|φ| driver ranking; single-node == multi-node (FEAT-6; #393)
    golden_oracle.hpp       zetta::ml::GoldenOracle/load_golden_oracle — the committed golden featurize/train/SHAP expectations format (validated boundary), for the native correctness oracle (RSK-8; #395)
    playbook.hpp            zetta::ml — UC-1 retention playbook: generate_playbook (deterministic driver→action→owner via PlaybookPolicy + frozen expected-retained-value priority risk×ARR×save_rate, ranked) behind a constrained PlaybookNarrator seam (default-Claude ConstrainedNarrator over an injected NarratorCompletion, validated at the playbook_entry schema boundary, never invents numbers; RulesEngineNarrator offline fallback) + OutcomeLog (FEAT-6; #397)
  index/
    index_kind.hpp          zetta::index::IndexKind — the search/vector/feature index-type taxonomy (header-only enum + names) (ENT-39/ENT-40)
    index_backend.hpp       zetta::index::IndexBackend/IndexSpec/IndexStatus/IndexPartition/IndexQuery — the build(over partitions)/query/drop seam (ADR-34; no-throw status)
    index_registry.hpp      zetta::index::IndexRegistry + index_spec_from_node — register backends, resolve an IndexSpec / IR `index` step → a backend (like compute::DeviceRegistry); the Node owns one (Node::index_registry(), #528)
    index_step_executor.hpp  zetta::index::IndexStepExecutor + read_index_partitions — the StepExecutor that runs a deployed IR `index` step: resolve backend → read table rows into partitions → build a real index → record the Index entity → emit an `index` output (queryable via the node registry); non-index kinds delegate to DefaultStepExecutor (#528)
    in_memory_index_backend.hpp  zetta::index::InMemoryIndexBackend — the from-scratch in-memory Search reference backend (inverted field→row-ids over partitions; name "memory_search")
    oramacore_fulltext_index.hpp  zetta::index::OramaCoreFulltextIndex — the first real backend: a full-text (BM25F) Search backend over the native oramacore-cpp engine (name "oramacore_fulltext"); pImpl over the engine, ZETTA_WITH_ORAMACORE-guarded so a checkout without the private submodule still builds (#527)
    distributed_fulltext.hpp  zetta::index::DistributedFulltextIndex/merge_fulltext_hits/FulltextHit — per-partition distributed full-text: build a sub-index per LOCAL owned partition (compute-near-data) into an owned IndexBackend, answer a query by scatter-gather (the fulltext.query signal-plane RPC, schema-validated) + merge/rank across nodes → a global ranking matching the consolidated-index oracle; the SignalHandler chained over a delegate (DistributedSorter pattern) (#529, TC-13 #503)
    index.hpp               zetta::index::Index/IndexValue — the top-level replicated index entity {table,kind,backend,fields} + Index JSON Schema (ENT-39/ENT-40)
  cloud/                    provisioners, cloud burst, the cloud inspector — CLI control planes behind CommandRunner, no cloud SDK
    node_capability.hpp     zetta::cloud::NodeCapability/Gpu/GpuKind — node compute capability model (cores/GPU/memory) + satisfies (ENT-32)
    provisioner.hpp         zetta::cloud::Provisioner/ProvisionedNode — provision N nodes against a capability spec / teardown / list (ARC-13; #373)
    add_nodes.hpp           zetta::cloud::add_nodes/AddNodesRequest/AddNodesResult — permanent-add control entry point: drive a Provisioner to bring up N nodes that auto-join + persist as members; provision-only (never teardown/revoke), the autoscaler's permanent sibling; validated AddNodesRequest JSON boundary (#518). The add_nodes(Node&, …) overloads also trigger the between-jobs partition rebalance onto the new members (#519)
    burst_provisioner.hpp   zetta::cloud::BurstProvisioner<Config, ControlPlane> — the shared cloud-burst orchestration template (provision+mint / teardown+revoke-on-drain / provisioned); every cloud backend subclasses it so the orchestration lives once (#464)
    local_provisioner.hpp   zetta::cloud::LocalProvisioner — given-hardware Provisioner backend: uses existing cluster nodes best-effort, teardown is a no-op (ARC-13; #374)
    azure_provisioner.hpp   zetta::cloud::AzureProvisioner/AzureConfig/AzureControlPlane — reference cloud-burst backend: burst N Azure nodes + mint identities + teardown/revoke, behind an injectable control-plane seam, no cloud SDK (ARC-13/ADR-20; #376)
    azure_cli_control_plane.hpp  zetta::cloud::AzCliControlPlane — reference AzureControlPlane binding: shells out to the `az` CLI (VMSS/Spot shape) via the shared CommandRunner (#376)
    command_runner.hpp      zetta::cloud::CommandRunner/CommandResult/default_command_runner — shared, vendor-neutral exec seam every CLI cloud binding runs through (argv, no shell; fork/execvp; #376)
    aws_provisioner.hpp     zetta::cloud::AwsProvisioner/AwsConfig/AwsControlPlane — AWS cloud-burst backend mirroring the Azure pattern: burst N nodes + mint identities + teardown/revoke behind an injectable control-plane seam, no cloud SDK (ARC-13/ADR-20; #377)
    aws_cli_control_plane.hpp  zetta::cloud::AwsCliControlPlane — reference AwsControlPlane binding: shells out to the `aws` CLI (EC2 Auto Scaling Group / Spot shape) via the shared CommandRunner (#377)
    gcp_provisioner.hpp     zetta::cloud::GcpProvisioner/GcpConfig/GcpControlPlane — GCP cloud-burst backend mirroring Azure: burst N + mint identities + teardown/revoke behind the control-plane seam, no cloud SDK (ARC-13/ADR-20; #378)
    gcp_cli_control_plane.hpp  zetta::cloud::GcpCliControlPlane — reference GcpControlPlane binding: shells out to `gcloud` (Managed Instance Group/Spot shape) via the shared CommandRunner (#378)
    oci_provisioner.hpp     zetta::cloud::OciProvisioner/OciConfig/OciControlPlane — OCI cloud-burst backend mirroring Azure: burst N + mint identities + teardown/revoke behind the control-plane seam, no cloud SDK (ARC-13/ADR-20; #432)
    oci_cli_control_plane.hpp  zetta::cloud::OciCliControlPlane — reference OciControlPlane binding: shells out to `oci` (Instance Pool/preemptible shape, OCID-resolved by name) via the shared CommandRunner (#432)
    k8s_provisioner.hpp     zetta::cloud::K8sProvisioner/K8sConfig/K8sControlPlane — Kubernetes cloud-burst backend mirroring Azure: burst N pods into an existing cluster + mint identities + teardown/revoke behind the control-plane seam, no client lib (ARC-13/ADR-20; #434)
    k8s_cli_control_plane.hpp  zetta::cloud::K8sCliControlPlane — reference K8sControlPlane binding: shells out to `kubectl` (Deployment/scale shape) via the shared CommandRunner (#434)
    bare_metal_provisioner.hpp  zetta::cloud::BareMetalProvisioner/BareMetalConfig/BareMetalControlPlane — bare-metal (local-host) cloud-burst backend mirroring Azure: burst N local worker containers + mint identities + teardown/revoke behind the control-plane seam, no container SDK; bursts (vs LocalProvisioner which reports) (ARC-13/ADR-20; #436)
    bare_metal_cli_control_plane.hpp  zetta::cloud::BareMetalCliControlPlane — reference BareMetalControlPlane binding: shells out to docker/podman (local-container shape, launch-the-shortfall) via the shared CommandRunner (#436)
    provisioner_config.hpp  zetta::cloud::ProvisionerConfig — one cloud-burst provisioner's deployment context, detected from the operator's own local CLI; node-local observed state (like a CloudInspectReport — not replicated, not persisted), surfaced as system.provisioners; carries NO credentials, only the non-secret targeting (subscription/project/region/…) held as the provider's validated XConfig JSON so the GUI wiring can rebuild a real provisioner from it (docs/cloud/provisioner-detection.md)
    provisioner_detect.hpp  zetta::cloud::detect_provisioner_configs — detect which cloud-burst provisioners this node can drive by asking the local CLIs (az/aws/gcloud/oci/kubectl/docker) for their active deployment context; fast, read-only, non-interactive commands through the shared CommandRunner seam (testable against a fake, never touches a real cloud in CI); the credential-free sibling of the consent-gated Cloud Inspector
    burst_identity.hpp      zetta::cloud::BurstIdentityManager/BurstIdentity — bulk-mint/revoke short-lived service Principal+AccessKey per burst node, audited (ARC-13/RSK-9; #375)
    burst_approval_gate.hpp  zetta::cloud::BurstApprovalGate/BurstCap — large-burst cost-cap approval gate: a Provisioner decorator, block over-cap bursts pending approval + audit (APR-3/RSK-9; #379)
    cloud_provider.hpp      zetta::cloud::CloudProvider/CloudProviderCatalog — pure-metadata catalog of deployment-target backends for the permission-gated Cloud Inspector (no cloud access); each inspect command is an InspectCommand (display + no-shell argv)
    cloud_inspector.hpp     zetta::cloud::CloudInspector/CloudInspectReport — the real read-only inspect: run a provider's inspect_commands via the shared CommandRunner (behind GUI consent), parse each vendor's JSON at a validated boundary → identity/regions/nodes; no creds read (#445)
  governance/               access control (principals, keys, scopes, capabilities) + the audit trail
    principal.hpp           zetta::governance::Principal — an authenticated actor; owns access keys
    access_key.hpp          zetta::governance::AccessKey — a scoped + capability-bearing login credential + authorize()
    scope.hpp               zetta::governance::Scope/Resource — deny-by-default key-scoped access, the resource axis (RULE-3)
    capability.hpp          zetta::governance::Capability/CapabilitySet/Operation — deny-by-default rights, the verb axis, TTL'd (ENT-22)
    audit_event.hpp         zetta::governance::AuditEvent/AuditSink — immutable audit trail + JSON Schema (ENT-23)
  usecases/churn/           UC-1 churn — the first use case, end to end
    churn_step_executor.hpp  zetta::usecases::churn::ChurnStepExecutor — the real churn StepExecutor: connect→load-to-DDF, clean→featurize, model→train+calibrate (registers a Model), index→build a real entity index (#528) + score/rank, serve→explain (TreeSHAP), act→playbook, each heavy step a ComputeJob; behind the seam, orchestrator untouched (FEAT-5/FEAT-6/UC-1; #396)
    churn_solution.hpp      zetta::usecases::churn::run_churn_solution_on_node — THE one public churn runner (#637): wire the catalog churn blueprint to the real engine over the node's live JobCoordinator (the #579 spine, #580): encode a ChurnDataset → numeric DDF shards, build the standard featurize spec + GBDT params, inject a ChurnStepExecutor, run the graph → real Model + ranked at-risk list + drivers + playbook; + is_churn_graph (connect+clean+model predicate) + demo_churn_dataset (no-files demo) (FEAT-4/UC-1; #398/#580/#637)
    churn_dataset.hpp       zetta::usecases::churn::ChurnDataset / load_churn_dataset — synthetic UC-1 churn fixture loader (FEAT-6 / #402)
    entity_resolution.hpp   zetta::usecases::churn::EntityResolver — resolve raw comms mentions → CRM accounts division-correct (FullName) / coarse Brand trap; emit the entity_resolution annotation the gate verifies (FEAT-6/TC-4; #394)
    entity_resolution_check.hpp  zetta::usecases::churn::EntityResolutionCheck — division-correct resolution (TC-4)
  repl/
    console_model.hpp       zetta::repl::ConsoleModel — the engine-agnostic REPL session (frontend + input + transcript + submit) behind the CMP-6 console; no SDL/ImGui, so it backs the GUI component AND a future CLI (#524)
    governed_console.hpp    zetta::repl::GovernedConsole — the governed run handler behind the SCR-8 Console/REPL screen: capability-scope a statement, lower sim-Python to the IR + run the correctness gate, execute (SQL via QueryService / sim-Python via the injectable graph runner — default run_graph, or the real churn engine over the live cluster for a churn-shaped graph, #580), record one data_access audit event; zetta-core / no-SDL (#525/#580)
  testkit/                  zetta::testkit — the in-app test-suite catalog (headless-runnable; #462)
    test_suite.hpp          TestCase/TestOption/TestOptions/TestResult/TestSuite + run_test_case + builtin() — the catalog model the GUI renders
    harness.hpp             ScratchDir + LoopbackCluster (a real N-node loopback cluster) + the in-code synthetic churn dataset + numeric encoding
    example_script.hpp      zetta::testkit::ExampleScript/ExampleExpectations/ExampleRunReport + run_example_script + example_scripts() — the sim-Python example-script harness: run a script end to end (parse->IR->gate->run) + assert on outputs, surfacing the offending statement on failure (the example programs ARE the integration tests; #523)
  util/                     small shared utilities
    uuid.hpp                zetta::util::new_uuid() — v4 UUID for folder names
    sha256.hpp              zetta::util::sha256_hex — large-object body fingerprint
    timestamp.hpp           zetta::util::now_iso8601() / iso8601_seconds_ago() — UTC ISO-8601 stamps (created/updated + window cutoffs)
    bytes.hpp / duration.hpp  human-readable size / time formatters
src/core/                   zetta-core impl (api/: registry.cpp + bridge.cpp + capabilities.cpp + http_server.cpp — the ONE beast2/Boost.JSON adapter TU, builds into zetta-api; observability/: trace.cpp + activity.cpp + crash.cpp)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  catalog.hpp               flat-key scheme for the replicated catalog (internal)
  json_boundary.hpp         internal: the shared JSON-boundary field readers (string/int/…, required/optional) every cloud-burst Config from_json parser uses, so the boundary checks live once instead of copy-pasted per <x>_provisioner.cpp
  base64.hpp                shared base64 (gossip blobs + signal payloads; internal)
  node.cpp                  load_databases()/load_projects()/load_deployments()/load_runs()/load_jobs()/load_dataset_partitions() + replication capture/reconcile/persist + Node::datasets() (the computed-at-query-time DatasetSummary join, #585)
  runtime.cpp               zetta::runtime::parallel_for / parallel_chunks — data-parallel fan-out over the shared Capy thread pool
  headless.cpp              signal-driven headless loop
  signals.cpp               SIGINT/SIGTERM stop flag (OS signals, not the signal plane)
  cluster/
    cluster.cpp             cluster networking adapter (the one Corosio user) + signal-plane transport; mutual TLS 1.3 per link (openssl_stream over the token-derived CA, handshake before any protocol line, fail-closed) when security is on (#466)
    security.cpp            zetta::cluster security material (the one OpenSSL-libcrypto user): join-token CSPRNG + deterministic Ed25519 CA derivation + per-node cert minting + PEM/secret-file persistence with the schema-validated boundary (#466)
    signal.cpp              zetta::cluster::Signal — envelope JSON form + JSON-Schema boundary (encode/decode)
    collectives.cpp         zetta::cluster::Collectives — collective rounds (star reduce/broadcast) + message JSON-Schema boundary
    replication.cpp         replication CRDT adapter (the one automerge-cpp user)
    node_metrics.cpp        zetta::cluster::NodeMetricsSampler — per-platform OS-counter sampling (macOS mach/sysctl/getifaddrs, Linux /proc, POSIX statvfs), CPU/network rates from deltas; the source behind system.node_metrics (#581)
    node_telemetry.cpp      zetta::cluster::NodeTelemetryExchange — the node.metrics wire boundary (embedded draft-07 schema + pboettch validation, encode/decode) + the thread-safe peer cache (receive on the network thread, record local + snapshot on the loop thread, expire stale) (#584)
  data/
    database.cpp
    table.cpp
    jsonl_store.cpp
    partitioner.cpp         zetta::data::HashPartitioner/RoundRobinPartitioner assign_partition (FNV-1a key hash / surrogate mod) + to_json + partitioner_from_json config boundary (#513)
    rendezvous_placement.cpp  zetta::data::RendezvousPlacement/rendezvous_owners — rendezvous (HRW) partition→node placement: FNV-1a weight, top-k by weight, ~1/N moved on membership change (#514)
    partition_writer.cpp    zetta::data::write_partitioned — the partitioned write path: bucket rows by assign_partition (stamping the round-robin surrogate), serialize each non-empty partition to JSON Lines, store via LargeObjectStore::import_bytes, record a DatasetPartition per partition (cumulative row range + place_partition owners) (#511)
    partition_rebalance.cpp  zetta::data::rebalance_partitions — between-jobs rebalance: per placed partition recompute rendezvous_owners(stats.partition, members, rf=owner count) and set_owner_nodes on a change; skip replicate-to-all (vs previous_members); the Node::tick auto-trigger + Node::cluster_members live in node.cpp (#517); the add_nodes direct trigger lives in add_nodes.cpp (#519)
    dataset_partition.cpp   zetta::data::DatasetPartition — value, apply_value, JSON-Schema boundary validation (parse_dataset_partition_value)
    large_object.cpp        zetta::data::LargeObjectStore — blob folders + body I/O (import_file/import_bytes, sha256 fingerprint)
    import.cpp              zetta::data::import_records — JSON/JSONL file → table rows
    add_dataset.cpp         zetta::data::add_dataset_file — decode parquet/jsonl/json/csv by extension → free-form rows (native Parquet reader + CSV type inference), create a uniquely-named table, write_partitioned with RendezvousPlacement (#587)
    replication_strategy.cpp  zetta::data::ReplicationStrategy — kind names, JSON boundary validation, replication_strategy_from_meta reader (ENT-35; #509)
    replicated_blob_store.cpp  large-object body transfer (CRDT metadata + disk)
  query/
    postgres.cpp            zetta::query::PostgresDialect — the tokenizer + recursive-descent parser (Postgres subset → the generic AST)
    executor.cpp            zetta::query::Executor — statement execution against the Node + the read-only system.* observability tables (nodes / deployments / runs / models / indexes / jobs / principals / keys / audit / activity / datasets / dataset_partitions / checkpoints / node_metrics / pipeline_jobs / collective_rounds / spans / traces / crashes / cloud_providers / cloud_status / provisioners)
    service.cpp             zetta::query::QueryService — parse-with-Postgres + execute-each-statement facade (the Outcome the REPL/GUI render)
  parquet/
    thrift_compact.hpp      zetta::parquet::detail::CompactReader — the shared, non-throwing Thrift compact-protocol cursor (used by both the footer and page-header parsers; internal header)
    metadata.cpp            zetta::parquet — Thrift compact-protocol decoder + FileMetaData footer decode + to_json + parquet_metadata_schema/validate boundary (zetta-core; #505)
    snappy.cpp              zetta::parquet::snappy_decompress — Snappy raw-block decoder (preamble varint + literal/copy elements, bounds-checked) for Parquet data pages (#507)
    column_decoder.cpp      zetta::parquet — page-header parse + RLE/bit-packed-hybrid + PLAIN/dictionary value decode + definition levels → DecodedColumn (dense values + validity); parquet_columns_schema/validate boundary (zetta-core; #506)
    reader.cpp              zetta::parquet::read_parquet_rows — decode every leaf column + transpose to one JSON object per row (the untyped whole-file reader; powers load_churn_dataset Parquet) (zetta-core; #508)
  ir/
    execution_graph.cpp     ExecutionGraph IR build + traversal
    serialization.cpp       ExecutionGraph IR ↔ JSON (+ boundary validation)
  input/
    python_frontend.cpp     zetta::input::PythonFrontend — non-throwing tokenizer + recursive-descent parser (Python subset) + AST→IR lowering + the engine-faithful python_step_kind verb table (#520/#522)
    cluster_bindings.cpp    zetta::input::run_add_nodes_step/add_nodes_count — read the count from a lowered cluster.add_nodes IR node + drive zetta::cloud::add_nodes (#522)
    datasets_bindings.cpp   zetta::input::run_load_dataset_step/DatasetStepExecutor — read the source + column projection from a lowered load_dataset IR node, resolve bytes via the DatasetResolver seam, decode with the native parquet reader, project, and populate a (partitioned) table (#521)
  pipeline/
    project.cpp             zetta::pipeline::Project — persist/rename project.json + input/graph CRUD
    project_input.cpp       zetta::pipeline::ProjectInput — persist input.json + payload body
    project_graph.cpp       zetta::pipeline::ProjectGraph — persist a graph's json projection
    intent_compiler.cpp     compile_intent — templated churn/generic skeleton (NL intent → IR)
    n8n_import.cpp          zetta::pipeline::import_n8n_workflow — n8n workflow JSON Schema + validate + store as artifact input
    n8n_compile.cpp         zetta::pipeline::compile_n8n_workflow — n8n node-type→StepKind table + connections→edges → ExecutionGraph IR
    graph_plan.cpp          project_cluster_plan — per-step cluster placement projection (kind → placement; near-data owner propagation along DataFlow edges; drop stale owners) for the Graph Workspace plan view (#591)
    executor.cpp            GraphExecutor + topological_order + DefaultStepExecutor (graph → Run)
    run_progress.cpp        project_run_progress — derive per-step run status from the GraphExecutor log grammar + attribute outputs to producing steps (#626)
    validation.cpp          ValidationReport verdict + JSON round-trip/validation
    gate.cpp                CorrectnessGate runner + NonEmptyGraphCheck + make_check
    graph_checks.cpp        SchemaTypeCheck + DataLeakageCheck (#309) + annotation JSON-Schema validation
    run.cpp                 zetta::pipeline::Run — value, mutators, JSON-Schema boundary validation (parse_run_value)
    deployment_target.cpp   zetta::pipeline::DeploymentTarget — names + JSON (boundary-validated)
    deployment.cpp          zetta::pipeline::Deployment — persist deployment.json
    deployment_controller.cpp  zetta::pipeline::DeploymentController — reconcile pass: start/self-heal a deployment's run
    solution_catalog.cpp    zetta::pipeline::SolutionCatalog — built-in library + schema/IR boundary parse
  compute/
    device_buffer.cpp       DeviceBuffer lifetime (free via device) + ComputeDevice::make_buffer (→ zetta-compute lib)
    host_stub_device.cpp    HostStubDevice — host-memory allocate/transfer; operators unsupported (→ zetta-compute)
    cuda_device.cu          CudaDevice — CUDA runtime memory model + Thrust/custom operator kernels, CpuDevice-conformant (→ zetta-compute; CUDA build only, #363/#361)
    cpu_device.cpp          CpuDevice — every operator on host memory, multi-threaded via runtime::parallel_for (zetta-core; #360)
    device_registry.cpp     DeviceRegistry — enumerate/report/select devices + DeviceInfo JSON boundary (zetta-core; #362)
    work_stealing_scheduler.cpp  WorkStealingScheduler — worker threads, per-worker deques + the steal loop (zetta-core; #366)
    distributed_scheduler.cpp  DistributedScheduler — the cross-node steal protocol + its JSON-Schema boundary + elastic membership re-queue (zetta-core; #366/#367)
    job_coordinator.cpp     JobCoordinator — lifecycle driving (Running→Succeeded|Failed) + the shared signal-handler chain over Collectives + DistributedScheduler (zetta-core; #372)
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
    model.cpp               zetta::ml::Model — value, mutators, weights body I/O, JSON-Schema boundary validation (parse_model_value)
    gbdt.cpp                zetta::ml — histogram GBDT trainer (binning + split-finding + boosting loop), the binary-logistic model + GbdtParams/GbdtMetrics JSON boundaries (gbdt_model via pboettch) (zetta-core; #390)
    gbdt_train_job.cpp      run_train_job — read the device-backed feature frame, drive the boosting loop with the JobRound's allreduce, compute global metrics (gather/broadcast), register the Model on the root; opt-in held-out calibration (content-hash split → gather/fit/broadcast); opt-in checkpoint/restart (resume the partial model, allreduce-min the cluster resume round, checkpoint each round via TrainHooks; #468) (zetta-core; #390/#391)
    gbdt_score.cpp          zetta::ml — pure scorer (align columns to the model's feature order, predict_proba, rank desc + top_k) + load_gbdt_model (weights body → GbdtModel JSON Schema boundary) (zetta-core; #392)
    gbdt_score_job.cpp      run_score_job — read device-backed columns, predict the shard, gather (entity,risk) to root, rank on the device (sort_by_key), broadcast the global at-risk list (zetta-core; #392)
    local_train_job.cpp     run_near_data_train_job — load the local shard via load_local_partitions then drive run_train_job (+calibration) → run_explain_job → run_score_job over it, summing wire_bytes across the phases (zetta-core; #533)
    feature_columns.hpp     zetta::ml::detail::read_column_as_double — shared device-download helper (frame column → host doubles, dtype-widening) used by the train + score + explain job bodies (zetta-core internal; #392/#393)
    calibration.cpp         zetta::ml — Platt (robust Newton, Lin et al. 2007) + isotonic (PAVA) fit, Calibrator apply/JSON/wire boundary, Brier/ECE metrics (zetta-core; #391)
    tree_shap.cpp           zetta::ml — exact TreeSHAP recursion (extend/unwind/unwound_path_sum), model_base_value (cover-weighted expectation), dominant_driver (zetta-core; #393)
    gbdt_explain_job.cpp    run_explain_job — read the device-backed feature shard, per-entity tree_shap (parallel_for), allreduce the global mean-|φ| driver ranking, drive the `explain` job lifecycle (zetta-core; #393)
    golden_oracle.cpp       load_golden_oracle — parse + schema-validate (pboettch draft-07) the committed golden oracle fixture at the boundary + structural rules, GoldenOracle::to_json (zetta-core; #395)
    playbook.cpp            zetta::ml — playbook engine: PlaybookPolicy churn dictionary + hand-written JSON boundary, expected-retained-value priority + ranking, RulesEngineNarrator + ConstrainedNarrator (pboettch playbook_entry schema validation + semantic checks + fallback), generate_playbook, OutcomeLog (zetta-core; #397)
  index/
    index.cpp               zetta::index::Index — IndexValue to_json/parse + Index JSON Schema (pboettch) + persist/apply (parse_index_value) (ENT-39/ENT-40)
    index_registry.cpp      zetta::index::IndexRegistry resolve + IndexSpec::key() + index_spec_from_node (IR `index` step → IndexSpec boundary)
    index_step_executor.cpp  zetta::index::IndexStepExecutor — run an IR `index` step: resolve backend (node registry) → read table rows into partitions → build → record Index entity → `index` output; + read_index_partitions (shared) (#528)
    in_memory_index_backend.cpp  zetta::index::InMemoryIndexBackend — inverted-index build over partitions / query / idempotent drop (the Search reference backend)
    oramacore_fulltext_index.cpp  zetta::index::OramaCoreFulltextIndex — full-text BM25F build over partitions / ranked query (now carrying per-hit scores) / drop via the oramacore-cpp MemoryIndex (pImpl); ZETTA_WITH_ORAMACORE-guarded engine-absent stub (#527)
    distributed_fulltext.cpp  zetta::index::DistributedFulltextIndex — per-partition build from owned partitions, scatter-gather query over the fulltext.query RPC, merge_fulltext_hits + the request/response JSON-Schema boundary (#529)
  cloud/
    node_capability.cpp     zetta::cloud::NodeCapability — GpuKind names, satisfies(), JSON boundary validation (ENT-32)
    provisioner.cpp         zetta::cloud::ProvisionedNode — JSON boundary validation for the Provisioner DTO
    provisioner_detect.cpp  zetta::cloud::detect_provisioner_configs — the per-provider detectors: run each local CLI's fast read-only context queries through the CommandRunner, parse the output, build the provider's validated XConfig JSON + one-line summary → ProvisionerConfig
    add_nodes.cpp           zetta::cloud::add_nodes — permanent-add control action over the Provisioner seam (provision + report, no teardown) + AddNodesRequest JSON boundary (#518); the Node& overloads drive rebalance_partitions onto the new members (#519)
    local_provisioner.cpp   zetta::cloud::LocalProvisioner — given-hardware backend: best-effort select over the live cluster fleet, no-op teardown (#374)
    azure_provisioner.cpp   zetta::cloud::AzureProvisioner — burst lifecycle (provision+mint / teardown+revoke) over the AzureControlPlane seam + AzureConfig JSON boundary (#376)
    azure_cli_control_plane.cpp  zetta::cloud::AzCliControlPlane — `az vmss` argv construction + JSON parsing at the ProvisionedNode boundary (#376)
    command_runner.cpp      zetta::cloud::default_command_runner — the shared fork/execvp/pipe exec runner (argv, no shell; #376)
    aws_provisioner.cpp     zetta::cloud::AwsProvisioner — burst lifecycle (provision+mint / teardown+revoke) over the AwsControlPlane seam + AwsConfig JSON boundary (#377)
    aws_cli_control_plane.cpp  zetta::cloud::AwsCliControlPlane — `aws autoscaling`/`aws ec2` argv construction + JSON parsing at the ProvisionedNode boundary (#377)
    gcp_provisioner.cpp     zetta::cloud::GcpProvisioner — GCP burst lifecycle (provision+mint / teardown+revoke) over the GcpControlPlane seam + GcpConfig JSON boundary (#378)
    gcp_cli_control_plane.cpp  zetta::cloud::GcpCliControlPlane — `gcloud` MIG argv construction (template/create/resize/list/delete-instances) + JSON parse at the ProvisionedNode boundary (#378)
    oci_provisioner.cpp     zetta::cloud::OciProvisioner — OCI burst lifecycle (provision+mint / teardown+revoke) over the OciControlPlane seam + OciConfig JSON boundary (#432)
    oci_cli_control_plane.cpp  zetta::cloud::OciCliControlPlane — `oci` Instance Pool argv (resolve-by-name/instance-config/create-or-resize/list-instances/detach) + JSON parse at the ProvisionedNode boundary (#432)
    k8s_provisioner.cpp     zetta::cloud::K8sProvisioner — Kubernetes burst lifecycle (provision+mint / teardown+revoke) over the K8sControlPlane seam + K8sConfig JSON boundary (#434)
    k8s_cli_control_plane.cpp  zetta::cloud::K8sCliControlPlane — `kubectl` Deployment argv (create/set env+sa/patch spot/scale/get pods/delete pod) + JSON parse at the ProvisionedNode boundary (#434)
    bare_metal_provisioner.cpp  zetta::cloud::BareMetalProvisioner — bare-metal burst lifecycle (provision+mint / teardown+revoke) over the BareMetalControlPlane seam + BareMetalConfig JSON boundary (#436)
    bare_metal_cli_control_plane.cpp  zetta::cloud::BareMetalCliControlPlane — docker/podman argv (ps --filter label/run -d shortfall/rm -f) + NDJSON parse at the ProvisionedNode boundary (#436)
    burst_identity.cpp      zetta::cloud::BurstIdentityManager — bulk mint/revoke burst identities over the Node's principal CRUD + audit sink (#375)
    burst_approval_gate.cpp  zetta::cloud::BurstApprovalGate — cost-cap gate logic: estimate, default cost, audit the decision, BurstCap JSON boundary (APR-3)
    cloud_provider.cpp      zetta::cloud::CloudProviderCatalog — built-in provider metadata (local/bare-metal/k8s/aws/gcp/azure/oci); no cloud access
    cloud_inspector.cpp     zetta::cloud::CloudInspector — run a provider's read-only commands, per-vendor JSON parse + boundary validation → CloudInspectReport, report JSON boundary (#445)
  governance/
    principal.cpp           zetta::governance::Principal — persist/rename principal.json + key CRUD
    access_key.cpp          zetta::governance::AccessKey — persist key.json + authorize() decisions (resource + operation)
    scope.cpp               zetta::governance::Scope — permits() + JSON boundary validation
    capability.cpp          zetta::governance::CapabilitySet — permits()/TTL + JSON boundary validation (ENT-22)
    audit_event.cpp         zetta::governance::AuditEvent — value, JSON form, JSON-Schema boundary validation
  usecases/churn/
    churn_step_executor.cpp  ChurnStepExecutor — the per-kind real churn compute, each heavy step a ComputeJob (composes featurize/train/score/explain); index step also builds a real entity index via the node registry (#396/#528)
    churn_solution.cpp      run_churn_solution_on_node — the ChurnDataset→numeric encoder + standard churn pipeline config + the one public runner (resolve Node::job_coordinator → inject ChurnStepExecutor into a GraphExecutor over the live spine; run_churn_solution_with_coordinator is the file-local isolated-coordinator detail) + is_churn_graph + demo_churn_dataset (#398/#580/#637)
    churn_dataset.cpp       load_churn_dataset — JSONL/CSV churn fixture reader + 3 embedded draft-07 schemas (boundary validation)
    entity_resolution.cpp   EntityResolver — normalized full-name / brand matching + entity_resolution_annotation builder (FEAT-6/TC-4; #394)
    entity_resolution_check.cpp  EntityResolutionCheck — conflation/fragmentation + schema boundary
  repl/
    console_model.cpp       zetta::repl::ConsoleModel — submit/clear logic + Frontend names + format_result/format_transcript text projections (CMP-6; #524)
    governed_console.cpp    zetta::repl::GovernedConsole — the governance pipeline (capability check → sim-Python lower+gate → execute via the injectable graph runner → data_access audit) + required_operation + the SQL Outcome→Result grid mapping (SCR-8; #525/#580)
  testkit/                  zetta::testkit — the in-app test-suite catalog (no SDL; #462)
    test_suite.cpp          TestCase/TestOption/TestOptions/TestResult/TestSuite + run_test_case + builtin()
    harness.cpp             ScratchDir + LoopbackCluster (real N-node loopback) + sample_churn_dataset + numeric encoding
    cases.hpp               internal: the per-category register_*_cases registrars TestSuite::builtin() assembles (one TU per category keeps includes local)
    cases_*.cpp             the canned cases per category (compute/ddf/parquet/ml/distributed/fulltext/governance/pipeline/uc1_scale/examples; cases_fulltext.cpp = the TC-13 cluster.fulltext_index distributed case, #503)
    parquet_fixture_data.hpp  GENERATED by scripts/gen-parquet-fixture.py — the embedded (base64) pyarrow saas_sample.parquet fixture + its golden DataFrame projection, so the in-app TC-9 Parquet parity case is self-contained (same golden as tests/data/test_parquet_reader.cpp; #462)
    example_script.cpp      run_example_script — parse->IR->gate->run + expectation checks + offending-statement reconstruction (#523)
    cases_examples.cpp      example_scripts() built-in catalog + register_example_cases (each sim-Python example a TestCase under TestCategory::Examples)
  util/
    uuid.cpp
    bytes.cpp / duration.cpp  the size / time formatter impls
    sha256.cpp              self-contained SHA-256 (no crypto dependency)
    timestamp.cpp           zetta::util::now_iso8601() / iso8601_seconds_ago() — UTC ISO-8601 stamps
src/gui/                    headed UI (links SDL3 + ImGui)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  app.hpp                   zetta::gui::App — the one class declaring every panel method (forward-declares SDL/ImGui types)
  app.cpp                   lifecycle: ctor/dtor, init/run/render_frame, apply_display_scale, the menu bar + view switcher, file ingestion, the g_mono_font definition + the two load-bearing rendering bits
  app_internal.hpp          shared GUI-only helpers: the Orama design tokens, Tone + tone_color/tone_dot_color/tone_background + the status widget forms status_label (rows) / status_pill (headers) / status_dot(tone) (#636), the widget grammar (page_header/header_action/section_card/stat_tile/empty_state/banner, #630), the master-detail scaffold (MasterRow/detail_header/master_detail, #634), wash/eyebrow/mono_line/primary_button, activity_surface_label (#622), extern g_mono_font/g_heading_font (not in the public include/zetta surface)
  tones.hpp                 the ONE mapping site from engine statuses to visual tones (#636): tone_of(RunStatus/StepRunStatus/JobStatus/ScalingAction/CheckStatus/TestOutcome/ActivityOutcome/cluster::NodeStatus/StepPlacement) + tone_of_log_level + tone_of_utilization (the shared gauge thresholds), with static_asserts locking the cross-lifecycle agreements
  app_views.cpp             the view registry (App::view_registry, one ViewInfo row per view) + navigate_to/take_pending_selection + entity_link + the sidebar rail (#628) + sidebar badges
  cloud_wiring.hpp/.cpp     zetta::gui::CloudProvisioners — wires the cloud-burst Provisioner backends (Azure/AWS/GCP/OCI/K8s/bare-metal) into the headed App's "Add nodes" control from the node's detected provisioner configs (detect_provisioner_configs → App::set_provisioner) and OWNS them for the App's lifetime (stable-address std::optional members, declared before the App); the consent gate stays deny-by-default — a provider with no detected config stays unwired, and every cloud add remains per-action consent-gated
  cluster_telemetry.hpp     zetta::gui::NodeGaugeState/Sparkline/Gauge — pure-data (no ImGui) animation state for the cluster node-box gauges (#584): per-gauge eased value + sparkline history ring, keyed by node id; a thin renderer's working copy of Node::telemetry_snapshot(); the drawing lives in app_cluster.cpp
  cluster_pipeline.hpp      zetta::gui::PipelineFlow/ClusterPipelineState/PipelineInputs — pure-data (no ImGui) animation state for the distributed-algorithm-at-work overlay on the cluster node-box canvas (#590, #575 area 5): short-lived eased "flows" (partition placement / near-data load / scatter-gather collective rounds / rebalance moves) diffed each frame from the live instrumentation read views (system.dataset_partitions / system.pipeline_jobs / system.collective_rounds); owns no numbers, only the animation; the diff (update_cluster_pipeline) + drawing live in app_cluster.cpp
  app_home.cpp / app_explorer.cpp / app_governance.cpp
                            the home/data-flow, explorer, and admin+audit App::draw_* panels — each an App member TU over app.hpp + app_internal.hpp (#464)
  app_operations_internal.hpp
                            helpers shared by the per-view operations TUs below (#638): the cluster node-box palette (kClusterBox*), entity_short_id, join_refs, and the render_model_card declaration (defined in app_models.cpp; also used by the Runs detail) — internal to src/gui, not in the public include/zetta surface
  app_graph_workspace.cpp   the Graph Workspace view (open_graph + draw_graph_workspace + draw_graph_canvas + draw_graph_editor), its graph-over-cluster plan view (draw_graph_plan + build_graph_plan + the canvas placement overlay; #591), and the live-run overlay (latest_run_for_open_graph + draw_graph_run_status + run_open_graph + the canvas status dots/pulse/pinned failure; #626)
  app_deploy.cpp            the correctness gate (draw_gate) + Deploy panel (draw_deploy) + the live churn demo run (run_churn_demo)
  app_activity.cpp          the Activity view (draw_activity; #622): the merged audit-trail+runs feed via observability::activity_entries, time-window + surface filters, entity_link click-through per entry (docs/gui/activity-feed.md)
  app_traces.cpp            the Traces view (#640, docs/gui/traces.md): the per-operation trace pane over Node::span_recorder() (draw_traces — recent traces, per-span four-capture detail; the GUI form of system.traces / system.spans)
  app_runs.cpp              the Runs / outputs / lineage view (draw_runs + render_run_detail)
  app_models.cpp            the Models registry view (draw_models + draw_add_model_form) + render_model_card (shared with the Runs detail via app_operations_internal.hpp)
  app_datasets.cpp          the Datasets view (draw_datasets + the partition-placement overlay draw_dataset_placement; #585)
  app_jobs.cpp              the Compute-jobs view (draw_jobs + the objective/scaling preview draw_job_scaling_controls)
  app_cluster.cpp           the Cluster view: the spatial node-box canvas (draw_cluster; #582), the animated per-node resource gauges + sparklines (#584) over the telemetry snapshot, the distributed-algorithm-at-work overlay (update_cluster_pipeline + draw_pipeline_flow + draw_pipeline_activity; #590) over the live instrumentation read views, the node detail pane, and the consent-gated Add-nodes control (draw_add_nodes_control + run/poll_add_nodes; #583)
  app_cloud.cpp             the Cloud Inspector view (draw_cloud + draw_cloud_inspect_result + the consent-gated run_cloud_inspect)
  app_test_suite.cpp        the in-app Test-suite panel (draw_test_suite + start_test; #462) — a thin renderer over the headless zetta::testkit catalog
  repl_console.hpp/.cpp     zetta::gui::ReplConsole — the thin Dear ImGui renderer over repl::ConsoleModel: frontend selector, multiline editor, run/clear, scrollback transcript, result grid (CMP-6; #524). draw_console (in app_explorer.cpp) hosts it over the live node
src/main.cpp                arg parsing + mode dispatch
schemas/                    generated, drift-checked JSON schemas (the C++ embedded schema is the source of truth)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
docs/                       domain-organized system docs (see docs/README.md)
  CLAUDE.md                 directory rule-card (tree-scoped rules)
modules/                    vendored deps as git submodules (capy, corosio)
tests/                      GoogleTest unit tests (CTest); link zetta-core
  CLAUDE.md                 directory rule-card (tree-scoped rules)
  CMakeLists.txt            the test target — every test TU is listed here, grouped by domain
  temp_dir.hpp              per-test isolated data_dir helper
  fixtures/                 committed test fixtures (churn/ golden/ parquet/ churn_workflow.json)
  test_node.cpp / test_integrity.cpp / test_schemas.cpp / test_runtime.cpp
                            cross-domain root tests (Node CRUD, integrity, schema drift-check, runtime seam)
  util/ cluster/ data/ ddf/ ml/ compute/ index/ pipeline/ cloud/ governance/ observability/ repl/ testkit/ usecases/churn/
                            one test directory per domain, mirroring include/zetta + src/core (each test file lives with its domain)
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
  audit/                     ← the immutable, append-only audit trail
    <audit-uuid>.json        ← { id, action, actor, target, allowed, detail,
                                 time }  (one file per event, never edited)
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
