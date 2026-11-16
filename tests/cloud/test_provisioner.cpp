// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::cloud::Gpu;
using zetta::cloud::GpuKind;
using zetta::cloud::NodeCapability;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;
using zetta::cloud::Provisioner;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

// A capacity-bounded, in-memory Provisioner — the test double the acceptance
// criterion calls for. It fabricates nodes that meet the requested spec (so each
// satisfies the floor), is bounded by `capacity` (to exercise best-effort), and
// tears down by id idempotently.
class FakeProvisioner : public Provisioner {
public:
    explicit FakeProvisioner(std::size_t capacity) : capacity_(capacity) {}

    ProvisionResult provision(const ProvisionRequest& request) override {
        ProvisionResult result;
        for (std::size_t index = 0; index < request.count; ++index) {
            if (live_.size() >= capacity_) {
                result.satisfied = false;
                result.detail = "capacity exhausted";
                return result;  // best-effort: return what we managed to bring up
            }
            ProvisionedNode node;
            node.node_id = "fake-" + std::to_string(next_id_++);
            node.address = "10.0.0." + std::to_string(live_.size() + 1) + ":7777";
            node.capability = request.spec;  // each fabricated node meets the floor
            live_.push_back(node);
            result.nodes.push_back(node);
        }
        result.satisfied = result.nodes.size() == request.count;
        result.detail = result.satisfied ? "ok" : "capacity exhausted";
        return result;
    }

    bool teardown(const std::string& node_id) override {
        for (auto it = live_.begin(); it != live_.end(); ++it) {
            if (it->node_id == node_id) {
                live_.erase(it);
                return true;
            }
        }
        return false;  // unknown / already-gone id is a no-op
    }

    std::vector<ProvisionedNode> provisioned() const override { return live_; }

private:
    std::size_t capacity_;
    std::size_t next_id_ = 0;
    std::vector<ProvisionedNode> live_;
};

NodeCapability cpu_node(std::uint32_t cores, std::uint64_t memory_bytes) {
    return NodeCapability{cores, memory_bytes, Gpu{}};
}

NodeCapability gpu_node(std::uint32_t cores, std::uint64_t memory_bytes,
                        GpuKind kind, double tflops) {
    NodeCapability capability = cpu_node(cores, memory_bytes);
    capability.gpu = Gpu{kind, tflops, ""};
    return capability;
}

}  // namespace

// ---- GpuKind names ---------------------------------------------------------

TEST(NodeCapability, GpuKindNamesRoundTrip) {
    EXPECT_EQ(zetta::cloud::to_string(GpuKind::None), "none");
    EXPECT_EQ(zetta::cloud::to_string(GpuKind::Cuda), "cuda");
    EXPECT_EQ(zetta::cloud::to_string(GpuKind::Metal), "metal");

    GpuKind kind{};
    EXPECT_TRUE(zetta::cloud::gpu_kind_from_string("rocm", kind));
    EXPECT_EQ(kind, GpuKind::Rocm);
    EXPECT_FALSE(zetta::cloud::gpu_kind_from_string("tpu", kind));  // unknown
}

// ---- the meet-or-exceed comparison -----------------------------------------

TEST(NodeCapability, SatisfiesComparesEveryDimension) {
    const NodeCapability big = gpu_node(32, 128 * kGiB, GpuKind::Cuda, 312.0);
    const NodeCapability small = cpu_node(8, 32 * kGiB);

    EXPECT_TRUE(big.satisfies(small));   // exceeds on every axis
    EXPECT_FALSE(small.satisfies(big));  // fewer cores, no GPU

    // cores / memory floors.
    EXPECT_FALSE(cpu_node(4, 64 * kGiB).satisfies(cpu_node(8, 64 * kGiB)));
    EXPECT_FALSE(cpu_node(8, 16 * kGiB).satisfies(cpu_node(8, 64 * kGiB)));
    EXPECT_TRUE(cpu_node(8, 64 * kGiB).satisfies(cpu_node(8, 64 * kGiB)));  // equal

    // A request with no GPU floor ignores the GPU dimension — both satisfy it.
    const NodeCapability cpu_request = cpu_node(8, 32 * kGiB);
    EXPECT_TRUE(big.satisfies(cpu_request));
    EXPECT_TRUE(cpu_node(8, 32 * kGiB).satisfies(cpu_request));
}

TEST(NodeCapability, SatisfiesMatchesGpuKindAndThroughput) {
    const NodeCapability cuda_request = gpu_node(8, 32 * kGiB, GpuKind::Cuda, 100.0);

    // A CPU-only node cannot satisfy a GPU request.
    EXPECT_FALSE(cpu_node(64, 256 * kGiB).satisfies(cuda_request));

    // A different GPU family does not satisfy it (kind must match).
    EXPECT_FALSE(gpu_node(8, 32 * kGiB, GpuKind::Metal, 200.0).satisfies(cuda_request));

    // Same kind but too slow fails; same kind and fast enough passes.
    EXPECT_FALSE(gpu_node(8, 32 * kGiB, GpuKind::Cuda, 50.0).satisfies(cuda_request));
    EXPECT_TRUE(gpu_node(8, 32 * kGiB, GpuKind::Cuda, 100.0).satisfies(cuda_request));
}

// ---- JSON boundary: NodeCapability -----------------------------------------

TEST(NodeCapability, JsonRoundTrip) {
    NodeCapability labelled = gpu_node(16, 64 * kGiB, GpuKind::Cuda, 156.0);
    labelled.gpu.model = "A100";

    const nlohmann::json json = labelled.to_json();
    EXPECT_EQ(json["cores"], 16);
    EXPECT_EQ(json["gpu"]["kind"], "cuda");
    EXPECT_EQ(json["gpu"]["model"], "A100");

    const auto parsed = NodeCapability::from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, labelled);

    // CPU-only round-trips with gpu kind "none" and no model field.
    const NodeCapability cpu = cpu_node(8, 32 * kGiB);
    const nlohmann::json cpu_json = cpu.to_json();
    EXPECT_EQ(cpu_json["gpu"]["kind"], "none");
    EXPECT_FALSE(cpu_json["gpu"].contains("model"));
    const auto cpu_parsed = NodeCapability::from_json(cpu_json);
    ASSERT_TRUE(cpu_parsed);
    EXPECT_EQ(*cpu_parsed, cpu);

    // A document with no gpu object at all defaults to CPU-only.
    const auto no_gpu = NodeCapability::from_json(
        nlohmann::json{{"cores", 4}, {"memory_bytes", 8 * kGiB}});
    ASSERT_TRUE(no_gpu);
    EXPECT_EQ(no_gpu->gpu.kind, GpuKind::None);
}

TEST(NodeCapability, FromJsonRejectsMalformedAtTheBoundary) {
    EXPECT_FALSE(NodeCapability::from_json(nlohmann::json::array()));      // not obj
    EXPECT_FALSE(NodeCapability::from_json(nlohmann::json::object()));     // no cores
    EXPECT_FALSE(NodeCapability::from_json(
        nlohmann::json{{"cores", "lots"}, {"memory_bytes", 1}}));         // wrong type
    EXPECT_FALSE(NodeCapability::from_json(
        nlohmann::json{{"cores", -1}, {"memory_bytes", 1}}));             // negative
    EXPECT_FALSE(NodeCapability::from_json(nlohmann::json{{"cores", 4}}));  // no memory
    EXPECT_FALSE(NodeCapability::from_json(nlohmann::json{
        {"cores", 4}, {"memory_bytes", 1}, {"gpu", {{"kind", "tpu"}}}}));  // bad kind
    EXPECT_FALSE(NodeCapability::from_json(nlohmann::json{
        {"cores", 4},
        {"memory_bytes", 1},
        {"gpu", {{"kind", "cuda"}, {"throughput_tflops", -5.0}}}}));       // neg tflops
}

// ---- JSON boundary: ProvisionedNode ----------------------------------------

TEST(ProvisionedNode, JsonRoundTrip) {
    ProvisionedNode node;
    node.node_id = "node-7";
    node.address = "10.0.0.7:7777";
    node.capability = gpu_node(16, 64 * kGiB, GpuKind::Cuda, 156.0);

    const nlohmann::json json = node.to_json();
    const auto parsed = ProvisionedNode::from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, node);

    // address is omitted when empty and round-trips to empty.
    ProvisionedNode headless = node;
    headless.address.clear();
    const nlohmann::json headless_json = headless.to_json();
    EXPECT_FALSE(headless_json.contains("address"));
    const auto headless_parsed = ProvisionedNode::from_json(headless_json);
    ASSERT_TRUE(headless_parsed);
    EXPECT_TRUE(headless_parsed->address.empty());
}

TEST(ProvisionedNode, FromJsonRejectsMalformedAtTheBoundary) {
    EXPECT_FALSE(ProvisionedNode::from_json(nlohmann::json::array()));     // not obj
    EXPECT_FALSE(ProvisionedNode::from_json(
        nlohmann::json{{"capability", {{"cores", 1}, {"memory_bytes", 1}}}}));  // no id
    EXPECT_FALSE(ProvisionedNode::from_json(nlohmann::json{{"node_id", "x"}}));  // no cap
    EXPECT_FALSE(ProvisionedNode::from_json(
        nlohmann::json{{"node_id", "x"}, {"capability", {{"cores", "bad"}}}}));  // bad cap
}

// ---- the seam: provision / list / teardown round-trip ----------------------

TEST(Provisioner, FakeProvisionerRoundTripsProvisionAndTeardown) {
    FakeProvisioner provisioner{/*capacity=*/8};
    const NodeCapability spec = gpu_node(8, 32 * kGiB, GpuKind::Cuda, 50.0);

    // Nothing provisioned to start.
    EXPECT_TRUE(provisioner.provisioned().empty());

    // Provision 3 nodes meeting the spec.
    const ProvisionResult result = provisioner.provision(ProvisionRequest{3, spec});
    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 3u);
    for (const ProvisionedNode& node : result.nodes) {
        EXPECT_FALSE(node.node_id.empty());
        EXPECT_TRUE(node.capability.satisfies(spec));  // each meets the floor
    }
    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // Teardown one — list shrinks; tearing the same id down again is a no-op.
    const std::string victim = result.nodes.front().node_id;
    EXPECT_TRUE(provisioner.teardown(victim));
    EXPECT_EQ(provisioner.provisioned().size(), 2u);
    EXPECT_FALSE(provisioner.teardown(victim));        // idempotent
    EXPECT_FALSE(provisioner.teardown("never-existed"));
    EXPECT_EQ(provisioner.provisioned().size(), 2u);

    // Tear down the rest — back to empty.
    for (const ProvisionedNode& node : provisioner.provisioned()) {
        EXPECT_TRUE(provisioner.teardown(node.node_id));
    }
    EXPECT_TRUE(provisioner.provisioned().empty());
}

TEST(Provisioner, ProvisionIsBestEffortWhenCapacityIsShort) {
    FakeProvisioner provisioner{/*capacity=*/2};
    const NodeCapability spec = cpu_node(4, 16 * kGiB);

    const ProvisionResult result = provisioner.provision(ProvisionRequest{5, spec});
    EXPECT_FALSE(result.satisfied);             // asked 5, capacity caps it
    EXPECT_EQ(result.nodes.size(), 2u);         // got what it could
    EXPECT_FALSE(result.detail.empty());
    EXPECT_EQ(provisioner.provisioned().size(), 2u);

    // A zero-count request is a satisfied no-op.
    const ProvisionResult none = provisioner.provision(ProvisionRequest{0, spec});
    EXPECT_TRUE(none.satisfied);
    EXPECT_TRUE(none.nodes.empty());
}
