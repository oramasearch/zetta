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
- [query.md](query.md) — the dialect-agnostic query layer: the Postgres subset, `GROUP BY` through the [DDF engine](../ddf/ddf.md), and the virtual `system` tables.
- [virtual-tables.md](virtual-tables.md) — `CREATE VIRTUAL TABLE <name> OVER CRDT '<prefix>' (…)`: a user-declared, read-only table over any CRDT key-family (durable/ephemeral tiers), stored as durable `vtable:` catalog metadata (`zetta::data::VirtualTable`, #809 slice 3).
- [replication-strategy.md](replication-strategy.md) — the per-table/db strategy: `ReplicateToAll` | `Partitioned` | `PartitionedWithReplicas(rf)`, recorded + replicated (`zetta::data::ReplicationStrategy`, ENT-35).
- [partitioner.md](partitioner.md) — the `Partitioner` seam: record→partition (Hash/RoundRobin) + partition→node (`RendezvousPlacement` HRW) over a fixed P >> nodes (ADR-31 / #512–#514).
- [dataset-partitions.md](dataset-partitions.md) — the replicate-to-all partition catalog: where each slice of a table's data lives (`zetta::data::DatasetPartition`, ENT-36 / #510).
- [partitioned-write-path.md](partitioned-write-path.md) — rows → partition → blob, placement recorded in the catalog (`zetta::data::write_partitioned`, #511).
- [partition-body.md](partition-body.md) — the body format of a partition: Parquet for every dataset (`_zid` as a column), format sniffed from the bytes so legacy `.jsonl` bodies still read, JSON Lines fallback for rows Parquet cannot express (`zetta::data::serialize_partition_body`/`read_partition_body`, #833).
- [sharded-blob-placement.md](sharded-blob-placement.md) — a partition body is held only by its owner node(s), not replicate-to-all (`zetta::data::ReplicatedBlobStore`, #515).
- [partition-rebalance.md](partition-rebalance.md) — between-jobs rebalance on a membership change: ≈rf/N partitions move onto a new node, bodies follow (`zetta::data::rebalance_partitions`, #517).
- [system-datasets.md](system-datasets.md) — datasets as a first-class observability primitive: the computed `system.datasets` view over the live tree + catalog (`Node::datasets()`, #585).
- [add-dataset.md](add-dataset.md) — the "Add dataset" affordance backend: load a parquet/jsonl/json/csv file as a partitioned table placed across the cluster (`zetta::data::add_dataset_file`, #587; `AsyncDatasetAdd` with progress/cancel + audit, #624; `add_dataset_rows` for in-memory rows, #827).
- [connector-query.md](connector-query.md) — source connectors: run a read-only query against an external system (Snowflake's SQL API v2 first) and land its rows as a partitioned dataset; the injected `HttpTransport` seam, no foreign runtime (#827).
- [parquet-metadata.md](parquet-metadata.md) — the native (no Arrow) Parquet footer/metadata parser via a self-contained Thrift compact decoder (`parse_parquet_metadata`, ADR-32 / #505).
- [parquet-snappy.md](parquet-snappy.md) — the self-contained Snappy raw-block codec for Parquet's default-codec data pages: `snappy_decompress` (#507) plus the write-side `snappy_compress` and its literals-only reference (#833).
- [parquet-column-decoders.md](parquet-column-decoders.md) — column-chunk decoders: PLAIN + dictionary pages, definition levels → typed values + validity bitmap (`decode_column`, #506).
- [parquet-writer.md](parquet-writer.md) — the native (no Arrow/pyarrow) Parquet **writer**: Thrift compact emission, PLAIN values + RLE definition levels, SNAPPY pages, schema inference from JSON rows; round-trips through `read_parquet_rows` (`write_parquet_rows`, #833).
- [parquet-store.md](parquet-store.md) — the disk-backed, paging `StorageEngine` over a Parquet body: footer-only open, a row group per page, a byte-budgeted page cache with weak_ptr re-adoption, refused mutations (`zetta::storage::ParquetStore`, #833).
- [parquet-reader.md](parquet-reader.md) — the capstone reader → DDF columns with column projection (drop `embedding_*` unread) + the untyped `read_parquet_rows` (#508, TC-9).
