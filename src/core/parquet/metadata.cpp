#include "zetta/parquet/metadata.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>
#include <nlohmann/json-schema.hpp>

// Native Parquet footer parser. Two layers:
//
//   1. CompactReader — a minimal, non-throwing Thrift *compact protocol*
//      decoder over a byte span. Thrift compact is how parquet.thrift's
//      FileMetaData is serialized: varint field-id deltas, zigzag varints for
//      ints, length-prefixed binary, and a small set of element-type codes. The
//      reader can read the field types we model and *skip* any field/struct/
//      list/map it does not (forward compatibility — real files carry
//      statistics, logical types, column orders, etc. we ignore here).
//
//   2. FileMetaData decode — locate the footer (magic + length), then walk the
//      FileMetaData / SchemaElement / RowGroup / ColumnChunk / ColumnMetaData
//      structs, dispatching on field id and skipping the rest.
//
// Every read is bounds-checked against the byte span; a short or malformed
// footer fails with a reason instead of reading out of bounds.
namespace zetta::parquet {
namespace {

// Thrift compact protocol element-type codes (struct field types + list/map
// element types). BoolTrue/BoolFalse carry a bool field's value in the type
// nibble itself (no separate value byte).
enum class CompactType : std::uint8_t {
    Stop = 0x00,
    BoolTrue = 0x01,
    BoolFalse = 0x02,
    Byte = 0x03,
    I16 = 0x04,
    I32 = 0x05,
    I64 = 0x06,
    Double = 0x07,
    Binary = 0x08,
    List = 0x09,
    Set = 0x0a,
    Map = 0x0b,
    Struct = 0x0c,
};

// A cursor over a byte span. Every accessor returns false (without consuming)
// when fewer bytes remain than required, so a caller can fail the parse cleanly.
class CompactReader {
public:
    explicit CompactReader(std::string_view data) : data_(data) {}

    bool at_end() const { return offset_ >= data_.size(); }

    bool read_byte(std::uint8_t& out) {
        if (offset_ >= data_.size()) return false;
        out = static_cast<std::uint8_t>(data_[offset_++]);
        return true;
    }

    // Unsigned LEB128 varint (lengths, sizes, field-header deltas). Capped at 10
    // continuation bytes (a full 64-bit value); overlong input fails.
    bool read_uvarint(std::uint64_t& out) {
        std::uint64_t value = 0;
        unsigned shift = 0;
        for (unsigned byte_index = 0; byte_index < 10; ++byte_index) {
            std::uint8_t byte = 0;
            if (!read_byte(byte)) return false;
            value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
            if ((byte & 0x80) == 0) {
                out = value;
                return true;
            }
            shift += 7;
        }
        return false;  // unterminated varint
    }

    // Zigzag-decoded signed varint (compact i16/i32/i64 and list i* elements).
    bool read_zigzag(std::int64_t& out) {
        std::uint64_t encoded = 0;
        if (!read_uvarint(encoded)) return false;
        out = static_cast<std::int64_t>(encoded >> 1) ^ -static_cast<std::int64_t>(encoded & 1);
        return true;
    }

    // Length-prefixed bytes (compact binary/string).
    bool read_binary(std::string& out) {
        std::uint64_t length = 0;
        if (!read_uvarint(length)) return false;
        if (length > data_.size() - offset_) return false;  // (offset_ <= size invariant)
        out.assign(data_.data() + offset_, static_cast<std::size_t>(length));
        offset_ += static_cast<std::size_t>(length);
        return true;
    }

    bool skip_bytes(std::size_t count) {
        if (count > data_.size() - offset_) return false;
        offset_ += count;
        return true;
    }

    // Read a compact field header. On success sets `type` (Stop ends the struct)
    // and, for non-Stop, `field_id` (resolved from the delta or an explicit
    // zigzag id), updating the running last-field-id.
    bool read_field_header(std::int16_t& last_field_id, std::int16_t& field_id, CompactType& type) {
        std::uint8_t header = 0;
        if (!read_byte(header)) return false;
        const std::uint8_t type_code = header & 0x0f;
        if (type_code == 0) {
            type = CompactType::Stop;
            return true;
        }
        type = static_cast<CompactType>(type_code);
        const std::uint8_t delta = (header & 0xf0) >> 4;
        if (delta == 0) {
            std::int64_t explicit_id = 0;
            if (!read_zigzag(explicit_id)) return false;
            field_id = static_cast<std::int16_t>(explicit_id);
        } else {
            field_id = static_cast<std::int16_t>(last_field_id + delta);
        }
        last_field_id = field_id;
        return true;
    }

    // Read a compact list/set header: element type + element count.
    bool read_collection_header(CompactType& element_type, std::uint32_t& size) {
        std::uint8_t header = 0;
        if (!read_byte(header)) return false;
        element_type = static_cast<CompactType>(header & 0x0f);
        std::uint32_t short_size = (header & 0xf0) >> 4;
        if (short_size == 0x0f) {
            std::uint64_t long_size = 0;
            if (!read_uvarint(long_size)) return false;
            if (long_size > data_.size()) return false;  // can't have more elements than bytes
            size = static_cast<std::uint32_t>(long_size);
        } else {
            size = short_size;
        }
        return true;
    }

    // Skip a value of the given compact type. `in_collection` distinguishes a
    // bool *element* (a full 0x00/0x01 byte) from a bool *field* (value in the
    // type nibble — nothing to read). Recurses through nested structs/lists/maps.
    bool skip_value(CompactType type, bool in_collection) {
        switch (type) {
            case CompactType::BoolTrue:
            case CompactType::BoolFalse: {
                if (in_collection) {
                    std::uint8_t ignored = 0;
                    return read_byte(ignored);
                }
                return true;
            }
            case CompactType::Byte: {
                std::uint8_t ignored = 0;
                return read_byte(ignored);
            }
            case CompactType::I16:
            case CompactType::I32:
            case CompactType::I64: {
                std::int64_t ignored = 0;
                return read_zigzag(ignored);
            }
            case CompactType::Double:
                return skip_bytes(8);
            case CompactType::Binary: {
                std::uint64_t length = 0;
                if (!read_uvarint(length)) return false;
                if (length > data_.size() - offset_) return false;
                return skip_bytes(static_cast<std::size_t>(length));
            }
            case CompactType::List:
            case CompactType::Set: {
                CompactType element_type = CompactType::Stop;
                std::uint32_t size = 0;
                if (!read_collection_header(element_type, size)) return false;
                for (std::uint32_t index = 0; index < size; ++index) {
                    if (!skip_value(element_type, /*in_collection=*/true)) return false;
                }
                return true;
            }
            case CompactType::Map: {
                std::uint64_t size = 0;
                if (!read_uvarint(size)) return false;
                if (size == 0) return true;  // empty map is just the 0 size byte
                std::uint8_t kv_types = 0;
                if (!read_byte(kv_types)) return false;
                const auto key_type = static_cast<CompactType>((kv_types & 0xf0) >> 4);
                const auto value_type = static_cast<CompactType>(kv_types & 0x0f);
                for (std::uint64_t index = 0; index < size; ++index) {
                    if (!skip_value(key_type, /*in_collection=*/true)) return false;
                    if (!skip_value(value_type, /*in_collection=*/true)) return false;
                }
                return true;
            }
            case CompactType::Struct:
                return skip_struct();
            case CompactType::Stop:
                return false;  // STOP is not a value
        }
        return false;
    }

    // Skip a whole struct (its fields until STOP).
    bool skip_struct() {
        std::int16_t last_field_id = 0;
        while (true) {
            std::int16_t field_id = 0;
            CompactType type = CompactType::Stop;
            if (!read_field_header(last_field_id, field_id, type)) return false;
            if (type == CompactType::Stop) return true;
            if (!skip_value(type, /*in_collection=*/false)) return false;
        }
    }

private:
    std::string_view data_;
    std::size_t offset_ = 0;
};

PhysicalType physical_type_from_code(std::int64_t code) {
    switch (code) {
        case 0: return PhysicalType::Boolean;
        case 1: return PhysicalType::Int32;
        case 2: return PhysicalType::Int64;
        case 3: return PhysicalType::Int96;
        case 4: return PhysicalType::Float;
        case 5: return PhysicalType::Double;
        case 6: return PhysicalType::ByteArray;
        case 7: return PhysicalType::FixedLenByteArray;
        default: return PhysicalType::Unknown;
    }
}

RepetitionType repetition_from_code(std::int64_t code) {
    switch (code) {
        case 0: return RepetitionType::Required;
        case 1: return RepetitionType::Optional;
        case 2: return RepetitionType::Repeated;
        default: return RepetitionType::Undefined;
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

CompressionCodec codec_from_code(std::int64_t code) {
    switch (code) {
        case 0: return CompressionCodec::Uncompressed;
        case 1: return CompressionCodec::Snappy;
        case 2: return CompressionCodec::Gzip;
        case 3: return CompressionCodec::Lzo;
        case 4: return CompressionCodec::Brotli;
        case 5: return CompressionCodec::Lz4;
        case 6: return CompressionCodec::Zstd;
        case 7: return CompressionCodec::Lz4Raw;
        default: return CompressionCodec::Unknown;
    }
}

// --- per-struct decoders (parquet.thrift field ids) -------------------------

bool read_schema_element(CompactReader& reader, SchemaElement& element) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        std::int64_t integer = 0;
        switch (field_id) {
            case 1:  // type (Type)
                if (!reader.read_zigzag(integer)) return false;
                element.type = physical_type_from_code(integer);
                element.has_type = true;
                break;
            case 2:  // type_length
                if (!reader.read_zigzag(integer)) return false;
                element.type_length = static_cast<std::int32_t>(integer);
                break;
            case 3:  // repetition_type (FieldRepetitionType)
                if (!reader.read_zigzag(integer)) return false;
                element.repetition = repetition_from_code(integer);
                break;
            case 4:  // name
                if (!reader.read_binary(element.name)) return false;
                break;
            case 5:  // num_children
                if (!reader.read_zigzag(integer)) return false;
                element.num_children = static_cast<std::int32_t>(integer);
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

bool read_column_meta_data(CompactReader& reader, ColumnChunkMeta& chunk) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        std::int64_t integer = 0;
        switch (field_id) {
            case 1:  // type (Type)
                if (!reader.read_zigzag(integer)) return false;
                chunk.type = physical_type_from_code(integer);
                break;
            case 2: {  // encodings (list<Encoding>)
                CompactType element_type = CompactType::Stop;
                std::uint32_t size = 0;
                if (!reader.read_collection_header(element_type, size)) return false;
                chunk.encodings.reserve(size);
                for (std::uint32_t index = 0; index < size; ++index) {
                    if (!reader.read_zigzag(integer)) return false;
                    chunk.encodings.push_back(encoding_from_code(integer));
                }
                break;
            }
            case 3: {  // path_in_schema (list<string>)
                CompactType element_type = CompactType::Stop;
                std::uint32_t size = 0;
                if (!reader.read_collection_header(element_type, size)) return false;
                chunk.path_in_schema.reserve(size);
                for (std::uint32_t index = 0; index < size; ++index) {
                    std::string component;
                    if (!reader.read_binary(component)) return false;
                    chunk.path_in_schema.push_back(std::move(component));
                }
                break;
            }
            case 4:  // codec (CompressionCodec)
                if (!reader.read_zigzag(integer)) return false;
                chunk.codec = codec_from_code(integer);
                break;
            case 5:  // num_values
                if (!reader.read_zigzag(chunk.num_values)) return false;
                break;
            case 6:  // total_uncompressed_size
                if (!reader.read_zigzag(chunk.total_uncompressed_size)) return false;
                break;
            case 7:  // total_compressed_size
                if (!reader.read_zigzag(chunk.total_compressed_size)) return false;
                break;
            case 9:  // data_page_offset
                if (!reader.read_zigzag(chunk.data_page_offset)) return false;
                break;
            case 11:  // dictionary_page_offset
                if (!reader.read_zigzag(chunk.dictionary_page_offset)) return false;
                chunk.has_dictionary_page = true;
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

bool read_column_chunk(CompactReader& reader, ColumnChunkMeta& chunk) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        switch (field_id) {
            case 2:  // file_offset
                if (!reader.read_zigzag(chunk.file_offset)) return false;
                break;
            case 3:  // meta_data (ColumnMetaData)
                if (!read_column_meta_data(reader, chunk)) return false;
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

bool read_row_group(CompactReader& reader, RowGroupMeta& group) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        switch (field_id) {
            case 1: {  // columns (list<ColumnChunk>)
                CompactType element_type = CompactType::Stop;
                std::uint32_t size = 0;
                if (!reader.read_collection_header(element_type, size)) return false;
                group.columns.resize(size);
                for (std::uint32_t index = 0; index < size; ++index) {
                    if (!read_column_chunk(reader, group.columns[index])) return false;
                }
                break;
            }
            case 2:  // total_byte_size
                if (!reader.read_zigzag(group.total_byte_size)) return false;
                break;
            case 3:  // num_rows
                if (!reader.read_zigzag(group.num_rows)) return false;
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

bool read_file_meta_data(CompactReader& reader, FileMetaData& metadata) {
    std::int16_t last_field_id = 0;
    while (true) {
        std::int16_t field_id = 0;
        CompactType type = CompactType::Stop;
        if (!reader.read_field_header(last_field_id, field_id, type)) return false;
        if (type == CompactType::Stop) return true;
        std::int64_t integer = 0;
        switch (field_id) {
            case 1:  // version
                if (!reader.read_zigzag(integer)) return false;
                metadata.version = static_cast<std::int32_t>(integer);
                break;
            case 2: {  // schema (list<SchemaElement>)
                CompactType element_type = CompactType::Stop;
                std::uint32_t size = 0;
                if (!reader.read_collection_header(element_type, size)) return false;
                metadata.schema.resize(size);
                for (std::uint32_t index = 0; index < size; ++index) {
                    if (!read_schema_element(reader, metadata.schema[index])) return false;
                }
                break;
            }
            case 3:  // num_rows
                if (!reader.read_zigzag(metadata.num_rows)) return false;
                break;
            case 4: {  // row_groups (list<RowGroup>)
                CompactType element_type = CompactType::Stop;
                std::uint32_t size = 0;
                if (!reader.read_collection_header(element_type, size)) return false;
                metadata.row_groups.resize(size);
                for (std::uint32_t index = 0; index < size; ++index) {
                    if (!read_row_group(reader, metadata.row_groups[index])) return false;
                }
                break;
            }
            case 6:  // created_by
                if (!reader.read_binary(metadata.created_by)) return false;
                break;
            default:
                if (!reader.skip_value(type, /*in_collection=*/false)) return false;
                break;
        }
    }
}

constexpr std::string_view kMagic = "PAR1";
constexpr std::string_view kEncryptedMagic = "PARE";

ParquetMetadataResult fail(std::string reason) {
    ParquetMetadataResult result;
    result.error = std::move(reason);
    return result;
}

// --- embedded JSON Schema for to_json's output ------------------------------

// The draft-07 schema for the metadata JSON projection (the schema-as-code
// source of truth; schemas/parquet_metadata.schema.json is generated from it).
const nlohmann::json& metadata_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA({
      "$schema": "http://json-schema.org/draft-07/schema#",
      "title": "Parquet FileMetaData",
      "type": "object",
      "additionalProperties": false,
      "required": ["version", "created_by", "num_rows", "num_columns", "columns", "row_groups"],
      "properties": {
        "version": { "type": "integer" },
        "created_by": { "type": "string" },
        "num_rows": { "type": "integer", "minimum": 0 },
        "num_columns": { "type": "integer", "minimum": 0 },
        "columns": {
          "type": "array",
          "items": {
            "type": "object",
            "additionalProperties": false,
            "required": ["path", "type"],
            "properties": {
              "path": { "type": "string" },
              "type": { "type": "string" }
            }
          }
        },
        "row_groups": {
          "type": "array",
          "items": {
            "type": "object",
            "additionalProperties": false,
            "required": ["num_rows", "total_byte_size", "columns"],
            "properties": {
              "num_rows": { "type": "integer", "minimum": 0 },
              "total_byte_size": { "type": "integer", "minimum": 0 },
              "columns": {
                "type": "array",
                "items": {
                  "type": "object",
                  "additionalProperties": false,
                  "required": [
                    "path", "type", "codec", "encodings", "num_values",
                    "total_uncompressed_size", "total_compressed_size",
                    "data_page_offset", "file_offset"
                  ],
                  "properties": {
                    "path": { "type": "string" },
                    "type": { "type": "string" },
                    "codec": { "type": "string" },
                    "encodings": { "type": "array", "items": { "type": "string" } },
                    "num_values": { "type": "integer", "minimum": 0 },
                    "total_uncompressed_size": { "type": "integer", "minimum": 0 },
                    "total_compressed_size": { "type": "integer", "minimum": 0 },
                    "data_page_offset": { "type": "integer", "minimum": 0 },
                    "file_offset": { "type": "integer", "minimum": 0 },
                    "dictionary_page_offset": { "type": "integer", "minimum": 0 }
                  }
                }
              }
            }
          }
        }
      }
    })JSONSCHEMA");
    return schema;
}

// One validator, compiled once (the expensive step), then cheap to run.
nlohmann::json_schema::json_validator& metadata_validator() {
    static nlohmann::json_schema::json_validator validator(metadata_schema());
    return validator;
}

// Collects the first validation failure via the error-handler overload, so a
// failure never crosses the validator-library boundary as an exception.
class collecting_error_handler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json& /*instance*/,
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

std::string_view to_string(PhysicalType type) {
    switch (type) {
        case PhysicalType::Boolean: return "boolean";
        case PhysicalType::Int32: return "int32";
        case PhysicalType::Int64: return "int64";
        case PhysicalType::Int96: return "int96";
        case PhysicalType::Float: return "float";
        case PhysicalType::Double: return "double";
        case PhysicalType::ByteArray: return "byte_array";
        case PhysicalType::FixedLenByteArray: return "fixed_len_byte_array";
        case PhysicalType::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view to_string(RepetitionType repetition) {
    switch (repetition) {
        case RepetitionType::Required: return "required";
        case RepetitionType::Optional: return "optional";
        case RepetitionType::Repeated: return "repeated";
        case RepetitionType::Undefined: return "undefined";
    }
    return "undefined";
}

std::string_view to_string(Encoding encoding) {
    switch (encoding) {
        case Encoding::Plain: return "plain";
        case Encoding::PlainDictionary: return "plain_dictionary";
        case Encoding::Rle: return "rle";
        case Encoding::BitPacked: return "bit_packed";
        case Encoding::DeltaBinaryPacked: return "delta_binary_packed";
        case Encoding::DeltaLengthByteArray: return "delta_length_byte_array";
        case Encoding::DeltaByteArray: return "delta_byte_array";
        case Encoding::RleDictionary: return "rle_dictionary";
        case Encoding::ByteStreamSplit: return "byte_stream_split";
        case Encoding::Unknown: return "unknown";
    }
    return "unknown";
}

std::string_view to_string(CompressionCodec codec) {
    switch (codec) {
        case CompressionCodec::Uncompressed: return "uncompressed";
        case CompressionCodec::Snappy: return "snappy";
        case CompressionCodec::Gzip: return "gzip";
        case CompressionCodec::Lzo: return "lzo";
        case CompressionCodec::Brotli: return "brotli";
        case CompressionCodec::Lz4: return "lz4";
        case CompressionCodec::Zstd: return "zstd";
        case CompressionCodec::Lz4Raw: return "lz4_raw";
        case CompressionCodec::Unknown: return "unknown";
    }
    return "unknown";
}

std::string ColumnChunkMeta::path() const {
    std::string joined;
    for (std::size_t index = 0; index < path_in_schema.size(); ++index) {
        if (index != 0) joined.push_back('.');
        joined += path_in_schema[index];
    }
    return joined;
}

std::vector<LeafColumn> FileMetaData::leaf_columns() const {
    std::vector<LeafColumn> leaves;
    if (schema.empty()) return leaves;

    // The schema is a depth-first preorder flattening. schema[0] is the root
    // group (its name is not part of any path). Walk with a stack of ancestor
    // names + how many children each still expects; a node with num_children > 0
    // is a group, otherwise it is a leaf (data) column whose path is the joined
    // ancestor names below the root + its own name.
    struct Frame {
        std::string name;       // ancestor name (empty for the root, excluded)
        std::int32_t remaining;  // children still to consume
    };
    std::vector<Frame> stack;
    stack.push_back(Frame{std::string(), schema[0].num_children});

    for (std::size_t index = 1; index < schema.size() && !stack.empty(); ++index) {
        const SchemaElement& element = schema[index];

        std::string path;
        for (std::size_t depth = 1; depth < stack.size(); ++depth) {  // skip root frame
            path += stack[depth].name;
            path.push_back('.');
        }
        path += element.name;

        if (element.num_children > 0) {
            stack.push_back(Frame{element.name, element.num_children});
            continue;
        }

        leaves.push_back(LeafColumn{path, element.type});

        // Account for this leaf against its parent, popping any groups that have
        // now consumed all their children.
        while (!stack.empty()) {
            stack.back().remaining -= 1;
            if (stack.back().remaining > 0) break;
            stack.pop_back();
        }
    }
    return leaves;
}

ParquetMetadataResult parse_parquet_metadata(std::string_view file_bytes) {
    // Smallest legal file: leading magic + footer length + trailing magic.
    constexpr std::size_t kMinSize = kMagic.size() + 4 + kMagic.size();
    if (file_bytes.size() < kMinSize) {
        return fail("parquet: file too small to contain a footer");
    }
    if (file_bytes.substr(0, kMagic.size()) != kMagic) {
        if (file_bytes.substr(0, kMagic.size()) == kEncryptedMagic) {
            return fail("parquet: encrypted files (PARE magic) are not supported");
        }
        return fail("parquet: bad leading magic (not a parquet file)");
    }
    const std::string_view trailing = file_bytes.substr(file_bytes.size() - kMagic.size());
    if (trailing != kMagic) {
        if (trailing == kEncryptedMagic) {
            return fail("parquet: encrypted footer (PARE magic) is not supported");
        }
        return fail("parquet: bad trailing magic (truncated or not a parquet file)");
    }

    // The 4 bytes before the trailing magic are the little-endian footer length.
    const std::size_t length_offset = file_bytes.size() - kMagic.size() - 4;
    std::uint32_t footer_length = 0;
    for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
        footer_length |= static_cast<std::uint32_t>(
                             static_cast<std::uint8_t>(file_bytes[length_offset + byte_index]))
                         << (8 * byte_index);
    }

    // The footer must sit between the leading magic and the length field.
    if (footer_length > length_offset - kMagic.size()) {
        return fail("parquet: footer length exceeds the file");
    }
    const std::size_t footer_start = length_offset - footer_length;
    const std::string_view footer = file_bytes.substr(footer_start, footer_length);

    CompactReader reader(footer);
    ParquetMetadataResult result;
    if (!read_file_meta_data(reader, result.metadata)) {
        return fail("parquet: malformed or truncated FileMetaData footer");
    }
    if (result.metadata.schema.empty()) {
        return fail("parquet: FileMetaData has no schema");
    }
    return result;
}

nlohmann::json to_json(const FileMetaData& metadata) {
    const std::vector<LeafColumn> leaves = metadata.leaf_columns();

    nlohmann::json columns = nlohmann::json::array();
    for (const LeafColumn& leaf : leaves) {
        columns.push_back({{"path", leaf.path}, {"type", to_string(leaf.type)}});
    }

    nlohmann::json row_groups = nlohmann::json::array();
    for (const RowGroupMeta& group : metadata.row_groups) {
        nlohmann::json chunks = nlohmann::json::array();
        for (const ColumnChunkMeta& chunk : group.columns) {
            nlohmann::json encodings = nlohmann::json::array();
            for (Encoding encoding : chunk.encodings) {
                encodings.push_back(to_string(encoding));
            }
            nlohmann::json entry = {
                {"path", chunk.path()},
                {"type", to_string(chunk.type)},
                {"codec", to_string(chunk.codec)},
                {"encodings", std::move(encodings)},
                {"num_values", chunk.num_values},
                {"total_uncompressed_size", chunk.total_uncompressed_size},
                {"total_compressed_size", chunk.total_compressed_size},
                {"data_page_offset", chunk.data_page_offset},
                {"file_offset", chunk.file_offset},
            };
            if (chunk.has_dictionary_page) {
                entry["dictionary_page_offset"] = chunk.dictionary_page_offset;
            }
            chunks.push_back(std::move(entry));
        }
        row_groups.push_back({
            {"num_rows", group.num_rows},
            {"total_byte_size", group.total_byte_size},
            {"columns", std::move(chunks)},
        });
    }

    return nlohmann::json{
        {"version", metadata.version},
        {"created_by", metadata.created_by},
        {"num_rows", metadata.num_rows},
        {"num_columns", static_cast<std::int64_t>(leaves.size())},
        {"columns", std::move(columns)},
        {"row_groups", std::move(row_groups)},
    };
}

const nlohmann::json& parquet_metadata_schema() { return metadata_schema(); }

std::string validate_parquet_metadata_json(const nlohmann::json& document) {
    collecting_error_handler handler;
    metadata_validator().validate(document, handler);
    return handler.failed ? handler.reason : std::string();
}

}  // namespace zetta::parquet
