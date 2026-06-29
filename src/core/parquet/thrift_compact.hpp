#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// Shared Thrift *compact protocol* reader for the native Parquet reader.
//
// Both the footer parser (metadata.cpp, #505) and the page-header / column-chunk
// decoders (column_decoder.cpp, #506) consume Thrift compact structs — the
// footer's `FileMetaData` and each page's `PageHeader`. This is the one
// trust-boundary-critical cursor over those bytes, so it lives in a single
// header rather than being copied per translation unit.
//
// `CompactReader` is a cursor over a byte span. Every accessor is non-throwing
// and bounds-checked: it returns false (without consuming) when fewer bytes
// remain than required, so a caller can fail the parse cleanly instead of
// reading out of bounds (zetta-core is compiled `-fexperimental-library` — no
// exceptions across the seam).
namespace zetta::parquet::detail {

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

    // The number of bytes consumed so far — used to locate a page body, which
    // begins immediately after the page header struct.
    std::size_t position() const { return offset_; }

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

}  // namespace zetta::parquet::detail
