#include "zetta/data/partition_body.hpp"

#include <sstream>
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
            return out;
        }
        // The rows have no flat scalar schema (a nested value, a column mixing
        // families). JSON Lines can still carry them exactly — writing them as
        // Parquet would mean coercing, and coercion loses data.
    }

    out.bytes = write_jsonl(rows);
    out.format = PartitionBodyFormat::Jsonl;
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
