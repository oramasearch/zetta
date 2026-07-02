#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/cloud/add_nodes.hpp"
#include "zetta/cloud/local_provisioner.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::add_nodes;
using zetta::AddNodesRequest;
using zetta::AddNodesResult;
using zetta::Gpu;
using zetta::GpuKind;
using zetta::LocalProvisioner;
using zetta::NodeCapability;
using zetta::ProvisionedNode;
using zetta::ProvisionRequest;
using zetta::ProvisionResult;
using zetta::Provisioner;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

NodeCapability gpu_spec() {
    NodeCapability capability;
    capability.cores = 8;
    capability.memory_bytes = 64 * kGiB;
    capability.gpu = Gpu{GpuKind::Cuda, 312.0, "A100"};
    return capability;
}

// A fake cloud-style Provisioner modelling a burst pool directly at the seam (the
// add_nodes control action programs against the abstract Provisioner&, so it never
// sees a control plane). bring-up scales the pool to the requested count, bounded
// by `capacity` to exercise best-effort; teardown removes one node and is COUNTED
// so a test can prove add_nodes never tears a permanent member down.
class FakeProvisioner : public Provisioner {
public:
    std::size_t capacity = 1000;        // best-effort ceiling on pool size
    std::vector<ProvisionedNode> pool;  // what is currently up
    int provision_calls = 0;
    int teardown_calls = 0;             // add_nodes must never increment this

    ProvisionResult provision(const ProvisionRequest& request) override {
        ++provision_calls;
        if (request.count == 0) {
            ProvisionResult empty;
            empty.satisfied = true;
            empty.detail = "no nodes requested";
            return empty;
        }
        const std::size_t target = std::min(request.count, capacity);
        pool.clear();
        for (std::size_t index = 0; index < target; ++index) {
            ProvisionedNode node;
            node.node_id = "node-" + std::to_string(index);
            node.address = "10.1.0." + std::to_string(index) + ":7777";
            node.capability = request.spec;
            pool.push_back(node);
        }
        ProvisionResult result;
        result.nodes = pool;
        result.satisfied = pool.size() >= request.count;
        result.detail = result.satisfied ? "added" : "best-effort short";
        return result;
    }

    bool teardown(const std::string& node_id) override {
        ++teardown_calls;
        const auto found = std::find_if(
            pool.begin(), pool.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == pool.end()) return false;
        pool.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> provisioned() const override { return pool; }
};

}  // namespace

// ---- add_nodes control action -----------------------------------------------

TEST(AddNodes, AddsPermanentMembers) {
    FakeProvisioner provider;

    const AddNodesResult result = add_nodes(provider, 3, gpu_spec());

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.members.size(), 3u);
    EXPECT_EQ(provider.provision_calls, 1);
    // Each added member carries the requested capability floor and is reachable.
    EXPECT_EQ(result.members.front().node_id, "node-0");
    EXPECT_EQ(result.members.front().capability, gpu_spec());
    EXPECT_FALSE(result.members.front().address.empty());
}

TEST(AddNodes, IsBestEffortWhenCapacityIsShort) {
    FakeProvisioner provider;
    provider.capacity = 2;  // only two can come up

    const AddNodesResult result = add_nodes(provider, 5, gpu_spec());

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.members.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
}

TEST(AddNodes, ZeroCountIsASatisfiedNoOp) {
    FakeProvisioner provider;

    const AddNodesResult result = add_nodes(provider, 0);

    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.members.empty());
    EXPECT_TRUE(provider.pool.empty());
}

// The defining property of the permanent-add control action: it provisions and
// reports, but NEVER tears a member down or revokes it — the members it adds stay.
TEST(AddNodes, NeverTearsMembersDown) {
    FakeProvisioner provider;

    const AddNodesResult result = add_nodes(provider, 3, gpu_spec());
    ASSERT_EQ(result.members.size(), 3u);

    // The members persist — they are still up after the add returns, and add_nodes
    // called teardown zero times.
    EXPECT_EQ(provider.provisioned().size(), 3u);
    EXPECT_EQ(provider.teardown_calls, 0);

    // A second add (idempotent re-scale at the seam) still never tears anything
    // down.
    add_nodes(provider, 4, gpu_spec());
    EXPECT_EQ(provider.teardown_calls, 0);
}

TEST(AddNodes, RequestOverloadEqualsConvenienceOverload) {
    FakeProvisioner via_request;
    FakeProvisioner via_convenience;

    const AddNodesResult from_request =
        add_nodes(via_request, AddNodesRequest{2, gpu_spec()});
    const AddNodesResult from_convenience = add_nodes(via_convenience, 2, gpu_spec());

    EXPECT_EQ(from_request, from_convenience);
}

// Vendor-neutral: the same control action drives a concrete, non-cloud backend
// (the given-hardware LocalProvisioner) unchanged, because it programs against the
// abstract Provisioner& seam.
TEST(AddNodes, WorksOverAnyProvisionerBackend) {
    std::vector<ProvisionedNode> fleet;
    for (int index = 0; index < 3; ++index) {
        ProvisionedNode node;
        node.node_id = "host-" + std::to_string(index);
        node.address = "192.168.0." + std::to_string(index) + ":7777";
        node.capability = gpu_spec();
        fleet.push_back(node);
    }
    LocalProvisioner provider{[&] { return fleet; }};

    // Ask for two permanent nodes from the existing fleet — best-effort over given
    // hardware, but it is the same add_nodes entry point and result shape.
    const AddNodesResult result = add_nodes(provider, 2, gpu_spec());
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.members.size(), 2u);
}

// ---- AddNodesRequest JSON boundary ------------------------------------------

TEST(AddNodesRequest, RoundTripsMinimal) {
    const AddNodesRequest request{4, NodeCapability{}};
    const auto parsed = AddNodesRequest::from_json(request.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, request);
}

TEST(AddNodesRequest, RoundTripsWithCapabilityFloor) {
    const AddNodesRequest request{10, gpu_spec()};
    const auto parsed = AddNodesRequest::from_json(request.to_json());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, request);
}

TEST(AddNodesRequest, SpecIsOptionalAndDefaultsToNoFloor) {
    // A command with only a count parses, with the no-floor capability default.
    const nlohmann::json document{{"count", 5}};
    const auto parsed = AddNodesRequest::from_json(document);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->count, 5u);
    EXPECT_EQ(parsed->spec, NodeCapability{});
}

TEST(AddNodesRequest, RejectsMalformedDocuments) {
    EXPECT_FALSE(AddNodesRequest::from_json(nlohmann::json::array()).has_value());
    EXPECT_FALSE(AddNodesRequest::from_json("not-an-object").has_value());

    // Missing the required count.
    EXPECT_FALSE(AddNodesRequest::from_json(nlohmann::json::object()).has_value());

    // A negative or wrong-typed count.
    EXPECT_FALSE(AddNodesRequest::from_json({{"count", -1}}).has_value());
    EXPECT_FALSE(AddNodesRequest::from_json({{"count", "ten"}}).has_value());

    // An invalid embedded spec (gpu missing its required kind).
    nlohmann::json bad_spec{{"count", 2}};
    bad_spec["spec"] = {{"cores", 8}, {"memory_bytes", 1024}, {"gpu", {{"x", 1}}}};
    EXPECT_FALSE(AddNodesRequest::from_json(bad_spec).has_value());
}
