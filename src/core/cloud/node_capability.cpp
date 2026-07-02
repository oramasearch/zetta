#include "zetta/cloud/node_capability.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace zetta {

std::string_view to_string(GpuKind kind) {
    switch (kind) {
        case GpuKind::None:  return "none";
        case GpuKind::Cuda:  return "cuda";
        case GpuKind::Rocm:  return "rocm";
        case GpuKind::Metal: return "metal";
    }
    return "unknown";
}

bool gpu_kind_from_string(std::string_view text, GpuKind& out) {
    constexpr std::array<GpuKind, 4> kinds{GpuKind::None, GpuKind::Cuda,
                                           GpuKind::Rocm, GpuKind::Metal};
    for (const GpuKind kind : kinds) {
        if (to_string(kind) == text) {
            out = kind;
            return true;
        }
    }
    return false;
}

bool NodeCapability::satisfies(const NodeCapability& request) const {
    if (cores < request.cores) return false;
    if (memory_bytes < request.memory_bytes) return false;
    if (request.gpu.present()) {
        if (gpu.kind != request.gpu.kind) return false;
        if (gpu.throughput_tflops < request.gpu.throughput_tflops) return false;
    }
    return true;
}

namespace {

// Accept a JSON integer that is non-negative, whether nlohmann stored it signed
// (a plain `4` literal) or unsigned (what to_json writes). Returns false if it is
// not an integer or is negative — the boundary check for a count/size field.
bool as_nonneg_u64(const nlohmann::json& value, std::uint64_t& out) {
    if (value.is_number_unsigned()) {
        out = value.get<std::uint64_t>();
        return true;
    }
    if (value.is_number_integer()) {  // signed-but-non-negative
        const std::int64_t signed_value = value.get<std::int64_t>();
        if (signed_value < 0) return false;
        out = static_cast<std::uint64_t>(signed_value);
        return true;
    }
    return false;
}

nlohmann::json gpu_to_json(const Gpu& gpu) {
    nlohmann::json document{{"kind", to_string(gpu.kind)},
                            {"throughput_tflops", gpu.throughput_tflops}};
    if (!gpu.model.empty()) document["model"] = gpu.model;
    return document;
}

// Parse a gpu sub-object at the boundary. The whole NodeCapability rejects on
// failure, so a malformed accelerator never silently degrades to CPU-only.
std::optional<Gpu> gpu_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("kind") || !value["kind"].is_string()) return std::nullopt;
    GpuKind kind{};
    if (!gpu_kind_from_string(value["kind"].get<std::string>(), kind)) {
        return std::nullopt;
    }
    Gpu gpu;
    gpu.kind = kind;
    if (value.contains("throughput_tflops")) {
        if (!value["throughput_tflops"].is_number()) return std::nullopt;
        gpu.throughput_tflops = value["throughput_tflops"].get<double>();
        if (gpu.throughput_tflops < 0.0) return std::nullopt;
    }
    if (value.contains("model")) {
        if (!value["model"].is_string()) return std::nullopt;
        gpu.model = value["model"].get<std::string>();
    }
    return gpu;
}

}  // namespace

nlohmann::json NodeCapability::to_json() const {
    return nlohmann::json{{"cores", cores},
                          {"memory_bytes", memory_bytes},
                          {"gpu", gpu_to_json(gpu)}};
}

std::optional<NodeCapability> NodeCapability::from_json(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    std::uint64_t cores = 0;
    if (!value.contains("cores") || !as_nonneg_u64(value["cores"], cores) ||
        cores > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    std::uint64_t memory_bytes = 0;
    if (!value.contains("memory_bytes") ||
        !as_nonneg_u64(value["memory_bytes"], memory_bytes)) {
        return std::nullopt;
    }
    NodeCapability capability;
    capability.cores = static_cast<std::uint32_t>(cores);
    capability.memory_bytes = memory_bytes;
    // A gpu object is optional; absent means CPU-only (GpuKind::None). When
    // present it must be valid.
    if (value.contains("gpu")) {
        std::optional<Gpu> gpu = gpu_from_json(value["gpu"]);
        if (!gpu) return std::nullopt;
        capability.gpu = std::move(*gpu);
    }
    return capability;
}

}  // namespace zetta
