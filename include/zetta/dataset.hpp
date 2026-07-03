#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace zetta {

// Where one of a dataset's partitions physically lives, flattened from the
// partition catalog (DatasetPartition; ENT-36 / TBL-13): the contiguous row slice
// it covers and the node(s) that own a copy of its body. The cross-cluster
// placement — *which node owns which partition* — that the system.datasets view
// surfaces per dataset. A non-partitioned (replicate-to-all) dataset has none.
struct DatasetPlacement {
    std::string partition_id;              // part:<uuid> (the catalog entry id)
    std::string blob_ref;                  // lo:<uuid> body holding the slice's bytes
    std::uint64_t row_start = 0;           // [start, end) row ordinals this slice covers
    std::uint64_t row_end = 0;             // exclusive
    std::vector<std::string> owner_nodes;  // node ids holding a copy of the body
};

// A read-only, **computed-at-query-time** summary of one dataset — a table together
// with the data it holds — the live engine source behind the read-only
// `system.datasets` observability view (#585, part of the area-3 dataset view in
// epic #575). It treats a dataset as a first-class primitive even though no
// `Dataset` *entity* is stored: **one dataset == one table**, and the summary is
// recomputed from the live Node tree (databases → tables) joined with the
// replicated partition catalog on every call — so it never drifts from the truth
// (the "compute from the live source, never store/hand-sync" rule). The same
// summary backs both the SQL view and a future GUI dataset panel, so the two can't
// diverge. See docs/data/system-datasets.md.
struct DatasetSummary {
    std::string name;       // dataset name (= the table name)
    std::string database;   // owning database name
    std::string table_id;   // the table id (a bare tbl uuid; joins to
                            //   system.dataset_partitions.table)
    std::string format;     // on-disk storage format of the rows (e.g. "jsonl") —
                            //   the live storage engine's format, not the original
                            //   import format (parquet/csv is not retained: it is
                            //   not part of any live source)
    std::string replication;  // replication strategy name ("replicate_to_all" /
                            //   "partitioned" / "partitioned_with_replicas")
    std::uint64_t rows = 0;       // row count
    std::uint64_t size_bytes = 0;  // on-disk bytes of the row data (the JSONL chunks)
    std::vector<std::string> columns;  // inferred schema — field names in first-seen
                                       //   order (the engine is schemaless)
    std::vector<DatasetPlacement> partitions;  // partition placement across the
                                               //   cluster (empty = not partitioned)
};

}  // namespace zetta
