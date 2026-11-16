// SPDX-License-Identifier: MIT

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

// --- compressor (#833) ------------------------------------------------------
//
// The write side of the codec. The property under test is the one the Parquet
// writer depends on: every encoder output decodes back to its exact input,
// through the same decoder the reader uses on a real pyarrow page. Inputs are
// chosen adversarially — empty, single byte, every literal-length boundary the
// tag encoding switches on (60/61, 256/257, 65536/65537), all-one-byte,
// pseudo-random (incompressible), highly repetitive, and every byte value
// including embedded NULs.
namespace {

using zetta::parquet::snappy_compress;
using zetta::parquet::snappy_compress_literals;

// A deterministic, self-contained pseudo-random generator (xorshift64*) so the
// "incompressible input" cases are reproducible without depending on <random>
// implementation details across toolchains.
class DeterministicBytes {
public:
    explicit DeterministicBytes(std::uint64_t seed) : state_(seed | 1u) {}

    std::string take(std::size_t count) {
        std::string out;
        out.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            out.push_back(static_cast<char>(next() & 0xff));
        }
        return out;
    }

    std::uint64_t next() {
        state_ ^= state_ >> 12;
        state_ ^= state_ << 25;
        state_ ^= state_ >> 27;
        return state_ * 0x2545f4914f6cdd1dull;
    }

private:
    std::uint64_t state_;
};

// Both encoders must satisfy the round trip, and their declared preamble length
// must match the input — the two facts the Parquet page path relies on.
void expect_round_trip(const std::string& input, const std::string& name) {
    for (const bool literals_only : {false, true}) {
        const auto compressed =
            literals_only ? snappy_compress_literals(input) : snappy_compress(input);
        const std::string which = literals_only ? " [literals]" : " [matched]";
        ASSERT_TRUE(compressed.ok()) << name << which << ": " << compressed.error;

        const auto declared = snappy_uncompressed_length(compressed.data);
        ASSERT_TRUE(declared.has_value()) << name << which;
        EXPECT_EQ(*declared, input.size()) << name << which;

        const auto restored = snappy_decompress(compressed.data);
        ASSERT_TRUE(restored.ok()) << name << which << ": " << restored.error;
        EXPECT_EQ(restored.data, input) << name << which;
    }
}

}  // namespace

// Sizes straddling every internal boundary of the literal-length encoding, over
// several input shapes.
TEST(ParquetSnappyCompress, RoundTripsAcrossLengthBoundaries) {
    const std::vector<std::size_t> sizes = {0,   1,     2,     59,    60,    61,    62,
                                            63,  64,    127,   128,   254,   255,   256,
                                            257, 511,   512,   1023,  1024,  4095,  4096,
                                            65534, 65535, 65536, 65537, 70000};
    DeterministicBytes noise(0x5eed1234u);
    for (std::size_t size : sizes) {
        const std::string label = "size=" + std::to_string(size);
        expect_round_trip(std::string(size, '\0'), label + " zeros");
        expect_round_trip(std::string(size, 'x'), label + " same-byte");
        expect_round_trip(noise.take(size), label + " random");

        // A repetitive pattern longer than one copy element can express, so the
        // match encoder has to chain copies.
        const std::string unit("the quick brown fox\0jumps", 25);  // embedded NUL
        std::string repetitive;
        while (repetitive.size() < size) repetitive += unit;
        repetitive.resize(size);
        expect_round_trip(repetitive, label + " repetitive");

        // Every byte value cycled, including embedded NULs and 0xff.
        std::string all_bytes;
        for (std::size_t index = 0; index < size; ++index) {
            all_bytes.push_back(static_cast<char>(index % 256));
        }
        expect_round_trip(all_bytes, label + " all-bytes");
    }
}

// Property-style sweep: many sizes, mixed compressibility, one loop.
TEST(ParquetSnappyCompress, RoundTripsOverGeneratedInputs) {
    DeterministicBytes generator(0xabcdef01u);
    for (int iteration = 0; iteration < 400; ++iteration) {
        const std::size_t size = static_cast<std::size_t>(generator.next() % 3000u);
        // Mix a small alphabet (compressible) with raw noise (not), varying the
        // ratio per iteration so matches appear at every density.
        const std::uint64_t alphabet = 1 + (generator.next() % 200u);
        std::string input;
        input.reserve(size);
        for (std::size_t index = 0; index < size; ++index) {
            input.push_back(static_cast<char>(generator.next() % alphabet));
        }
        expect_round_trip(input, "iteration=" + std::to_string(iteration));
    }
}

// The literals-only reference never compresses (it is framing overhead on top
// of the input) — that is exactly what makes it the baseline: a round-trip
// failure it shares with the real encoder is a framing bug, not a match bug.
TEST(ParquetSnappyCompress, LiteralReferenceNeverShrinksItsInput) {
    for (std::size_t size : {std::size_t{0}, std::size_t{1}, std::size_t{1000}}) {
        const std::string input(size, 'a');
        const auto compressed = snappy_compress_literals(input);
        ASSERT_TRUE(compressed.ok()) << compressed.error;
        EXPECT_GT(compressed.data.size(), input.size());
    }
}

// The shipped encoder must actually compress — otherwise a partition body is
// larger on disk than the rows it holds and the SNAPPY codec is a lie. Checked
// against the literals-only reference on the same input, which is the honest
// "no compression at all" number.
TEST(ParquetSnappyCompress, MatchEncoderShrinksRepetitiveInput) {
    struct Case {
        std::string name;
        std::string input;
        double max_ratio;  // compressed / uncompressed the encoder must beat
    };
    std::vector<Case> cases;

    // A single copy element spans at most 64 bytes, so even an infinitely
    // repetitive input costs ~3 bytes per 64 — ~4.7% is the format's floor here,
    // not a weakness of the search.
    cases.push_back({"one byte repeated", std::string(100000, 'a'), 0.06});

    {
        // A column-shaped body: the same 16-byte value repeated, the way a
        // low-cardinality parquet page packs.
        std::string body;
        while (body.size() < 64000) body += "ABCDEFGHIJKLMNOP";
        cases.push_back({"repeated pattern", body, 0.05});
    }
    {
        // PLAIN-encoded int64s of a slowly increasing counter: seven of every
        // eight bytes are zero, the realistic numeric-column shape.
        std::string body;
        for (std::int64_t value = 0; value < 8000; ++value) {
            for (unsigned byte_index = 0; byte_index < 8; ++byte_index) {
                body.push_back(static_cast<char>((value >> (8 * byte_index)) & 0xff));
            }
        }
        cases.push_back({"plain int64 column", body, 0.60});
    }
    {
        // Length-prefixed strings from a small vocabulary — a byte_array column.
        std::string body;
        const std::vector<std::string> vocabulary = {"active", "churned", "trial", "paused"};
        for (int index = 0; index < 4000; ++index) {
            const std::string& value = vocabulary[static_cast<std::size_t>(index) % vocabulary.size()];
            const auto length = static_cast<std::uint32_t>(value.size());
            for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
                body.push_back(static_cast<char>((length >> (8 * byte_index)) & 0xff));
            }
            body += value;
        }
        cases.push_back({"byte_array column", body, 0.10});
    }

    for (const Case& test_case : cases) {
        const auto compressed = snappy_compress(test_case.input);
        ASSERT_TRUE(compressed.ok()) << test_case.name << ": " << compressed.error;
        const auto literals = snappy_compress_literals(test_case.input);
        ASSERT_TRUE(literals.ok()) << test_case.name;

        EXPECT_LT(compressed.data.size(), literals.data.size()) << test_case.name;
        EXPECT_LT(static_cast<double>(compressed.data.size()) /
                      static_cast<double>(test_case.input.size()),
                  test_case.max_ratio)
            << test_case.name;

        const auto restored = snappy_decompress(compressed.data);
        ASSERT_TRUE(restored.ok()) << test_case.name << ": " << restored.error;
        EXPECT_EQ(restored.data, test_case.input) << test_case.name;
    }
}

// Incompressible input must not blow up in size: the encoder falls back to the
// literal framing rather than paying for copy elements that buy nothing.
TEST(ParquetSnappyCompress, IncompressibleInputStaysNearItsOriginalSize) {
    DeterministicBytes noise(0x13579bdfu);
    const std::string input = noise.take(50000);
    const auto compressed = snappy_compress(input);
    ASSERT_TRUE(compressed.ok()) << compressed.error;
    EXPECT_LT(compressed.data.size(), input.size() + input.size() / 16);
    const auto restored = snappy_decompress(compressed.data);
    ASSERT_TRUE(restored.ok()) << restored.error;
    EXPECT_EQ(restored.data, input);
}
