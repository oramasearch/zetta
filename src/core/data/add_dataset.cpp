#include "zetta/data/add_dataset.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/large_object.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/parquet/metadata.hpp"
#include "zetta/parquet/reader.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/runtime.hpp"
#include "zetta/util/timestamp.hpp"
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
// `closed` is false when a quoted field runs past the end of the line.
std::vector<std::string> parse_csv_line(std::string_view line, bool& closed) {
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
    closed = !in_quotes;
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

// The order a JSON source writes its keys in (#1021). A parsed object reads its
// keys back sorted, so when `row` brings a key not yet in `columns`, `text` (the
// row's own JSON) is read again preserving order and its new keys are appended
// in the order written. Usually only the first row is read twice.
void note_key_order(const nlohmann::json& row, std::string_view text,
                    std::vector<std::string>& columns,
                    std::unordered_set<std::string>& seen) {
    if (!row.is_object()) return;
    bool fresh = false;
    for (auto field = row.begin(); field != row.end() && !fresh; ++field) {
        fresh = !seen.contains(field.key());
    }
    if (!fresh) return;
    const nlohmann::ordered_json ordered = nlohmann::ordered_json::parse(
        text.begin(), text.end(), nullptr, /*allow_exceptions=*/false);
    if (ordered.is_discarded() || !ordered.is_object()) return;
    for (auto field = ordered.begin(); field != ordered.end(); ++field) {
        if (seen.insert(field.key()).second) columns.push_back(field.key());
    }
}

// Whether `text` is well-formed UTF-8 (RFC 3629: no overlong forms, no
// surrogates, nothing past U+10FFFF). nlohmann's serializer refuses anything
// else by throwing when a row is written to the table's store, so the decoder
// refuses such a line first.
bool valid_utf8(std::string_view text) {
    std::size_t index = 0;
    while (index < text.size()) {
        const auto lead = static_cast<unsigned char>(text[index]);
        if (lead < 0x80) {
            ++index;
            continue;
        }
        // The sequence length, and the range its second byte must fall in.
        std::size_t length = 0;
        unsigned char second_low = 0x80;
        unsigned char second_high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
        } else if (lead == 0xE0) {
            length = 3;
            second_low = 0xA0;  // no overlong form
        } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
            length = 3;
        } else if (lead == 0xED) {
            length = 3;
            second_high = 0x9F;  // no surrogate
        } else if (lead == 0xF0) {
            length = 4;
            second_low = 0x90;  // no overlong form
        } else if (lead >= 0xF1 && lead <= 0xF3) {
            length = 4;
        } else if (lead == 0xF4) {
            length = 4;
            second_high = 0x8F;  // nothing past U+10FFFF
        } else {
            return false;
        }
        if (text.size() - index < length) return false;
        const auto second = static_cast<unsigned char>(text[index + 1]);
        if (second < second_low || second > second_high) return false;
        for (std::size_t offset = 2; offset < length; ++offset) {
            const auto continuation = static_cast<unsigned char>(text[index + offset]);
            if (continuation < 0x80 || continuation > 0xBF) return false;
        }
        index += length;
    }
    return true;
}

}  // namespace

DatasetLineDecoder::DatasetLineDecoder(dataset_file::Format format) : format_(format) {}

bool DatasetLineDecoder::line(std::string_view text, std::vector<nlohmann::json>& rows) {
    return decode(text, &rows);
}

bool DatasetLineDecoder::check(std::string_view text) { return decode(text, nullptr); }

bool DatasetLineDecoder::refuse(std::string reason) {
    error_ = std::move(reason);
    return false;
}

// CSV: the first non-empty line is the header (column names), each later
// non-empty line one row. Refuses (#1221), naming the 1-based line, a header
// column with no name or named twice, a quoted field left open at the end of
// its line, and a row whose field count is not the header's — a ragged row used
// to be padded with nulls or cut short without a word. The wording is lake's
// (lake PR #459), so a caller reads one answer from either layer. Each row is
// an object keyed by the header, cells type-inferred.
//
// JSONL: one JSON document per line, whitespace-only lines skipped. A line that
// does not parse fails the whole load (all-or-nothing, like import_records).
//
// Either format: a line that is not well-formed UTF-8 is refused first.
bool DatasetLineDecoder::decode(std::string_view text, std::vector<nlohmann::json>* rows) {
    if (!error_.empty()) return false;
    ++lines_;
    if (!text.empty() && text.back() == '\r') text.remove_suffix(1);  // CRLF
    if (!valid_utf8(text)) return refuse("line " + std::to_string(lines_) + ": not valid UTF-8");
    if (format_ == dataset_file::Format::Jsonl) {
        if (text.find_first_not_of(" \t") == std::string_view::npos) return true;
        auto value = nlohmann::json::parse(text.begin(), text.end(), nullptr,
                                           /*allow_exceptions=*/false);
        if (value.is_discarded()) {
            return refuse("invalid JSON on line " + std::to_string(lines_));
        }
        note_key_order(value, text, columns_, seen_);  // the source's order (#1021)
        if (rows) rows->push_back(std::move(value));
        return true;
    }
    if (format_ != dataset_file::Format::Csv) {
        return refuse(std::string("format '") + dataset_file::format_name(format_) +
                      "' is not decoded a line at a time");
    }
    if (text.empty()) return true;  // blank lines carry no row
    bool closed = true;
    std::vector<std::string> fields = parse_csv_line(text, closed);
    const std::string at = "line " + std::to_string(lines_) + ": ";
    if (!closed) {
        return refuse(at + "a quoted field is not closed (a row must stay on one line)");
    }
    if (!header_seen_) {
        header_seen_ = true;
        std::set<std::string> names;
        for (std::size_t index = 0; index < fields.size(); ++index) {
            if (fields[index].empty()) {
                return refuse(at + "header column " + std::to_string(index + 1) +
                              " has no name");
            }
            if (!names.insert(fields[index]).second) {
                return refuse(at + "the header names column '" + fields[index] + "' twice");
            }
        }
        header_ = std::move(fields);
        columns_ = header_;  // the file's own order (#1021)
        return true;
    }
    if (fields.size() != header_.size()) {
        return refuse(at + std::to_string(fields.size()) + " fields, but the header has " +
                      std::to_string(header_.size()) + " columns");
    }
    if (!rows) return true;
    nlohmann::json row = nlohmann::json::object();
    for (std::size_t column = 0; column < header_.size(); ++column) {
        row[header_[column]] = csv_cell_to_json(fields[column]);
    }
    rows->push_back(std::move(row));
    return true;
}

bool DatasetLineDecoder::finish() {
    if (!error_.empty()) return false;
    if (format_ == dataset_file::Format::Csv && !header_seen_) {
        return refuse("the file has no header row");
    }
    return true;
}

namespace {

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

// Decode an in-memory CSV or JSONL text through DatasetLineDecoder, a
// '\n'-separated line at a time (a text ending in '\n' has no empty last
// line). With `rows`, each row is decoded into it; without, the text is only
// checked. With `columns`, the source's column order is written to it (#1021).
// On a refusal `error` is set and `rows` is cleared.
void decode_lines(std::string_view bytes, dataset_file::Format format, std::string& error,
                  std::vector<nlohmann::json>* rows, const DecodeProgress& progress = {},
                  std::vector<std::string>* columns = nullptr) {
    DatasetLineDecoder decoder(format);
    std::size_t start = 0;
    while (start < bytes.size() && error.empty()) {
        if (progress.cancelled()) {
            error = kCancelledError;
            break;
        }
        std::size_t end = bytes.find('\n', start);
        if (end == std::string_view::npos) end = bytes.size();
        const std::string_view line = bytes.substr(start, end - start);
        start = end + 1;
        if (!(rows ? decoder.line(line, *rows) : decoder.check(line))) {
            error = decoder.error();
            break;
        }
        if (rows) progress.count(rows->size());
    }
    if (error.empty() && !decoder.finish()) error = decoder.error();
    if (!error.empty() && rows) rows->clear();
    if (error.empty() && columns) *columns = decoder.columns();
}

// Decode a CSV file's bytes into rows (see DatasetLineDecoder for what is
// refused).
std::vector<nlohmann::json> decode_csv(const std::string& bytes,
                                       std::string& error,
                                       const DecodeProgress& progress = {},
                                       std::vector<std::string>* columns = nullptr) {
    std::vector<nlohmann::json> rows;
    decode_lines(bytes, dataset_file::Format::Csv, error, &rows, progress, columns);
    return rows;
}

// Decode a JSON Lines file: one JSON document per line, blank lines skipped
// (see DatasetLineDecoder).
std::vector<nlohmann::json> decode_jsonl(const std::string& bytes,
                                         std::string& error,
                                         const DecodeProgress& progress = {},
                                         std::vector<std::string>* columns = nullptr) {
    std::vector<nlohmann::json> rows;
    decode_lines(bytes, dataset_file::Format::Jsonl, error, &rows, progress, columns);
    return rows;
}

// Decode a JSON file: an array becomes one row per element, any other value a
// single row. A parse error fails the load.
std::vector<nlohmann::json> decode_json(const std::string& bytes,
                                        std::string& error,
                                        std::vector<std::string>* columns = nullptr) {
    auto value = nlohmann::json::parse(bytes, nullptr, /*allow_exceptions=*/false);
    if (value.is_discarded()) {
        error = "invalid JSON";
        return {};
    }
    // The key order the document writes (#1021): read once more, preserving it.
    if (columns) {
        const nlohmann::ordered_json ordered =
            nlohmann::ordered_json::parse(bytes, nullptr, /*allow_exceptions=*/false);
        std::unordered_set<std::string> seen;
        const auto note = [&](const nlohmann::ordered_json& element) {
            if (!element.is_object()) return;
            for (auto field = element.begin(); field != element.end(); ++field) {
                if (seen.insert(field.key()).second) columns->push_back(field.key());
            }
        };
        if (ordered.is_array()) {
            for (const nlohmann::ordered_json& element : ordered) note(element);
        } else if (!ordered.is_discarded()) {
            note(ordered);
        }
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
                                           std::string& error,
                                           std::vector<std::string>* columns = nullptr) {
    parquet::ParquetRowsResult parsed = parquet::read_parquet_rows(bytes);
    if (!parsed.ok()) {
        error = "parquet: " + parsed.error;
        return {};
    }
    // The file's schema order (#1021): its leaf columns, as the footer lists them.
    if (columns) {
        const parquet::ParquetMetadataResult metadata = parquet::parse_parquet_metadata(bytes);
        if (metadata.ok()) {
            for (const parquet::LeafColumn& leaf : metadata.metadata.leaf_columns()) {
                columns->push_back(leaf.path);
            }
        }
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
    const DecodeProgress& progress = {},
    std::vector<std::string>* columns = nullptr) {
    const std::string bytes = read_file_bytes(path, error);
    if (!error.empty()) return {};
    std::vector<nlohmann::json> rows;
    switch (format) {
        case dataset_file::Format::Parquet:
            rows = decode_parquet(bytes, error, columns);
            break;
        case dataset_file::Format::Jsonl:
            rows = decode_jsonl(bytes, error, progress, columns);
            break;
        case dataset_file::Format::Json: rows = decode_json(bytes, error, columns); break;
        case dataset_file::Format::Csv:
            rows = decode_csv(bytes, error, progress, columns);
            break;
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
                               AddDatasetResult& result,
                               const std::vector<std::string>& decoded_columns = {}) {
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
        table_name, std::nullopt, options.partition_only, result.source_format);
    if (!table) {
        result.error = "could not create table '" + table_name + "'";
        return;
    }
    // The source's column order (#1021): the caller's, else the decoder's.
    table->set_column_order(options.columns.empty() ? decoded_columns : options.columns);
    result.table = table;
}

// Create (or find) the target database + a uniquely named table for `path`
// (the file path's base name, unless overridden). Fills result.database /
// result.table; on failure sets result.error.
void create_target_table(Node& node, const fs::path& path,
                         const AddDatasetOptions& options,
                         AddDatasetResult& result,
                         const std::vector<std::string>& decoded_columns = {}) {
    const std::string base =
        options.table_name.empty() ? derive_table_name(path) : options.table_name;
    create_named_target_table(node, base, options, result, decoded_columns);
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
    // What the rows came from (#1221): a connector pull names its connector
    // rather than the `jsonl` its in-memory rows happen to be shaped as.
    event.detail = "dataset '" + result.table->name() + "' added: " +
                   std::to_string(result.rows) + " row(s), " +
                   std::to_string(result.partitions.size()) + " partition(s), " +
                   (result.source_format.empty()
                        ? std::string(dataset_file::format_name(result.format))
                        : result.source_format);
    node.record_audit_event(std::move(event));
}

// Words the SQL subset reads as syntax, so a table named one cannot be queried
// unquoted — the same list lake checks (lake PR #459).
constexpr std::array<std::string_view, 25> kSqlKeywords = {
    "all",    "and",   "as",    "asc",  "by",    "desc",   "distinct", "from",  "group",
    "having", "in",    "is",    "join", "like",  "limit",  "not",      "null",  "offset",
    "on",     "or",    "order", "select", "union", "where", "with"};

}  // namespace

std::string dataset_name_problem(std::string_view name) {
    if (name.empty()) return "a name is required";
    if (name.size() > 63) return "a name is at most 63 characters";
    const auto letter = [](char character) {
        return std::isalpha(static_cast<unsigned char>(character)) != 0 ||
               character == '_';
    };
    const std::string quoted = "'" + std::string(name) + "'";
    if (!letter(name.front())) {
        return quoted + " must start with a letter or an underscore";
    }
    for (const char character : name) {
        if (!letter(character) &&
            std::isdigit(static_cast<unsigned char>(character)) == 0) {
            return quoted + " can hold only letters, digits and underscores (not '" +
                   std::string(1, character) + "'), so SQL can query it";
        }
    }
    std::string lowered(name);
    std::ranges::transform(lowered, lowered.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    if (std::ranges::find(kSqlKeywords, lowered) != kSqlKeywords.end()) {
        return quoted + " is a SQL keyword; choose another name";
    }
    return {};
}

std::string csv_problem(std::string_view content) {
    std::string error;
    decode_lines(content, dataset_file::Format::Csv, error, nullptr);
    return error;
}

std::string dataset_name_taken(std::string_view table, std::string_view database) {
    return "a dataset named '" + std::string(table) + "' already exists in " +
           std::string(database) +
           "; choose another name, or pass if_exists: \"rename\" to save it under the "
           "next free name";
}

AddDatasetResult add_dataset_file(Node& node, const fs::path& path,
                                  const AddDatasetOptions& options) {
    AddDatasetResult result;
    result.format = dataset_file::format_from_path(path);
    result.source_format = options.source_format.empty()
                               ? dataset_file::format_name(result.format)
                               : options.source_format;

    std::string error;
    std::vector<std::string> columns;
    std::vector<nlohmann::json> rows =
        decode_dataset_file(path, result.format, error, {}, &columns);
    if (!error.empty()) {
        result.error = error;
        return result;
    }

    // Materialize into a table on the node.
    create_target_table(node, path, options, result, columns);
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
    result.source_format =
        options.source_format.empty() ? "rows" : options.source_format;

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
    result.source_format = options.source_format.empty()
                               ? dataset_file::format_name(format)
                               : options.source_format;
    if (content.size() > kMaxDatasetContentBytes) {
        result.error = "content is " + std::to_string(content.size()) +
                       " bytes; the most a dataset upload accepts is " +
                       std::to_string(kMaxDatasetContentBytes) +
                       " bytes (16 MiB). Upload a larger file to a bucket and load "
                       "it with load_dataset_object";
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
    std::vector<std::string> columns;
    std::vector<nlohmann::json> rows;
    switch (format) {
        case dataset_file::Format::Csv:   rows = decode_csv(bytes, error, {}, &columns); break;
        case dataset_file::Format::Jsonl: rows = decode_jsonl(bytes, error, {}, &columns); break;
        case dataset_file::Format::Json:  rows = decode_json(bytes, error, &columns); break;
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
    create_named_target_table(node, base, options, result, columns);
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
    std::vector<std::string> columns;  // same: the source's column order (#1021)
    std::string error;                 // same
};

namespace {

// The decode coroutine: pure file work on a runtime pool worker (no node
// access). Takes its state by value (shared_ptr) — the coroutine frame owns
// it, the spawn-precedent discipline against dangling captures.
zetta::runtime::task<> decode_dataset_task(
    std::shared_ptr<AsyncDatasetAdd::DecodeState> state) {
    DecodeProgress progress{&state->rows_decoded, &state->cancel};
    state->rows = decode_dataset_file(state->path, state->format, state->error,
                                      progress, &state->columns);
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
    result_.source_format = options_.source_format.empty()
                                ? dataset_file::format_name(result_.format)
                                : options_.source_format;
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
            create_target_table(node_, decode_->path, options_, result_,
                                decode_->columns);
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

// --- AsyncObjectDatasetLoad (#1302) ------------------------------------------

struct AsyncObjectDatasetLoad::State {
    State(Node& node, ObjectStore& objects) : node(node), objects(objects) {}

    Node& node;
    ObjectStore& objects;
    ObjectDatasetSource source;
    AddDatasetOptions options;
    std::shared_ptr<zetta::pipeline::Run> run;  // touched on the owner thread only
    AddDatasetResult result;                     // written on the owner thread
    std::string table_id;  // the table the first batch created (owner thread)
    // The job span, parent of the phase spans (set by the worker first thing).
    zetta::observability::TraceContext trace;
    // Owner-thread time spent landing batches, for the materialize span.
    std::size_t batches = 0;
    double materialize_ms = 0.0;
    std::string materialize_started;
    std::atomic<bool> stop{false};      // the handle is going away
    std::atomic<bool> finished{false};  // the worker has let go of everything
};

namespace {

using LoadState = AsyncObjectDatasetLoad::State;

// Rows handed to the owner thread at a time: a batch is the only decoded rows
// held outside the table.
constexpr std::size_t kRowsPerBatch = 10'000;
constexpr std::uint64_t kMebibyte = std::uint64_t{1024} * 1024;

constexpr const char* kStoppedError = "the load was stopped before it finished";
constexpr const char* kNodeStoppedError = "the node stopped before the load finished";

// `value` with its thousands grouped by commas: 1204331 -> "1,204,331".
std::string grouped(std::uint64_t value) {
    std::string digits = std::to_string(value);
    for (std::size_t at = digits.size(); at > 3; at -= 3) digits.insert(at - 3, ",");
    return digits;
}

// Run `work` (which returns the failure, or empty) on the owner thread. What it
// throws becomes the failure: run_on_owner work must not throw, and an
// exception unwinding out of the owner loop's tick would take the node down or
// leave this worker waiting on a completion that never comes.
template <class Work>
std::string on_owner(LoadState& state, Work&& work) {
    std::string failure;
    state.node.run_on_owner([&] {
        try {
            failure = work();
        } catch (const std::exception& error) {
            failure = std::string("the load failed: ") + error.what();
        } catch (...) {
            failure = "the load failed on an unknown error";
        }
    });
    return failure;
}

// Add one entry to the run's log, on the owner thread.
void log_run(LoadState& state, std::string level, std::string message) {
    on_owner(state, [&] {
        state.run->add_log({zetta::util::now_iso8601(), std::move(level), std::move(message)});
        return std::string();
    });
}

// Empty while the database still maps the load's table name to the table the
// load created; otherwise the failure. Owner thread. Another caller can drop
// the table (delete_dataset refuses while the run reads it, a SQL DROP TABLE
// does not) or drop it and create another under the same name.
std::string table_removed(const LoadState& state) {
    if (state.table_id.empty()) return {};
    const AddDatasetResult& result = state.result;
    const std::shared_ptr<Database> database = state.node.find_database(result.database->name());
    const std::shared_ptr<Table> table =
        database ? database->find_table(result.table->name()) : nullptr;
    if (table && table->id() == state.table_id) return {};
    return "the table " + result.database->name() + "." + result.table->name() +
           " was removed while the load ran";
}

// Remove what a failed load wrote: the table's partition catalog entries and
// their bodies (as delete_dataset and a discarded ingest do), then the table,
// found by id, so a table created under the same name meanwhile is never
// dropped. Owner thread; it never calls the object store.
void discard_loaded_table(Node& node, const std::string& database_name,
                          const std::string& table_id) {
    const std::shared_ptr<Database> database = node.find_database(database_name);
    for (const std::shared_ptr<DatasetPartition>& partition :
         node.dataset_partitions()) {  // a copy: the loop drops entries
        if (!partition || partition->table() != table_id) continue;
        if (database && !partition->blob_ref().empty()) {
            database->large_objects().unlink(partition->blob_ref());
        }
        node.drop_dataset_partition(partition->id());
    }
    if (!database) return;
    if (const std::shared_ptr<Table> table = database->find_table_by_id(table_id)) {
        database->drop_table(table->name());
    }
}

// Hand `batch` to the owner thread and wait for it to land. The first batch
// creates the table, honouring `rename_if_exists`, and records it in the run's
// reads; a later one first checks the table is still the load's. Returns the
// failure, or empty. `batch` is empty afterwards.
std::string land_batch(LoadState& state, std::vector<nlohmann::json>& batch) {
    std::string failure = on_owner(state, [&]() -> std::string {
        const auto started = std::chrono::steady_clock::now();
        if (state.materialize_started.empty()) {
            state.materialize_started = zetta::util::now_iso8601();
        }
        AddDatasetResult& result = state.result;
        if (state.table_id.empty()) {
            const AddDatasetOptions& options = state.options;
            if (!state.source.rename_if_exists) {
                // The handler checked this too, but the name can be taken while
                // the object is read.
                const std::shared_ptr<Database> database = state.node.find_database(options.database);
                if (database && database->find_table(options.table_name)) {
                    return dataset_name_taken(options.table_name, options.database);
                }
            }
            create_named_target_table(state.node, options.table_name, options, result);
            if (!result.table) return result.error;
            state.table_id = result.table->id();
            // delete_dataset refuses a table a running run reads (zetta#1258),
            // and recovery finds the table to drop here.
            state.run->add_reads(
                {zetta::pipeline::TableRead{result.database->name(), result.table->name(),
                                            state.table_id}});
        } else if (std::string removed = table_removed(state); !removed.empty()) {
            return removed;
        }
        for (nlohmann::json& row : batch) result.table->add_row(std::move(row));
        ++state.batches;
        state.materialize_ms += std::chrono::duration<double, std::milli>(
                                    std::chrono::steady_clock::now() - started)
                                    .count();
        return {};
    });
    batch.clear();
    return failure;
}

// Read the object a slice at a time, decode it line by line and land the rows
// in batches (the worker phase). Returns the failure, or empty.
std::string read_object_rows(LoadState& state, zetta::observability::ScopedSpan& span) {
    const ObjectDatasetSource& source = state.source;
    std::shared_ptr<ObjectReader> reader;
    const ObjectOutcome opened =
        state.objects.open_read(source.bucket, source.key, 0, std::nullopt, reader);
    if (!opened.ok() || !reader) {
        if (opened.status == ObjectStatus::NotFound) {
            return "no object " + source.key + " in bucket " + source.bucket;
        }
        return "could not open " + source.bucket + "/" + source.key + ": " + opened.message;
    }
    if (source.wrap_reader) reader = source.wrap_reader(std::move(reader));
    const std::uint64_t size = reader->length();
    span.set_attribute("bytes", size);
    log_run(state, "info",
            "loading " + source.bucket + "/" + source.key + ": " + std::to_string(size) +
                " bytes as " + dataset_file::format_name(source.format));
    if (size > source.max_bytes) {
        return "the object is " + std::to_string(size) +
               " bytes; a dataset loads from at most " + std::to_string(source.max_bytes) +
               " bytes";
    }
    if (size == 0) return "the object is empty";
    state.options.approx_bytes = static_cast<std::size_t>(size);

    DatasetLineDecoder decoder(source.format);
    std::vector<nlohmann::json> batch;
    std::size_t rows = 0;
    const auto too_long = [&] {
        return "line " + std::to_string(decoder.lines() + 1) + " is longer than " +
               std::to_string(kMaxDatasetLineBytes / kMebibyte) + " MiB";
    };
    // Decode one complete line into the batch, landing the batch when full.
    const auto take_line = [&](std::string_view line) -> std::string {
        if (line.size() > kMaxDatasetLineBytes) return too_long();
        const std::size_t before = batch.size();
        if (!decoder.line(line, batch)) return decoder.error();
        rows += batch.size() - before;
        if (batch.size() >= kRowsPerBatch) return land_batch(state, batch);
        return {};
    };

    const std::uint64_t progress = std::max<std::uint64_t>(source.progress_bytes, 1);
    std::string slice;
    std::string carry;  // the partial last line of the slices read so far
    std::uint64_t next_progress = progress;
    std::size_t slices = 0;
    for (std::uint64_t offset = 0; offset < size;) {
        if (state.stop.load(std::memory_order_acquire)) return kStoppedError;
        const std::uint64_t count = std::min<std::uint64_t>(ObjectStore::kSliceBytes, size - offset);
        slice.clear();
        if (!reader->read(offset, count, slice) || slice.size() != count) {
            return "reading the object failed at byte " + std::to_string(offset);
        }
        offset += count;
        ++slices;
        const std::string_view view(slice);
        std::size_t start = 0;
        for (std::size_t end = view.find('\n'); end != std::string_view::npos;
             end = view.find('\n', start)) {
            const std::string_view piece = view.substr(start, end - start);
            start = end + 1;
            std::string failure;
            if (carry.empty()) {
                failure = take_line(piece);
            } else {
                // The size is checked before the carry grows, so it never
                // holds more than one line of at most kMaxDatasetLineBytes.
                if (carry.size() + piece.size() > kMaxDatasetLineBytes) return too_long();
                carry.append(piece);
                failure = take_line(carry);
                carry.clear();
            }
            if (!failure.empty()) return failure;
        }
        const std::string_view rest = view.substr(start);
        if (carry.size() + rest.size() > kMaxDatasetLineBytes) return too_long();
        carry.append(rest);
        if (offset >= next_progress && offset < size) {
            log_run(state, "info",
                    "read " + std::to_string(offset / kMebibyte) + " MiB of " +
                        std::to_string(size / kMebibyte) + " MiB, " + grouped(rows) + " rows");
            next_progress = (offset / progress + 1) * progress;
        }
    }
    span.set_attribute("slices", slices);
    span.set_attribute("rows", rows);
    if (state.stop.load(std::memory_order_acquire)) return kStoppedError;
    if (!carry.empty()) {  // a last line with no newline
        if (std::string failure = take_line(carry); !failure.empty()) return failure;
    }
    if (!decoder.finish()) return decoder.error();
    if (!batch.empty()) {
        if (std::string failure = land_batch(state, batch); !failure.empty()) return failure;
    }
    if (rows == 0) return "the object decoded to no rows";
    return {};
}

// Settle a failed load on the owner thread: nothing of it survives, and the run
// ends Failed with the reason.
void settle_failed(LoadState& state, const std::string& failure) {
    AddDatasetResult& result = state.result;
    if (!state.table_id.empty()) {
        discard_loaded_table(state.node, result.database->name(), state.table_id);
    }
    result.table.reset();
    result.partitions.clear();
    result.error = failure;
    state.run->set_outputs({});
    state.run->add_log({zetta::util::now_iso8601(), "error", failure});
    state.run->set_status(zetta::pipeline::RunStatus::Failed);
}

// Settle a load whose rows all landed: partition the table, release the
// loading node's copy of the rows, and end the run Succeeded with the dataset
// as its output. Returns the failure, or empty. Owner thread.
std::string settle_loaded(LoadState& state) {
    if (std::string removed = table_removed(state); !removed.empty()) return removed;
    AddDatasetResult& result = state.result;
    const AddDatasetOptions& options = state.options;
    result.rows = result.table->row_count();
    {
        zetta::observability::ScopedSpan span(&state.node.span_recorder(), state.trace,
                                              "load_dataset_object:partition");
        span.set_attribute("rows", result.rows);
        partition_target_table(state.node, options, result);
        span.set_attribute("partitions", result.partitions.size());
        if (result.error.empty() && state.source.after_partition_write) {
            result.error = state.source.after_partition_write();
        }
        span.finish(result.error.empty(), result.error);
        // A failure here can follow partitions already written: settle_failed
        // removes them with the table.
        if (!result.error.empty()) return result.error;
    }
    // The partition bodies hold the rows now; the local copy is read by nothing.
    result.table->release_local_rows();
    audit_dataset_added(state.node, result);
    const std::string& table = result.table->name();
    const std::string reference = result.database->name() + "." + table;
    state.run->set_outputs({zetta::pipeline::RunOutput{"dataset", table, reference}});
    std::string message = "loaded " + std::to_string(result.rows) + " rows into " +
                          reference + " in " + std::to_string(result.partitions.size()) +
                          " partitions";
    if (table != options.table_name) {
        message += " (asked for " + options.table_name + "; that name was taken)";
    }
    state.run->add_log({zetta::util::now_iso8601(), "info", std::move(message)});
    state.run->set_status(zetta::pipeline::RunStatus::Succeeded);
    return {};
}

// Partition the table and settle the run on the owner thread: Succeeded with
// the dataset as its output, or Failed with the reason and no table left.
void finish_object_load(LoadState& state, std::string failure) {
    if (failure.empty()) failure = on_owner(state, [&] { return settle_loaded(state); });
    if (failure.empty()) return;
    const std::string unsettled = on_owner(state, [&] {
        settle_failed(state, failure);
        return std::string();
    });
    if (!unsettled.empty()) state.result.error = failure + "; " + unsettled;
}

// Remove the object after the load, as delete_object does (the same store
// call on a worker), and record it in the audit trail as that capability's
// call would be. A failure adds a `warn` entry to the run.
void remove_loaded_object(LoadState& state) {
    const ObjectDatasetSource& source = state.source;
    const ObjectOutcome removed = state.objects.remove(source.bucket, source.key, ObjectWrite{});
    const std::string reason =
        removed.ok() ? std::string()
                     : (removed.message.empty() ? std::string(to_string(removed.status))
                                                : removed.message);
    on_owner(state, [&] {
        zetta::governance::AuditEventValue event;
        event.action = zetta::governance::AuditAction::DataAccess;
        event.actor = source.acting_key.empty() ? "local" : source.acting_key;
        event.target = "api:delete_object";
        event.allowed = true;
        event.detail = "bucket " + source.bucket + ": " + source.key +
                       (removed.ok() ? " removed" : " not removed (" + reason + ")") +
                       " after load_dataset_object run " + state.run->id();
        state.node.record_audit_event(std::move(event));
        if (!removed.ok()) {
            state.run->add_log({zetta::util::now_iso8601(), "warn",
                                "could not remove " + source.bucket + "/" + source.key + ": " +
                                    reason});
        }
        return std::string();
    });
}

// The worker: read, decode and land the rows, settle the run, then remove the
// object when asked. Takes its state by value (shared_ptr), the spawn-precedent
// discipline against dangling captures.
zetta::runtime::task<> load_object_task(std::shared_ptr<LoadState> state) {
    zetta::observability::SpanRecorder* recorder = &state->node.span_recorder();
    zetta::observability::ScopedSpan job(recorder, state->source.trace, "load_dataset_object");
    job.set_attribute("intent", "load " + state->source.bucket + "/" + state->source.key +
                                    " into " + state->options.database + "." +
                                    state->options.table_name);
    job.set_attribute("run_id", state->run->id());
    job.set_attribute("format", dataset_file::format_name(state->source.format));
    state->trace = {state->source.trace.trace_id, job.span_id()};

    std::string failure;
    {
        zetta::observability::ScopedSpan read(recorder, state->trace, "load_dataset_object:read");
        try {
            failure = read_object_rows(*state, read);
        } catch (const std::exception& error) {
            failure = std::string("the load failed: ") + error.what();
        } catch (...) {
            failure = "the load failed on an unknown error";
        }
        read.finish(failure.empty(), failure);
    }
    if (state->batches > 0) {
        // Landing is interleaved with reading, so its span is the owner-thread
        // time summed over the batches rather than a wall-clock interval.
        zetta::observability::Span materialize;
        materialize.trace_id = state->trace.trace_id;
        materialize.span_id = zetta::observability::make_span_id();
        materialize.parent_span_id = state->trace.span_id;
        materialize.name = "load_dataset_object:materialize";
        materialize.started_at = state->materialize_started;
        materialize.duration_ms = state->materialize_ms;
        materialize.attributes = {{"batches", state->batches}};
        recorder->record(std::move(materialize));
    }
    finish_object_load(*state, std::move(failure));
    if (state->source.remove_object) remove_loaded_object(*state);
    job.set_attribute("result_shape", {{"rows", state->result.rows},
                                       {"partitions", state->result.partitions.size()}});
    job.finish(state->result.ok(), state->result.error);
    co_return;
}

}  // namespace

std::shared_ptr<AsyncObjectDatasetLoad> AsyncObjectDatasetLoad::start(
    Node& node, ObjectStore& objects, ObjectDatasetSource source, AddDatasetOptions options) {
    std::shared_ptr<AsyncObjectDatasetLoad> handle(new AsyncObjectDatasetLoad());
    auto state = std::make_shared<State>(node, objects);
    state->source = std::move(source);
    state->options = std::move(options);
    if (state->options.table_name.empty()) state->options.table_name = "dataset";
    if (!state->source.trace.valid()) {
        state->source.trace = zetta::observability::make_root_trace();
    }
    state->options.source_format = dataset_file::format_name(state->source.format);
    // The loaded table is partition-only (#833): its rows live in the partition
    // bodies on their owners (replication_factor copies) and a SELECT is
    // scatter-gather, never captured row by row into the replicated document.
    // Capturing them made a 67 MiB CSV (1.4M rows) take 126 s, add 5.8 GB of
    // node memory and stall the owner loop 8.5 s; partition-only took 41 s,
    // 1.7 GB and 2.7 s.
    state->options.partition_only = true;
    state->result.format = state->source.format;
    state->result.source_format = state->options.source_format;

    // On the owner thread by contract, so the durable handle exists and is
    // Running before start() returns.
    state->run = node.create_run("datasets", std::string(kObjectDatasetLoadGraph));
    if (!state->run) return nullptr;
    state->run->set_trace_id(state->source.trace.trace_id);
    // The request, and the node running it: recover_object_dataset_loads
    // settles only the runs its own node left Running.
    const ObjectDatasetSource& request = state->source;
    if (!state->run->log_params({{"node", node.node_id()},
                                 {"bucket", request.bucket},
                                 {"key", request.key},
                                 {"format", dataset_file::format_name(request.format)},
                                 {"database", state->options.database},
                                 {"table_name", state->options.table_name},
                                 {"rename_if_exists", request.rename_if_exists},
                                 {"remove_object", request.remove_object}})) {
        node.drop_run(state->run->id());
        return nullptr;
    }
    state->run->set_status(zetta::pipeline::RunStatus::Running);
    handle->run_id_ = state->run->id();
    handle->state_ = state;
    zetta::runtime::spawn(load_object_task(state), [state] {
        state->finished.store(true, std::memory_order_release);
    });
    return handle;
}

AsyncObjectDatasetLoad::~AsyncObjectDatasetLoad() {
    if (!state_) return;
    // The worker holds the node until it finishes and hands its batches to the
    // owner queue: ask it to stop, and pump the queue while it winds down, so
    // dropping an unfinished handle on the owner thread cannot deadlock.
    state_->stop.store(true, std::memory_order_release);
    while (!state_->finished.load(std::memory_order_acquire)) {
        state_->node.drain_owner_work();
        std::this_thread::yield();
    }
}

bool AsyncObjectDatasetLoad::done() const {
    return state_->finished.load(std::memory_order_acquire);
}

const AddDatasetResult& AsyncObjectDatasetLoad::result() const { return state_->result; }

std::size_t recover_object_dataset_loads(Node& node) {
    std::size_t settled = 0;
    for (const std::shared_ptr<zetta::pipeline::Run>& run : node.runs()) {
        if (!run || run->graph() != kObjectDatasetLoadGraph ||
            run->status() != zetta::pipeline::RunStatus::Running) {
            continue;
        }
        // Only this node's own loads: a peer may be loading right now.
        const nlohmann::json& params = run->value().metrics.at("params");
        const auto loading_node = params.find("node");
        if (loading_node == params.end() || !loading_node->is_string() ||
            loading_node->get<std::string>() != node.node_id()) {
            continue;
        }
        for (const zetta::pipeline::TableRead& read : run->reads()) {
            discard_loaded_table(node, read.database, read.table_id);
        }
        run->add_log({zetta::util::now_iso8601(), "error", kNodeStoppedError});
        run->set_status(zetta::pipeline::RunStatus::Failed);
        ++settled;
    }
    return settled;
}

}  // namespace zetta::data
