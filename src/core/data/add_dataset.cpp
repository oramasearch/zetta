#include "zetta/data/add_dataset.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/node.hpp"
#include "zetta/parquet/reader.hpp"
#include "zetta/data/partition_writer.hpp"
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

    // Float: let from_chars decide, but require it to consume the whole cell so a
    // value like "1.2.3" or "3 apples" stays a string, and require a digit so a
    // bare "." / "-" does not slip through.
    if (cell.find_first_of("0123456789") != std::string::npos) {
        double real = 0.0;
        const auto parsed = std::from_chars(begin, end, real);
        if (parsed.ec == std::errc() && parsed.ptr == end) return real;
    }

    return cell;  // plain string
}

// Decode a CSV file's bytes into rows: the first non-empty line is the header
// (column names), each subsequent line one object keyed by the header, cells
// type-inferred. A row with fewer cells than the header leaves the missing columns
// absent; extra cells are dropped. `error` is set (and {} returned) when there is
// no header row.
std::vector<nlohmann::json> decode_csv(const std::string& bytes,
                                       std::string& error) {
    std::vector<nlohmann::json> rows;
    std::istringstream stream(bytes);
    std::string line;
    std::vector<std::string> header;
    while (std::getline(stream, line)) {
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
    }
    if (header.empty()) error = "csv file has no header row";
    return rows;
}

// Decode a JSON Lines file: one JSON document per line, blank lines skipped. A
// line that does not parse fails the whole load (all-or-nothing, like
// import_records).
std::vector<nlohmann::json> decode_jsonl(const std::string& bytes,
                                         std::string& error) {
    std::vector<nlohmann::json> rows;
    std::istringstream stream(bytes);
    std::string line;
    std::size_t line_number = 0;
    while (std::getline(stream, line)) {
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

}  // namespace

AddDatasetResult add_dataset_file(Node& node, const fs::path& path,
                                  const AddDatasetOptions& options) {
    AddDatasetResult result;
    result.format = dataset_file::format_from_path(path);

    if (result.format == dataset_file::Format::Unknown) {
        result.error =
            "unsupported file type '" + path.extension().string() +
            "' (add a parquet, jsonl, json, or csv file)";
        return result;
    }

    std::string error;
    const std::string bytes = read_file_bytes(path, error);
    if (!error.empty()) {
        result.error = error;
        return result;
    }

    // Decode the bytes into rows by format.
    std::vector<nlohmann::json> rows;
    switch (result.format) {
        case dataset_file::Format::Parquet: rows = decode_parquet(bytes, error); break;
        case dataset_file::Format::Jsonl:   rows = decode_jsonl(bytes, error); break;
        case dataset_file::Format::Json:    rows = decode_json(bytes, error); break;
        case dataset_file::Format::Csv:     rows = decode_csv(bytes, error); break;
        case dataset_file::Format::Unknown: break;  // handled above
    }
    if (!error.empty()) {
        result.error = error;
        return result;
    }

    // Materialize into a table on the node.
    std::shared_ptr<Database> database = node.find_database(options.database);
    if (!database) database = node.create_database(options.database);
    if (!database) {
        result.error = "could not create database '" + options.database + "'";
        return result;
    }
    result.database = database;

    const std::string base =
        options.table_name.empty() ? derive_table_name(path) : options.table_name;
    const std::string table_name = unique_table_name(*database, base);
    std::shared_ptr<Table> table = database->create_table(table_name);
    if (!table) {
        result.error = "could not create table '" + table_name + "'";
        return result;
    }
    result.table = table;

    for (nlohmann::json& row : rows) table->add_row(std::move(row));
    result.rows = table->row_count();

    // Partition the populated table so its slices are placed across the cluster.
    // RendezvousPlacement shards rows into P partitions and HRW-places each onto
    // specific owner node(s) — so the placement actually spreads (vs. the
    // replicate-to-all strategies). Skip an empty dataset (nothing to place).
    if (options.partitioned && result.rows > 0) {
        table->set_partitioner(std::make_shared<RendezvousPlacement>(
            options.partition_count, options.replication_factor));
        PartitionWriteResult written =
            write_partitioned(node, *database, *table, options.members);
        if (!written.ok()) {
            // The table + its rows are already created (and visible); surface the
            // partition failure rather than swallowing it.
            result.error = "partition: " + written.error;
            return result;
        }
        result.partitions = std::move(written.partitions);
    }

    return result;
}

}  // namespace zetta::data