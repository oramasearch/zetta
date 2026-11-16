// SPDX-License-Identifier: MIT

#pragma once

// The safetensors binary format (#555; docs/agent/README.md) — the standard
// HuggingFace weight-serialization format, so it is how the native transformer
// saves a checkpoint and how real open weights (Qwen and friends) are ingested.
// Layout: an 8-byte little-endian u64 header length, then that many bytes of JSON
// header ({ "<name>": {"dtype","shape","data_offsets":[begin,end]}, "__metadata__":
// {...} }), then a contiguous little-endian data buffer; data_offsets are relative
// to the start of that buffer. Serialization writes F32; deserialization reads F32
// and BF16 (Qwen ships BF16, decoded to F32 here). Lives in zetta-compute under
// ZETTA_WITH_MLX, like the rest of the model runtime.
#if defined(ZETTA_WITH_MLX)

#include <map>
#include <string>
#include <string_view>

#include <mlx/mlx.h>

namespace zetta::compute::model {

// Serialize named F32 arrays to safetensors bytes. Tensors are written in key
// order (deterministic).
std::string serialize_safetensors(
    const std::map<std::string, mlx::core::array>& tensors);

// Parse safetensors bytes into named F32 arrays (F32 copied, BF16 decoded to F32).
// Malformed input yields an empty map (non-throwing boundary).
std::map<std::string, mlx::core::array> deserialize_safetensors(
    std::string_view bytes);

// File convenience over the byte API.
bool write_safetensors_file(
    const std::string& path,
    const std::map<std::string, mlx::core::array>& tensors);
std::map<std::string, mlx::core::array> read_safetensors_file(
    const std::string& path);

}  // namespace zetta::compute::model

#endif  // ZETTA_WITH_MLX
