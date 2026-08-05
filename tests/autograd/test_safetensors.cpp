#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/autograd/safetensors.hpp"

namespace {

using zetta::autograd::parse_safetensors;
using zetta::autograd::read_safetensors_file;
using zetta::autograd::SafeTensor;
using zetta::autograd::SafeTensors;
using zetta::autograd::serialize_safetensors;
using zetta::autograd::write_safetensors_file;

// Assemble a raw safetensors buffer from a JSON header and its data bytes: the
// 8-byte little-endian header length, the header, then the data.
std::string make_buffer(const std::string& header, const std::vector<std::uint8_t>& data) {
    std::string out;
    const std::uint64_t header_size = header.size();
    for (int byte = 0; byte < 8; ++byte) {
        out.push_back(static_cast<char>((header_size >> (8 * byte)) & 0xFF));
    }
    out.append(header);
    out.append(reinterpret_cast<const char*>(data.data()), data.size());
    return out;
}

TEST(SafeTensors, RoundTripsF32ExactlyThroughSerialize) {
    SafeTensors original;
    original["a"] = SafeTensor{{2, 3}, {1.0F, -2.5F, 3.25F, 0.0F, 100.0F, -0.125F}};
    original["b"] = SafeTensor{{2}, {42.0F, -1.0F}};

    const std::string bytes = serialize_safetensors(original);
    const std::optional<SafeTensors> parsed = parse_safetensors(bytes);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->size(), 2U);
    EXPECT_EQ(parsed->at("a").shape, (std::vector<int>{2, 3}));
    EXPECT_EQ(parsed->at("a").data, original["a"].data);  // F32 is exact
    EXPECT_EQ(parsed->at("b").shape, (std::vector<int>{2}));
    EXPECT_EQ(parsed->at("b").data, original["b"].data);
}

// BF16 is the high 16 bits of an f32; values with zero low mantissa bits decode
// exactly. 1.0=0x3F80, 2.0=0x4000, -0.5=0xBF00, 0.0=0x0000 (little-endian pairs).
TEST(SafeTensors, DecodesBf16) {
    const std::string header =
        R"({"w":{"dtype":"BF16","shape":[4],"data_offsets":[0,8]}})";
    const std::vector<std::uint8_t> data = {0x80, 0x3F, 0x00, 0x40,
                                            0x00, 0xBF, 0x00, 0x00};
    const std::optional<SafeTensors> parsed = parse_safetensors(make_buffer(header, data));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->at("w").shape, (std::vector<int>{4}));
    EXPECT_EQ(parsed->at("w").data, (std::vector<float>{1.0F, 2.0F, -0.5F, 0.0F}));
}

TEST(SafeTensors, IgnoresMetadataEntry) {
    const std::string header =
        R"({"__metadata__":{"format":"pt"},)"
        R"("w":{"dtype":"F32","shape":[1],"data_offsets":[0,4]}})";
    const std::vector<std::uint8_t> data = {0x00, 0x00, 0x80, 0x3F};  // 1.0f
    const std::optional<SafeTensors> parsed = parse_safetensors(make_buffer(header, data));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->size(), 1U);
    EXPECT_EQ(parsed->at("w").data, (std::vector<float>{1.0F}));
}

TEST(SafeTensors, WritesAndReadsAFile) {
    const zetta::test::TempDir temp;
    const std::string path = (temp.path() / "model.safetensors").string();
    SafeTensors original;
    original["x"] = SafeTensor{{3}, {0.5F, 1.5F, -2.0F}};
    ASSERT_TRUE(write_safetensors_file(path, original));

    const std::optional<SafeTensors> parsed = read_safetensors_file(path);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->at("x").data, original["x"].data);
}

// Malformed inputs are a non-throwing boundary: nullopt + a reason.
TEST(SafeTensors, RejectsMalformedInput) {
    std::string error;

    EXPECT_FALSE(parse_safetensors("abc", &error).has_value());  // < 8 bytes
    EXPECT_FALSE(error.empty());

    // Header length claims more bytes than the buffer holds.
    std::string too_long(8, '\0');
    too_long[0] = static_cast<char>(200);
    EXPECT_FALSE(parse_safetensors(too_long, &error).has_value());

    // Unsupported dtype.
    const std::string header =
        R"({"w":{"dtype":"I64","shape":[1],"data_offsets":[0,8]}})";
    EXPECT_FALSE(
        parse_safetensors(make_buffer(header, {0, 0, 0, 0, 0, 0, 0, 0}), &error)
            .has_value());
    EXPECT_NE(error.find("I64"), std::string::npos);

    // data_offsets past the end of the data section.
    const std::string oob =
        R"({"w":{"dtype":"F32","shape":[1],"data_offsets":[0,999]}})";
    EXPECT_FALSE(parse_safetensors(make_buffer(oob, {0, 0, 0, 0}), &error).has_value());

    EXPECT_FALSE(read_safetensors_file("/no/such/path.safetensors", &error).has_value());
}

}  // namespace
