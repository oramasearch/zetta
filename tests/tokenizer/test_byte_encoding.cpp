// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "zetta/tokenizer/byte_encoding.hpp"

namespace {

using zetta::tokenizer::byte_to_unicode;
using zetta::tokenizer::unicode_to_byte;
using zetta::tokenizer::utf8_decode;
using zetta::tokenizer::utf8_encode;

std::string encode_all(const std::vector<char32_t>& code_points) {
    std::string out;
    for (char32_t cp : code_points) utf8_encode(cp, out);
    return out;
}

TEST(ByteEncoding, Utf8RoundTripsAscii) {
    const std::string text = "Hello, world! 123";
    const std::vector<char32_t> code_points = utf8_decode(text);
    EXPECT_EQ(code_points.size(), text.size());  // all one-byte
    EXPECT_EQ(encode_all(code_points), text);
}

TEST(ByteEncoding, Utf8RoundTripsMultibyte) {
    // é (U+00E9, 2 bytes), € (U+20AC, 3 bytes), 😀 (U+1F600, 4 bytes), Ġ (U+0120).
    const std::string text = "café €10 😀 Ġ";
    const std::vector<char32_t> code_points = utf8_decode(text);
    ASSERT_FALSE(code_points.empty());
    EXPECT_EQ(encode_all(code_points), text);
    // Spot-check specific code points are decoded.
    bool has_euro = false;
    bool has_emoji = false;
    for (char32_t cp : code_points) {
        if (cp == 0x20AC) has_euro = true;
        if (cp == 0x1F600) has_emoji = true;
    }
    EXPECT_TRUE(has_euro);
    EXPECT_TRUE(has_emoji);
}

TEST(ByteEncoding, Utf8MalformedDecodesToReplacement) {
    const std::string bad = "\xFF\xFE";  // invalid lead bytes
    const std::vector<char32_t> code_points = utf8_decode(bad);
    ASSERT_EQ(code_points.size(), 2U);
    EXPECT_EQ(code_points[0], 0xFFFDu);
    EXPECT_EQ(code_points[1], 0xFFFDu);
}

// The GPT-2 byte<->unicode map is a bijection over all 256 bytes.
TEST(ByteEncoding, ByteUnicodeMapIsABijection) {
    const auto& forward = byte_to_unicode();
    std::vector<char32_t> seen;
    for (int byte = 0; byte < 256; ++byte) {
        const char32_t cp = forward[byte];
        EXPECT_EQ(unicode_to_byte(cp), byte);  // inverse recovers the byte
        seen.push_back(cp);
    }
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(std::unique(seen.begin(), seen.end()), seen.end());  // all distinct
}

// The known GPT-2 anchors: printable ASCII maps to itself; space maps to Ġ.
TEST(ByteEncoding, KnownByteMappings) {
    const auto& forward = byte_to_unicode();
    EXPECT_EQ(forward['A'], U'A');       // printable maps to itself
    EXPECT_EQ(forward['~'], U'~');
    EXPECT_EQ(forward[' '], U'Ġ');  // space -> Ġ (the leading-space marker)
    EXPECT_EQ(forward[0], U'Ā');    // NUL -> Ā (first remapped byte)
    EXPECT_EQ(forward['\n'], U'Ċ');  // newline (0x0A) -> Ċ
    EXPECT_EQ(unicode_to_byte(U'Ġ'), ' ');
    EXPECT_EQ(unicode_to_byte(U'A'), 'A');
    EXPECT_EQ(unicode_to_byte(U'A'), 'A');
    EXPECT_EQ(unicode_to_byte(0x9999), -1);  // not in the map
}

}  // namespace
