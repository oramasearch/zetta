// SPDX-License-Identifier: MIT

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A host-float safetensors reader/writer for zetta's autograd (#555;
// docs/autograd/README.md). It parses the safetensors container — an 8-byte
// little-endian header length, a JSON header naming each tensor's dtype / shape /
// byte range, then the contiguous little-endian tensor bytes — into plain host f32
// tensors, decoding F32 and BF16 (the dtypes Qwen ships). No foreign runtime and
// no MLX: this is the always-compiled path that loads a checkpoint into a
// `QwenModel` by tensor name. Malformed input is a non-throwing boundary — parsing
// returns std::nullopt with a reason, never an exception.
namespace zetta::autograd {

// One tensor, decoded to host f32 regardless of its on-disk dtype.
struct SafeTensor {
    std::vector<int> shape;
    std::vector<float> data;
};

// Tensor name -> tensor. Ordered so serialization is deterministic.
using SafeTensors = std::map<std::string, SafeTensor>;

// Parse a safetensors byte buffer. Returns std::nullopt on malformed input,
// writing a reason to `*error` when provided. Supports F32 and BF16.
std::optional<SafeTensors> parse_safetensors(std::string_view bytes,
                                             std::string* error = nullptr);

// Serialize tensors to a safetensors byte string (written as F32 on disk).
std::string serialize_safetensors(const SafeTensors& tensors);

// Read + parse a .safetensors file. std::nullopt (with a reason) if the file is
// missing or malformed.
std::optional<SafeTensors> read_safetensors_file(const std::string& path,
                                                 std::string* error = nullptr);

// Serialize + write tensors to a .safetensors file. False if the file can't be
// written.
bool write_safetensors_file(const std::string& path, const SafeTensors& tensors);

}  // namespace zetta::autograd
