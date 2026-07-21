#include "zetta/parquet/writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "zetta/parquet/snappy.hpp"
#include "thrift_compact.hpp"

// Native Parquet writer (#833) — the inverse of the reader stack, and written
// against it: every structure emitted here has a decoder a few files away, and
// the tests assert the two agree.
//
// The file it produces:
//
//   PAR1
//   [page header][page body] ...        one chunk per (row group, column),
//                                       chunks laid out column-major within a
//                                       row group
//   [Thrift FileMetaData]               schema + row groups + chunk offsets
//   [footer length, 4-byte LE]
//   PAR1
//
// A page body is the definition-level stream (only for an OPTIONAL column:
// 4-byte LE length, then an RLE/bit-packed-hybrid run of 1-bit levels) followed
// by the PLAIN-encoded present values, and the whole body is then compressed
// with the page codec.
//
// Nothing throws. Every failure — an unrepresentable value, a null in a REQUIRED
// column, a codec with no encoder — returns a result carrying a path-qualified
// reason.
namespace zetta::parquet {
namespace {

using detail::CompactType;
using detail::CompactWriter;

// --- parquet.thrift enum codes (the inverse of metadata.cpp's *_from_code) ---

constexpr std::int32_t kEncodingPlain = 0;
constexpr std::int32_t kEncodingRle = 3;
constexpr std::int32_t kPageTypeDataPage = 0;
constexpr std::int32_t kConvertedTypeUtf8 = 0;
constexpr std::int32_t kRepetitionRequired = 0;
constexpr std::int32_t kRepetitionOptional = 1;
constexpr std::int32_t kFormatVersion = 1;  // data page v1

constexpr std::string_view kMagic = "PAR1";

// The physical-type code, or -1 for a type this writer cannot encode.
std::int32_t physical_type_code(PhysicalType type) {
    switch (type) {
        case PhysicalType::Boolean: return 0;
        case PhysicalType::Int32: return 1;
        case PhysicalType::Int64: return 2;
        case PhysicalType::Float: return 4;
        case PhysicalType::Double: return 5;
        case PhysicalType::ByteArray: return 6;
        default: return -1;  // Int96 / FixedLenByteArray / Unknown
    }
}

// The codec code, or -1 for a codec zetta has no encoder for. Refusing is the
// point: a file zetta cannot read back is worse than no file.
std::int32_t codec_code(CompressionCodec codec) {
    switch (codec) {
        case CompressionCodec::Uncompressed: return 0;
        case CompressionCodec::Snappy: return 1;
        default: return -1;
    }
}

// --- little-endian value appends --------------------------------------------

void append_u32_le(std::string& out, std::uint32_t value) {
    for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
        out.push_back(static_cast<char>((value >> (8 * byte_index)) & 0xff));
    }
}

// PLAIN stores fixed-width values in little-endian; on every platform zetta
// targets the native layout already is that, so the object representation is
// copied straight out (the exact inverse of the decoder's memcpy).
template <typename Value>
void append_native(std::string& out, Value value) {
    static_assert(std::is_trivially_copyable_v<Value>);
    char bytes[sizeof(Value)];
    std::memcpy(bytes, &value, sizeof(Value));
    out.append(bytes, sizeof(Value));
}

// --- one cell of a page ------------------------------------------------------

// A present (non-null) value plus the row it came from, so an encoding failure
// can name the row.
struct PresentCell {
    const nlohmann::json* value = nullptr;
    std::size_t row = 0;
};

std::string cell_type_name(const nlohmann::json& value) {
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer()) return "integer";
    if (value.is_number_float()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    if (value.is_object()) return "object";
    return "null";
}

std::string cell_error(const WriterColumn& column, std::size_t row, const std::string& detail) {
    return "parquet writer: column \"" + column.name + "\" row " + std::to_string(row) + ": " +
           detail;
}

// Encode the present values of one page as PLAIN. Returns false with a reason
// when a value's JSON type cannot be stored in the column's physical type — the
// writer never coerces silently, because a coerced value would break the
// round-trip the caller is relying on.
bool append_plain_values(const std::vector<PresentCell>& cells, const WriterColumn& column,
                         std::string& out, std::string& error) {
    switch (column.type) {
        case PhysicalType::Boolean: {
            // Bit-packed, one bit per present value, LSB-first within each byte.
            std::uint8_t packed = 0;
            std::size_t bit = 0;
            for (const PresentCell& cell : cells) {
                if (!cell.value->is_boolean()) {
                    error = cell_error(column, cell.row,
                                       "expected boolean, got " + cell_type_name(*cell.value));
                    return false;
                }
                if (cell.value->get<bool>()) packed |= static_cast<std::uint8_t>(1u << bit);
                if (++bit == 8) {
                    out.push_back(static_cast<char>(packed));
                    packed = 0;
                    bit = 0;
                }
            }
            if (bit != 0) out.push_back(static_cast<char>(packed));
            return true;
        }
        case PhysicalType::Int32:
        case PhysicalType::Int64: {
            const bool narrow = column.type == PhysicalType::Int32;
            for (const PresentCell& cell : cells) {
                if (!cell.value->is_number_integer()) {
                    error = cell_error(column, cell.row,
                                       "expected integer, got " + cell_type_name(*cell.value));
                    return false;
                }
                if (cell.value->is_number_unsigned() &&
                    cell.value->get<std::uint64_t>() >
                        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
                    error = cell_error(column, cell.row, "integer exceeds the int64 range");
                    return false;
                }
                const auto value = cell.value->get<std::int64_t>();
                if (narrow) {
                    if (value < std::numeric_limits<std::int32_t>::min() ||
                        value > std::numeric_limits<std::int32_t>::max()) {
                        error = cell_error(column, cell.row, "integer exceeds the int32 range");
                        return false;
                    }
                    append_native(out, static_cast<std::int32_t>(value));
                } else {
                    append_native(out, value);
                }
            }
            return true;
        }
        case PhysicalType::Float:
        case PhysicalType::Double: {
            const bool narrow = column.type == PhysicalType::Float;
            for (const PresentCell& cell : cells) {
                if (!cell.value->is_number()) {
                    error = cell_error(column, cell.row,
                                       "expected number, got " + cell_type_name(*cell.value));
                    return false;
                }
                const auto value = cell.value->get<double>();
                if (narrow) {
                    append_native(out, static_cast<float>(value));
                } else {
                    append_native(out, value);
                }
            }
            return true;
        }
        case PhysicalType::ByteArray: {
            // Each value: 4-byte LE length, then that many bytes.
            for (const PresentCell& cell : cells) {
                if (!cell.value->is_string()) {
                    error = cell_error(column, cell.row,
                                       "expected string, got " + cell_type_name(*cell.value));
                    return false;
                }
                const auto& text = cell.value->get_ref<const std::string&>();
                if (text.size() > std::numeric_limits<std::uint32_t>::max()) {
                    error = cell_error(column, cell.row, "string exceeds the 32-bit length field");
                    return false;
                }
                append_u32_le(out, static_cast<std::uint32_t>(text.size()));
                out.append(text);
            }
            return true;
        }
        default:
            error = "parquet writer: column \"" + column.name + "\": unsupported physical type " +
                    std::string(to_string(column.type));
            return false;
    }
}

// Encode a page's definition levels as an RLE/bit-packed hybrid stream. A flat
// OPTIONAL column has max definition level 1, so a level is one bit and its RLE
// run body is one byte; runs of equal levels collapse to a varint header plus
// that byte, which is why an all-present or all-null page costs a handful of
// bytes regardless of its length.
std::string encode_definition_levels(const std::vector<std::uint8_t>& levels) {
    std::string out;
    CompactWriter writer(out);
    std::size_t index = 0;
    while (index < levels.size()) {
        std::size_t run = 1;
        while (index + run < levels.size() && levels[index + run] == levels[index]) ++run;
        writer.write_uvarint(static_cast<std::uint64_t>(run) << 1);  // RLE run header
        writer.write_byte(levels[index]);
        index += run;
    }
    return out;
}

// Build one data page's uncompressed body for rows [begin, end) of `column`.
bool build_page_body(const std::vector<nlohmann::json>& rows, std::size_t begin, std::size_t end,
                     const WriterColumn& column, std::string& body, std::string& error) {
    std::vector<PresentCell> present;
    present.reserve(end - begin);
    std::vector<std::uint8_t> levels;
    if (column.optional) levels.reserve(end - begin);

    for (std::size_t row = begin; row < end; ++row) {
        const nlohmann::json& record = rows[row];
        const nlohmann::json* cell = nullptr;
        const auto found = record.find(column.name);
        if (found != record.end() && !found->is_null()) cell = &*found;

        if (cell == nullptr && !column.optional) {
            error = cell_error(column, row, "null in a REQUIRED column");
            return false;
        }
        if (column.optional) levels.push_back(cell != nullptr ? 1 : 0);
        if (cell != nullptr) present.push_back(PresentCell{cell, row});
    }

    if (column.optional) {
        const std::string level_bytes = encode_definition_levels(levels);
        if (level_bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
            error = "parquet writer: column \"" + column.name +
                    "\": definition levels exceed the 32-bit length field";
            return false;
        }
        append_u32_le(body, static_cast<std::uint32_t>(level_bytes.size()));
        body += level_bytes;
    }
    return append_plain_values(present, column, body, error);
}

// --- Thrift emission ---------------------------------------------------------

// A v1 DataPageHeader. The level encodings are declared even for a REQUIRED
// column: parquet.thrift marks both fields required, and no levels are written
// for such a column anyway.
void write_page_header(std::string& out, std::int32_t uncompressed_size,
                       std::int32_t compressed_size, std::int32_t num_values) {
    CompactWriter writer(out);
    writer.begin_struct();
    writer.write_i32_field(1, kPageTypeDataPage);
    writer.write_i32_field(2, uncompressed_size);
    writer.write_i32_field(3, compressed_size);
    writer.begin_struct_field(5);  // data_page_header
    writer.write_i32_field(1, num_values);
    writer.write_i32_field(2, kEncodingPlain);
    writer.write_i32_field(3, kEncodingRle);  // definition_level_encoding
    writer.write_i32_field(4, kEncodingRle);  // repetition_level_encoding
    writer.end_struct();
    writer.end_struct();
}

// What one column chunk turned into on disk — everything the footer must record
// about it.
struct ChunkLayout {
    std::int64_t num_values = 0;
    std::int64_t compressed_size = 0;    // page headers + compressed bodies
    std::int64_t uncompressed_size = 0;  // page headers + uncompressed bodies
    std::int64_t first_page_offset = 0;
};

struct RowGroupLayout {
    std::int64_t num_rows = 0;
    std::int64_t total_byte_size = 0;
    std::vector<ChunkLayout> chunks;  // one per column, in column order
};

// The flattened schema tree: the root group followed by one leaf per column.
void write_schema(CompactWriter& writer, const std::vector<WriterColumn>& columns) {
    writer.write_list_field_header(2, CompactType::Struct, columns.size() + 1);

    writer.begin_struct();  // root
    writer.write_binary_field(4, "schema");
    writer.write_i32_field(5, static_cast<std::int32_t>(columns.size()));  // num_children
    writer.end_struct();

    for (const WriterColumn& column : columns) {
        writer.begin_struct();
        writer.write_i32_field(1, physical_type_code(column.type));
        writer.write_i32_field(3, column.optional ? kRepetitionOptional : kRepetitionRequired);
        writer.write_binary_field(4, column.name);
        if (column.type == PhysicalType::ByteArray) {
            // JSON strings are UTF-8 by definition of the data model, so annotate
            // the column — zetta's own reader ignores this, but every other tool
            // renders an unannotated byte_array as opaque binary.
            writer.write_i32_field(6, kConvertedTypeUtf8);
        }
        writer.end_struct();
    }
}

void write_column_chunk(CompactWriter& writer, const WriterColumn& column,
                        const ChunkLayout& chunk, std::int32_t codec) {
    writer.begin_struct();
    writer.write_i64_field(2, chunk.first_page_offset);  // file_offset
    writer.begin_struct_field(3);                        // meta_data (ColumnMetaData)
    writer.write_i32_field(1, physical_type_code(column.type));
    writer.write_list_field_header(2, CompactType::I32, 2);  // encodings
    writer.write_zigzag(kEncodingPlain);
    writer.write_zigzag(kEncodingRle);
    writer.write_list_field_header(3, CompactType::Binary, 1);  // path_in_schema
    writer.write_binary(column.name);
    writer.write_i32_field(4, codec);
    writer.write_i64_field(5, chunk.num_values);
    writer.write_i64_field(6, chunk.uncompressed_size);
    writer.write_i64_field(7, chunk.compressed_size);
    writer.write_i64_field(9, chunk.first_page_offset);  // data_page_offset
    writer.end_struct();
    writer.end_struct();
}

std::string build_footer(const std::vector<WriterColumn>& columns,
                         const std::vector<RowGroupLayout>& groups, std::int64_t num_rows,
                         std::int32_t codec, const std::string& created_by) {
    std::string out;
    CompactWriter writer(out);
    writer.begin_struct();
    writer.write_i32_field(1, kFormatVersion);
    write_schema(writer, columns);
    writer.write_i64_field(3, num_rows);

    writer.write_list_field_header(4, CompactType::Struct, groups.size());
    for (const RowGroupLayout& group : groups) {
        writer.begin_struct();
        writer.write_list_field_header(1, CompactType::Struct, group.chunks.size());
        for (std::size_t index = 0; index < group.chunks.size(); ++index) {
            write_column_chunk(writer, columns[index], group.chunks[index], codec);
        }
        writer.write_i64_field(2, group.total_byte_size);
        writer.write_i64_field(3, group.num_rows);
        writer.end_struct();
    }

    writer.write_binary_field(6, created_by);
    writer.end_struct();
    return out;
}

// --- failures ----------------------------------------------------------------

ParquetWriteResult write_failure(std::string reason) {
    ParquetWriteResult result;
    result.error = std::move(reason);
    return result;
}

ParquetSchemaResult schema_failure(std::string reason) {
    ParquetSchemaResult result;
    result.error = std::move(reason);
    return result;
}

// What a column's values looked like across every row — the evidence the type
// inference decides on.
struct ColumnObservation {
    bool boolean = false;
    bool integer = false;
    bool real = false;
    bool string = false;
    bool null = false;         // an explicit JSON null
    std::size_t present = 0;   // rows in which the key appeared at all
    std::size_t first_row = 0;
    bool unsupported = false;
    std::string unsupported_kind;
};

// Compress a page body with the file's codec.
bool compress_body(CompressionCodec codec, const std::string& body, std::string& out,
                   std::string& error) {
    if (codec == CompressionCodec::Uncompressed) {
        out = body;
        return true;
    }
    const SnappyResult compressed = snappy_compress(body);
    if (!compressed.ok()) {
        error = compressed.error;
        return false;
    }
    out = compressed.data;
    return true;
}

}  // namespace

ParquetSchemaResult infer_parquet_schema(const std::vector<nlohmann::json>& rows) {
    // std::map keeps the columns in sorted key order, which is the order
    // nlohmann::json objects iterate in and therefore the order
    // read_parquet_rows hands rows back in — so a round trip is order-stable.
    std::map<std::string, ColumnObservation> observed;

    for (std::size_t row = 0; row < rows.size(); ++row) {
        const nlohmann::json& record = rows[row];
        if (!record.is_object()) {
            return schema_failure("parquet writer: row " + std::to_string(row) +
                                  " is not a JSON object");
        }
        for (auto entry = record.begin(); entry != record.end(); ++entry) {
            ColumnObservation& observation = observed[entry.key()];
            if (observation.present == 0) observation.first_row = row;
            ++observation.present;
            const nlohmann::json& value = entry.value();
            if (value.is_null()) {
                observation.null = true;
            } else if (value.is_boolean()) {
                observation.boolean = true;
            } else if (value.is_number_integer()) {
                observation.integer = true;
            } else if (value.is_number_float()) {
                observation.real = true;
            } else if (value.is_string()) {
                observation.string = true;
            } else if (!observation.unsupported) {
                observation.unsupported = true;
                observation.unsupported_kind = cell_type_name(value);
                observation.first_row = row;
            }
        }
    }

    ParquetSchemaResult result;
    result.columns.reserve(observed.size());
    for (const auto& [name, observation] : observed) {
        const std::string column_prefix = "parquet writer: column \"" + name + "\"";
        if (observation.unsupported) {
            return schema_failure(column_prefix + " row " +
                                  std::to_string(observation.first_row) + ": cannot store a JSON " +
                                  observation.unsupported_kind + " in a flat Parquet column");
        }
        const bool numeric = observation.integer || observation.real;
        if ((observation.string && (numeric || observation.boolean)) ||
            (observation.boolean && numeric)) {
            // No row is named: the conflict is between rows, not in one of them.
            return schema_failure(column_prefix +
                                  ": mixed value types in one column (no physical type would "
                                  "round-trip it) — write it with an explicit schema instead");
        }

        WriterColumn column;
        column.name = name;
        column.optional = observation.null || observation.present < rows.size();
        if (observation.string) {
            column.type = PhysicalType::ByteArray;
        } else if (observation.boolean) {
            column.type = PhysicalType::Boolean;
        } else if (observation.real) {
            column.type = PhysicalType::Double;  // an integer among reals widens
        } else if (observation.integer) {
            column.type = PhysicalType::Int64;
        } else {
            // No non-null value anywhere. Every row of the column is null, so
            // any type round-trips; byte_array is the widest and says the least.
            column.type = PhysicalType::ByteArray;
        }
        result.columns.push_back(std::move(column));
    }
    return result;
}

ParquetWriteResult write_parquet_rows(const std::vector<nlohmann::json>& rows,
                                      const WriterOptions& options) {
    const ParquetSchemaResult schema = infer_parquet_schema(rows);
    if (!schema.ok()) return write_failure(schema.error);
    return write_parquet_rows(rows, schema.columns, options);
}

ParquetWriteResult write_parquet_rows(const std::vector<nlohmann::json>& rows,
                                      const std::vector<WriterColumn>& columns,
                                      const WriterOptions& options) {
    if (options.max_rows_per_row_group <= 0) {
        return write_failure("parquet writer: max_rows_per_row_group must be positive");
    }
    if (options.max_rows_per_page <= 0) {
        return write_failure("parquet writer: max_rows_per_page must be positive");
    }
    const std::int32_t codec = codec_code(options.codec);
    if (codec < 0) {
        return write_failure("parquet writer: no encoder for codec " +
                             std::string(to_string(options.codec)) +
                             " (snappy and uncompressed are supported)");
    }

    std::set<std::string> names;
    for (const WriterColumn& column : columns) {
        if (column.name.empty()) {
            return write_failure("parquet writer: a column has an empty name");
        }
        if (!names.insert(column.name).second) {
            return write_failure("parquet writer: duplicate column \"" + column.name + "\"");
        }
        if (physical_type_code(column.type) < 0) {
            return write_failure("parquet writer: column \"" + column.name +
                                 "\": unsupported physical type " +
                                 std::string(to_string(column.type)));
        }
    }
    for (std::size_t row = 0; row < rows.size(); ++row) {
        if (!rows[row].is_object()) {
            return write_failure("parquet writer: row " + std::to_string(row) +
                                 " is not a JSON object");
        }
    }

    std::string out;
    out.append(kMagic);

    const auto group_rows = static_cast<std::size_t>(options.max_rows_per_row_group);
    const auto page_rows = static_cast<std::size_t>(options.max_rows_per_page);

    std::vector<RowGroupLayout> groups;
    for (std::size_t group_begin = 0; group_begin < rows.size(); group_begin += group_rows) {
        const std::size_t group_end = std::min(group_begin + group_rows, rows.size());
        RowGroupLayout group;
        group.num_rows = static_cast<std::int64_t>(group_end - group_begin);
        group.chunks.reserve(columns.size());

        for (const WriterColumn& column : columns) {
            ChunkLayout chunk;
            chunk.first_page_offset = static_cast<std::int64_t>(out.size());

            for (std::size_t page_begin = group_begin; page_begin < group_end;
                 page_begin += page_rows) {
                const std::size_t page_end = std::min(page_begin + page_rows, group_end);

                std::string body;
                std::string error;
                if (!build_page_body(rows, page_begin, page_end, column, body, error)) {
                    return write_failure(error);
                }
                std::string compressed;
                if (!compress_body(options.codec, body, compressed, error)) {
                    return write_failure(error);
                }
                if (body.size() > std::numeric_limits<std::int32_t>::max() ||
                    compressed.size() > std::numeric_limits<std::int32_t>::max()) {
                    return write_failure("parquet writer: column \"" + column.name +
                                         "\": page exceeds the 32-bit page-size field (lower "
                                         "max_rows_per_page)");
                }

                std::string header;
                write_page_header(header, static_cast<std::int32_t>(body.size()),
                                  static_cast<std::int32_t>(compressed.size()),
                                  static_cast<std::int32_t>(page_end - page_begin));

                chunk.num_values += static_cast<std::int64_t>(page_end - page_begin);
                chunk.uncompressed_size += static_cast<std::int64_t>(header.size() + body.size());
                chunk.compressed_size +=
                    static_cast<std::int64_t>(header.size() + compressed.size());
                out += header;
                out += compressed;
            }

            group.total_byte_size += chunk.uncompressed_size;
            group.chunks.push_back(chunk);
        }
        groups.push_back(std::move(group));
    }

    // A schema with no rows still needs one row group: the reader decodes a
    // column by walking its chunks, and a column with nowhere to look is an
    // error rather than an empty column. The chunk holds zero values, so no page
    // is ever read from it.
    if (rows.empty() && !columns.empty()) {
        RowGroupLayout group;
        group.chunks.assign(columns.size(),
                            ChunkLayout{0, 0, 0, static_cast<std::int64_t>(out.size())});
        groups.push_back(std::move(group));
    }

    const std::string footer =
        build_footer(columns, groups, static_cast<std::int64_t>(rows.size()), codec,
                     options.created_by);
    if (footer.size() > std::numeric_limits<std::uint32_t>::max()) {
        return write_failure("parquet writer: footer exceeds the 32-bit footer-length field");
    }
    out += footer;
    append_u32_le(out, static_cast<std::uint32_t>(footer.size()));
    out.append(kMagic);

    ParquetWriteResult result;
    result.data = std::move(out);
    return result;
}

}  // namespace zetta::parquet
