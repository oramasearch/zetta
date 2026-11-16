// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

// The safetensors reader/writer. Compiles only under ZETTA_WITH_MLX.
#if defined(ZETTA_WITH_MLX)

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <mlx/mlx.h>

#include "zetta/compute/safetensors.hpp"

namespace {

namespace mx = mlx::core;
using zetta::compute::model::deserialize_safetensors;
using zetta::compute::model::serialize_safetensors;

mx::array make(std::vector<float> values, mx::Shape shape) {
    return mx::array(values.data(), std::move(shape), mx::float32);
}

std::vector<float> host(const mx::array& array) {
    mx::array evaluated = array;
    mx::eval(evaluated);
    return std::vector<float>(evaluated.data<float>(),
                              evaluated.data<float>() + evaluated.size());
}

TEST(Safetensors, RoundTripsNamedF32Tensors) {
    std::map<std::string, mx::array> tensors;
    tensors.emplace("weight", make({1, 2, 3, 4, 5, 6}, mx::Shape{2, 3}));
    tensors.emplace("bias", make({-1.0F, 0.5F, 2.0F}, mx::Shape{3}));

    const std::string bytes = serialize_safetensors(tensors);

    // Format: 8-byte little-endian header length, header begins with '{', total
    // size is 8 + header + the F32 data (9 floats).
    ASSERT_GT(bytes.size(), 8u);
    std::uint64_t header_size = 0;
    for (int i = 0; i < 8; ++i) {
        header_size |= static_cast<std::uint64_t>(
                           static_cast<unsigned char>(bytes[i]))
                       << (8 * i);
    }
    EXPECT_EQ(bytes[8], '{');
    EXPECT_EQ(bytes.size(), 8 + header_size + 9 * sizeof(float));

    const auto loaded = deserialize_safetensors(bytes);
    ASSERT_EQ(loaded.size(), 2u);
    ASSERT_EQ(loaded.count("weight"), 1u);
    ASSERT_EQ(loaded.count("bias"), 1u);
    EXPECT_EQ(loaded.at("weight").shape(0), 2);
    EXPECT_EQ(loaded.at("weight").shape(1), 3);
    EXPECT_EQ(host(loaded.at("weight")), (std::vector<float>{1, 2, 3, 4, 5, 6}));
    EXPECT_EQ(host(loaded.at("bias")), (std::vector<float>{-1.0F, 0.5F, 2.0F}));
}

// Real HuggingFace checkpoints (Qwen included) ship BF16; the reader must decode it.
TEST(Safetensors, DecodesBf16LikeARealCheckpoint) {
    // One BF16 tensor "t" = {1.0, -2.0}. BF16(1.0)=0x3F80, BF16(-2.0)=0xC000,
    // stored little-endian.
    const std::string header =
        R"({"t":{"dtype":"BF16","shape":[2],"data_offsets":[0,4]}})";
    std::string bytes;
    const std::uint64_t header_size = header.size();
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<char>((header_size >> (8 * i)) & 0xFF));
    }
    bytes += header;
    bytes.push_back(static_cast<char>(0x80));  // 0x3F80 low
    bytes.push_back(static_cast<char>(0x3F));  // high
    bytes.push_back(static_cast<char>(0x00));  // 0xC000 low
    bytes.push_back(static_cast<char>(0xC0));  // high

    const auto loaded = deserialize_safetensors(bytes);
    ASSERT_EQ(loaded.size(), 1u);
    const std::vector<float> values = host(loaded.at("t"));
    ASSERT_EQ(values.size(), 2u);
    EXPECT_FLOAT_EQ(values[0], 1.0F);
    EXPECT_FLOAT_EQ(values[1], -2.0F);
}

TEST(Safetensors, MalformedInputYieldsEmpty) {
    EXPECT_TRUE(deserialize_safetensors("").empty());
    EXPECT_TRUE(deserialize_safetensors("not a safetensors file").empty());
}

}  // namespace

#endif  // ZETTA_WITH_MLX
