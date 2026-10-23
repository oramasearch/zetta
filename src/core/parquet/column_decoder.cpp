#include "zetta/parquet/column_decoder.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

#include "zetta/parquet/snappy.hpp"
#include "thrift_compact.hpp"

// Parquet column-chunk decoders (#506). The footer parser (#505) hands back,
// per column chunk, where its pages live + how they are stored; this walks
// those pages and decodes their values:
//
//   1. PageHeader parse — each page begins with a Thrift compact `PageHeader`
//      (shared CompactReader), immediately followed by its (compressed) body of
//      `compressed_page_size` bytes.
//   2. decompress — uncompressed pass-through or snappy (#507); other codecs
//      (zstd / gzip) fail with a reason until their decoder lands.
//   3. levels — an OPTIONAL column's per-page definition levels are an
//      RLE/bit-packed-hybrid stream (4-byte length-prefixed in a v1 data page)
//      decoded into a validity bitmap; a REQUIRED column carries none.
//   4. values — PLAIN (fixed-width LE / bit-packed bool / length-prefixed bytes)
//      or dictionary indices (RLE/bit-packed) mapped through the chunk's PLAIN
//      dictionary page.
//
// Every read is bounds-checked: a malformed / truncated / unsupported page fails
// with a reason instead of reading out of bounds or producing wrong values.
namespace zetta::parquet {
namespace {

using detail::CompactReader;
using detail::CompactType;

// parquet.thrift PageType.
enum class PageType { DataPage, IndexPage, DictionaryPage, DataPageV2, Unknown };

PageType page_type_from_code(std::int64_t code) {
    switch (code) {
        case 0: return PageType::DataPage;
        case 1: return PageType::IndexPage;
        case 2: return PageType::DictionaryPage;
        case 3: return PageType::DataPageV2;
        default: return PageType::Unknown;
    }
}

Encoding encoding_from_code(std::int64_t code) {
    switch (code) {
        case 0: return Encoding::Plain;
        case 2: return Encoding::PlainDictionary;
        case 3: return Encoding::Rle;
        case 4: return Encoding::BitPacked;
        case 5: return Encoding::DeltaBinaryPacked;
        case 6: return Encoding::DeltaLengthByteArray;
        case 7: return Encoding::DeltaByteArray;
        case 8: return Encoding::RleDictionary;
        case 9: return Encoding::ByteStreamSplit;
        default: return Encoding::Unknown;
    }
}

// The decoded fields of a Thrift `PageHeader` we model (the rest are skipped).
struct PageHeader {
    PageType type = PageType::Unknown;
    std::int32_t uncompressed_page_size = 0;
    std::int32_t compressed_page_size = 0;
    std::size_t header_length = 0;       // bytes the header occupies (body starts after)
    // DataPageHeader (v1) / DictionaryPageHeader fields:
    std::int32_t num_values = 0;
    Encoding encoding = Encoding::Unknown;            // value encoding
    Encoding definition_level_encoding = Encoding::Unknown;
    Encoding repetition_level_encoding = Encoding::Unknown;
};

// Read a DataPageHeader (v1) into the page (fields num_values / encodings).
bool read_data_page_header(CompactReader& reader, PageHeader& page) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        std::int64_t integer = 0;
        switch (field_id) {
            case 1:  // num_values
                if (!reader.read_zigzag(integer)) return false;
                page.num_values = static_cast<std::int32_t>(integer);
                break;
            case 2:  // encoding
                if (!reader.read_zigzag(integer)) return false;
                page.encoding = encoding_from_code(integer);
                break;
            case 3:  // definition_level_encoding
                if (!reader.read_zigzag(integer)) return false;
                page.definition_level_encoding = encoding_from_code(integer);
                break;
            case 4:  // repetition_level_encoding
                if (!reader.read_zigzag(integer)) return false;
                page.repetition_level_encoding = encoding_from_code(integer);
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

// Read a DictionaryPageHeader into the page (num_values = dictionary size).
bool read_dictionary_page_header(CompactReader& reader, PageHeader& page) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        std::int64_t integer = 0;
        switch (field_id) {
            case 1:  // num_values
                if (!reader.read_zigzag(integer)) return false;
                page.num_values = static_cast<std::int32_t>(integer);
                break;
            case 2:  // encoding
                if (!reader.read_zigzag(integer)) return false;
                page.encoding = encoding_from_code(integer);
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

// Parse a `PageHeader` from the bytes starting at a page offset. On success
// `page.header_length` is the header's byte size (the body begins right after).
bool read_page_header(std::string_view bytes, PageHeader& page) {
    CompactReader reader(bytes);
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) break;
        std::int64_t integer = 0;
        switch (field_id) {
            case 1:  // type (PageType)
                if (!reader.read_zigzag(integer)) return false;
                page.type = page_type_from_code(integer);
                break;
            case 2:  // uncompressed_page_size
                if (!reader.read_zigzag(integer)) return false;
                page.uncompressed_page_size = static_cast<std::int32_t>(integer);
                break;
            case 3:  // compressed_page_size
                if (!reader.read_zigzag(integer)) return false;
                page.compressed_page_size = static_cast<std::int32_t>(integer);
                break;
            case 5:  // data_page_header (DataPageHeader, v1)
                if (!read_data_page_header(reader, page)) return false;
                break;
            case 7:  // dictionary_page_header (DictionaryPageHeader)
                if (!read_dictionary_page_header(reader, page)) return false;
                break;
            default:  // crc, data_page_header_v2 (detected by type), etc.
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
    page.header_length = reader.position();
    return true;
}

// The number of bits needed to represent the values [0, max_value].
int bit_width(std::uint64_t max_value) {
    int width = 0;
    while ((max_value >> width) != 0) ++width;
    return width;
}

// Read a 4-byte little-endian unsigned int at `offset` in `bytes`.
bool read_u32_le(std::string_view bytes, std::size_t offset, std::uint32_t& out) {
    if (offset + 4 > bytes.size()) return false;
    out = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset])) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
          (static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
    return true;
}

// Decode exactly `count` values of `bit_width` bits from an RLE/bit-packed
// hybrid stream (parquet's level + dictionary-index encoding). A run is either a
// bit-packed run (header `(num_groups << 1) | 1`, groups of 8 values, each
// `bit_width` bits LSB-first) or an RLE run (header `(run_length << 1) | 0`, one
// `ceil(bit_width/8)`-byte LE value repeated). The last bit-packed group may
// carry more than `count` values; the extras are discarded. Returns false on a
// truncated / malformed stream.
bool decode_rle_hybrid(std::string_view data, int width, std::int64_t count,
                       std::vector<std::uint64_t>& out) {
    out.clear();
    if (count < 0) return false;
    out.reserve(static_cast<std::size_t>(count));
    if (count == 0) return true;

    CompactReader reader(data);
    const std::size_t byte_width = (static_cast<std::size_t>(width) + 7) / 8;
    const std::uint64_t mask =
        width >= 64 ? ~static_cast<std::uint64_t>(0)
                    : ((static_cast<std::uint64_t>(1) << width) - 1);

    while (static_cast<std::int64_t>(out.size()) < count) {
        std::uint64_t header = 0;
        if (!reader.read_uvarint(header)) return false;

        if ((header & 1) == 1) {
            // bit-packed run: (header >> 1) groups of 8 values.
            const std::uint64_t group_count = header >> 1;
            const std::uint64_t value_count = group_count * 8;
            const std::size_t run_bytes = static_cast<std::size_t>(group_count) *
                                          static_cast<std::size_t>(width);
            const std::size_t start = reader.position();
            if (!reader.skip_bytes(run_bytes)) return false;

            std::uint64_t bit_buffer = 0;
            int bits_available = 0;
            std::size_t byte_index = start;
            for (std::uint64_t value_index = 0;
                 value_index < value_count && static_cast<std::int64_t>(out.size()) < count;
                 ++value_index) {
                while (bits_available < width) {
                    bit_buffer |= static_cast<std::uint64_t>(
                                      static_cast<unsigned char>(data[byte_index++]))
                                  << bits_available;
                    bits_available += 8;
                }
                out.push_back(bit_buffer & mask);
                bit_buffer >>= width;
                bits_available -= width;
            }
        } else {
            // RLE run: a single value repeated (header >> 1) times.
            const std::uint64_t run_length = header >> 1;
            const std::size_t start = reader.position();
            if (!reader.skip_bytes(byte_width)) return false;
            std::uint64_t value = 0;
            for (std::size_t byte_index = 0; byte_index < byte_width; ++byte_index) {
                value |= static_cast<std::uint64_t>(static_cast<unsigned char>(data[start + byte_index]))
                         << (8 * byte_index);
            }
            for (std::uint64_t repeat = 0;
                 repeat < run_length && static_cast<std::int64_t>(out.size()) < count; ++repeat) {
                out.push_back(value);
            }
        }
    }
    return static_cast<std::int64_t>(out.size()) == count;
}

// Append `count` PLAIN-encoded values of `type` from `data` into `out`. Returns
// false if the bytes run short or the type is not a PLAIN-decodable physical type.
bool decode_plain(std::string_view data, PhysicalType type, std::int64_t count,
                  ColumnValues& out) {
    if (count < 0) return false;
    const auto n = static_cast<std::size_t>(count);

    switch (type) {
        case PhysicalType::Boolean: {
            // Bit-packed, one bit per value, LSB-first within each byte.
            const std::size_t needed = (n + 7) / 8;
            if (data.size() < needed) return false;
            out.bool_values.reserve(out.bool_values.size() + n);
            for (std::size_t index = 0; index < n; ++index) {
                const std::uint8_t byte = static_cast<unsigned char>(data[index / 8]);
                out.bool_values.push_back((byte >> (index % 8)) & 1);
            }
            return true;
        }
        case PhysicalType::Int32: {
            if (data.size() < n * 4) return false;
            out.int32_values.reserve(out.int32_values.size() + n);
            for (std::size_t index = 0; index < n; ++index) {
                std::int32_t value = 0;
                std::memcpy(&value, data.data() + index * 4, 4);
                out.int32_values.push_back(value);
            }
            return true;
        }
        case PhysicalType::Int64: {
            if (data.size() < n * 8) return false;
            out.int64_values.reserve(out.int64_values.size() + n);
            for (std::size_t index = 0; index < n; ++index) {
                std::int64_t value = 0;
                std::memcpy(&value, data.data() + index * 8, 8);
                out.int64_values.push_back(value);
            }
            return true;
        }
        case PhysicalType::Float: {
            if (data.size() < n * 4) return false;
            out.float_values.reserve(out.float_values.size() + n);
            for (std::size_t index = 0; index < n; ++index) {
                float value = 0;
                std::memcpy(&value, data.data() + index * 4, 4);
                out.float_values.push_back(value);
            }
            return true;
        }
        case PhysicalType::Double: {
            if (data.size() < n * 8) return false;
            out.double_values.reserve(out.double_values.size() + n);
            for (std::size_t index = 0; index < n; ++index) {
                double value = 0;
                std::memcpy(&value, data.data() + index * 8, 8);
                out.double_values.push_back(value);
            }
            return true;
        }
        case PhysicalType::ByteArray: {
            // Each value: 4-byte LE length, then that many bytes.
            std::size_t offset = 0;
            out.byte_array_values.reserve(out.byte_array_values.size() + n);
            for (std::size_t index = 0; index < n; ++index) {
                std::uint32_t length = 0;
                if (!read_u32_le(data, offset, length)) return false;
                offset += 4;
                if (offset + length > data.size()) return false;
                out.byte_array_values.emplace_back(data.substr(offset, length));
                offset += length;
            }
            return true;
        }
        default:  // Int96 / FixedLenByteArray / Unknown — not needed by the target.
            return false;
    }
}

// Append present values for a data page encoded as dictionary indices: read the
// leading bit-width byte, decode `count` RLE/bit-packed indices, and map each
// through the chunk's already-decoded PLAIN `dictionary`. Returns false on a
// malformed stream or an out-of-range index.
bool decode_dictionary_indices(std::string_view data, PhysicalType type, std::int64_t count,
                               const ColumnValues& dictionary, ColumnValues& out) {
    if (data.empty()) return count == 0;
    const int width = static_cast<unsigned char>(data[0]);
    if (width > 64) return false;

    std::vector<std::uint64_t> indices;
    if (!decode_rle_hybrid(data.substr(1), width, count, indices)) return false;

    auto map_index = [&](std::uint64_t index, std::size_t size) { return index < size; };
    switch (type) {
        case PhysicalType::Int32:
            out.int32_values.reserve(out.int32_values.size() + indices.size());
            for (std::uint64_t index : indices) {
                if (!map_index(index, dictionary.int32_values.size())) return false;
                out.int32_values.push_back(dictionary.int32_values[index]);
            }
            return true;
        case PhysicalType::Int64:
            out.int64_values.reserve(out.int64_values.size() + indices.size());
            for (std::uint64_t index : indices) {
                if (!map_index(index, dictionary.int64_values.size())) return false;
                out.int64_values.push_back(dictionary.int64_values[index]);
            }
            return true;
        case PhysicalType::Float:
            out.float_values.reserve(out.float_values.size() + indices.size());
            for (std::uint64_t index : indices) {
                if (!map_index(index, dictionary.float_values.size())) return false;
                out.float_values.push_back(dictionary.float_values[index]);
            }
            return true;
        case PhysicalType::Double:
            out.double_values.reserve(out.double_values.size() + indices.size());
            for (std::uint64_t index : indices) {
                if (!map_index(index, dictionary.double_values.size())) return false;
                out.double_values.push_back(dictionary.double_values[index]);
            }
            return true;
        case PhysicalType::Boolean:
            out.bool_values.reserve(out.bool_values.size() + indices.size());
            for (std::uint64_t index : indices) {
                if (!map_index(index, dictionary.bool_values.size())) return false;
                out.bool_values.push_back(dictionary.bool_values[index]);
            }
            return true;
        case PhysicalType::ByteArray:
            out.byte_array_values.reserve(out.byte_array_values.size() + indices.size());
            for (std::uint64_t index : indices) {
                if (!map_index(index, dictionary.byte_array_values.size())) return false;
                out.byte_array_values.push_back(dictionary.byte_array_values[index]);
            }
            return true;
        default:
            return false;
    }
}

DecodedColumnResult fail(std::string reason) {
    DecodedColumnResult result;
    result.error = std::move(reason);
    return result;
}

// Decompress a page body per its codec. Uncompressed is a pass-through; snappy
// goes through the #507 decoder. zstd / gzip have no decoder yet, so they fail
// with a clear reason rather than silently producing garbage.
bool decompress_page(CompressionCodec codec, std::string_view body,
                     std::int32_t uncompressed_size, std::string& out, std::string& error) {
    switch (codec) {
        case CompressionCodec::Uncompressed:
            out.assign(body);
            return true;
        case CompressionCodec::Snappy: {
            const SnappyResult result = snappy_decompress(body);
            if (!result.ok()) {
                error = result.error;
                return false;
            }
            out = result.data;
            return true;
        }
        default:
            error = "parquet: unsupported page codec for decode: " +
                    std::string(to_string(codec));
            return false;
    }
    (void)uncompressed_size;
}

// --- embedded JSON Schema for to_json's output ------------------------------

const nlohmann::json& columns_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "Parquet decoded columns",
      "type": "object",
      "additionalProperties": false,
      "required": ["num_rows", "columns"],
      "properties": {
        "num_rows": { "type": "integer", "minimum": 0 },
        "columns": {
          "type": "array",
          "items": {
            "type": "object",
            "additionalProperties": false,
            "required": ["path", "type", "values"],
            "properties": {
              "path": { "type": "string" },
              "type": {
                "type": "string",
                "enum": ["boolean", "int32", "int64", "float", "double",
                         "byte_array", "fixed_len_byte_array"]
              },
              "values": {
                "type": "array",
                "items": {
                  "type": ["integer", "number", "boolean", "string", "null"]
                }
              }
            }
          }
        }
      }
    })JSONSCHEMA");
    return schema;
}

nlohmann::json_schema::json_validator& columns_validator() {
    static nlohmann::json_schema::json_validator validator(columns_schema());
    return validator;
}

class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer, const nlohmann::json& /*instance*/,
               const std::string& message) override {
        if (failed) return;  // keep the first, most specific failure
        failed = true;
        const std::string at = pointer.to_string();
        reason = (at.empty() ? "/" : at) + ": " + message;
    }
    bool failed = false;
    std::string reason;
};

}  // namespace

std::string validate_decoded_column(const DecodedColumn& column) {
    switch (column.type) {
        case PhysicalType::Boolean:
        case PhysicalType::Int32:
        case PhysicalType::Int64:
        case PhysicalType::Float:
        case PhysicalType::Double:
        case PhysicalType::ByteArray:
        case PhysicalType::FixedLenByteArray:
            break;
        default:
            return "parquet: column " + column.path + " has an unsupported physical type";
    }
    if (column.num_rows < 0) {
        return "parquet: column " + column.path + " has a negative row count";
    }
    std::int64_t present = column.num_rows;
    if (!column.validity.empty()) {
        if (column.validity.size() != static_cast<std::size_t>(column.num_rows)) {
            return "parquet: column " + column.path + " has " +
                   std::to_string(column.validity.size()) + " validity entries for " +
                   std::to_string(column.num_rows) + " rows";
        }
        present = std::ranges::count_if(column.validity,
                                        [](std::uint8_t valid) { return valid != 0; });
    }
    // Exactly one vector is populated: the total across all six is the length of
    // the one `type` selects.
    const ColumnValues& values = column.values;
    const std::size_t total = values.int32_values.size() + values.int64_values.size() +
                              values.float_values.size() + values.double_values.size() +
                              values.bool_values.size() + values.byte_array_values.size();
    if (column.valid_count() != present || total != static_cast<std::size_t>(present)) {
        return "parquet: column " + column.path + " holds " + std::to_string(total) +
               " decoded values for " + std::to_string(present) + " present rows of type " +
               std::string(to_string(column.type));
    }
    return {};
}

std::int64_t DecodedColumn::valid_count() const {
    switch (type) {
        case PhysicalType::Boolean: return static_cast<std::int64_t>(values.bool_values.size());
        case PhysicalType::Int32: return static_cast<std::int64_t>(values.int32_values.size());
        case PhysicalType::Int64: return static_cast<std::int64_t>(values.int64_values.size());
        case PhysicalType::Float: return static_cast<std::int64_t>(values.float_values.size());
        case PhysicalType::Double: return static_cast<std::int64_t>(values.double_values.size());
        case PhysicalType::ByteArray:
        case PhysicalType::FixedLenByteArray:
            return static_cast<std::int64_t>(values.byte_array_values.size());
        default: return 0;
    }
}

DecodedColumnResult decode_column_chunk(std::string_view file_bytes, const FileMetaData& metadata,
                                        std::size_t row_group_index, std::size_t column_index) {
    if (row_group_index >= metadata.row_groups.size()) {
        return fail("parquet: row group index out of range");
    }
    const RowGroupMeta& group = metadata.row_groups[row_group_index];
    if (column_index >= group.columns.size()) {
        return fail("parquet: column index out of range");
    }
    const ColumnChunkMeta& chunk = group.columns[column_index];

    const std::vector<LeafColumn> leaves = metadata.leaf_columns();
    if (column_index >= leaves.size()) {
        return fail("parquet: column index has no matching schema leaf");
    }
    const LeafColumn& leaf = leaves[column_index];
    if (leaf.max_repetition_level > 0) {
        return fail("parquet: nested / repeated columns are not supported (" + leaf.path + ")");
    }
    // The page decoders follow the chunk's type and every consumer follows the
    // leaf's; a footer that states two different types for one column is refused
    // here, so no row group can append to a different typed vector (LSR-167).
    if (chunk.type != leaf.type) {
        return fail("parquet: column chunk physical type " + std::string(to_string(chunk.type)) +
                    " does not match the schema's " + std::string(to_string(leaf.type)) +
                    " for " + leaf.path);
    }
    const int max_definition_level = leaf.max_definition_level;
    const int level_bit_width = bit_width(static_cast<std::uint64_t>(max_definition_level));

    DecodedColumn column;
    column.path = leaf.path;
    column.type = chunk.type;

    // The chunk's pages are contiguous, starting at the dictionary page if there
    // is one, else the first data page. Bound the walk to the chunk's compressed
    // byte span and the file.
    std::size_t offset = chunk.has_dictionary_page
                             ? static_cast<std::size_t>(chunk.dictionary_page_offset)
                             : static_cast<std::size_t>(chunk.data_page_offset);
    const std::size_t chunk_end = offset + static_cast<std::size_t>(chunk.total_compressed_size);

    ColumnValues dictionary;
    bool have_dictionary = false;
    std::int64_t produced = 0;

    while (produced < chunk.num_values) {
        if (offset >= file_bytes.size() || offset >= chunk_end) {
            return fail("parquet: column chunk pages ended before all values were read");
        }
        PageHeader page;
        if (!read_page_header(file_bytes.substr(offset), page)) {
            return fail("parquet: malformed page header");
        }
        if (page.compressed_page_size < 0) {
            return fail("parquet: negative page size");
        }
        const std::size_t body_offset = offset + page.header_length;
        const auto body_size = static_cast<std::size_t>(page.compressed_page_size);
        if (body_offset + body_size > file_bytes.size()) {
            return fail("parquet: page body runs past end of file");
        }
        const std::string_view body = file_bytes.substr(body_offset, body_size);

        std::string page_bytes;
        std::string decompress_error;
        if (!decompress_page(chunk.codec, body, page.uncompressed_page_size, page_bytes,
                             decompress_error)) {
            return fail(decompress_error);
        }
        const std::string_view page_data(page_bytes);

        if (page.type == PageType::DictionaryPage) {
            ColumnValues fresh;
            if (!decode_plain(page_data, chunk.type, page.num_values, fresh)) {
                return fail("parquet: malformed dictionary page for " + leaf.path);
            }
            dictionary = std::move(fresh);
            have_dictionary = true;
        } else if (page.type == PageType::DataPage) {
            if (page.repetition_level_encoding != Encoding::Unknown && leaf.max_repetition_level > 0) {
                return fail("parquet: nested / repeated columns are not supported");
            }
            const std::int32_t page_values = page.num_values;

            std::size_t cursor = 0;
            std::int64_t present = page_values;
            if (max_definition_level > 0) {
                if (page.definition_level_encoding != Encoding::Rle) {
                    return fail("parquet: unsupported definition-level encoding for " + leaf.path);
                }
                std::uint32_t level_bytes = 0;
                if (!read_u32_le(page_data, cursor, level_bytes)) {
                    return fail("parquet: truncated definition-level length");
                }
                cursor += 4;
                if (cursor + level_bytes > page_data.size()) {
                    return fail("parquet: definition levels run past page end");
                }
                std::vector<std::uint64_t> levels;
                if (!decode_rle_hybrid(page_data.substr(cursor, level_bytes), level_bit_width,
                                       page_values, levels)) {
                    return fail("parquet: malformed definition levels for " + leaf.path);
                }
                cursor += level_bytes;
                present = 0;
                column.validity.reserve(column.validity.size() + levels.size());
                for (std::uint64_t level : levels) {
                    const bool valid =
                        level >= static_cast<std::uint64_t>(max_definition_level);
                    column.validity.push_back(valid ? 1 : 0);
                    if (valid) ++present;
                }
            }

            const std::string_view values = page_data.substr(cursor);
            if (page.encoding == Encoding::Plain) {
                if (!decode_plain(values, chunk.type, present, column.values)) {
                    return fail("parquet: malformed PLAIN data page for " + leaf.path);
                }
            } else if (page.encoding == Encoding::RleDictionary ||
                       page.encoding == Encoding::PlainDictionary) {
                if (!have_dictionary) {
                    return fail("parquet: dictionary-encoded page with no dictionary for " +
                                leaf.path);
                }
                if (!decode_dictionary_indices(values, chunk.type, present, dictionary,
                                               column.values)) {
                    return fail("parquet: malformed dictionary-indexed data page for " + leaf.path);
                }
            } else {
                return fail("parquet: unsupported value encoding (" +
                            std::string(to_string(page.encoding)) + ") for " + leaf.path);
            }

            produced += page_values;
        } else if (page.type == PageType::DataPageV2) {
            return fail("parquet: data page v2 is not supported yet (" + leaf.path + ")");
        } else {
            // An index page or an unknown page type: advance past it (it carries
            // no column values).
        }

        offset = body_offset + body_size;
    }

    if (produced != chunk.num_values) {
        return fail("parquet: decoded value count does not match the column chunk");
    }
    // A REQUIRED column carries no definition levels, so validity stays empty
    // (every row present); only OPTIONAL columns accumulate a bitmap.
    column.num_rows = produced;
    if (std::string shape = validate_decoded_column(column); !shape.empty()) {
        return fail(std::move(shape));
    }

    DecodedColumnResult result;
    result.column = std::move(column);
    return result;
}

DecodedColumnResult decode_column(std::string_view file_bytes, const FileMetaData& metadata,
                                  std::size_t column_index) {
    DecodedColumn merged;
    bool initialized = false;

    for (std::size_t group_index = 0; group_index < metadata.row_groups.size(); ++group_index) {
        DecodedColumnResult chunk = decode_column_chunk(file_bytes, metadata, group_index,
                                                        column_index);
        if (!chunk.ok()) return chunk;
        DecodedColumn& part = chunk.column;
        if (!initialized) {
            merged.path = part.path;
            merged.type = part.type;
            initialized = true;
        } else if (part.type != merged.type) {
            return fail("parquet: row group " + std::to_string(group_index) +
                        " changes the physical type of " + merged.path + " from " +
                        std::string(to_string(merged.type)) + " to " +
                        std::string(to_string(part.type)));
        }
        merged.num_rows += part.num_rows;
        // Concatenate validity. If any chunk carries a bitmap the column is
        // OPTIONAL, so every chunk does; a REQUIRED column never does.
        if (!part.validity.empty()) {
            merged.validity.insert(merged.validity.end(), part.validity.begin(),
                                   part.validity.end());
        }
        auto& values = merged.values;
        const auto& source = part.values;
        values.int32_values.insert(values.int32_values.end(), source.int32_values.begin(),
                                   source.int32_values.end());
        values.int64_values.insert(values.int64_values.end(), source.int64_values.begin(),
                                   source.int64_values.end());
        values.float_values.insert(values.float_values.end(), source.float_values.begin(),
                                   source.float_values.end());
        values.double_values.insert(values.double_values.end(), source.double_values.begin(),
                                    source.double_values.end());
        values.bool_values.insert(values.bool_values.end(), source.bool_values.begin(),
                                  source.bool_values.end());
        values.byte_array_values.insert(values.byte_array_values.end(),
                                        source.byte_array_values.begin(),
                                        source.byte_array_values.end());
    }

    if (!initialized) {
        return fail("parquet: no row groups to decode");
    }
    if (std::string shape = validate_decoded_column(merged); !shape.empty()) {
        return fail(std::move(shape));
    }
    DecodedColumnResult result;
    result.column = std::move(merged);
    return result;
}

nlohmann::json to_json(const DecodedColumn& column) {
    nlohmann::json values = nlohmann::json::array();

    // Walk logical rows; a present row pops the next dense value, a null row
    // emits JSON null. A present row past the end of the dense values (a column
    // that fails validate_decoded_column) is emitted as null, never read.
    std::size_t next = 0;
    const ColumnValues& source = column.values;
    const auto dense_count = static_cast<std::size_t>(column.valid_count());
    for (std::int64_t row = 0; row < column.num_rows; ++row) {
        if (!column.row_is_valid(row) || next >= dense_count) {
            values.push_back(nullptr);
            continue;
        }
        switch (column.type) {
            case PhysicalType::Boolean:
                values.push_back(source.bool_values[next] != 0);
                break;
            case PhysicalType::Int32:
                values.push_back(source.int32_values[next]);
                break;
            case PhysicalType::Int64:
                values.push_back(source.int64_values[next]);
                break;
            case PhysicalType::Float:
                values.push_back(source.float_values[next]);
                break;
            case PhysicalType::Double:
                values.push_back(source.double_values[next]);
                break;
            case PhysicalType::ByteArray:
            case PhysicalType::FixedLenByteArray:
                values.push_back(source.byte_array_values[next]);
                break;
            default:
                values.push_back(nullptr);
                break;
        }
        ++next;
    }

    return nlohmann::json{
        {"path", column.path},
        {"type", to_string(column.type)},
        {"values", std::move(values)},
    };
}

const nlohmann::json& parquet_columns_schema() { return columns_schema(); }

std::string validate_parquet_columns_json(const nlohmann::json& document) {
    collecting_error_handler handler;
    columns_validator().validate(document, handler);
    return handler.failed ? handler.reason : std::string();
}

}  // namespace zetta::parquet
