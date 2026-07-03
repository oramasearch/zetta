// Snappy raw-block decoder tests (the Parquet SNAPPY page codec, #507).
//
// We have no offline Snappy *compressor* in the build (ADR-17 forbids a foreign
// runtime), so the ground truth is the published Snappy block format itself,
// made executable: each `SpecVector` is a compressed byte sequence hand-built
// per google/snappy's format_description.txt next to the exact bytes it must
// decode to. The decoder passing them proves it matches the on-disk format, not
// merely that it round-trips with some encoder of our own. A larger
// "page-shaped" golden with real back-references and a battery of malformed
// inputs round out the coverage.

#include "zetta/parquet/snappy.hpp"

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using zetta::parquet::snappy_decompress;
using zetta::parquet::snappy_uncompressed_length;

namespace {

// Helpers that build compressed elements byte-for-byte per the spec, so the
// test data is transparently correct rather than produced by an encoder we'd
// also have to trust.
std::string varint(std::uint32_t value) {
    std::string out;
    while (value >= 0x80) {
        out.push_back(static_cast<char>((value & 0x7f) | 0x80));
        value >>= 7;
    }
    out.push_back(static_cast<char>(value));
    return out;
}

// A short literal (length 1..60): tag = (length-1)<<2, then the bytes.
std::string literal(const std::string& bytes) {
    std::string out;
    out.push_back(static_cast<char>((bytes.size() - 1) << 2));
    out += bytes;
    return out;
}

// A copy with a 2-byte offset: low 2 bits = 10, length = (tag>>2)+1, offset LE.
std::string copy2(std::uint32_t offset, std::size_t length) {
    std::string out;
    out.push_back(static_cast<char>(((length - 1) << 2) | 0x02));
    out.push_back(static_cast<char>(offset & 0xff));
    out.push_back(static_cast<char>((offset >> 8) & 0xff));
    return out;
}

struct SpecVector {
    std::string name;
    std::string compressed;
    std::string expected;
};

}  // namespace

// Each element type decoded against a hand-derived expected output.
TEST(ParquetSnappy, CanonicalSpecVectors) {
    std::vector<SpecVector> vectors;

    // Empty block: preamble length 0, no elements.
    vectors.push_back({"empty", varint(0), ""});

    // Short literal "abc": preamble 3, tag 0x08 (len-1=2), then "abc".
    vectors.push_back(
        {"short_literal", varint(3) + std::string("\x08", 1) + "abc", "abc"});

    // Literal of exactly 60 bytes — the largest that still fits in the tag
    // (length_minus_1 = 59 < 60, no extra length bytes).
    {
        const std::string body(60, 'x');
        std::string compressed = varint(60);
        compressed.push_back(static_cast<char>(59 << 2));  // tag, len-1 = 59
        compressed += body;
        vectors.push_back({"literal_60_inline", compressed, body});
    }

    // Literal of 61 bytes — the first that overflows the tag, so length-1 (60)
    // is stored in 1 following byte (tag>>2 == 60).
    {
        const std::string body(61, 'y');
        std::string compressed = varint(61);
        compressed.push_back(static_cast<char>(60 << 2));  // tag value 60
        compressed.push_back(static_cast<char>(60));       // length-1, 1 byte LE
        compressed += body;
        vectors.push_back({"literal_61_one_extra_byte", compressed, body});
    }

    // Literal of 300 bytes — length-1 (299 = 0x012B) stored in 2 bytes LE
    // (tag>>2 == 61).
    {
        const std::string body(300, 'z');
        std::string compressed = varint(300);
        compressed.push_back(static_cast<char>(61 << 2));  // tag value 61
        compressed.push_back(static_cast<char>(0x2b));     // 299 & 0xff
        compressed.push_back(static_cast<char>(0x01));     // 299 >> 8
        compressed += body;
        vectors.push_back({"literal_300_two_extra_bytes", compressed, body});
    }

    // Copy, 1-byte offset: "abc" then copy(offset=3, length=6) → "abcabcabc".
    // tag = (length-4)<<2 | 0x01 with the offset's high 3 bits in bits 5..7;
    // offset 3 fits in the low byte, high bits 0 → tag 0x09, offset byte 0x03.
    vectors.push_back({"copy1_nonoverlapping",
                       varint(9) + literal("abc") +
                           std::string("\x09\x03", 2),
                       "abcabcabc"});

    // Overlapping copy (RLE): "a" then copy(offset=1, length=5) → "aaaaaa".
    // length 5 → (5-4)=1 → tag (1<<2)|0x01 = 0x05; offset byte 0x01.
    vectors.push_back({"copy1_overlap_rle",
                       varint(6) + literal("a") + std::string("\x05\x01", 2),
                       "aaaaaa"});

    // Copy, 2-byte offset: "hello" then copy(offset=5, length=5) → "hellohello".
    vectors.push_back({"copy2",
                       varint(10) + literal("hello") + copy2(5, 5),
                       "hellohello"});

    // Copy, 4-byte offset: a 70-byte literal forces an offset that still fits
    // 2 bytes, so build the 4-byte form explicitly to exercise element_type 3.
    {
        const std::string head(70, 'q');
        std::string compressed = varint(75);
        // 70-byte literal (length-1 = 69 < 60? no — 69 >= 60), use the 1-extra
        // form: tag value 60, then length-1 = 69.
        compressed.push_back(static_cast<char>(60 << 2));
        compressed.push_back(static_cast<char>(69));
        compressed += head;
        // copy(offset=70, length=5) with the 4-byte offset form: tag low bits
        // 11, length = (tag>>2)+1 → (5-1)<<2 | 0x03 = 0x13; offset 70 LE.
        compressed.push_back(static_cast<char>(((5 - 1) << 2) | 0x03));
        compressed.push_back(static_cast<char>(70));
        compressed.push_back(0);
        compressed.push_back(0);
        compressed.push_back(0);
        vectors.push_back({"copy4", compressed, head + std::string(5, 'q')});
    }

    for (const auto& vector : vectors) {
        const auto result = snappy_decompress(vector.compressed);
        ASSERT_TRUE(result.ok()) << vector.name << ": " << result.error;
        EXPECT_EQ(result.data, vector.expected) << vector.name;
        // The preamble-only length helper must agree with the real output size.
        const auto length = snappy_uncompressed_length(vector.compressed);
        ASSERT_TRUE(length.has_value()) << vector.name;
        EXPECT_EQ(*length, vector.expected.size()) << vector.name;
    }
}

// A realistic "data page" golden: a 16-byte dictionary pattern repeated 64×
// (1024 bytes), the way a parquet snappy page packs repetitive column data —
// built as one literal plus a chain of back-reference copies, decoded back to
// the byte-identical uncompressed golden.
TEST(ParquetSnappy, PageShapedGoldenByteIdentical) {
    const std::string pattern = "ABCDEFGHIJKLMNOP";  // 16 bytes
    const int repeats = 64;

    std::string golden;
    for (int i = 0; i < repeats; ++i) golden += pattern;
    ASSERT_EQ(golden.size(), 1024u);

    // Compressed: preamble, the pattern once as a literal, then (repeats-1)
    // copies of offset=16,length=16 reproducing each subsequent block from the
    // output already written.
    std::string compressed = [&] {
        std::string out;
        out += [](std::uint32_t v) {
            std::string s;
            while (v >= 0x80) { s.push_back(char((v & 0x7f) | 0x80)); v >>= 7; }
            s.push_back(char(v));
            return s;
        }(static_cast<std::uint32_t>(golden.size()));
        // literal pattern (16 bytes → tag (16-1)<<2 = 0x3c)
        out.push_back(static_cast<char>((pattern.size() - 1) << 2));
        out += pattern;
        // copy(offset=16, length=16), 2-byte offset form, repeated.
        for (int i = 0; i < repeats - 1; ++i) {
            out.push_back(static_cast<char>(((16 - 1) << 2) | 0x02));
            out.push_back(static_cast<char>(16));
            out.push_back(0);
        }
        return out;
    }();

    EXPECT_LT(compressed.size(), golden.size());  // it actually compressed
    const auto result = snappy_decompress(compressed);
    ASSERT_TRUE(result.ok()) << result.error;
    EXPECT_EQ(result.data, golden);
}

// Malformed input is rejected with a reason, never a crash or out-of-bounds
// read — the binary trust boundary.
TEST(ParquetSnappy, MalformedInputsAreRejected) {
    // Truncated preamble (continuation bit set, no following byte).
    {
        const auto result = snappy_decompress(std::string("\x80", 1));
        EXPECT_FALSE(result.ok());
        EXPECT_FALSE(snappy_uncompressed_length(std::string("\x80", 1)).has_value());
    }
    // Overlong varint (> 5 bytes, exceeds 32 bits).
    {
        const std::string overlong("\x80\x80\x80\x80\x80\x01", 6);
        EXPECT_FALSE(snappy_decompress(overlong).ok());
        EXPECT_FALSE(snappy_uncompressed_length(overlong).has_value());
    }
    // Literal claims more bytes than the input holds.
    {
        std::string compressed;
        compressed.push_back(10);                          // declared length 10
        compressed.push_back(static_cast<char>(9 << 2));   // literal len 10
        compressed += "abc";                               // only 3 present
        EXPECT_FALSE(snappy_decompress(compressed).ok());
    }
    // Copy offset points before the start of the output.
    {
        std::string compressed;
        compressed.push_back(5);                                // declared len 5
        compressed.push_back(static_cast<char>(((5 - 1) << 2) | 0x02));  // copy2 len5
        compressed.push_back(static_cast<char>(1));             // offset 1...
        compressed.push_back(0);                                // ...but output empty
        EXPECT_FALSE(snappy_decompress(compressed).ok());
    }
    // Copy with a zero offset is invalid.
    {
        std::string compressed;
        compressed.push_back(2);
        compressed.push_back(static_cast<char>(0));  // literal 'a'
        compressed.push_back('a');
        compressed.push_back(static_cast<char>(((1 - 1) << 2) | 0x02));  // copy2 len1
        compressed.push_back(0);                                          // offset 0
        compressed.push_back(0);
        EXPECT_FALSE(snappy_decompress(compressed).ok());
    }
    // Output ends up shorter than the preamble declared (truncated stream).
    {
        std::string compressed;
        compressed.push_back(10);                          // declared length 10
        compressed.push_back(static_cast<char>(2 << 2));   // literal len 3
        compressed += "abc";                               // only 3 produced
        const auto result = snappy_decompress(compressed);
        EXPECT_FALSE(result.ok());
    }
    // A literal that would overrun the declared length.
    {
        std::string compressed;
        compressed.push_back(2);                           // declared length 2
        compressed.push_back(static_cast<char>(4 << 2));   // literal len 5
        compressed += "abcde";
        EXPECT_FALSE(snappy_decompress(compressed).ok());
    }
}

// An empty input (no preamble at all) is malformed, not an empty block.
TEST(ParquetSnappy, EmptyInputIsRejected) {
    EXPECT_FALSE(snappy_decompress("").ok());
    EXPECT_FALSE(snappy_uncompressed_length("").has_value());
}
