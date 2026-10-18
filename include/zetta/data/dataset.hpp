#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "zetta/data/dataset_columns.hpp"

namespace zetta::data {

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
                            //   import format, which is `source_format` below
    std::string source_format;  // what the rows were loaded from (#1221): csv /
                                //   jsonl / json / parquet, a connector type
                                //   (postgres, snowflake), or rows; empty when
                                //   the table was not loaded (SQL, results)
    std::string replication;  // replication strategy name ("replicate_to_all" /
                            //   "partitioned" / "partitioned_with_replicas")
    std::uint64_t rows = 0;       // row count
    std::uint64_t size_bytes = 0;  // on-disk bytes of the row data (the JSONL chunks)
    std::vector<std::string> columns;  // the column names, in the source's order
                                       //   when one was recorded (#1021), else in
                                       //   first-seen order (the engine is schemaless)
    // Whether `columns` follows a recorded source order (#1021) — the CSV
    // header, the Parquet schema, a connector query's SELECT list — or, false,
    // the first-seen order of the stored rows, whose keys read back sorted.
    bool column_order_recorded = false;
    // The same columns, typed (#1021): what get_dataset answers as `columns`.
    std::vector<DatasetColumn> typed_columns;
    std::vector<DatasetPlacement> partitions;  // partition placement across the
                                               //   cluster (empty = not partitioned)

    // **Degraded** (#837): this node cannot see the whole dataset. The partition
    // manifest says the dataset has `expected_partitions` catalog entries and
    // `expected_rows` rows; fewer entries are visible here, so `rows` above is a
    // FRACTION, not a total, and a read of this dataset fails rather than
    // answering short. The view says so plainly instead of quietly
    // under-reporting — the whole reason the manifest exists.
    //
    // A dataset with no manifest (written before manifests, or by a path that
    // produces none) is never reported degraded: `expected_partitions` is 0.
    bool degraded = false;
    std::string degraded_reason;         // empty unless degraded
    std::uint64_t expected_partitions = 0;  // 0 = no manifest on this node
    std::uint64_t expected_rows = 0;
};

}  // namespace zetta::data