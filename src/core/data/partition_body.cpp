// SPDX-License-Identifier: MIT

#include "zetta/data/partition_body.hpp"

#include <cstddef>
#include <sstream>
#include <unordered_map>
#include <utility>

#include "zetta/parquet/reader.hpp"
#include "zetta/parquet/writer.hpp"

namespace zetta::data {
namespace {

constexpr std::string_view kMagic = "PAR1";
constexpr const char* kIdColumn = "_zid";

// The legacy body: one `{"_zid": <id>, "doc": <row>}` object per line, read the
// tolerant way JsonlStore::load does (a bare object is the row itself), but
// failing on the first line that is not a JSON object.
PartitionBodyReadResult read_jsonl(std::string_view body, std::string_view partition_id) {
    PartitionBodyReadResult out;
    out.format = PartitionBodyFormat::Jsonl;

    std::istringstream in{std::string(body)};
    std::string line;
    std::size_t line_no = 0;
    while (std::getline(in, line)) {
        ++line_no;
        if (!line.empty() && line.back() == '\r') line.pop_back();  // CRLF
        if (line.empty()) continue;
        nlohmann::json parsed =
            nlohmann::json::parse(line, /*cb=*/nullptr, /*allow_exceptions=*/false);
        if (parsed.is_discarded()) {
            out.rows.clear();
            out.error = "partition '" + std::string(partition_id) + "' line " +
                        std::to_string(line_no) + " is not valid JSON";
            return out;
        }
        PartitionRow row;
        if (parsed.is_object() && parsed.contains(kIdColumn) && parsed.contains("doc")) {
            const nlohmann::json& id = parsed[kIdColumn];
            row.id = id.is_string() ? id.get<std::string>()
                                    : std::string(partition_id) + "#" +
                                          std::to_string(line_no);
            row.doc = std::move(parsed["doc"]);
        } else if (parsed.is_object()) {
            row.id = std::string(partition_id) + "#" + std::to_string(line_no);
            row.doc = std::move(parsed);
        } else {
            out.rows.clear();
            out.error = "partition '" + std::string(partition_id) + "' line " +
                        std::to_string(line_no) + " is not a JSON object";
            return out;
        }
        out.rows.push_back(std::move(row));
    }
    return out;
}

// The JSON family of one value — the coarse type a JSON Lines body can honestly
// claim for a column (Parquet's physical types do not apply: the body fell back
// to JSON Lines precisely because no flat scalar schema fitted).
std::string_view json_family(const nlohmann::json& value) {
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_object()) return "object";
    if (value.is_array()) return "array";
    return "null";
}

// The schema of a JSON Lines body: every field any row carries, in first-seen
// order, with the family its values share ("mixed" when they do not, "null" when
// every value is null) and whether any row omits it or holds null for it.
std::vector<PartitionBodyColumn> jsonl_schema(const std::vector<PartitionRow>& rows) {
    std::vector<PartitionBodyColumn> columns;
    std::unordered_map<std::string, std::size_t> index;
    const auto note = [&](const std::string& name, const nlohmann::json& value) {
        auto slot = index.find(name);
        if (slot == index.end()) {
            slot = index.emplace(name, columns.size()).first;
            columns.push_back(PartitionBodyColumn{name, std::string(json_family(value)),
                                                  /*optional=*/false});
        }
        PartitionBodyColumn& column = columns[slot->second];
        const std::string_view family = json_family(value);
        if (family == "null") {
            column.optional = true;
        } else if (column.type == "null") {
            column.type = std::string(family);
        } else if (column.type != family) {
            // An integer column that later sees a real number widens; anything
            // else genuinely mixes.
            const bool numeric = (column.type == "integer" || column.type == "number") &&
                                 (family == "integer" || family == "number");
            column.type = numeric ? "number" : "mixed";
        }
    };
    for (const PartitionRow& row : rows) {
        if (!row.doc.is_object()) continue;
        for (auto field = row.doc.begin(); field != row.doc.end(); ++field) {
            note(field.key(), field.value());
        }
    }
    // A field absent from some row reads back as null, so it is optional.
    for (PartitionBodyColumn& column : columns) {
        for (const PartitionRow& row : rows) {
            if (row.doc.is_object() && !row.doc.contains(column.name)) {
                column.optional = true;
                break;
            }
        }
    }
    columns.push_back(PartitionBodyColumn{kIdColumn, "string", /*optional=*/false});
    return columns;
}

std::string write_jsonl(const std::vector<PartitionRow>& rows) {
    std::string body;
    for (const PartitionRow& row : rows) {
        const nlohmann::json line{{kIdColumn, row.id}, {"doc", row.doc}};
        body += line.dump();
        body += '\n';
    }
    return body;
}

}  // namespace

std::string_view to_string(PartitionBodyFormat format) {
    switch (format) {
        case PartitionBodyFormat::Parquet: return "parquet";
        case PartitionBodyFormat::Jsonl:   return "jsonl";
    }
    return "parquet";
}

std::string_view body_extension(PartitionBodyFormat format) {
    switch (format) {
        case PartitionBodyFormat::Parquet: return ".parquet";
        case PartitionBodyFormat::Jsonl:   return ".jsonl";
    }
    return ".parquet";
}

PartitionBodyFormat sniff_partition_body(std::string_view body) {
    // The bytes decide, not the name: a body named `.jsonl` that starts and ends
    // with PAR1 is Parquet, and vice versa. Both magics are checked because a
    // JSON Lines body could conceivably begin with the four characters.
    const std::size_t minimum = kMagic.size() + 4 + kMagic.size();
    if (body.size() >= minimum && body.substr(0, kMagic.size()) == kMagic &&
        body.substr(body.size() - kMagic.size()) == kMagic) {
        return PartitionBodyFormat::Parquet;
    }
    return PartitionBodyFormat::Jsonl;
}

PartitionBodyWriteResult serialize_partition_body(const std::vector<PartitionRow>& rows) {
    PartitionBodyWriteResult out;

    // Flatten to the Parquet row shape: the row's own fields, plus the id as a
    // `_zid` column. A row that is not an object, or that already carries a
    // `_zid` field of its own (which the id column would collide with), cannot
    // take that shape.
    std::vector<nlohmann::json> flat;
    flat.reserve(rows.size());
    bool flattenable = true;
    for (const PartitionRow& row : rows) {
        if (!row.doc.is_object() || row.doc.contains(kIdColumn)) {
            flattenable = false;
            break;
        }
        nlohmann::json record = row.doc;
        record[kIdColumn] = row.id;
        flat.push_back(std::move(record));
    }

    if (flattenable) {
        const zetta::parquet::ParquetWriteResult written =
            zetta::parquet::write_parquet_rows(flat);
        if (written.ok()) {
            out.bytes = written.data;
            out.format = PartitionBodyFormat::Parquet;
            // The body's own schema, recorded so the catalog can carry it: the
            // same inference the writer just used, so what the catalog says the
            // blob holds is what the blob holds.
            const zetta::parquet::ParquetSchemaResult inferred =
                zetta::parquet::infer_parquet_schema(flat);
            for (const zetta::parquet::WriterColumn& column : inferred.columns) {
                out.schema.push_back(PartitionBodyColumn{
                    column.name, std::string(zetta::parquet::to_string(column.type)),
                    column.optional});
            }
            return out;
        }
        // The rows have no flat scalar schema (a nested value, a column mixing
        // families). JSON Lines can still carry them exactly — writing them as
        // Parquet would mean coercing, and coercion loses data.
    }

    out.bytes = write_jsonl(rows);
    out.format = PartitionBodyFormat::Jsonl;
    out.schema = jsonl_schema(rows);
    return out;
}

PartitionBodyReadResult read_partition_body(std::string_view body,
                                            std::string_view partition_id) {
    if (sniff_partition_body(body) == PartitionBodyFormat::Jsonl) {
        return read_jsonl(body, partition_id);
    }

    PartitionBodyReadResult out;
    out.format = PartitionBodyFormat::Parquet;
    zetta::parquet::ParquetRowsResult read = zetta::parquet::read_parquet_rows(body);
    if (!read.ok()) {
        out.error = "partition '" + std::string(partition_id) + "': " + read.error;
        return out;
    }

    out.rows.reserve(read.rows.size());
    for (std::size_t index = 0; index < read.rows.size(); ++index) {
        nlohmann::json& record = read.rows[index];
        PartitionRow row;
        if (const auto id = record.find(kIdColumn);
            id != record.end() && id->is_string()) {
            row.id = id->get<std::string>();
            record.erase(kIdColumn);  // the id is the row's key, not a field of it
        } else {
            row.id = std::string(partition_id) + "#" + std::to_string(index);
            if (id != record.end()) record.erase(kIdColumn);
        }
        row.doc = std::move(record);
        out.rows.push_back(std::move(row));
    }
    return out;
}

}  // namespace zetta::data
