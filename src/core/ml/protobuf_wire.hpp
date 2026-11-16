// SPDX-License-Identifier: MIT

#pragma once

// The protobuf wire format, read and written by hand (lake#681). The engine reads
// ONNX tree-ensemble models and writes them back out; ONNX is protobuf, and the
// handful of messages a tree ensemble uses (ModelProto, GraphProto, NodeProto,
// AttributeProto, TensorProto, ValueInfoProto) need only the wire format: varints,
// fixed 32/64-bit values and length-delimited fields. So the engine carries this
// rather than a protobuf runtime and generated code (we write our own).
//
// Internal to zetta-core (src/core/ml); see docs/ml/model-import.md.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace zetta::ml::protobuf {

enum class WireType : std::uint8_t {
    Varint = 0,
    Fixed64 = 1,
    LengthDelimited = 2,
    Fixed32 = 5,
};

// One field of a message: its number, wire type, and either the integer value
// (varint / fixed) or the bytes (length-delimited).
struct Field {
    std::uint32_t number = 0;
    WireType wire_type = WireType::Varint;
    std::uint64_t integer = 0;
    std::string_view bytes;

    float as_float() const noexcept {
        const auto bits = static_cast<std::uint32_t>(integer);
        float value = 0.0F;
        std::memcpy(&value, &bits, sizeof value);
        return value;
    }
    double as_double() const noexcept {
        double value = 0.0;
        std::memcpy(&value, &integer, sizeof value);
        return value;
    }
    std::int64_t as_int64() const noexcept { return static_cast<std::int64_t>(integer); }
};

// Reads the fields of one message in order. A malformed message (a truncated
// varint or length, an unknown wire type) stops the reader and sets `failed`.
class Reader {
public:
    explicit Reader(std::string_view message) : rest_(message) {}

    // The next field, or nullopt at the end of the message or on malformed input.
    std::optional<Field> next() {
        if (rest_.empty() || failed_) return std::nullopt;
        const std::optional<std::uint64_t> key = varint();
        if (!key) return fail();
        Field field;
        field.number = static_cast<std::uint32_t>(*key >> 3);
        const auto wire = static_cast<std::uint8_t>(*key & 0x7);
        switch (wire) {
            case 0: {
                const std::optional<std::uint64_t> value = varint();
                if (!value) return fail();
                field.wire_type = WireType::Varint;
                field.integer = *value;
                return field;
            }
            case 1: {
                if (rest_.size() < 8) return fail();
                std::uint64_t value = 0;
                for (int index = 7; index >= 0; --index) {
                    value = (value << 8) | static_cast<std::uint8_t>(rest_[static_cast<std::size_t>(index)]);
                }
                rest_.remove_prefix(8);
                field.wire_type = WireType::Fixed64;
                field.integer = value;
                return field;
            }
            case 2: {
                const std::optional<std::uint64_t> length = varint();
                if (!length || *length > rest_.size()) return fail();
                field.wire_type = WireType::LengthDelimited;
                field.bytes = rest_.substr(0, static_cast<std::size_t>(*length));
                rest_.remove_prefix(static_cast<std::size_t>(*length));
                return field;
            }
            case 5: {
                if (rest_.size() < 4) return fail();
                std::uint64_t value = 0;
                for (int index = 3; index >= 0; --index) {
                    value = (value << 8) | static_cast<std::uint8_t>(rest_[static_cast<std::size_t>(index)]);
                }
                rest_.remove_prefix(4);
                field.wire_type = WireType::Fixed32;
                field.integer = value;
                return field;
            }
            default:
                return fail();
        }
    }

    bool failed() const noexcept { return failed_; }

    // Decode a packed run of varints (a packed repeated int64/int32/enum field).
    static std::optional<std::vector<std::int64_t>> packed_varints(std::string_view bytes) {
        Reader reader(bytes);
        std::vector<std::int64_t> values;
        while (!reader.rest_.empty()) {
            const std::optional<std::uint64_t> value = reader.varint();
            if (!value) return std::nullopt;
            values.push_back(static_cast<std::int64_t>(*value));
        }
        return values;
    }

    // Decode a packed run of little-endian 32-bit floats.
    static std::optional<std::vector<float>> packed_floats(std::string_view bytes) {
        if (bytes.size() % 4 != 0) return std::nullopt;
        std::vector<float> values(bytes.size() / 4);
        for (std::size_t index = 0; index < values.size(); ++index) {
            std::uint32_t bits = 0;
            for (int byte = 3; byte >= 0; --byte) {
                bits = (bits << 8) |
                       static_cast<std::uint8_t>(bytes[index * 4 + static_cast<std::size_t>(byte)]);
            }
            std::memcpy(&values[index], &bits, sizeof bits);
        }
        return values;
    }

    // Decode a packed run of little-endian 64-bit doubles.
    static std::optional<std::vector<double>> packed_doubles(std::string_view bytes) {
        if (bytes.size() % 8 != 0) return std::nullopt;
        std::vector<double> values(bytes.size() / 8);
        for (std::size_t index = 0; index < values.size(); ++index) {
            std::uint64_t bits = 0;
            for (int byte = 7; byte >= 0; --byte) {
                bits = (bits << 8) |
                       static_cast<std::uint8_t>(bytes[index * 8 + static_cast<std::size_t>(byte)]);
            }
            std::memcpy(&values[index], &bits, sizeof bits);
        }
        return values;
    }

private:
    std::optional<std::uint64_t> varint() {
        std::uint64_t value = 0;
        for (int shift = 0; shift < 64; shift += 7) {
            if (rest_.empty()) return std::nullopt;
            const auto byte = static_cast<std::uint8_t>(rest_.front());
            rest_.remove_prefix(1);
            value |= static_cast<std::uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return value;
        }
        return std::nullopt;
    }

    std::optional<Field> fail() {
        failed_ = true;
        return std::nullopt;
    }

    std::string_view rest_;
    bool failed_ = false;
};

// Builds one message, field by field.
class Writer {
public:
    Writer& varint_field(std::uint32_t number, std::uint64_t value) {
        key(number, WireType::Varint);
        varint(value);
        return *this;
    }
    Writer& bytes_field(std::uint32_t number, std::string_view value) {
        key(number, WireType::LengthDelimited);
        varint(value.size());
        bytes_.append(value);
        return *this;
    }
    Writer& message_field(std::uint32_t number, const Writer& message) {
        return bytes_field(number, message.bytes());
    }
    Writer& float_field(std::uint32_t number, float value) {
        key(number, WireType::Fixed32);
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof bits);
        for (int byte = 0; byte < 4; ++byte) {
            bytes_.push_back(static_cast<char>((bits >> (8 * byte)) & 0xFF));
        }
        return *this;
    }

    const std::string& bytes() const noexcept { return bytes_; }

    // Little-endian doubles, the layout of a TensorProto's raw_data.
    static std::string raw_doubles(const std::vector<double>& values) {
        std::string raw;
        raw.reserve(values.size() * 8);
        for (const double value : values) {
            std::uint64_t bits = 0;
            std::memcpy(&bits, &value, sizeof bits);
            for (int byte = 0; byte < 8; ++byte) {
                raw.push_back(static_cast<char>((bits >> (8 * byte)) & 0xFF));
            }
        }
        return raw;
    }

private:
    void key(std::uint32_t number, WireType wire_type) {
        varint((static_cast<std::uint64_t>(number) << 3) | static_cast<std::uint64_t>(wire_type));
    }
    void varint(std::uint64_t value) {
        while (value >= 0x80) {
            bytes_.push_back(static_cast<char>((value & 0x7F) | 0x80));
            value >>= 7;
        }
        bytes_.push_back(static_cast<char>(value));
    }

    std::string bytes_;
};

}  // namespace zetta::ml::protobuf
