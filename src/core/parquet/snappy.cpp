#include "zetta/parquet/snappy.hpp"

#include <cstddef>
#include <cstdint>

// Snappy raw-block decoder. The block layout (google/snappy
// format_description.txt) is:
//
//   <preamble>          uncompressed length, little-endian varint
//   <element>*          a sequence of literals and back-reference copies
//
// Each element starts with a tag byte whose low 2 bits select the type:
//   00  literal                 length in the tag (or the following 1..4 bytes)
//   01  copy, 1-byte offset      length 4..11,  11-bit offset
//   10  copy, 2-byte offset      length 1..64,  16-bit little-endian offset
//   11  copy, 4-byte offset      length 1..64,  32-bit little-endian offset
//
// A copy is an LZ77 back-reference into the already-produced output and may
// overlap it (offset < length → run-length expansion), so copies advance one
// byte at a time. Every read is bounds-checked: this is a trust boundary on
// bytes that arrive from disk / a peer, so malformed input fails with a reason
// instead of reading out of bounds.
namespace zetta::parquet {
namespace {

// Reserve hint for the output. A correct preamble tells us the exact size, but
// a corrupt one could claim gigabytes from a few bytes — so we cap the upfront
// reserve and let the (validated, bounded) decode loop grow it as needed.
constexpr std::size_t kMaxReserveBytes = 64u * 1024u * 1024u;

// Decode the leading little-endian varint (the uncompressed length). On success
// returns the value and advances `offset` past the varint; on a truncated or
// overlong (> 5 byte, i.e. > 32-bit) varint returns std::nullopt.
std::optional<std::uint32_t> read_preamble_length(std::string_view input,
                                                  std::size_t& offset) {
    std::uint64_t value = 0;
    unsigned shift = 0;
    for (unsigned byte_index = 0; byte_index < 5; ++byte_index) {
        if (offset >= input.size()) return std::nullopt;  // truncated varint
        const auto byte = static_cast<unsigned char>(input[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) {
            if (value > 0xffffffffull) return std::nullopt;  // exceeds 32 bits
            return static_cast<std::uint32_t>(value);
        }
        shift += 7;
    }
    return std::nullopt;  // 5 continuation bytes with no terminator
}

// Read `count` little-endian bytes starting at `offset` into an unsigned value,
// advancing `offset`. Returns false (without advancing) if fewer than `count`
// bytes remain.
bool read_little_endian(std::string_view input, std::size_t& offset,
                        unsigned count, std::uint32_t& out) {
    if (offset + count > input.size()) return false;
    std::uint32_t value = 0;
    for (unsigned byte_index = 0; byte_index < count; ++byte_index) {
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(input[offset + byte_index]))
                 << (8 * byte_index);
    }
    offset += count;
    out = value;
    return true;
}

SnappyResult fail(std::string reason) {
    SnappyResult result;
    result.error = std::move(reason);
    return result;
}

}  // namespace

std::optional<std::uint32_t> snappy_uncompressed_length(std::string_view compressed) {
    std::size_t offset = 0;
    return read_preamble_length(compressed, offset);
}

SnappyResult snappy_decompress(std::string_view compressed) {
    std::size_t offset = 0;
    const std::optional<std::uint32_t> declared_length =
        read_preamble_length(compressed, offset);
    if (!declared_length) {
        return fail("snappy: truncated or overlong length preamble");
    }

    const std::size_t expected = *declared_length;
    std::string output;
    output.reserve(expected < kMaxReserveBytes ? expected : kMaxReserveBytes);

    while (offset < compressed.size()) {
        const auto tag = static_cast<unsigned char>(compressed[offset++]);
        const unsigned element_type = tag & 0x03;

        if (element_type == 0) {
            // Literal: copy `length` raw bytes straight to the output.
            std::size_t length = (tag >> 2) + 1u;
            if (length > 60) {
                // tag>>2 was 60..63 → the real (length-1) is in the next 1..4
                // little-endian bytes.
                const unsigned extra_bytes = static_cast<unsigned>(length) - 60u;
                std::uint32_t length_minus_one = 0;
                if (!read_little_endian(compressed, offset, extra_bytes,
                                        length_minus_one)) {
                    return fail("snappy: truncated literal length");
                }
                length = static_cast<std::size_t>(length_minus_one) + 1u;
            }
            if (offset + length > compressed.size()) {
                return fail("snappy: literal runs past end of input");
            }
            if (output.size() + length > expected) {
                return fail("snappy: literal overruns declared output length");
            }
            output.append(compressed, offset, length);
            offset += length;
            continue;
        }

        // Copy: a back-reference of `length` bytes at `offset` before the
        // current output position.
        std::size_t length = 0;
        std::uint32_t back_offset = 0;
        if (element_type == 1) {
            length = ((tag >> 2) & 0x07) + 4u;       // 4..11
            std::uint32_t low = 0;
            if (!read_little_endian(compressed, offset, 1, low)) {
                return fail("snappy: truncated 1-byte copy offset");
            }
            back_offset = (static_cast<std::uint32_t>(tag >> 5) << 8) | low;  // 11-bit
        } else if (element_type == 2) {
            length = (tag >> 2) + 1u;                 // 1..64
            if (!read_little_endian(compressed, offset, 2, back_offset)) {
                return fail("snappy: truncated 2-byte copy offset");
            }
        } else {  // element_type == 3
            length = (tag >> 2) + 1u;                 // 1..64
            if (!read_little_endian(compressed, offset, 4, back_offset)) {
                return fail("snappy: truncated 4-byte copy offset");
            }
        }

        if (back_offset == 0) {
            return fail("snappy: copy with zero offset");
        }
        if (back_offset > output.size()) {
            return fail("snappy: copy offset points before output start");
        }
        if (output.size() + length > expected) {
            return fail("snappy: copy overruns declared output length");
        }
        // Byte-at-a-time so an overlapping copy (offset < length) expands
        // correctly; the source index trails the write index by `back_offset`.
        std::size_t source = output.size() - back_offset;
        for (std::size_t written = 0; written < length; ++written) {
            output.push_back(output[source++]);
        }
    }

    if (output.size() != expected) {
        return fail("snappy: decompressed size does not match preamble length");
    }

    SnappyResult result;
    result.data = std::move(output);
    return result;
}

}  // namespace zetta::parquet
