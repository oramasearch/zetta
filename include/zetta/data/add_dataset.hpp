#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/partition_scheduler.hpp"
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
// [`RendezvousPlacement`](rendezvous_placement.hpp) partitioner — so the rows
// shard across the partitions and each partition is HRW-placed onto specific
// cluster node(s) (genuinely *across the cluster*, vs. replicate-to-all).
// `members` is the cluster membership placement is computed over (empty = this
// node only). `table_name` overrides the name derived from the file (empty =
// derive it).
//
// **How many partitions is decided by the
// [`PartitionScheduler`](partition_scheduler.hpp), not by a constant** (#833):
// it targets a partition SIZE (per-blob overhead below it, lost parallelism and
// decode memory above it) with a floor of partitions per node so every node gets
// work. `partition_count` here is a manual override, honoured exactly. The plan
// it emitted — including its rationale — comes back in `AddDatasetResult::plan`.
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
    // **How many partitions — `nullopt` means let the scheduler decide** (#833).
    // This used to be a hard-coded 8 (eight because eight), which is wrong in
    // both directions at once: eight footers + eight replicated catalog entries
    // for a 4 KB dataset, and 25 GB bodies for a 200 GB one. The
    // [`PartitionScheduler`](partition_scheduler.hpp) targets a partition SIZE
    // and a floor of partitions per node instead. A value here is a **manual
    // override, honoured exactly**.
    std::optional<std::uint32_t> partition_count;
    std::size_t replication_factor =       // owners per partition (see below)
        kDefaultReplicationFactor;
    std::vector<std::string> members;      // cluster membership for placement
    std::string table_name;                // override the derived table name
    bool partition_only = false;           // rows never captured into the CRDT (#833)
    // When non-empty, place every partition on THIS node id instead of hashing
    // it onto the roster — [fetch affinity](fetch_affinity_placement.hpp), #833
    // section 6. The distributed connector ingest sets it to the fetching node's
    // own id so the slice it just pulled is never shuffled somewhere else;
    // replicas (a factor above 1) still spread by rendezvous over the rest.
    std::string fetch_affinity_owner;
    // The source's natural shape (#833), passed through to the scheduler so the
    // plan's rationale records what it was planning over. A local file or a
    // block of in-memory rows is flexible by construction — the caller holds all
    // the rows and lays them out however the plan says — so the default is
    // right for both `add_dataset_file` and `add_dataset_rows`.
    SourceLayout source;
    // Serialized size of the dataset, when the caller already knows it (0 ⇒
    // `add_dataset_rows` estimates it from the rows; the file path uses the
    // file's size). The scheduler needs BYTES, not rows, to target a size.
    std::size_t approx_bytes = 0;
    // The policy that decides the plan; null = `default_partition_scheduler()`.
    // A non-owning back-reference: the scheduler outlives the call.
    const PartitionScheduler* scheduler = nullptr;
    // What the rows were loaded from, recorded on the table (#1221; see
    // Table::source_format). Empty = the path's own: the file or content
    // format for `add_dataset_file` / `add_dataset_content`, `rows` for
    // `add_dataset_rows`. A connector pull sets its connector type.
    std::string source_format;
    // The source's column order, recorded on the table (#1021; see
    // Table::column_order). Empty = the path's own: the CSV header, the Parquet
    // file's schema, the order a JSON or JSON Lines source first writes each
    // key — and nothing for in-memory rows, whose objects hold no order. A
    // connector pull sets its query's SELECT list.
    std::vector<std::string> columns;
};

// The outcome of adding a dataset. On success `error` is empty, `table` is the
// populated table, `rows` is the count loaded, `format` the detected file format,
// and (when partitioned) `partitions` lists the catalog entries written.
struct AddDatasetResult {
    std::shared_ptr<Database> database;
    std::shared_ptr<Table> table;
    dataset_file::Format format = dataset_file::Format::Unknown;
    std::string source_format;  // recorded on the table (#1221)
    std::size_t rows = 0;
    std::vector<std::shared_ptr<DatasetPartition>> partitions;  // when partitioned
    // The plan the scheduler emitted for this write — the count it chose, the
    // sizes it targeted, the owners, and the RATIONALE. Surfaced so the decision
    // is observable rather than a number that appeared from nowhere. Empty
    // (partition_count 0) when the dataset was not partitioned.
    PartitionPlan plan;
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

// Why `name` cannot name a dataset or a database that SQL reads unquoted, or
// empty when it can (#1221): a letter or underscore, then letters, digits and
// underscores, at most 63 characters, and not a SQL keyword. Lake applies the
// same rule with the same words (lake PR #459), so a caller gets one answer.
std::string dataset_name_problem(std::string_view name);

// Why CSV `content` would not load row for row, or empty when it would
// (#1221): a header that is missing, has an unnamed column or names one twice,
// a quoted field left open at the end of its line, or a row whose field count
// differs from the header's. Each names its 1-based line, e.g.
// `line 3: 3 fields, but the header has 2 columns`. The CSV decoder refuses on
// exactly these, so a ragged row is never padded with nulls or cut short.
std::string csv_problem(std::string_view content);

// The most file text `add_dataset_content` accepts: 16 MiB. The upload path
// (`upload_dataset`, #1008) holds the whole text in the request and then in
// memory while it decodes, on the owner loop, so the cap bounds both the request
// and the owner-loop stall. Larger data belongs in a file on the node
// (`add_dataset`) or a source connection (`create_dataset_from_query`), both of
// which land it partition by partition.
inline constexpr std::size_t kMaxDatasetContentBytes = std::size_t{16} * 1024 * 1024;

// Load a file's TEXT, already in memory, as a new partitioned table on `node` —
// the path for a caller that cannot put a file on the node's filesystem (a
// remote REST/MCP client, #1008). `content` is decoded exactly as
// `add_dataset_file` decodes a file of `format` (the same CSV header +
// type-inferred cells, the same one-document-per-line JSONL, the same JSON
// array), then materialized through the same table-creation, partition-write and
// audit path. `format` must be Csv, Jsonl or Json (Parquet is binary and has no
// text form; Unknown is rejected). `options.table_name` is the table's base name
// (required in spirit: empty falls back to "dataset"); a clash gets the same
// `_2`, `_3` suffix a re-added file does — `upload_dataset` refuses a taken
// name before it gets here unless the caller asked for that rename. Non-throwing: content over
// `kMaxDatasetContentBytes`, an unsupported format, empty or malformed content
// each return a result whose `error` says why, and create nothing.
AddDatasetResult add_dataset_content(Node& node, std::string_view content,
                                     dataset_file::Format format,
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