// SPDX-License-Identifier: MIT

#pragma once

// Internal (not a public header): the shared JSON-boundary field readers the
// provisioner Config `from_json` parsers use. Each cloud-burst Config
// (AzureConfig / AwsConfig / GcpConfig / OciConfig / K8sConfig / BareMetalConfig)
// validates its shape at the boundary with the same helpers, so they live here
// once in src/core rather than being copy-pasted into each <x>_provisioner.cpp.

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace zetta::detail {

// A string field on a JSON object: must be a string when present. `required`
// fields must be present and non-empty. Returns false (out untouched) on a
// missing-required / wrong-typed / empty-required field — the boundary check.
inline bool read_string(const nlohmann::json& object, const char* key,
                        bool required, std::string& out) {
    if (!object.contains(key)) return !required;
    const nlohmann::json& value = object[key];
    if (!value.is_string()) return false;
    std::string text = value.get<std::string>();
    if (required && text.empty()) return false;
    out = std::move(text);
    return true;
}

// An optional non-negative uint32 field: absent leaves `out` untouched. Accepts a
// JSON integer whether stored signed or unsigned; rejects a non-integer, a negative
// value, or one above the uint32 range — the boundary check for a count/size field.
inline bool read_uint32(const nlohmann::json& object, const char* key,
                        std::uint32_t& out) {
    if (!object.contains(key)) return true;  // absent is fine (keep the default)
    const nlohmann::json& value = object[key];
    if (value.is_number_unsigned()) {
        const std::uint64_t raw = value.get<std::uint64_t>();
        if (raw > std::numeric_limits<std::uint32_t>::max()) return false;
        out = static_cast<std::uint32_t>(raw);
        return true;
    }
    if (value.is_number_integer()) {
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value < 0) return false;
        if (signed_value > std::numeric_limits<std::uint32_t>::max()) return false;
        out = static_cast<std::uint32_t>(signed_value);
        return true;
    }
    return false;  // not an integer
}

}  // namespace zetta::detail
