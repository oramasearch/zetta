// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "zetta/cloud/local_provisioner.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::cloud::Gpu;
using zetta::cloud::GpuKind;
using zetta::cloud::LocalProvisioner;
using zetta::cloud::NodeCapability;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

NodeCapability cpu_node(std::uint32_t cores, std::uint64_t memory_bytes) {
    return NodeCapability{cores, memory_bytes, Gpu{}};
}

NodeCapability gpu_node(std::uint32_t cores, std::uint64_t memory_bytes,
                        GpuKind kind, double tflops) {
    NodeCapability capability = cpu_node(cores, memory_bytes);
    capability.gpu = Gpu{kind, tflops, ""};
    return capability;
}

ProvisionedNode make_node(std::string id, std::string address,
                          NodeCapability capability) {
    ProvisionedNode node;
    node.node_id = std::move(id);
    node.address = std::move(address);
    node.capability = std::move(capability);
    return node;
}

// A small fixed cluster fleet: two beefy GPU boxes and one modest CPU box.
std::vector<ProvisionedNode> sample_fleet() {
    return {
        make_node("node-a", "10.0.0.1:7777", gpu_node(32, 128 * kGiB, GpuKind::Cuda, 312.0)),
        make_node("node-b", "10.0.0.2:7777", gpu_node(32, 128 * kGiB, GpuKind::Cuda, 312.0)),
        make_node("node-c", "10.0.0.3:7777", cpu_node(8, 32 * kGiB)),
    };
}

}  // namespace

// ---- the acceptance criterion ----------------------------------------------

// provision() returns the current cluster nodes (a no-floor spec matches all).
TEST(LocalProvisioner, ProvisionReturnsCurrentClusterNodes) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{fleet.size(), NodeCapability{}});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), fleet.size());
    EXPECT_EQ(result.nodes, fleet);  // same nodes, in fleet order
    EXPECT_FALSE(result.detail.empty());
}

// teardown() is a no-op for given hardware: always false, fleet unchanged.
TEST(LocalProvisioner, TeardownIsANoOp) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    EXPECT_FALSE(provisioner.teardown("node-a"));        // a node it "has up"
    EXPECT_FALSE(provisioner.teardown("never-existed"));  // an unknown id
    EXPECT_EQ(provisioner.provisioned().size(), fleet.size());  // nothing released
}

// provisioned() is the live fleet — the whole cluster it has up.
TEST(LocalProvisioner, ProvisionedListsTheWholeCluster) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    EXPECT_EQ(provisioner.provisioned(), fleet);
}

// ---- best-effort selection over the spec floor -----------------------------

TEST(LocalProvisioner, ProvisionSelectsNodesMeetingTheSpec) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    // Ask for two CUDA boxes: only node-a and node-b qualify; node-c is CPU-only.
    const NodeCapability cuda_spec = gpu_node(16, 64 * kGiB, GpuKind::Cuda, 100.0);
    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, cuda_spec});

    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 2u);
    for (const ProvisionedNode& node : result.nodes) {
        EXPECT_TRUE(node.capability.satisfies(cuda_spec));
    }
    EXPECT_EQ(result.nodes[0].node_id, "node-a");
    EXPECT_EQ(result.nodes[1].node_id, "node-b");
}

TEST(LocalProvisioner, ProvisionIsBestEffortWhenTheClusterIsTooSmall) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    // Ask for three CUDA boxes — only two exist (node-c is CPU-only). Given
    // hardware can't add capacity, so it returns what it has, unsatisfied.
    const NodeCapability cuda_spec = gpu_node(16, 64 * kGiB, GpuKind::Cuda, 100.0);
    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, cuda_spec});

    EXPECT_FALSE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 2u);
    EXPECT_FALSE(result.detail.empty());
}

TEST(LocalProvisioner, ProvisionRespectsTheRequestedCount) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    // The cluster has three nodes; asking for one returns exactly one.
    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{1, NodeCapability{}});
    EXPECT_TRUE(result.satisfied);
    ASSERT_EQ(result.nodes.size(), 1u);
    EXPECT_EQ(result.nodes.front().node_id, "node-a");
}

TEST(LocalProvisioner, ZeroCountRequestIsASatisfiedNoOp) {
    const std::vector<ProvisionedNode> fleet = sample_fleet();
    LocalProvisioner provisioner{[&] { return fleet; }};

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{0, NodeCapability{}});
    EXPECT_TRUE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
}

// ---- live membership: the source is re-read on every call ------------------

TEST(LocalProvisioner, ReflectsLiveMembershipChanges) {
    std::vector<ProvisionedNode> fleet = sample_fleet();
    // The source closes over a mutable fleet, modelling self-healing membership.
    LocalProvisioner provisioner{[&] { return fleet; }};

    EXPECT_EQ(provisioner.provisioned().size(), 3u);

    // A node leaves the cluster — it stops appearing without any teardown call.
    fleet.pop_back();
    EXPECT_EQ(provisioner.provisioned().size(), 2u);

    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{3, NodeCapability{}});
    EXPECT_FALSE(result.satisfied);     // only two nodes left in the cluster
    EXPECT_EQ(result.nodes.size(), 2u);
}

// An empty / unset fleet source behaves as an empty cluster.
TEST(LocalProvisioner, EmptySourceIsAnEmptyCluster) {
    LocalProvisioner provisioner{LocalProvisioner::FleetSource{}};

    EXPECT_TRUE(provisioner.provisioned().empty());
    const ProvisionResult result =
        provisioner.provision(ProvisionRequest{2, NodeCapability{}});
    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
}
