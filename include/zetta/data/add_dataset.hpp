#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/replication_strategy.hpp"

namespace zetta { class Node; }

namespace zetta::data {

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
//
// **`replication_factor` defaults to 3, clamped to the node count** (#833
// section 8). It used to default to 1, which was harmless only because a
// partitioned table ALSO replicated every row through the CRDT — belt and
// braces. The moment rows stop being captured (`partition_only`), the factor is
// the only thing standing between a dataset and "one copy, no backup". A
// connector result set is the deliberate exception: it is a cache, re-pullable
// from its source, so that path passes 1 explicitly. The clamp is what keeps a
// single-node cluster placing successfully.
//
// `partition_only` (#833) marks the table as one whose rows are **never captured
// into the replicated document** — the partition bodies and the replicate-to-all
// catalog are the dataset, and a `SELECT` over it is answered by scatter-gather
// across the partition owners. Use it for data a node lands as partitions it
// owns (a connector result set); capturing those rows would put them on the wire
// a second time. See `Table::partition_only`.
struct AddDatasetOptions {
    std::string database = "datasets";
    bool partitioned = true;
    std::uint32_t partition_count = 8;     // P (chosen P >> nodes)
    std::size_t replication_factor =       // owners per partition (see below)
        kDefaultReplicationFactor;
    std::vector<std::string> members;      // cluster membership for placement
    std::string table_name;                // override the derived table name
    bool partition_only = false;           // rows never captured into the CRDT (#833)
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

// Load already-decoded in-memory `rows` (JSON documents) into a new partitioned
// table on `node` per `options` — the **in-memory sibling** of add_dataset_file
// (which decodes a file to exactly these rows first). This is the landing path a
// **connector query** (or any row producer) uses: the producer's only job is to
// yield rows; this shards + places them across the cluster through the same
// validated `write_partitioned` path. Creates the target database + a uniquely
// named table (`options.table_name`, else "dataset"), adds the rows, and — when
// partitioned and non-empty — writes the partitions across the cluster. Records
// a `dataset_added` audit event on success. Non-throwing: any failure returns a
// result whose `error` says why (the table is still created + visible if only
// the partition write fails, so the failure is observable rather than silent).
AddDatasetResult add_dataset_rows(Node& node, std::vector<nlohmann::json> rows,
                                  const AddDatasetOptions& options = {});

// --- Add a dataset WITHOUT blocking the caller's thread (#624) ---------------
//
// AsyncDatasetAdd splits add_dataset_file into its two natural phases and runs
// each where it belongs:
//
//   1. **Decode** (the slow, pure part — read + parse the file into rows) runs
//      on a runtime worker via runtime::spawn, reporting live row counts and
//      honoring a cooperative cancel, with NO node access at all.
//   2. **Materialize** (the node writes — create the table, add the rows,
//      write the partitions, record the audit event) runs on the node's owner
//      thread in bounded slices: the owner loop calls pump() once per
//      frame/tick, and each pump adds at most a chunk of rows, so the UI stays
//      interactive for a large file. The partition write is a single pump
//      today (chunking inside write_partitioned is a later slice).
//
// Progress: rows_decoded() is live during decode (jsonl/csv count as they
// parse; json/parquet decode whole-file and report at the end); rows_total() /
// rows_added() drive a real fraction during materialize. Cancel: during decode
// nothing has touched the node; during materialize the partially-added table is
// dropped — either way the result's error is "cancelled by user". A successful
// add records a `dataset_added` audit event, so it surfaces in the activity
// feed (system.activity) like every other completed action.
//
// Threading: construct via start() and call everything else — pump(), done(),
// result() — from the node's owner thread only. rows_decoded() is safe any
// time (atomic). Dropping the handle mid-decode is safe: the decode worker
// owns its state via shared_ptr and finishes (or cancels) on its own.
class AsyncDatasetAdd {
public:
    // Where the add currently is. Decoding -> Materializing -> Partitioning ->
    // Done; failures and cancels jump straight to Done with result().error set.
    enum class Phase { Decoding, Materializing, Partitioning, Done };

    // Launch the decode worker. `options.members` is snapshotted here (call on
    // the owner thread with the live membership, like the synchronous path).
    static std::shared_ptr<AsyncDatasetAdd> start(Node& node,
                                                  std::filesystem::path path,
                                                  AddDatasetOptions options);

    // Advance the owner-thread phases within a bounded slice; call once per
    // frame/tick until done(). A no-op while the decode worker is still busy.
    void pump();

    bool done() const { return phase_ == Phase::Done; }
    Phase phase() const { return phase_; }

    // Live progress. rows_total() is 0 until the decode finishes.
    std::size_t rows_decoded() const;
    std::size_t rows_total() const { return rows_total_; }
    std::size_t rows_added() const { return next_row_; }

    // Ask the add to stop: mid-decode nothing touched the node; mid-materialize
    // the next pump() drops the partial table. Idempotent.
    void request_cancel();
    bool cancel_requested() const;

    // The file being added (for progress affordances).
    const std::filesystem::path& source_path() const;

    // The outcome (same shape as the synchronous add). Valid once done().
    const AddDatasetResult& result() const { return result_; }

    // The state shared with the spawned decode worker (public so the worker
    // coroutine — a free function in the implementation — can take it by
    // value; opaque outside the implementation).
    struct DecodeState;

private:
    AsyncDatasetAdd(Node& node, std::filesystem::path path,
                    AddDatasetOptions options);

    Node& node_;
    AddDatasetOptions options_;
    std::shared_ptr<DecodeState> decode_;
    Phase phase_ = Phase::Decoding;
    std::size_t rows_total_ = 0;
    std::size_t next_row_ = 0;  // rows materialized so far
    AddDatasetResult result_;
};

}  // namespace zetta::data