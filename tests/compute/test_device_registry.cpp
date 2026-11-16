// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/compute/compute_device.hpp"
#include "zetta/compute/device_registry.hpp"
#include "zetta/compute/host_stub_device.hpp"

namespace {

using zetta::compute::ComputeDevice;
using zetta::compute::ComputeStatus;
using zetta::compute::DataType;
using zetta::compute::DeviceBuffer;
using zetta::compute::DeviceInfo;
using zetta::compute::DeviceKind;
using zetta::compute::DeviceRegistry;
using zetta::compute::DeviceRequest;
using zetta::compute::HostStubDevice;

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

// A test-double device that reports a configurable DeviceInfo and supports no
// transfers or operators. The registry only ever calls info() (it enumerates,
// reports, and selects on capability — it never allocates or runs ops), so a fake
// that answers info() is all selection/enumeration needs. It stands in for a CUDA
// device (and for capacity-tie cases) without a real backend — the same shape as
// the FakeProvisioner the Provisioner slice tests against.
class FakeDevice final : public ComputeDevice {
public:
    explicit FakeDevice(DeviceInfo info) : info_(std::move(info)) {}

    DeviceInfo info() const override { return info_; }

    DeviceBuffer allocate(DataType, std::size_t) override { return {}; }
    ComputeStatus copy_from_host(const void*, DeviceBuffer&,
                                 std::size_t) override {
        return unsupported();
    }
    ComputeStatus copy_to_host(const DeviceBuffer&, void*,
                               std::size_t) const override {
        return unsupported();
    }
    ComputeStatus synchronize() override { return ComputeStatus::success(); }

    ComputeStatus map(zetta::compute::UnaryOp, const DeviceBuffer&,
                      DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus elementwise(zetta::compute::BinaryOp, const DeviceBuffer&,
                              const DeviceBuffer&, DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus reduce(zetta::compute::ReduceOp, const DeviceBuffer&,
                         DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus scan(zetta::compute::ReduceOp, zetta::compute::ScanInclusivity,
                       const DeviceBuffer&, DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus histogram(const DeviceBuffer&, std::size_t, double, double,
                            DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus gemm(const zetta::compute::GemmParams&, const DeviceBuffer&,
                       const DeviceBuffer&, DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus axpy(double, const DeviceBuffer&, DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus sort(DeviceBuffer&, zetta::compute::SortOrder) override {
        return unsupported();
    }
    ComputeStatus sort_by_key(DeviceBuffer&, DeviceBuffer&,
                              zetta::compute::SortOrder) override {
        return unsupported();
    }
    ComputeStatus gather(const DeviceBuffer&, const DeviceBuffer&,
                         DeviceBuffer&) override {
        return unsupported();
    }
    ComputeStatus scatter(const DeviceBuffer&, const DeviceBuffer&,
                          DeviceBuffer&) override {
        return unsupported();
    }

private:
    static ComputeStatus unsupported() {
        return ComputeStatus::unsupported("fake");
    }
    void release_buffer(void*) noexcept override {}

    DeviceInfo info_;
};

// A CUDA-kind fake with the given capability. Selection ranks by kind first, then
// compute units, then memory — so these knobs drive the selection tests.
std::shared_ptr<FakeDevice> cuda_device(const std::string& name,
                                        std::size_t streaming_multiprocessors,
                                        std::uint64_t memory_bytes) {
    DeviceInfo info;
    info.kind = DeviceKind::Cuda;
    info.name = name;
    info.compute_units = streaming_multiprocessors;
    info.memory_bytes = memory_bytes;
    info.unified_memory = false;
    return std::make_shared<FakeDevice>(std::move(info));
}

}  // namespace

// ---- Enumeration -----------------------------------------------------------

TEST(DeviceRegistry, EnumeratesRegisteredDevicesInOrder) {
    DeviceRegistry registry;
    EXPECT_TRUE(registry.empty());
    EXPECT_EQ(registry.size(), 0u);
    EXPECT_EQ(registry.at(0), nullptr);
    EXPECT_EQ(registry.select(), nullptr);  // nothing to select from

    const std::size_t cpu_index =
        registry.add(std::make_shared<HostStubDevice>());
    const std::size_t cuda_index = registry.add(cuda_device("A100", 108, 80 * kGiB));

    EXPECT_EQ(cpu_index, 0u);
    EXPECT_EQ(cuda_index, 1u);
    EXPECT_EQ(registry.size(), 2u);
    EXPECT_FALSE(registry.empty());
    EXPECT_NE(registry.at(0), nullptr);
    EXPECT_NE(registry.at(1), nullptr);
    EXPECT_EQ(registry.at(2), nullptr);  // out of range

    // A null device is ignored — never stored, so lookups stay safe.
    EXPECT_EQ(registry.add(nullptr), 2u);
    EXPECT_EQ(registry.size(), 2u);
}

// ---- The acceptance criterion: CPU on any machine, CUDA where present ------

TEST(DeviceRegistry, ReportsCpuAlwaysAndCudaOnlyWherePresent) {
    // A node with only the (CPU-kind) host device — every machine has this.
    DeviceRegistry cpu_only;
    cpu_only.add(std::make_shared<HostStubDevice>());
    EXPECT_TRUE(cpu_only.has(DeviceKind::Cpu));
    EXPECT_FALSE(cpu_only.has(DeviceKind::Cuda));  // no CUDA backend registered
    ASSERT_NE(cpu_only.find(DeviceKind::Cpu), nullptr);
    EXPECT_EQ(cpu_only.find(DeviceKind::Cpu)->info().kind, DeviceKind::Cpu);
    EXPECT_EQ(cpu_only.find(DeviceKind::Cuda), nullptr);

    // A node where a CUDA backend was registered reports both.
    DeviceRegistry with_gpu;
    with_gpu.add(std::make_shared<HostStubDevice>());
    with_gpu.add(cuda_device("A100", 108, 80 * kGiB));
    EXPECT_TRUE(with_gpu.has(DeviceKind::Cpu));
    EXPECT_TRUE(with_gpu.has(DeviceKind::Cuda));
    ASSERT_NE(with_gpu.find(DeviceKind::Cuda), nullptr);
    EXPECT_EQ(with_gpu.find(DeviceKind::Cuda)->info().name, "A100");

    // The capability report lists each device's coarse capability.
    const std::vector<DeviceInfo> infos = with_gpu.device_infos();
    ASSERT_EQ(infos.size(), 2u);
    EXPECT_EQ(infos[0].kind, DeviceKind::Cpu);
    EXPECT_EQ(infos[1].kind, DeviceKind::Cuda);
}

// ---- The selection unit test (acceptance: "a selection unit test") ---------

TEST(DeviceRegistry, SelectsBackendPerRequest) {
    DeviceRegistry registry;
    const std::shared_ptr<ComputeDevice> cpu =
        std::make_shared<HostStubDevice>();
    const std::shared_ptr<ComputeDevice> cuda = cuda_device("A100", 108, 80 * kGiB);
    registry.add(cpu);
    registry.add(cuda);

    // Default request: prefer an accelerator — routes a heavy op to the GPU.
    EXPECT_EQ(registry.select(), cuda);
    EXPECT_EQ(registry.select(DeviceRequest{}), cuda);

    // Prefer the CPU explicitly — routes back to the host device.
    EXPECT_EQ(registry.select(DeviceRequest{.prefer_accelerator = false}), cpu);

    // A hard kind requirement filters to that kind.
    EXPECT_EQ(registry.select(DeviceRequest{.require_kind = DeviceKind::Cuda}),
              cuda);
    EXPECT_EQ(registry.select(DeviceRequest{.require_kind = DeviceKind::Cpu}), cpu);

    // A device-memory floor excludes the host device (0 = unknown < floor), so the
    // request resolves to the GPU even when the CPU is otherwise preferred.
    EXPECT_EQ(registry.select(DeviceRequest{.prefer_accelerator = false,
                                            .min_memory_bytes = 8 * kGiB}),
              cuda);
}

TEST(DeviceRegistry, SelectionFallsBackAndCanComeUpEmpty) {
    // CPU-only node: even an accelerator-preferring request falls back to the CPU.
    DeviceRegistry cpu_only;
    const std::shared_ptr<ComputeDevice> cpu =
        std::make_shared<HostStubDevice>();
    cpu_only.add(cpu);
    EXPECT_EQ(cpu_only.select(), cpu);  // no GPU → the CPU is the best available

    // A CUDA-only requirement on a CPU-only node finds nothing.
    EXPECT_EQ(cpu_only.select(DeviceRequest{.require_kind = DeviceKind::Cuda}),
              nullptr);

    // A memory floor no device can prove finds nothing (host RAM reports 0).
    EXPECT_EQ(cpu_only.select(DeviceRequest{.min_memory_bytes = 1}), nullptr);
}

TEST(DeviceRegistry, SelectionBreaksTiesByCapacityThenRegistrationOrder) {
    DeviceRegistry registry;
    const std::shared_ptr<ComputeDevice> small = cuda_device("T4", 40, 16 * kGiB);
    const std::shared_ptr<ComputeDevice> big = cuda_device("A100", 108, 80 * kGiB);
    registry.add(small);  // registered first
    registry.add(big);

    // More compute units wins regardless of registration order.
    EXPECT_EQ(registry.select(), big);

    // Two equal devices: the earlier-registered keeps the slot (stable).
    DeviceRegistry equal;
    const std::shared_ptr<ComputeDevice> first = cuda_device("g0", 108, 80 * kGiB);
    const std::shared_ptr<ComputeDevice> second = cuda_device("g1", 108, 80 * kGiB);
    equal.add(first);
    equal.add(second);
    EXPECT_EQ(equal.select(), first);
}

// ---- JSON boundary: the capability report ----------------------------------

TEST(DeviceRegistry, ToJsonReportsEachDeviceCapability) {
    DeviceRegistry registry;
    registry.add(std::make_shared<HostStubDevice>());
    registry.add(cuda_device("A100", 108, 80 * kGiB));

    const nlohmann::json json = registry.to_json();
    ASSERT_TRUE(json.contains("devices"));
    ASSERT_TRUE(json["devices"].is_array());
    ASSERT_EQ(json["devices"].size(), 2u);
    EXPECT_EQ(json["devices"][0]["kind"], "cpu");
    EXPECT_EQ(json["devices"][1]["kind"], "cuda");
    EXPECT_EQ(json["devices"][1]["name"], "A100");
    EXPECT_EQ(json["devices"][1]["compute_units"], 108);
    EXPECT_EQ(json["devices"][1]["memory_bytes"], 80 * kGiB);

    // parse_device_report() is the validating inverse: the report round-trips.
    const auto parsed = zetta::compute::parse_device_report(json);
    ASSERT_TRUE(parsed);
    ASSERT_EQ(parsed->size(), 2u);
    EXPECT_EQ((*parsed)[1].kind, DeviceKind::Cuda);
    EXPECT_EQ((*parsed)[1].name, "A100");
    EXPECT_EQ((*parsed)[1].compute_units, 108u);
    EXPECT_EQ((*parsed)[1].memory_bytes, 80 * kGiB);
}

TEST(DeviceRegistry, DeviceInfoJsonRoundTrips) {
    DeviceInfo info;
    info.kind = DeviceKind::Cuda;
    info.name = "H100";
    info.compute_units = 132;
    info.memory_bytes = 80 * kGiB;
    info.unified_memory = false;

    const nlohmann::json json = zetta::compute::device_info_to_json(info);
    const auto parsed = zetta::compute::device_info_from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->kind, info.kind);
    EXPECT_EQ(parsed->name, info.name);
    EXPECT_EQ(parsed->compute_units, info.compute_units);
    EXPECT_EQ(parsed->memory_bytes, info.memory_bytes);
    EXPECT_EQ(parsed->unified_memory, info.unified_memory);

    // unified_memory is optional, defaulting to false when absent.
    const auto no_unified = zetta::compute::device_info_from_json(nlohmann::json{
        {"kind", "cpu"},
        {"name", "host"},
        {"compute_units", 8},
        {"memory_bytes", 0}});
    ASSERT_TRUE(no_unified);
    EXPECT_FALSE(no_unified->unified_memory);
}

TEST(DeviceRegistry, DeviceKindNamesRoundTrip) {
    EXPECT_EQ(zetta::compute::to_string(DeviceKind::Cpu), "cpu");
    EXPECT_EQ(zetta::compute::to_string(DeviceKind::Cuda), "cuda");

    DeviceKind kind{};
    EXPECT_TRUE(zetta::compute::device_kind_from_string("cuda", kind));
    EXPECT_EQ(kind, DeviceKind::Cuda);
    EXPECT_TRUE(zetta::compute::device_kind_from_string("cpu", kind));
    EXPECT_EQ(kind, DeviceKind::Cpu);
    EXPECT_FALSE(zetta::compute::device_kind_from_string("rocm", kind));  // unknown
}

TEST(DeviceRegistry, FromJsonRejectsMalformedAtTheBoundary) {
    using zetta::compute::device_info_from_json;
    using zetta::compute::parse_device_report;

    // DeviceInfo
    EXPECT_FALSE(device_info_from_json(nlohmann::json::array()));   // not an object
    EXPECT_FALSE(device_info_from_json(nlohmann::json::object()));  // missing fields
    EXPECT_FALSE(device_info_from_json(nlohmann::json{
        {"kind", "tpu"}, {"name", "x"}, {"compute_units", 1}, {"memory_bytes", 1}}));
    EXPECT_FALSE(device_info_from_json(nlohmann::json{
        {"kind", "cpu"}, {"name", 7}, {"compute_units", 1}, {"memory_bytes", 1}}));
    EXPECT_FALSE(device_info_from_json(nlohmann::json{{"kind", "cpu"},
                                                      {"name", "x"},
                                                      {"compute_units", -1},
                                                      {"memory_bytes", 1}}));
    EXPECT_FALSE(device_info_from_json(nlohmann::json{
        {"kind", "cpu"}, {"name", "x"}, {"compute_units", 1}}));  // no memory
    EXPECT_FALSE(device_info_from_json(nlohmann::json{{"kind", "cpu"},
                                                      {"name", "x"},
                                                      {"compute_units", 1},
                                                      {"memory_bytes", 1},
                                                      {"unified_memory", "yes"}}));

    // Report wrapper
    EXPECT_FALSE(parse_device_report(nlohmann::json::array()));            // not obj
    EXPECT_FALSE(parse_device_report(nlohmann::json::object()));           // no list
    EXPECT_FALSE(parse_device_report(nlohmann::json{{"devices", 7}}));     // not arr
    EXPECT_FALSE(parse_device_report(
        nlohmann::json{{"devices", {nlohmann::json{{"kind", "tpu"}}}}}));  // bad item

    // An empty device list is a valid (degenerate) report.
    const auto empty = parse_device_report(nlohmann::json{
        {"devices", nlohmann::json::array()}});
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty->empty());
}
