#include "zetta/data/add_dataset.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/parquet/reader.hpp"
#include "zetta/runtime.hpp"
#include "zetta/data/partition_writer.hpp"
#include "zetta/data/fetch_affinity_placement.hpp"
#include "zetta/data/rendezvous_placement.hpp"
#include "zetta/data/table.hpp"

namespace zetta::data {
namespace dataset_file {

const char* format_name(Format format) {
    switch (format) {
        case Format::Parquet: return "parquet";
        case Format::Jsonl:   return "jsonl";
        case Format::Json:    return "json";
        case Format::Csv:     return "csv";
        case Format::Unknown: return "unknown";
    }
    return "unknown";
}

Format format_from_path(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    for (char& character : ext) {
        character =
            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (ext == ".parquet" || ext == ".pq") return Format::Parquet;
    if (ext == ".jsonl" || ext == ".ndjson") return Format::Jsonl;
    if (ext == ".json") return Format::Json;
    if (ext == ".csv") return Format::Csv;
    return Format::Unknown;
}

std::string dialog_extensions() { return "parquet;pq;jsonl;ndjson;json;csv"; }

}  // namespace dataset_file

namespace {

namespace fs = std::filesystem;

// Read the whole file into a string (binary). `error` is empty on success.
std::string read_file_bytes(const fs::path& path, std::string& error) {
    std::error_code code;
    if (!fs::is_regular_file(path, code) || code) {
        error = "'" + path.string() + "' is not a readable file";
        return {};
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "cannot open '" + path.string() + "'";
        return {};
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string bytes = buffer.str();
    if (bytes.empty()) error = "file '" + path.string() + "' is empty";
    return bytes;
}

// Split one CSV line into its fields, honoring double-quoted fields (a doubled
// quote inside a quoted field is a literal quote, and a comma inside quotes is a
// literal comma). Our rows never span lines. Mirrors the churn loader's parser.
std::vector<std::string> parse_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::string field;
    bool in_quotes = false;
    for (std::size_t index = 0; index < line.size(); ++index) {
        const char character = line[index];
        if (in_quotes) {
            if (character == '"') {
                if (index + 1 < line.size() && line[index + 1] == '"') {
                    field.push_back('"');
                    ++index;  // skip the escaped quote
                } else {
                    in_quotes = false;
                }
            } else {
                field.push_back(character);
            }
        } else if (character == '"') {
            in_quotes = true;
        } else if (character == ',') {
            fields.push_back(std::move(field));
            field.clear();
        } else {
            field.push_back(character);
        }
    }
    fields.push_back(std::move(field));
    return fields;
}

// Infer a JSON value for a raw CSV cell: empty → null, `true`/`false`
// (case-insensitive) → bool, a clean integer / float literal → number, else the
// string verbatim. A multi-digit integer with a leading zero stays a string (so a
// zip code / id is not silently re-numbered).
nlohmann::json csv_cell_to_json(const std::string& cell) {
    if (cell.empty()) return nullptr;

    std::string lowered = cell;
    for (char& character : lowered) {
        character =
            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    if (lowered == "true") return true;
    if (lowered == "false") return false;

    const char* const begin = cell.data();
    const char* const end = begin + cell.size();

    // A pure-integer shape (optional sign + only digits) is the only place the
    // leading-zero guard applies: "01234" stays a string (a zip code / id), not
    // 1234, but "3.14" / "1.5e3" still infer as floats below.
    const std::string_view digits =
        (cell.front() == '+' || cell.front() == '-')
            ? std::string_view(cell).substr(1)
            : std::string_view(cell);
    const bool is_integer_shape =
        !digits.empty() &&
        std::all_of(digits.begin(), digits.end(),
                    [](unsigned char character) { return std::isdigit(character); });
    if (is_integer_shape) {
        const bool leading_zero = digits.size() > 1 && digits.front() == '0';
        if (leading_zero) return cell;  // keep as a string
        long long integer = 0;
        const auto parsed = std::from_chars(begin, end, integer);
        if (parsed.ec == std::errc() && parsed.ptr == end) return integer;
        return cell;  // out of int64 range → keep the digits verbatim
    }

    // Float: parse the cell as a JSON number literal — locale-independent and
    // whole-cell by construction, so "1.2.3" / "3 apples" stay strings. (The
    // std::from_chars double overload is availability-gated on macOS in
    // current libc++; the JSON grammar is the portable spelling of "a clean
    // float literal".)
    if (cell.find_first_of("0123456789") != std::string::npos) {
        const auto parsed =
            nlohmann::json::parse(cell, nullptr, /*allow_exceptions=*/false);
        if (parsed.is_number_float()) return parsed.get<double>();
    }

    return cell;  // plain string
}

// The cancelled-load reason, shared by the decode + materialize phases.
constexpr const char* kCancelledError = "cancelled by user";

// Live decode instrumentation (#624): an optional row counter a background
// decode bumps as it parses (a panel renders it live) and an optional
// cooperative cancel flag it honors between rows. Both null on the
// synchronous path.
struct DecodeProgress {
    std::atomic<std::size_t>* rows_decoded = nullptr;
    const std::atomic<bool>* cancel = nullptr;

    void count(std::size_t rows) const {
        if (rows_decoded) rows_decoded->store(rows, std::memory_order_release);
    }
    bool cancelled() const {
        return cancel && cancel->load(std::memory_order_acquire);
    }
};

// Decode a CSV file's bytes into rows: the first non-empty line is the header
// (column names), each subsequent line one object keyed by the header, cells
// type-inferred. A row with fewer cells than the header leaves the missing columns
// absent; extra cells are dropped. `error` is set (and {} returned) when there is
// no header row.
std::vector<nlohmann::json> decode_csv(const std::string& bytes,
                                       std::string& error,
                                       const DecodeProgress& progress = {}) {
    std::vector<nlohmann::json> rows;
    std::istringstream stream(bytes);
    std::string line;
    std::vector<std::string> header;
    while (std::getline(stream, line)) {
        if (progress.cancelled()) {
            error = kCancelledError;
            return {};
        }
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
        if (header.empty()) {
            if (line.empty()) continue;  // skip leading blank lines
            header = parse_csv_line(line);
            continue;
        }
        if (line.empty()) continue;  // skip blank data lines
        const std::vector<std::string> cells = parse_csv_line(line);
        nlohmann::json row = nlohmann::json::object();
        for (std::size_t column = 0; column < header.size(); ++column) {
            if (header[column].empty()) continue;  // skip unnamed columns
            row[header[column]] =
                column < cells.size() ? csv_cell_to_json(cells[column]) : nullptr;
        }
        rows.push_back(std::move(row));
        progress.count(rows.size());
    }
    if (header.empty()) error = "csv file has no header row";
    return rows;
}

// Decode a JSON Lines file: one JSON document per line, blank lines skipped. A
// line that does not parse fails the whole load (all-or-nothing, like
// import_records).
std::vector<nlohmann::json> decode_jsonl(const std::string& bytes,
                                         std::string& error,
                                         const DecodeProgress& progress = {}) {
    std::vector<nlohmann::json> rows;
    std::istringstream stream(bytes);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
        if (progress.cancelled()) {
            error = kCancelledError;
            return {};
        }
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // A line of only whitespace is skipped.
        if (line.find_first_not_of(" \t") == std::string::npos) continue;
        auto value = nlohmann::json::parse(line, nullptr, /*allow_exceptions=*/false);
        if (value.is_discarded()) {
            error = "invalid JSON on line " + std::to_string(line_number);
            return {};
        }
        rows.push_back(std::move(value));
        progress.count(rows.size());
    }
    return rows;
}

// Decode a JSON file: an array becomes one row per element, any other value a
// single row. A parse error fails the load.
std::vector<nlohmann::json> decode_json(const std::string& bytes,
                                        std::string& error) {
    auto value = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        error = "invalid JSON";
        return {};
    }
    std::vector<nlohmann::json> rows;
    if (value.is_array()) {
        for (nlohmann::json& element : value) rows.push_back(std::move(element));
    } else {
        rows.push_back(std::move(value));
    }
    return rows;
}

// Decode a Parquet file with the native reader (no Apache Arrow / parquet-cpp).
std::vector<nlohmann::json> decode_parquet(const std::string& bytes,
                                           std::string& error) {
    parquet::ParquetRowsResult parsed = parquet::read_parquet_rows(bytes);
    if (!parsed.ok()) {
        error = "parquet: " + parsed.error;
        return {};
    }
    return std::move(parsed.rows);
}

// Derive a deterministic, filesystem-safe table name from a file path: the basename
// minus a recognized extension, lowercased with every run of non-alphanumeric
// characters folded to a single '_'. Empty result falls back to "dataset".
std::string derive_table_name(const fs::path& path) {
    std::string base = path.stem().string();  // basename without extension
    std::string name;
    name.reserve(base.size());
    bool pending_separator = false;
    for (const char character : base) {
        const char lower =
            static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if ((lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9')) {
            if (pending_separator && !name.empty()) name.push_back('_');
            pending_separator = false;
            name.push_back(lower);
        } else {
            pending_separator = true;
        }
    }
    if (name.empty()) name = "dataset";
    return name;
}

// A free table name in `database` starting from `base`: `base`, then `base_2`,
// `base_3`, … so loading the same file twice does not collide.
std::string unique_table_name(const Database& database, std::string base) {
    if (!database.find_table(base)) return base;
    for (int suffix = 2;; ++suffix) {
        std::string candidate = base + "_" + std::to_string(suffix);
        if (!database.find_table(candidate)) return candidate;
    }
}

// Read + decode `path` into rows by format (the slow, pure phase — no node
// access). On failure `error` says why and {} is returned.
std::vector<nlohmann::json> decode_dataset_file(
    const fs::path& path, dataset_file::Format format, std::string& error,
    const DecodeProgress& progress = {}) {
    const std::string bytes = read_file_bytes(path, error);
    if (!error.empty()) return {};
    std::vector<nlohmann::json> rows;
    switch (format) {
        case dataset_file::Format::Parquet: rows = decode_parquet(bytes, error); break;
        case dataset_file::Format::Jsonl:   rows = decode_jsonl(bytes, error, progress); break;
        case dataset_file::Format::Json:    rows = decode_json(bytes, error); break;
        case dataset_file::Format::Csv:     rows = decode_csv(bytes, error, progress); break;
        case dataset_file::Format::Unknown:
            error = "unsupported file type '" + path.extension().string() +
                    "' (add a parquet, jsonl, json, or csv file)";
            break;
    }
    // Whole-file decoders (parquet/json) report only on completion.
    if (error.empty()) progress.count(rows.size());
    if (progress.cancelled()) {
        error = kCancelledError;
        return {};
    }
    return rows;
}

// Create (or find) the target database + a uniquely named table with base name
// `base` (the shared, cheap materialize step for both the file and the
// in-memory-rows paths). Fills result.database / result.table; on failure sets
// result.error.
void create_named_target_table(Node& node, const std::string& base,
                               const AddDatasetOptions& options,
                               AddDatasetResult& result) {
    std::shared_ptr<Database> database = node.find_database(options.database);
    if (!database) database = node.create_database(options.database);
    if (!database) {
        result.error = "could not create database '" + options.database + "'";
        return;
    }
    result.database = database;

    const std::string table_name = unique_table_name(*database, base);
    // A partition-only dataset (#833) is marked at create time: the marker rides
    // in the tbl: value, so every node knows the rows live in the partition
    // bodies rather than in the document before any row is written.
    std::shared_ptr<Table> table = database->create_table(
        table_name, std::nullopt, options.partition_only);
    if (!table) {
        result.error = "could not create table '" + table_name + "'";
        return;
    }
    result.table = table;
}

// Create (or find) the target database + a uniquely named table for `path`
// (the file path's base name, unless overridden). Fills result.database /
// result.table; on failure sets result.error.
void create_target_table(Node& node, const fs::path& path,
                         const AddDatasetOptions& options,
                         AddDatasetResult& result) {
    const std::string base =
        options.table_name.empty() ? derive_table_name(path) : options.table_name;
    create_named_target_table(node, base, options, result);
}

// The serialized size of a table's rows, estimated from an evenly-spread sample
// rather than measured — the estimate feeds a size TARGET, and being a few
// percent out moves the partition count by nothing, while dumping every row to
// find out exactly would serialize the dataset twice.
std::size_t estimate_table_bytes(const Table& table) {
    const zetta::storage::StorageEngine& store = table.store();
    const std::size_t rows = store.size();
    if (rows == 0) return 0;
    constexpr std::size_t kSampleRows = 64;
    const std::size_t stride = std::max<std::size_t>(1, rows / kSampleRows);
    std::size_t sampled = 0;
    std::size_t bytes = 0;
    for (std::size_t index = 0; index < rows; index += stride) {
        const zetta::storage::ParquetLazyPtr row = store.at(index);
        if (!row) continue;
        bytes += row->dump().size();
        ++sampled;
    }
    return sampled == 0 ? 0 : (bytes / sampled) * rows;
}

// Partition the populated table so its slices are placed across the cluster.
// RendezvousPlacement shards rows into P partitions and HRW-places each onto
// specific owner node(s) — so the placement actually spreads (vs. the
// replicate-to-all strategies). Skip an empty dataset (nothing to place).
//
// **How many partitions comes from the scheduler** (#833), which is the ONE
// place that decision is made — a size target plus a per-node floor rather than
// a constant 8. `options.partition_count` is a manual override, honoured
// exactly. The plan it emitted (count, targets, owners, rationale) is returned
// on the result so the decision is observable.
void partition_target_table(Node& node, const AddDatasetOptions& options,
                            AddDatasetResult& result) {
    if (!options.partitioned || result.rows == 0 || !result.table) return;
    // Clamp the replication factor to the roster placement is computed over
    // (#833 section 8): the default is 3, and a one- or two-node cluster must
    // still place successfully rather than record owners that do not exist.
    // write_partitioned falls back to this node alone when `members` is empty.
    const std::size_t roster =
        options.members.empty() ? std::size_t{1} : options.members.size();
    const std::size_t replication_factor =
        std::min(std::max<std::size_t>(options.replication_factor, 1), roster);

    PartitionRequest ask;
    ask.row_count = result.rows;
    // The scheduler targets a partition SIZE, so it needs bytes. The caller
    // rarely knows them, and serializing the whole dataset to find out would
    // double the write — sample instead (see estimate_table_bytes).
    ask.approx_bytes = options.approx_bytes > 0
                           ? options.approx_bytes
                           : estimate_table_bytes(*result.table);
    // With fetch affinity these rows are THIS node's own slice and stay here, so
    // the count is a per-node count — the plan is asked over the owning node
    // alone. Otherwise the placement roster is the cluster (or this node, when
    // the caller passed none).
    ask.members = options.fetch_affinity_owner.empty()
                      ? (options.members.empty()
                             ? std::vector<std::string>{node.node_id()}
                             : options.members)
                      : std::vector<std::string>{options.fetch_affinity_owner};
    ask.replication_factor = replication_factor;
    ask.source = options.source;
    ask.partition_count = options.partition_count;
    ask.telemetry = node.telemetry_snapshot();
    const PartitionScheduler& scheduler =
        options.scheduler ? *options.scheduler : default_partition_scheduler();
    result.plan = scheduler.plan(ask);

    // Fetch affinity (#833 section 6) when the caller pulled this slice itself:
    // the partitions stay on the node that produced them, so nothing shuffles
    // after the write. Otherwise HRW, which spreads a locally-loaded dataset.
    if (options.fetch_affinity_owner.empty()) {
        result.table->set_partitioner(std::make_shared<RendezvousPlacement>(
            result.plan.partition_count, replication_factor));
    } else {
        result.table->set_partitioner(std::make_shared<FetchAffinityPlacement>(
            result.plan.partition_count, options.fetch_affinity_owner,
            replication_factor));
    }
    PartitionWriteResult written = write_partitioned(
        node, *result.database, *result.table, options.members);
    if (!written.ok()) {
        // The table + its rows are already created (and visible); surface the
        // partition failure rather than swallowing it.
        result.error = "partition: " + written.error;
        return;
    }
    result.partitions = std::move(written.partitions);
}

// Record the completed add on the audit trail (#624), so it surfaces in the
// activity feed (system.activity) like every other completed action.
void audit_dataset_added(Node& node, const AddDatasetResult& result) {
    if (!result.ok() || !result.table) return;
    zetta::governance::AuditEventValue event;
    event.action = zetta::governance::AuditAction::DatasetAdded;
    event.target = result.table->id();
    event.detail = "dataset '" + result.table->name() + "' added: " +
                   std::to_string(result.rows) + " row(s), " +
                   std::to_string(result.partitions.size()) + " partition(s), " +
                   dataset_file::format_name(result.format);
    node.record_audit_event(std::move(event));
}

}  // namespace

AddDatasetResult add_dataset_file(Node& node, const fs::path& path,
                                  const AddDatasetOptions& options) {
    AddDatasetResult result;
    result.format = dataset_file::format_from_path(path);

    std::string error;
    std::vector<nlohmann::json> rows =
        decode_dataset_file(path, result.format, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }

    // Materialize into a table on the node.
    create_target_table(node, path, options, result);
    if (!result.table) return result;

    for (nlohmann::json& row : rows) result.table->add_row(std::move(row));
    result.rows = result.table->row_count();

    partition_target_table(node, options, result);
    audit_dataset_added(node, result);
    return result;
}

AddDatasetResult add_dataset_rows(Node& node, std::vector<nlohmann::json> rows,
                                  const AddDatasetOptions& options) {
    AddDatasetResult result;
    // In-memory rows are JSON documents — the same shape a jsonl file decodes
    // to — so the dataset records the jsonl format (only the audit note reads
    // it; there is no file to classify).
    result.format = dataset_file::Format::Jsonl;

    const std::string base =
        options.table_name.empty() ? "dataset" : options.table_name;
    create_named_target_table(node, base, options, result);
    if (!result.table) return result;

    for (nlohmann::json& row : rows) result.table->add_row(std::move(row));
    result.rows = result.table->row_count();

    partition_target_table(node, options, result);
    audit_dataset_added(node, result);
    return result;
}

AddDatasetResult add_dataset_content(Node& node, std::string_view content,
                                     dataset_file::Format format,
                                     const AddDatasetOptions& options) {
    AddDatasetResult result;
    result.format = format;
    if (content.size() > kMaxDatasetContentBytes) {
        result.error = "content is " + std::to_string(content.size()) +
                       " bytes; the most a dataset upload accepts is " +
                       std::to_string(kMaxDatasetContentBytes) +
                       " bytes (16 MiB). Put larger data in a file on the node "
                       "(add_dataset) or load it from a source connection "
                       "(create_dataset_from_query)";
        return result;
    }
    if (content.find_first_not_of(" \t\r\n") == std::string_view::npos) {
        result.error = "content is empty";
        return result;
    }

    // The same decoders a file of this format goes through — only where the
    // bytes came from differs.
    const std::string bytes(content);
    std::string error;
    std::vector<nlohmann::json> rows;
    switch (format) {
        case dataset_file::Format::Csv:   rows = decode_csv(bytes, error); break;
        case dataset_file::Format::Jsonl: rows = decode_jsonl(bytes, error); break;
        case dataset_file::Format::Json:  rows = decode_json(bytes, error); break;
        case dataset_file::Format::Parquet:
        case dataset_file::Format::Unknown:
            error = std::string("format '") + dataset_file::format_name(format) +
                    "' cannot be uploaded as text (use csv, jsonl, or json)";
            break;
    }
    if (!error.empty()) {
        result.error = error;
        return result;
    }
    if (rows.empty()) {
        result.error = "content decoded to no rows";
        return result;
    }

    const std::string base =
        options.table_name.empty() ? "dataset" : options.table_name;
    create_named_target_table(node, base, options, result);
    if (!result.table) return result;

    for (nlohmann::json& row : rows) result.table->add_row(std::move(row));
    result.rows = result.table->row_count();

    partition_target_table(node, options, result);
    audit_dataset_added(node, result);
    return result;
}

// --- AsyncDatasetAdd (#624) ---------------------------------------------------

// The state the spawned decode worker shares with the handle. Held by
// shared_ptr from both sides, so dropping the handle mid-decode is safe — the
// worker owns everything it touches and dies with the last reference.
struct AsyncDatasetAdd::DecodeState {
    fs::path path;
    dataset_file::Format format = dataset_file::Format::Unknown;
    std::atomic<std::size_t> rows_decoded{0};
    std::atomic<bool> cancel{false};
    std::atomic<bool> finished{false};
    std::vector<nlohmann::json> rows;  // written before `finished` flips
    std::string error;                 // same
};

namespace {

// The decode coroutine: pure file work on a runtime pool worker (no node
// access). Takes its state by value (shared_ptr) — the coroutine frame owns
// it, the spawn-precedent discipline against dangling captures.
zetta::runtime::task<> decode_dataset_task(
    std::shared_ptr<AsyncDatasetAdd::DecodeState> state) {
    DecodeProgress progress{&state->rows_decoded, &state->cancel};
    state->rows =
        decode_dataset_file(state->path, state->format, state->error, progress);
    co_return;
}

// Rows materialized per pump() — small enough that a slice stays well inside a
// frame at ~30fps idle, large enough that a million-row file lands in seconds.
constexpr std::size_t kRowsPerPump = 4096;

}  // namespace

std::shared_ptr<AsyncDatasetAdd> AsyncDatasetAdd::start(
    Node& node, std::filesystem::path path, AddDatasetOptions options) {
    return std::shared_ptr<AsyncDatasetAdd>(
        new AsyncDatasetAdd(node, std::move(path), std::move(options)));
}

AsyncDatasetAdd::AsyncDatasetAdd(Node& node, std::filesystem::path path,
                                 AddDatasetOptions options)
    : node_(node), options_(std::move(options)) {
    decode_ = std::make_shared<DecodeState>();
    decode_->path = std::move(path);
    decode_->format = dataset_file::format_from_path(decode_->path);
    result_.format = decode_->format;
    std::shared_ptr<DecodeState> state = decode_;
    zetta::runtime::spawn(
        decode_dataset_task(state),
        // Completion runs on a pool worker: flip the (atomic) flag the owner
        // loop polls via pump(); everything else is behind it (release/acquire).
        [state] { state->finished.store(true, std::memory_order_release); });
}

std::size_t AsyncDatasetAdd::rows_decoded() const {
    return decode_->rows_decoded.load(std::memory_order_acquire);
}

void AsyncDatasetAdd::request_cancel() {
    decode_->cancel.store(true, std::memory_order_release);
}

bool AsyncDatasetAdd::cancel_requested() const {
    return decode_->cancel.load(std::memory_order_acquire);
}

const std::filesystem::path& AsyncDatasetAdd::source_path() const {
    return decode_->path;
}

void AsyncDatasetAdd::pump() {
    switch (phase_) {
        case Phase::Decoding: {
            if (!decode_->finished.load(std::memory_order_acquire)) return;
            if (!decode_->error.empty()) {  // includes a mid-decode cancel
                result_.error = decode_->error;
                phase_ = Phase::Done;
                return;
            }
            if (cancel_requested()) {
                // The cancel landed after the decode finished: bail before
                // anything touches the node, so a cancelled add that never
                // materialized leaves no trace.
                result_.error = kCancelledError;
                phase_ = Phase::Done;
                return;
            }
            rows_total_ = decode_->rows.size();
            create_target_table(node_, decode_->path, options_, result_);
            if (!result_.table) {
                phase_ = Phase::Done;
                return;
            }
            phase_ = Phase::Materializing;
            return;  // rows start on the next pump — keep every slice bounded
        }
        case Phase::Materializing: {
            if (cancel_requested()) {
                // Nothing durable should survive a cancelled add: drop the
                // partially-populated table (it was created this add).
                if (result_.database && result_.table) {
                    result_.database->drop_table(result_.table->name());
                }
                result_.table.reset();
                result_.error = kCancelledError;
                phase_ = Phase::Done;
                return;
            }
            std::vector<nlohmann::json>& rows = decode_->rows;
            const std::size_t end =
                std::min(rows.size(), next_row_ + kRowsPerPump);
            for (; next_row_ < end; ++next_row_) {
                result_.table->add_row(std::move(rows[next_row_]));
            }
            if (next_row_ < rows.size()) return;  // more slices to go
            result_.rows = result_.table->row_count();
            phase_ = Phase::Partitioning;
            return;  // partition write gets its own pump
        }
        case Phase::Partitioning: {
            // One slice today: chunking inside write_partitioned is a later
            // slice. A cancel here is too late — the add completes.
            partition_target_table(node_, options_, result_);
            audit_dataset_added(node_, result_);
            phase_ = Phase::Done;
            return;
        }
        case Phase::Done: return;
    }
}

}  // namespace zetta::data