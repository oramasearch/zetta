#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace zetta {

class Node;
class Database;
class Table;
class DatasetPartition;

// **Add a dataset from a file** (#587, part of the area-3 dataset view in epic
// #575) — the reusable engine backend behind the headed "Add dataset" affordance.
// Read a local `parquet` / `jsonl` / `json` / `csv` file, decode it into rows, and
// materialize them as a **partitioned table** on the node, so the new dataset shows
// up in [`system.datasets`](dataset.hpp) / the Datasets view with its partitions
// **placed across the cluster** (the partition catalog, system.dataset_partitions).
//
// This is the GUI counterpart to the sim-Python `datasets` shim
// ([`run_load_dataset_step`](input/datasets_bindings.hpp), #521): that path runs a
// lowered `load_dataset(...)` IR step and reads Parquet only; this path takes a
// plain file the user picked / dropped, dispatches on its extension across all
// three formats, and writes a partitioned table directly — no IR node required. It
// lives in **zetta-core** (no SDL), so it is headless-testable and the GUI stays a
// thin caller.
//
// **No new JSON format.** Parquet is decoded by the native
// [Parquet reader](parquet/reader.hpp) (no Apache Arrow / parquet-cpp), bounds-
// checked at the binary boundary; `jsonl`/`json` are parsed the same way
// [`import_records`](import.hpp) parses them; `csv` is decoded into free-form,
// type-inferred rows. The rows are stored as ordinary schemaless table rows, and
// placement rides the existing validated `DatasetPartition` catalog boundary
// (`write_partitioned`) — so nothing new is parsed back in. Non-throwing.
namespace dataset_file {

// The file formats the "Add dataset" path can ingest, classified by extension.
enum class Format {
    Parquet,  // .parquet / .pq  — native Parquet reader (no Arrow)
    Jsonl,    // .jsonl / .ndjson — one JSON document per line
    Json,     // .json — a JSON array (each element a row) or a single value
    Csv,      // .csv — header row → object keys, cells type-inferred
    Unknown,  // anything else — rejected with a clear reason
};

// A short, stable name for a format (for logs / status lines / errors).
const char* format_name(Format format);

// Classify a file by its lowercased extension. `.parquet`/`.pq` → Parquet,
// `.jsonl`/`.ndjson` → Jsonl, `.json` → Json, `.csv` → Csv, else Unknown.
Format format_from_path(const std::filesystem::path& path);

// The comma-separated extension filter the native file dialog uses, e.g.
// "parquet;pq;jsonl;ndjson;json;csv" — the formats `format_from_path` accepts.
std::string dialog_extensions();

}  // namespace dataset_file

// How to materialize the loaded rows. The `database` is created if absent (default
// "datasets"). When `partitioned`, the table is written through the
// [partitioned write path](partition_writer.hpp) with a
// [`RendezvousPlacement`](rendezvous_placement.hpp) partitioner of `partition_count`
// partitions and `replication_factor` owners each — so the rows shard across the
// partitions and each partition is HRW-placed onto specific cluster node(s)
// (genuinely *across the cluster*, vs. replicate-to-all). `members` is the cluster
// membership placement is computed over (empty = this node only). `table_name`
// overrides the name derived from the file (empty = derive it).
struct AddDatasetOptions {
    std::string database = "datasets";
    bool partitioned = true;
    std::uint32_t partition_count = 8;     // P (chosen P >> nodes)
    std::size_t replication_factor = 1;    // owners per partition (primary + replicas)
    std::vector<std::string> members;      // cluster membership for placement
    std::string table_name;                // override the derived table name
};

// The outcome of adding a dataset. On success `error` is empty, `table` is the
// populated table, `rows` is the count loaded, `format` the detected file format,
// and (when partitioned) `partitions` lists the catalog entries written.
struct AddDatasetResult {
    std::shared_ptr<Database> database;
    std::shared_ptr<Table> table;
    dataset_file::Format format = dataset_file::Format::Unknown;
    std::size_t rows = 0;
    std::vector<std::shared_ptr<DatasetPartition>> partitions;  // when partitioned
    std::string error;  // empty on success

    bool ok() const { return error.empty(); }
};

// Load `path` (a `parquet`/`jsonl`/`json`/`csv` file) into a new partitioned table
// on `node` per `options`. Reads the file, decodes it by format, creates a uniquely
// named table in the target database, adds the rows, and — when partitioned and the
// dataset is non-empty — writes it through `write_partitioned` so its partitions are
// placed across the cluster. Non-throwing: any failure (unsupported extension, an
// unreadable / empty / malformed file, a partition-write error) returns a result
// whose `error` says why. The table is still created (and visible) when the rows
// load but the partition write fails, so the failure is observable rather than
// silent.
AddDatasetResult add_dataset_file(Node& node, const std::filesystem::path& path,
                                  const AddDatasetOptions& options = {});

}  // namespace zetta
