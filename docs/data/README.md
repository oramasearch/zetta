# Data docs

How a node stores, replicates, places, and queries data. The central rule: the CRDT document is the
source of truth and disk is its projection; on top of that sit the per-table
`ReplicationStrategy` → `Partitioner` → partition-catalog → sharded-blob-placement chain (data
placement across the cluster) and the native, no-Arrow Parquet reader. Read here for anything
touching storage, ingestion, partitioning/placement, the query layer, or the `system.*` dataset
views; the compute that runs *over* this data lives in [../ddf/](../ddf/ddf.md) and
[../compute/](../compute/compute-near-data.md).

- [storage.md](storage.md) — on-disk format (UUID folders, JSONL chunks) and the storage-engine interface (`zetta::storage::StorageEngine`).
- [lazy-rows.md](lazy-rows.md) — the row handle `StorageEngine::at()` returns: refcounted pages that survive a reap, a borrowed (allocation-free) state for in-memory engines, and a failed read that reports a reason instead of null (`zetta::storage::ParquetLazyPtr`, #833).
- [large-objects.md](large-objects.md) — storing files as large objects (metadata in the CRDT, bodies peer-to-peer) and importing JSON/JSONL records.
- [query.md](query.md) — the dialect-agnostic query layer: the Postgres subset,
  aggregates with or without `GROUP BY` on any key type (dense numeric through the
  [DDF engine](../ddf/ddf.md), the rest over rows), unknown columns as errors,
  `columns` always filled, parse errors located by position (#1218), and the
  virtual `system` tables, including live pub/sub dispatch sizing in
  `system.node_metrics`.
- [record-store.md](record-store.md) — the keyed record store: opaque records by `(collection, key)`, `if_absent`/`if_revision` writes linearizable per key through the key's owner, synchronous fan-out to live peers, a fsynced journal, secondary-index lists and retention (`zetta::data::RecordStore`, #1231).
- [time-series.md](time-series.md) — labelled counters and gauges: per-node bucket keys that never conflict, range queries summed across nodes, grouped and stepped (`zetta::data::MetricStore`, #1231).
- [virtual-tables.md](virtual-tables.md) — `CREATE VIRTUAL TABLE <name> OVER CRDT '<prefix>' (…)`: a user-declared, read-only table over any CRDT key-family (durable/ephemeral tiers), stored as durable `vtable:` catalog metadata (`zetta::data::VirtualTable`, #809 slice 3).
- [replication-strategy.md](replication-strategy.md) — the per-table/db strategy: `ReplicateToAll` | `Partitioned` | `PartitionedWithReplicas(rf)`, recorded + replicated (`zetta::data::ReplicationStrategy`, ENT-35).
- [partitioner.md](partitioner.md) — the `Partitioner` boundary: record→partition (Hash/RoundRobin) + partition→node (`RendezvousPlacement` HRW) over a fixed P >> nodes (ADR-31 / #512–#514).
- [partition-scheduler.md](partition-scheduler.md) — the policy ABOVE `Partitioner` that decides how many partitions a dataset gets (a target body size of 64 MiB, with a floor of partitions per node), how big each targets, and which nodes hold them — weighted by live node telemetry, degrading to an even spread when there is none; `SourceLayout` says whether a source's units are flexible (fetch straight into the target shape, no rebalance) or fixed (fetch source-shaped, then rebalance). The plan is computed once by the coordinator and TRANSMITTED, because a capacity-aware plan is not derivable per node (`zetta::data::PartitionScheduler`, #833).
- [fetch-affinity-placement.md](fetch-affinity-placement.md) — placement that answers "the node that fetched this slice" instead of hashing, so a distributed ingest never shuffles what it just wrote; replicas still spread by rendezvous (`zetta::data::FetchAffinityPlacement`, #833).
- [dataset-partitions.md](dataset-partitions.md) — the replicate-to-all partition catalog: where each slice of a table's data lives (`zetta::data::DatasetPartition`, ENT-36 / #510).
- [partitioned-write-path.md](partitioned-write-path.md) — rows → partition → blob, placement recorded in the catalog (`zetta::data::write_partitioned`, #511).
- [partition-body.md](partition-body.md) — the body format of a partition: Parquet for every dataset (`_zid` as a column), format sniffed from the bytes so legacy `.jsonl` bodies still read, JSON Lines fallback for rows Parquet cannot express (`zetta::data::serialize_partition_body`/`read_partition_body`, #833).
- [partition-only-tables.md](partition-only-tables.md) — tables whose rows are never captured into the CRDT: the data is the partition bodies + the catalog, `system.datasets` counts from the catalog's row ranges, and a `SELECT` fans out to the owners (`zetta::data::Table::partition_only`, #833).
- [partition-manifest.md](partition-manifest.md) — the table-level record of how many partitions a dataset HAS (`mfst:<table-id>`), so a reader can tell an incomplete view of the partition catalog from a complete one and FAIL the query instead of answering short; enriched per-partition stats (the body's own schema + byte size) let a node plan without reading a body (`zetta::data::DatasetManifestValue`, #837).
- [scatter-gather-select.md](scatter-gather-select.md) — a `SELECT` over a partition-only table fans out to the partition owners and merges: one owner per partition (exactly-once at RF>1), WHERE + projection pushed near the data, aggregates merged (`avg` weighted by row count), an unreachable owner FAILS the query (`zetta::query::DistributedSelect`, #833).
- [sharded-blob-placement.md](sharded-blob-placement.md) — a partition body is held only by its owner node(s), not replicate-to-all (`zetta::data::ReplicatedBlobStore`, #515).
- [partition-rebalance.md](partition-rebalance.md) — between-jobs rebalance on a membership change: ≈rf/N partitions move onto a new node, bodies follow (`zetta::data::rebalance_partitions`, #517).
- [promote-dataset.md](promote-dataset.md) — raise a cached result set from RF=1 to the durable factor: add owners to each partition in the catalog and the bodies follow over the large-object transport; never removes an owner, clamped to the live roster (`zetta::data::promote_dataset`, #833).
- [system-datasets.md](system-datasets.md) — datasets as a first-class observability primitive: the computed `system.datasets` view over the live tree + catalog (`Node::datasets()`, #585).
- [add-dataset.md](add-dataset.md) — the "Add dataset" affordance backend: load a parquet/jsonl/json/csv file as a partitioned table placed across the cluster (`zetta::data::add_dataset_file`, #587; `AsyncDatasetAdd` with progress/cancel + audit, #624; `add_dataset_rows` for in-memory rows, #827; `add_dataset_content` + the `upload_dataset` capability for a file's text over the API, #1008; the recorded `source_format`, ragged-CSV refusal, and `upload_dataset`'s name and conflict rules, #1221).
- [dataset-path.md](dataset-path.md) — the one `dataset_path` grammar and column contract, including reversible text entity keys, boolean 0/1 columns, and explicit ISO-or-epoch cutoffs (#947, #957, #1199).
- [featurization.md](featurization.md) — dataset analysis and methodology-specific feature planning: semantic evidence, grain, keys, joins, leakage candidates, named corrections, run-time enforcement, and feature/exclusion provenance for all 21 runnable methodologies, shared by Runs and Study trials (#1062, #1216).
- [connector-query.md](connector-query.md) — source connectors: run a read-only query against an external system (Snowflake's SQL API v2 first) and land its rows as a partitioned dataset; the injected `HttpTransport` interface, no foreign runtime (#827); saved connections hold the credential sealed under a key derived from the join token, re-sealed on rotation, and no read surface — SQL on `zetta_connect` included — returns it (#1230).
- [connecting-snowflake.md](connecting-snowflake.md) — **runbook**: getting a Snowflake source in with a
  programmatic access token. The two walls Snowflake puts up first (a network policy is required ON THE
  USER, `390432`; a PAT cannot assume a privileged role, `390186`), registering the connection over MCP,
  and the consent gate that stops the pull until a different principal approves it.
- [adding-a-connector.md](adding-a-connector.md) — the **practice** for teaching zetta a new source: Perplexity deep-dive → connector-skill doc → transport decision (HTTP API vs driver, the ADR-17 gate) → build with the `new-connector` skill. Snowflake + Postgres (libpqxx, coroutine-wrapped) are the worked examples (#892).
- [distributed-connector-ingest.md](distributed-connector-ingest.md) — the cross-node ingest: submit once, co-launch the roster, every node fetches ONLY its own tile straight from the source and lands it as partitions it owns; a node dying mid-fetch fails the whole ingest. Tiling needs a result manifest, so it is Snowflake's path — an untileable source (Postgres) takes the single-shot path to the same dataset shape (`zetta::connect::run_distributed_connector_ingest`, #833, #994).
- [parquet-metadata.md](parquet-metadata.md) — the native (no Arrow) Parquet footer/metadata parser via a self-contained Thrift compact decoder (`parse_parquet_metadata`, ADR-32 / #505).
- [parquet-snappy.md](parquet-snappy.md) — the self-contained Snappy raw-block codec for Parquet's default-codec data pages: `snappy_decompress` (#507) plus the write-side `snappy_compress` and its literals-only reference (#833).
- [parquet-column-decoders.md](parquet-column-decoders.md) — column-chunk decoders: PLAIN + dictionary pages, definition levels → typed values + validity bitmap (`decode_column`, #506).
- [parquet-writer.md](parquet-writer.md) — the native (no Arrow/pyarrow) Parquet **writer**: Thrift compact emission, PLAIN values + RLE definition levels, SNAPPY pages, schema inference from JSON rows; round-trips through `read_parquet_rows` (`write_parquet_rows`, #833).
- [parquet-store.md](parquet-store.md) — the disk-backed, paging `StorageEngine` over a Parquet body: footer-only open, a row group per page, a byte-budgeted page cache with weak_ptr re-adoption, refused mutations (`zetta::storage::ParquetStore`, #833).
- [parquet-reader.md](parquet-reader.md) — the capstone reader → DDF columns with column projection (drop `embedding_*` unread) + the untyped `read_parquet_rows` (#508, TC-9).
