#include "zetta/parquet/snappy.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

// Snappy raw-block codec — decoder plus the encoder the native Parquet writer
// (#833) emits pages with. The block layout (google/snappy
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

// --- encoder --------------------------------------------------------------

// The largest block the format's 32-bit length preamble can describe.
constexpr std::size_t kMaxBlockBytes = 0xffffffffu;

// Append `value` as a little-endian varint (the preamble form).
void append_varint(std::string& out, std::uint32_t value) {
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
}

// Append one literal element covering `bytes`. Lengths 1..60 ride in the tag;
// longer ones put (length - 1) in the following 1..4 little-endian bytes and
// set the tag's length field to 59 + that byte count — exactly what the
// decoder's `length > 60` branch above reverses.
void append_literal(std::string& out, std::string_view bytes) {
    if (bytes.empty()) return;
    const std::size_t length = bytes.size();
    if (length <= 60) {
        out.push_back(static_cast<char>((length - 1) << 2));
    } else {
        const std::uint32_t length_minus_one = static_cast<std::uint32_t>(length - 1);
        unsigned extra_bytes = 1;
        while (extra_bytes < 4 && (length_minus_one >> (8 * extra_bytes)) != 0) ++extra_bytes;
        out.push_back(static_cast<char>((59u + extra_bytes) << 2));
        for (unsigned byte_index = 0; byte_index < extra_bytes; ++byte_index) {
            out.push_back(static_cast<char>((length_minus_one >> (8 * byte_index)) & 0xff));
        }
    }
    out.append(bytes);
}

// Append one back-reference of `length` bytes at `offset` before the current
// output position, splitting it into as many copy elements as the tag forms
// allow (a single element carries at most 64 bytes). The 1-byte-offset form is
// the most compact but only spans 4..11 bytes at an offset below 2048; below
// 65536 the 2-byte form applies; beyond that the 4-byte form.
void append_copy(std::string& out, std::uint32_t offset, std::size_t length) {
    while (length > 0) {
        const std::size_t chunk = length < 64 ? length : 64;
        if (chunk >= 4 && chunk <= 11 && offset < 2048) {
            out.push_back(static_cast<char>(0x01 | ((chunk - 4) << 2) | ((offset >> 8) << 5)));
            out.push_back(static_cast<char>(offset & 0xff));
        } else if (offset < 65536) {
            out.push_back(static_cast<char>(((chunk - 1) << 2) | 0x02));
            out.push_back(static_cast<char>(offset & 0xff));
            out.push_back(static_cast<char>((offset >> 8) & 0xff));
        } else {
            out.push_back(static_cast<char>(((chunk - 1) << 2) | 0x03));
            for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
                out.push_back(static_cast<char>((offset >> (8 * byte_index)) & 0xff));
            }
        }
        length -= chunk;
    }
}

// The 4 bytes at `offset` as one word — only ever used for equality and hashing,
// so the host byte order does not matter.
std::uint32_t load_word(std::string_view input, std::size_t offset) {
    std::uint32_t word = 0;
    std::memcpy(&word, input.data() + offset, 4);
    return word;
}

// Fibonacci-style multiplicative hash of a 4-byte window, as google/snappy uses:
// the high `32 - shift` bits of the product spread the whole word.
std::size_t hash_word(std::uint32_t word, unsigned shift) {
    constexpr std::uint32_t kMultiplier = 0x1e35a7bdu;
    return static_cast<std::size_t>((word * kMultiplier) >> shift);
}

// "No candidate yet" — a position can never be this, because the block length is
// capped at kMaxBlockBytes (0xffffffff) so the last indexable position is
// smaller.
constexpr std::uint32_t kNoCandidate = 0xffffffffu;

}  // namespace

std::optional<std::uint32_t> snappy_uncompressed_length(std::string_view compressed) {
    std::size_t offset = 0;
    return read_preamble_length(compressed, offset);
}

SnappyResult snappy_compress_literals(std::string_view input) {
    if (input.size() > kMaxBlockBytes) {
        return fail("snappy: input exceeds the 32-bit block length preamble");
    }
    std::string output;
    output.reserve(input.size() + 8);
    append_varint(output, static_cast<std::uint32_t>(input.size()));
    append_literal(output, input);

    SnappyResult result;
    result.data = std::move(output);
    return result;
}

SnappyResult snappy_compress(std::string_view input) {
    if (input.size() > kMaxBlockBytes) {
        return fail("snappy: input exceeds the 32-bit block length preamble");
    }
    const std::size_t size = input.size();

    // Below 4 bytes there is no window to hash, so there is nothing a copy could
    // ever reference — the literal reference encoder already is the answer.
    if (size < 4) return snappy_compress_literals(input);

    std::string output;
    output.reserve(size / 2 + 32);
    append_varint(output, static_cast<std::uint32_t>(size));

    // One hash bucket per recent 4-byte window. Sized to the input (capped at
    // 2^14 entries, google/snappy's default) so a small page does not pay for a
    // large table; collisions only cost a missed match, never correctness,
    // because every candidate is verified against the actual bytes.
    unsigned table_bits = 8;
    while ((std::size_t{1} << table_bits) < size && table_bits < 14) ++table_bits;
    const unsigned shift = 32 - table_bits;
    std::vector<std::uint32_t> table(std::size_t{1} << table_bits, kNoCandidate);

    std::size_t literal_start = 0;
    std::size_t position = 0;
    while (position + 4 <= size) {
        const std::uint32_t word = load_word(input, position);
        std::uint32_t& bucket = table[hash_word(word, shift)];
        const std::uint32_t candidate = bucket;
        bucket = static_cast<std::uint32_t>(position);

        // A hash hit is only a *candidate*: compare the real bytes before
        // emitting a back-reference, so a collision can never corrupt output.
        if (candidate == kNoCandidate || load_word(input, candidate) != word) {
            ++position;
            continue;
        }

        std::size_t length = 4;
        while (position + length < size && input[candidate + length] == input[position + length]) {
            ++length;
        }

        append_literal(output, input.substr(literal_start, position - literal_start));
        append_copy(output, static_cast<std::uint32_t>(position - candidate), length);

        // Index the windows the match just skipped over, so a later match can
        // still reference them (without this, long matches blind the table).
        for (std::size_t inner = position + 1; inner < position + length && inner + 4 <= size;
             ++inner) {
            table[hash_word(load_word(input, inner), shift)] = static_cast<std::uint32_t>(inner);
        }

        position += length;
        literal_start = position;
    }
    append_literal(output, input.substr(literal_start));

    // Never ship a "compressed" page larger than the literal framing of the same
    // bytes: a pathological input can make the copy elements cost more than the
    // literals they replace.
    if (output.size() >= size + 8) {
        SnappyResult literals = snappy_compress_literals(input);
        if (literals.ok() && literals.data.size() < output.size()) return literals;
    }

    SnappyResult result;
    result.data = std::move(output);
    return result;
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
