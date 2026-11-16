// SPDX-License-Identifier: MIT

#include "zetta/cloud/node_capability.hpp"

#include <array>
#include <cstdint>
#include <limits>

namespace zetta::cloud {

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

const nlohmann::json& node_capability_schema() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSONSCHEMA(
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://oramasearch.github.io/zetta/schemas/node_capability.schema.json",
  "title": "NodeCapability",
  "description": "The node capability model (ENT-32, part of the native distributed accelerated compute pass for UC-1 churn — ARC-13). Describes one node's compute resources — CPU cores, memory, and an optional GPU (type + throughput) — used both as the capability a node reports and as the per-node floor a provision request asks for. Mirrored by zetta::NodeCapability in include/zetta/node_capability.hpp and enforced by NodeCapability::from_json() in src/core/node_capability.cpp — keep them in sync. Distinct from the access-control capability in access_key.schema.json (that is a *right*; this is *compute*). A malformed capability is rejected at the boundary so a node never silently over- or under-states what it offers.",
  "type": "object",
  "required": ["cores", "memory_bytes"],
  "additionalProperties": false,
  "properties": {
    "cores": {
      "description": "Logical CPU cores the node provides.",
      "type": "integer",
      "minimum": 0
    },
    "memory_bytes": {
      "description": "Total memory in bytes.",
      "type": "integer",
      "minimum": 0
    },
    "gpu": {
      "description": "The node's accelerator. Optional; absent means CPU-only (equivalent to kind 'none').",
      "$ref": "#/$defs/gpu"
    }
  },
  "$defs": {
    "gpu": {
      "description": "A node's accelerator. kind 'none' (with zero throughput) is a CPU-only node; the named kinds are GPU families. Throughput is a single comparable scalar (teraFLOPS), not a vendor benchmark.",
      "type": "object",
      "required": ["kind"],
      "additionalProperties": false,
      "properties": {
        "kind": {
          "description": "The accelerator family. Append-only enum (the lowercase name is the stored identifier).",
          "type": "string",
          "enum": ["none", "cuda", "rocm", "metal"]
        },
        "throughput_tflops": {
          "description": "Coarse throughput in teraFLOPS the autoscaler ranks placements by. 0 when there is no GPU.",
          "type": "number",
          "minimum": 0
        },
        "model": {
          "description": "Optional free-form GPU model name (e.g. 'A100', 'H100', 'MI300X').",
          "type": "string"
        }
      }
    },
    "provisioned_node": {
      "description": "A node a Provisioner has stood up (or, for the Local backend, an existing cluster node it reports): identity + reachable address + capability. Mirrored by zetta::ProvisionedNode in include/zetta/provisioner.hpp (ProvisionedNode::from_json in src/core/provisioner.cpp).",
      "type": "object",
      "required": ["node_id", "capability"],
      "additionalProperties": false,
      "properties": {
        "node_id": {
          "description": "The node's stable id (its cluster / CRDT actor id).",
          "type": "string"
        },
        "address": {
          "description": "host:port to reach the node. Optional; omitted/empty when the node is not networked.",
          "type": "string"
        },
        "capability": {
          "description": "What the node provides.",
          "$ref": "#"
        }
      }
    }
  }
}
)JSONSCHEMA");
    return schema;
}

}  // namespace zetta::cloud