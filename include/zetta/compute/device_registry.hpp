#pragma once

#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"

// The per-node device registry (ARC-9, #362) — the piece that turns the
// ComputeDevice *interface* (#359) into a usable set of *backends* on a node.
// It has three jobs, exactly the issue's scope:
//
//   1. **Enumerate** the node's available ComputeDevices. A device is *registered*
//      into the registry; the registry does not manufacture backends itself
//      (CpuDevice #360 / CudaDevice #361 are separate slices, and CUDA must never
//      leak into zetta-core). The "CPU always, CUDA if built + present" policy is
//      the job of the zetta-compute assembly point (#363): it builds a CpuDevice
//      and, when CUDA is compiled in and a GPU is detected, a CudaDevice, and
//      `add()`s each here. The registry is the container + selector those backends
//      plug into — the same seam discipline as the Provisioner (#373).
//
//   2. **Report each device's capability** — its DeviceInfo (kind / compute units /
//      memory / unified-memory). `device_infos()` lists them, and `to_json()`
//      serializes them as the coordinator-facing capability report (the JSON
//      boundary the ComputeDevice slice anticipated for #362). This is what feeds
//      the autoscaler / coordinator's capability model.
//
//   3. **Select a backend per unit of work** — `select(DeviceRequest)` ranks the
//      registered devices and returns the best fit: by default the accelerator
//      (GPU) when one is present, falling back to the CPU, with hard filters for a
//      required kind (a CUDA-only op) and a device-memory floor.
//
// No backend lives here and no SDL/CUDA type appears in this header — it is pure
// zetta-core, like the ComputeDevice seam it builds on.
namespace zetta::compute {

// Does a DeviceKind drive an *accelerator* (a GPU), as opposed to the CPU? Used by
// selection to route heavy operators to a GPU when one is present. CPU is the only
// non-accelerator kind; every other kind (CUDA today, ROCm/Metal later) is one.
constexpr bool is_accelerator(DeviceKind kind) noexcept {
    return kind != DeviceKind::Cpu;
}

// Parse a stored/canonical device-kind name ("cpu" / "cuda") back to the enum;
// false (out untouched) on an unknown spelling — the boundary check for the enum,
// the inverse of to_string(DeviceKind). Lives here, not in the pure seam header,
// because it is only needed by the registry's JSON boundary.
bool device_kind_from_string(std::string_view text, DeviceKind& out);

// What a caller wants a device for — the registry ranks its devices against this.
// All-defaults asks for "the best device for a heavy op": prefer an accelerator,
// any kind, no memory floor.
struct DeviceRequest {
    // Prefer an accelerator (GPU) over the CPU when one is available. Heavy
    // operators leave this true so the coordinator routes them to the GPU; set it
    // false to prefer the CPU (e.g. a tiny op not worth a host<->device transfer).
    bool prefer_accelerator = true;

    // Require a specific device kind (e.g. an operator that only has a CUDA
    // kernel). std::nullopt accepts any kind.
    std::optional<DeviceKind> require_kind{};

    // Minimum device memory the work needs, in bytes (0 = no floor). A device that
    // reports 0 memory (host / unknown, like the CPU's host RAM) does NOT satisfy a
    // positive floor — selection only picks a device that can prove the capacity.
    std::size_t min_memory_bytes = 0;
};

// The registry: an ordered set of registered devices on one node, with capability
// reporting and per-op selection.
class DeviceRegistry {
public:
    // ---- Enumeration --------------------------------------------------------

    // Register `device` and return its index (registration order). A null device
    // is ignored (returns the unchanged size) so lookups never dereference null.
    std::size_t add(std::shared_ptr<ComputeDevice> device);

    bool empty() const noexcept { return devices_.empty(); }
    std::size_t size() const noexcept { return devices_.size(); }

    // Every registered device, in registration order.
    const std::vector<std::shared_ptr<ComputeDevice>>& devices() const noexcept {
        return devices_;
    }

    // The device at `index`, or nullptr when out of range (a safe handle, never a
    // raw pointer — the house style for lookups).
    std::shared_ptr<ComputeDevice> at(std::size_t index) const;

    // The first registered device of `kind`, or nullptr when none is present.
    std::shared_ptr<ComputeDevice> find(DeviceKind kind) const;

    // Is any device of `kind` registered? (CPU is reported on every machine; CUDA
    // only where a CUDA backend was registered — the acceptance criterion.)
    bool has(DeviceKind kind) const { return find(kind) != nullptr; }

    // ---- Capability reporting ----------------------------------------------

    // Each registered device's coarse capability, in registration order.
    std::vector<DeviceInfo> device_infos() const;

    // The coordinator-facing capability report:
    //   { "devices": [ <DeviceInfo>, ... ] }
    // Mirrors schemas/device_registry.schema.json. parse_device_report() is the
    // validating inverse on the receiving side.
    nlohmann::json to_json() const;

    // ---- Selection ----------------------------------------------------------

    // The best device for `request`, or nullptr when none qualifies. Filters out
    // devices that fail `require_kind` / `min_memory_bytes`, then ranks the rest:
    // the preferred class first (accelerator when prefer_accelerator, else CPU),
    // breaking ties by more compute units, then more memory, then earliest
    // registration. A per-op fallback when the chosen device returns Unsupported is
    // the coordinator's job (route to the next-best device); the registry supplies
    // the ranking.
    std::shared_ptr<ComputeDevice> select(const DeviceRequest& request = {}) const;

private:
    std::vector<std::shared_ptr<ComputeDevice>> devices_;
};

// ---- DeviceInfo JSON boundary ----------------------------------------------
// The per-device capability as it crosses to the coordinator. Kept here, not in
// compute_device.hpp, so the pure ComputeDevice seam header stays JSON-free.

// The JSON value shape:
//   { "kind": "<cpu|cuda>", "name": "<string>", "compute_units": <u64>,
//     "memory_bytes": <u64>, "unified_memory": <bool> }
nlohmann::json device_info_to_json(const DeviceInfo& info);

// Parse a DeviceInfo from JSON, validating the shape at the boundary so a
// malformed device report fails fast instead of silently mis-stating a node's
// hardware. std::nullopt unless `value` is an object with a known `kind`, a string
// `name`, and non-negative integer `compute_units` / `memory_bytes`
// (`unified_memory` is optional, default false).
std::optional<DeviceInfo> device_info_from_json(const nlohmann::json& value);

// Parse a whole capability report ({ "devices": [ <DeviceInfo>, ... ] }) at the
// boundary; std::nullopt if it is not an object with a `devices` array of valid
// DeviceInfos. The validating inverse of DeviceRegistry::to_json().
std::optional<std::vector<DeviceInfo>> parse_device_report(
    const nlohmann::json& value);

// The DeviceRegistry capability-report JSON Schema (draft 2020-12) — the
// code-owned source of truth; schemas/device_registry.schema.json is generated
// from it and drift-checked by tests/test_schemas.cpp (#646).
const nlohmann::json& device_registry_schema();

}  // namespace zetta::compute
