#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/governance/audit_event.hpp"
#include "zetta/cloud/burst_approval_gate.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"

using zetta::AuditAction;
using zetta::AuditEventValue;
using zetta::AuditSink;
using zetta::BurstApprovalGate;
using zetta::BurstApprover;
using zetta::BurstCap;
using zetta::BurstEstimate;
using zetta::default_burst_cost;
using zetta::Gpu;
using zetta::GpuKind;
using zetta::NodeCapability;
using zetta::ProvisionedNode;
using zetta::ProvisionRequest;
using zetta::ProvisionResult;
using zetta::Provisioner;

namespace {

constexpr std::uint64_t kGiB = std::uint64_t{1024} * 1024 * 1024;

// A Provisioner test double that records every call. It always brings up the
// requested count (capacity is unbounded here — capacity/best-effort is covered
// in test_provisioner.cpp; here we only care whether the gate let the call
// through to the inner backend at all).
class RecordingProvisioner : public Provisioner {
public:
    ProvisionResult provision(const ProvisionRequest& request) override {
        ++provision_calls;
        last_request = request;
        ProvisionResult result;
        for (std::size_t index = 0; index < request.count; ++index) {
            ProvisionedNode node;
            node.node_id = "node-" + std::to_string(next_id_++);
            node.capability = request.spec;
            live_.push_back(node);
            result.nodes.push_back(node);
        }
        result.satisfied = true;
        result.detail = "ok";
        return result;
    }

    bool teardown(const std::string& node_id) override {
        ++teardown_calls;
        for (auto it = live_.begin(); it != live_.end(); ++it) {
            if (it->node_id == node_id) {
                live_.erase(it);
                return true;
            }
        }
        return false;
    }

    std::vector<ProvisionedNode> provisioned() const override { return live_; }

    int provision_calls = 0;
    int teardown_calls = 0;
    ProvisionRequest last_request;

private:
    std::size_t next_id_ = 0;
    std::vector<ProvisionedNode> live_;
};

// An AuditSink that simply collects the events recorded against it.
class RecordingAuditSink : public AuditSink {
public:
    void record_audit_event(AuditEventValue value) override {
        events.push_back(std::move(value));
    }
    std::vector<AuditEventValue> events;
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

// Approver policies for the tests.
BurstApprover approve_all() {
    return [](const ProvisionRequest&, const BurstEstimate&) { return true; };
}

}  // namespace

// ---- acceptance: below the cap proceeds; above the cap is blocked + audited ---

TEST(BurstApprovalGate, BelowNodeCapProceedsUnaudited) {
    RecordingProvisioner inner;
    RecordingAuditSink audit;
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/4, /*max_cost=*/0.0},
                           &audit};

    const NodeCapability spec = cpu_node(8, 32 * kGiB);
    const ProvisionResult result = gate.provision(ProvisionRequest{3, spec});

    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 3u);
    EXPECT_EQ(inner.provision_calls, 1);    // passed straight through
    EXPECT_TRUE(audit.events.empty());      // the gate governs only the exception
}

TEST(BurstApprovalGate, AtTheNodeCapProceeds) {
    RecordingProvisioner inner;
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/4, /*max_cost=*/0.0}};

    const ProvisionResult result =
        gate.provision(ProvisionRequest{4, cpu_node(4, 8 * kGiB)});

    EXPECT_TRUE(result.satisfied);          // == cap is within cap (not over)
    EXPECT_EQ(result.nodes.size(), 4u);
    EXPECT_EQ(inner.provision_calls, 1);
}

TEST(BurstApprovalGate, AboveNodeCapIsBlockedPendingApprovalAndAudited) {
    RecordingProvisioner inner;
    RecordingAuditSink audit;
    // No approver wired → deny-by-default.
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/4, /*max_cost=*/0.0},
                           &audit};

    const ProvisionResult result =
        gate.provision(ProvisionRequest{10, cpu_node(8, 32 * kGiB)});

    // Blocked: nothing came up and the inner backend was never touched.
    EXPECT_FALSE(result.satisfied);
    EXPECT_TRUE(result.nodes.empty());
    EXPECT_EQ(inner.provision_calls, 0);
    EXPECT_FALSE(result.detail.empty());

    // Audited as a blocked provision burst.
    ASSERT_EQ(audit.events.size(), 1u);
    EXPECT_EQ(audit.events[0].action, AuditAction::ProvisionRequested);
    EXPECT_FALSE(audit.events[0].allowed);     // blocked
    EXPECT_EQ(audit.events[0].target, "10 nodes");
    EXPECT_NE(audit.events[0].detail.find("blocked pending approval"),
              std::string::npos);
}

TEST(BurstApprovalGate, AboveCapProceedsWhenApprovedAndIsAudited) {
    RecordingProvisioner inner;
    RecordingAuditSink audit;
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/4, /*max_cost=*/0.0},
                           &audit, approve_all()};

    const ProvisionResult result =
        gate.provision(ProvisionRequest{10, cpu_node(8, 32 * kGiB)});

    // Approved: the burst went through to the inner backend.
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(result.nodes.size(), 10u);
    EXPECT_EQ(inner.provision_calls, 1);

    // Still audited — the approval is recorded.
    ASSERT_EQ(audit.events.size(), 1u);
    EXPECT_EQ(audit.events[0].action, AuditAction::ProvisionRequested);
    EXPECT_TRUE(audit.events[0].allowed);      // approved
    EXPECT_NE(audit.events[0].detail.find("approved"), std::string::npos);
}

// ---- the cost dimension (independent of the node count) ----------------------

TEST(BurstApprovalGate, CostCapTripsOnAnExpensiveGpuBurst) {
    RecordingProvisioner inner;
    RecordingAuditSink audit;
    // Only the cost axis is capped (max_nodes = 0, unlimited). Default cost model:
    // per node ~ cores + GPU TFLOPS. A 2-node A100 burst (8 cores + 312 TFLOPS) is
    // 2 * 320 = 640 cost units — over a 500 cap.
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/0, /*max_cost=*/500.0},
                           &audit};

    const NodeCapability a100 = gpu_node(8, 64 * kGiB, GpuKind::Cuda, 312.0);
    const BurstEstimate est = gate.estimate(ProvisionRequest{2, a100});
    EXPECT_DOUBLE_EQ(est.cost, 640.0);
    EXPECT_TRUE(est.over_cost_cap);
    EXPECT_FALSE(est.over_node_cap);            // node axis is unlimited
    EXPECT_TRUE(est.over_cap());

    const ProvisionResult result = gate.provision(ProvisionRequest{2, a100});
    EXPECT_FALSE(result.satisfied);             // blocked (deny-by-default)
    EXPECT_EQ(inner.provision_calls, 0);
    ASSERT_EQ(audit.events.size(), 1u);
    EXPECT_NE(audit.events[0].detail.find("cost"), std::string::npos);
}

TEST(BurstApprovalGate, CheapCpuBurstStaysUnderTheCostCap) {
    RecordingProvisioner inner;
    // Same 500 cost cap; a 10-node CPU burst (8 cores, no GPU) = 80 units < 500,
    // and node axis unlimited → proceeds.
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/0, /*max_cost=*/500.0}};

    const ProvisionResult result =
        gate.provision(ProvisionRequest{10, cpu_node(8, 32 * kGiB)});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(inner.provision_calls, 1);
}

TEST(BurstApprovalGate, EitherAxisCanTripTheCap) {
    RecordingProvisioner inner;
    // Both axes capped. A burst trips when it exceeds *either*.
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/4, /*max_cost=*/100.0},
                           nullptr, approve_all()};

    // Over the node axis but under cost (4 cores * 5 nodes = 20 < 100).
    const BurstEstimate by_nodes =
        gate.estimate(ProvisionRequest{5, cpu_node(4, 8 * kGiB)});
    EXPECT_TRUE(by_nodes.over_node_cap);
    EXPECT_FALSE(by_nodes.over_cost_cap);
    EXPECT_TRUE(by_nodes.over_cap());

    // Under the node axis but over cost (3 nodes * 50 cores = 150 > 100).
    const BurstEstimate by_cost =
        gate.estimate(ProvisionRequest{3, cpu_node(50, 8 * kGiB)});
    EXPECT_FALSE(by_cost.over_node_cap);
    EXPECT_TRUE(by_cost.over_cost_cap);
    EXPECT_TRUE(by_cost.over_cap());
}

// ---- a custom cost estimator overrides the default ---------------------------

TEST(BurstApprovalGate, CustomCostEstimatorIsUsed) {
    RecordingProvisioner inner;
    // Flat $/node pricing: 3 nodes * $40 = $120 > $100 cap.
    auto dollars = [](const ProvisionRequest& request) {
        return static_cast<double>(request.count) * 40.0;
    };
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/0, /*max_cost=*/100.0},
                           nullptr, /*approver=*/{}, dollars};

    const BurstEstimate est = gate.estimate(ProvisionRequest{3, cpu_node(4, kGiB)});
    EXPECT_DOUBLE_EQ(est.cost, 120.0);
    EXPECT_TRUE(est.over_cost_cap);
    EXPECT_FALSE(gate.provision(ProvisionRequest{3, cpu_node(4, kGiB)}).satisfied);
    EXPECT_EQ(inner.provision_calls, 0);
}

// ---- the approver sees why the burst tripped --------------------------------

TEST(BurstApprovalGate, ApproverReceivesTheEstimate) {
    RecordingProvisioner inner;
    BurstEstimate seen;
    bool called = false;
    BurstApprover spy = [&](const ProvisionRequest&, const BurstEstimate& est) {
        seen = est;
        called = true;
        return false;  // still block
    };
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/4, /*max_cost=*/0.0},
                           nullptr, spy};

    gate.provision(ProvisionRequest{9, cpu_node(2, kGiB)});
    EXPECT_TRUE(called);
    EXPECT_EQ(seen.nodes, 9u);
    EXPECT_TRUE(seen.over_node_cap);
}

// ---- teardown / listing are not gated ---------------------------------------

TEST(BurstApprovalGate, TeardownAndListingDelegateToInner) {
    RecordingProvisioner inner;
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/2, /*max_cost=*/0.0},
                           nullptr, approve_all()};

    gate.provision(ProvisionRequest{3, cpu_node(2, kGiB)});  // over cap, approved
    EXPECT_EQ(gate.provisioned().size(), 3u);                // delegates to inner

    const std::string victim = gate.provisioned().front().node_id;
    EXPECT_TRUE(gate.teardown(victim));
    EXPECT_EQ(inner.teardown_calls, 1);
    EXPECT_EQ(gate.provisioned().size(), 2u);
    EXPECT_FALSE(gate.teardown("never-existed"));            // idempotent passthrough
}

// ---- an unlimited cap never engages -----------------------------------------

TEST(BurstApprovalGate, ZeroCapIsUnlimited) {
    RecordingProvisioner inner;
    RecordingAuditSink audit;
    BurstApprovalGate gate{inner, BurstCap{/*max_nodes=*/0, /*max_cost=*/0.0},
                           &audit};  // both axes unlimited

    const ProvisionResult result =
        gate.provision(ProvisionRequest{1000, gpu_node(64, 256 * kGiB,
                                                       GpuKind::Cuda, 312.0)});
    EXPECT_TRUE(result.satisfied);
    EXPECT_EQ(inner.provision_calls, 1);
    EXPECT_TRUE(audit.events.empty());
}

// ---- BurstCap JSON boundary --------------------------------------------------

TEST(BurstCap, JsonRoundTrip) {
    const BurstCap cap{/*max_nodes=*/16, /*max_cost=*/2500.0};
    const nlohmann::json json = cap.to_json();
    EXPECT_EQ(json["max_nodes"], 16);
    EXPECT_DOUBLE_EQ(json["max_cost"].get<double>(), 2500.0);

    const auto parsed = BurstCap::from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, cap);

    // A hand-written document with only one axis parses (the other defaults to 0).
    const auto nodes_only = BurstCap::from_json(nlohmann::json{{"max_nodes", 8}});
    ASSERT_TRUE(nodes_only);
    EXPECT_EQ(nodes_only->max_nodes, 8u);
    EXPECT_DOUBLE_EQ(nodes_only->max_cost, 0.0);

    // An empty object is the unlimited cap.
    const auto empty = BurstCap::from_json(nlohmann::json::object());
    ASSERT_TRUE(empty);
    EXPECT_EQ(*empty, BurstCap{});
}

TEST(BurstCap, FromJsonRejectsMalformedAtTheBoundary) {
    EXPECT_FALSE(BurstCap::from_json(nlohmann::json::array()));          // not obj
    EXPECT_FALSE(BurstCap::from_json(nlohmann::json{{"max_nodes", -1}}));  // negative
    EXPECT_FALSE(BurstCap::from_json(
        nlohmann::json{{"max_nodes", "lots"}}));                        // wrong type
    EXPECT_FALSE(BurstCap::from_json(nlohmann::json{{"max_cost", -0.5}}));  // negative
    EXPECT_FALSE(BurstCap::from_json(nlohmann::json{{"max_cost", "free"}}));  // wrong
}

// ---- default cost model ------------------------------------------------------

TEST(BurstApprovalGate, DefaultBurstCostWeightsGpuHeavily) {
    // CPU-only: per node = cores; GPU: per node = cores + TFLOPS.
    EXPECT_DOUBLE_EQ(default_burst_cost(ProvisionRequest{4, cpu_node(8, kGiB)}),
                     32.0);  // 4 * 8
    EXPECT_DOUBLE_EQ(
        default_burst_cost(
            ProvisionRequest{2, gpu_node(8, kGiB, GpuKind::Cuda, 312.0)}),
        640.0);  // 2 * (8 + 312)
    EXPECT_DOUBLE_EQ(default_burst_cost(ProvisionRequest{0, cpu_node(8, kGiB)}),
                     0.0);  // a zero-count burst costs nothing
}
