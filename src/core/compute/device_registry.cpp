#include "zetta/compute/device_registry.hpp"

#include <array>
#include <cstdint>
#include <utility>

// The per-node device registry. Enumeration + capability reporting + per-op
// selection over a set of registered ComputeDevices (ARC-9, #362). Boundary
// validation of the DeviceInfo JSON form is hand-written, mirroring
// schemas/device_registry.schema.json — the same pattern as NodeCapability
// (src/core/node_capability.cpp), its sibling in the accel-compute family.
namespace zetta::compute {

namespace {

// Accept a JSON integer that is non-negative, whether nlohmann stored it signed
// (a plain `4` literal) or unsigned (what to_json writes). False if it is not an
// integer or is negative — the boundary check for a count/size field. (Same shape
// as node_capability.cpp's helper; kept local so each boundary owns its checks.)
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

// Whether `candidate` should outrank `incumbent` for `request`: the preferred
// class wins first (accelerator when prefer_accelerator, else CPU), then more
// compute units, then more memory. Equal on every axis returns false, so the
// earlier-registered device keeps the slot (stable selection).
bool ranks_higher(const DeviceInfo& candidate, const DeviceInfo& incumbent,
                  const DeviceRequest& request) {
    const auto preferred = [&](const DeviceInfo& info) {
        const bool accelerator = is_accelerator(info.kind);
        return request.prefer_accelerator ? accelerator : !accelerator;
    };
    if (preferred(candidate) != preferred(incumbent)) return preferred(candidate);
    if (candidate.compute_units != incumbent.compute_units) {
        return candidate.compute_units > incumbent.compute_units;
    }
    return candidate.memory_bytes > incumbent.memory_bytes;
}

}  // namespace

bool device_kind_from_string(std::string_view text, DeviceKind& out) {
    constexpr std::array<DeviceKind, 2> kinds{DeviceKind::Cpu, DeviceKind::Cuda};
    for (const DeviceKind kind : kinds) {
        if (to_string(kind) == text) {
            out = kind;
            return true;
        }
    }
    return false;
}

std::size_t DeviceRegistry::add(std::shared_ptr<ComputeDevice> device) {
    if (device == nullptr) return devices_.size();  // ignore null; never stored
    const std::size_t index = devices_.size();
    devices_.push_back(std::move(device));
    return index;
}

std::shared_ptr<ComputeDevice> DeviceRegistry::at(std::size_t index) const {
    if (index >= devices_.size()) return nullptr;
    return devices_[index];
}

std::shared_ptr<ComputeDevice> DeviceRegistry::find(DeviceKind kind) const {
    for (const std::shared_ptr<ComputeDevice>& device : devices_) {
        if (device->info().kind == kind) return device;
    }
    return nullptr;
}

std::vector<DeviceInfo> DeviceRegistry::device_infos() const {
    std::vector<DeviceInfo> infos;
    infos.reserve(devices_.size());
    for (const std::shared_ptr<ComputeDevice>& device : devices_) {
        infos.push_back(device->info());
    }
    return infos;
}

std::shared_ptr<ComputeDevice> DeviceRegistry::select(
    const DeviceRequest& request) const {
    std::shared_ptr<ComputeDevice> best;
    DeviceInfo best_info;
    for (const std::shared_ptr<ComputeDevice>& device : devices_) {
        const DeviceInfo info = device->info();
        if (request.require_kind && info.kind != *request.require_kind) continue;
        if (request.min_memory_bytes != 0 &&
            info.memory_bytes < request.min_memory_bytes) {
            continue;
        }
        if (!best || ranks_higher(info, best_info, request)) {
            best = device;
            best_info = info;
        }
    }
    return best;
}

nlohmann::json DeviceRegistry::to_json() const {
    nlohmann::json devices = nlohmann::json::array();
    for (const std::shared_ptr<ComputeDevice>& device : devices_) {
        devices.push_back(device_info_to_json(device->info()));
    }
    return nlohmann::json{{"devices", std::move(devices)}};
}

// ---- DeviceInfo JSON boundary ----------------------------------------------

nlohmann::json device_info_to_json(const DeviceInfo& info) {
    return nlohmann::json{
        {"kind", to_string(info.kind)},
        {"name", info.name},
        {"compute_units", info.compute_units},
        {"memory_bytes", info.memory_bytes},
        {"unified_memory", info.unified_memory},
    };
}

std::optional<DeviceInfo> device_info_from_json(const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;

    if (!value.contains("kind") || !value["kind"].is_string()) return std::nullopt;
    DeviceKind kind{};
    if (!device_kind_from_string(value["kind"].get<std::string>(), kind)) {
        return std::nullopt;
    }

    if (!value.contains("name") || !value["name"].is_string()) return std::nullopt;

    std::uint64_t compute_units = 0;
    if (!value.contains("compute_units") ||
        !as_nonneg_u64(value["compute_units"], compute_units)) {
        return std::nullopt;
    }

    std::uint64_t memory_bytes = 0;
    if (!value.contains("memory_bytes") ||
        !as_nonneg_u64(value["memory_bytes"], memory_bytes)) {
        return std::nullopt;
    }

    bool unified_memory = false;
    if (value.contains("unified_memory")) {
        if (!value["unified_memory"].is_boolean()) return std::nullopt;
        unified_memory = value["unified_memory"].get<bool>();
    }

    DeviceInfo info;
    info.kind = kind;
    info.name = value["name"].get<std::string>();
    info.compute_units = static_cast<std::size_t>(compute_units);
    info.memory_bytes = static_cast<std::size_t>(memory_bytes);
    info.unified_memory = unified_memory;
    return info;
}

std::optional<std::vector<DeviceInfo>> parse_device_report(
    const nlohmann::json& value) {
    if (!value.is_object()) return std::nullopt;
    if (!value.contains("devices") || !value["devices"].is_array()) {
        return std::nullopt;
    }
    std::vector<DeviceInfo> infos;
    for (const nlohmann::json& element : value["devices"]) {
        std::optional<DeviceInfo> info = device_info_from_json(element);
        if (!info) return std::nullopt;
        infos.push_back(std::move(*info));
    }
    return infos;
}

}  // namespace zetta::compute
