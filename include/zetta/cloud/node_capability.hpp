#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

namespace zetta::cloud {

// The node capability model (ENT-32, part of the native distributed accelerated
// compute pass for UC-1 churn — ARC-13). It describes a single node's compute
// resources along three dimensions: CPU cores, memory, and an optional GPU
// (type + throughput). The same shape serves two roles: the *actual* capability
// a node reports, and a *requested* per-node floor a provision spec asks for —
// `satisfies()` is the meet-or-exceed comparison that links the two.
//
// NOTE: this is the *compute* capability of a machine — entirely distinct from
// zetta::governance::Capability (include/zetta/governance/capability.hpp), which is an access-control
// right. The names are kept apart deliberately (NodeCapability vs Capability).

// The accelerator family on a node. `None` is a CPU-only node; the named kinds
// are the GPU families we describe. Declaration order is stable because the
// lowercase name is BOTH the display label AND the stored JSON representation,
// so it is a durable on-disk identifier — append new kinds, never reorder.
enum class GpuKind {
    None,   // CPU-only node (no accelerator)
    Cuda,   // NVIDIA / CUDA
    Rocm,   // AMD / ROCm
    Metal,  // Apple Metal (macOS-first; relevant on developer machines)
};

// Canonical lowercase name of a GPU kind ("none" / "cuda" / "rocm" / "metal").
// Both the display label and the stored representation.
std::string_view to_string(GpuKind kind);

// Parse a stored/canonical GPU-kind name back to the enum; false (out untouched)
// on an unknown spelling — the boundary check for the enum.
bool gpu_kind_from_string(std::string_view text, GpuKind& out);

// A node's accelerator, when it has one: its kind, a coarse throughput figure
// the autoscaler ranks placements by, and an optional free-form model name. A
// CPU-only node has GpuKind::None and zero throughput. Throughput is measured in
// teraFLOPS (TFLOPS) — a single comparable scalar, not a vendor benchmark.
struct Gpu {
    GpuKind kind = GpuKind::None;
    double throughput_tflops = 0.0;  // 0 when there is no GPU
    std::string model;               // optional, e.g. "A100", "H100", "MI300X"

    // Does this describe a real accelerator (vs. a CPU-only node)?
    bool present() const { return kind != GpuKind::None; }

    bool operator==(const Gpu&) const = default;
};

// What one node provides, or what a provision request asks for per node — the
// same shape both ways (see Provisioner / ProvisionRequest in provisioner.hpp).
struct NodeCapability {
    std::uint32_t cores = 0;          // logical CPU cores
    std::uint64_t memory_bytes = 0;   // total memory
    Gpu gpu;                          // GpuKind::None when CPU-only

    // Does this node need a GPU at all (true iff its gpu is present)?
    bool has_gpu() const { return gpu.present(); }

    // Does this capability meet-or-exceed `request`? True iff cores >=, memory
    // >=, and — when the request needs a GPU — this has a GPU of the **same**
    // kind with at least the requested throughput. A request with no GPU floor
    // (GpuKind::None) ignores the GPU dimension, so a GPU node still satisfies it.
    bool satisfies(const NodeCapability& request) const;

    bool operator==(const NodeCapability&) const = default;

    // The JSON value shape:
    //   { "cores": <u32>, "memory_bytes": <u64>,
    //     "gpu": { "kind": "<gpu-kind>", "throughput_tflops": <number>,
    //              "model": "<string>"? } }
    // The gpu object is always present; `model` is omitted when empty. Mirrors
    // schemas/node_capability.schema.json — keep them in sync.
    nlohmann::json to_json() const;

    // Parse from JSON, **validating the shape at the boundary** so a malformed
    // capability fails fast instead of silently widening/narrowing what a node
    // claims to offer. Returns std::nullopt on anything malformed: not an object,
    // a missing/wrong-typed field, a negative number, or an unknown GPU kind.
    static std::optional<NodeCapability> from_json(const nlohmann::json& value);
};

}  // namespace zetta::cloud