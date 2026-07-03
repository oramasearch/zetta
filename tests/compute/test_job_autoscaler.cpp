#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_autoscaler.hpp"
#include "zetta/cloud/local_provisioner.hpp"
#include "zetta/cloud/node_capability.hpp"
#include "zetta/cloud/provisioner.hpp"
#include "zetta/compute/scaling_policy.hpp"

using zetta::compute::AutoscalerConfig;
using zetta::compute::AutoscaleTick;
using zetta::compute::ComputeJob;
using zetta::compute::ComputeJobValue;
using zetta::compute::JobAutoscaler;
using zetta::compute::JobObservation;
using zetta::compute::JobStatus;
using zetta::cloud::LocalProvisioner;
using zetta::cloud::NodeCapability;
using zetta::cloud::ProvisionedNode;
using zetta::cloud::ProvisionRequest;
using zetta::cloud::ProvisionResult;
using zetta::cloud::Provisioner;
using zetta::compute::ScalingAction;
using zetta::compute::ScalingBounds;
using zetta::compute::ScalingObjective;
using zetta::compute::ScalingPolicy;
using zetta::test::TempDir;

namespace {

// A deterministic fake Provisioner with the seam's contract: provision(count=N)
// ensures N nodes total (adds the shortfall — the "set the pool size" semantic the
// cloud backends implement), teardown(id) releases one, provisioned() lists the
// live fleet. Test helpers let a test simulate membership changes out from under
// the loop (elastic membership) and inspect how often each path was hit.
class FakeProvisioner : public Provisioner {
public:
    explicit FakeProvisioner(std::uint32_t initial = 0) { grow_to(initial); }

    ProvisionResult provision(const ProvisionRequest& request) override {
        ++provision_calls_;
        ProvisionResult result;
        if (request.count == 0) {
            result.satisfied = true;
            return result;
        }
        grow_to(static_cast<std::uint32_t>(request.count));
        const std::size_t reported = std::min<std::size_t>(request.count, nodes_.size());
        result.nodes.assign(nodes_.begin(), nodes_.begin() + reported);
        result.satisfied = nodes_.size() >= request.count;
        result.detail = "scaled to " + std::to_string(request.count);
        return result;
    }

    bool teardown(const std::string& node_id) override {
        ++teardown_calls_;
        const auto found = std::find_if(
            nodes_.begin(), nodes_.end(),
            [&](const ProvisionedNode& node) { return node.node_id == node_id; });
        if (found == nodes_.end()) return false;
        nodes_.erase(found);
        return true;
    }

    std::vector<ProvisionedNode> provisioned() const override { return nodes_; }

    // Simulate a node leaving the cluster (elastic membership) — drop `count` from
    // the tail without going through teardown.
    void drop(std::uint32_t count) {
        for (std::uint32_t removed = 0; removed < count && !nodes_.empty(); ++removed) {
            nodes_.pop_back();
        }
    }

    int provision_calls() const { return provision_calls_; }
    int teardown_calls() const { return teardown_calls_; }

private:
    void grow_to(std::uint32_t target) {
        while (nodes_.size() < target) {
            ProvisionedNode node;
            node.node_id = "node-" + std::to_string(next_id_++);
            node.capability = NodeCapability{};  // no floor: satisfies any spec
            nodes_.push_back(node);
        }
    }

    std::vector<ProvisionedNode> nodes_;
    std::uint32_t next_id_ = 0;
    int provision_calls_ = 0;
    int teardown_calls_ = 0;
};

// A ComputeJob in the given lifecycle state, backed by a real temp dir (the loop
// only reads status(), so nothing is persisted here).
ComputeJob make_job(const TempDir& tmp, JobStatus status) {
    ComputeJobValue value;
    value.status = status;
    return ComputeJob("job-1", value, tmp.path());
}

ScalingPolicy runtime_target(std::uint32_t min_nodes, std::uint32_t max_nodes,
                             double target_seconds, double cost_cap = 0.0) {
    return ScalingPolicy{ScalingObjective::RuntimeTarget,
                         ScalingBounds{min_nodes, max_nodes, cost_cap},
                         target_seconds};
}

ScalingPolicy minimize_cost(std::uint32_t min_nodes, std::uint32_t max_nodes,
                            double cost_cap = 0.0) {
    return ScalingPolicy{ScalingObjective::MinimizeCost,
                         ScalingBounds{min_nodes, max_nodes, cost_cap}, 0.0};
}

ScalingPolicy data_shape(std::uint32_t min_nodes, std::uint32_t max_nodes) {
    return ScalingPolicy{ScalingObjective::DataShape,
                         ScalingBounds{min_nodes, max_nodes, 0.0}, 0.0};
}

AutoscalerConfig confirm(std::uint32_t ticks) {
    AutoscalerConfig config;
    config.confirm_ticks = ticks;
    return config;
}

}  // namespace

// ---- the acceptance criterion: scale up under a runtime target -------------

TEST(JobAutoscaler, ScalesUpUnderRuntimeTarget) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Running);
    FakeProvisioner provisioner(/*initial=*/2);

    // 25% done after 50s → projected 200s, double the 100s target → behind.
    JobObservation observation{/*progress=*/0.25, /*elapsed=*/50.0, /*cost=*/0.0};
    JobAutoscaler autoscaler(job, runtime_target(1, 8, /*target=*/100.0),
                             provisioner, [&] { return observation; }, confirm(2));

    // Tick 1: a single ScaleUp recommendation does not move the fleet (hysteresis).
    const AutoscaleTick first = autoscaler.tick();
    EXPECT_EQ(first.decision.action, ScalingAction::ScaleUp);
    EXPECT_EQ(first.confirmed_action, ScalingAction::Hold);
    EXPECT_FALSE(first.acted);
    EXPECT_EQ(autoscaler.nodes(), 2u);

    // Tick 2: a second consecutive ScaleUp confirms → grow toward the projection.
    // base 2, projected 200, target 100 → ceil(2 * 200/100) = 4 nodes.
    const AutoscaleTick second = autoscaler.tick();
    EXPECT_EQ(second.confirmed_action, ScalingAction::ScaleUp);
    EXPECT_TRUE(second.acted);
    EXPECT_EQ(second.nodes_before, 2u);
    EXPECT_EQ(second.nodes_after, 4u);
    EXPECT_EQ(autoscaler.nodes(), 4u);
    EXPECT_EQ(autoscaler.total_scale_ups(), 1u);
    EXPECT_EQ(autoscaler.total_scale_downs(), 0u);
}

// ---- the acceptance criterion: scale down under a cost objective ------------

TEST(JobAutoscaler, ScalesDownUnderCostObjective) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Running);
    FakeProvisioner provisioner(/*initial=*/6);

    JobObservation observation{/*progress=*/0.5, /*elapsed=*/10.0, /*cost=*/10.0};
    JobAutoscaler autoscaler(job, minimize_cost(1, 8), provisioner,
                             [&] { return observation; }, confirm(2));

    // minimize_cost always recommends the floor → ScaleDown from 6.
    const AutoscaleTick first = autoscaler.tick();
    EXPECT_EQ(first.decision.action, ScalingAction::ScaleDown);
    EXPECT_EQ(first.confirmed_action, ScalingAction::Hold);  // awaiting confirmation
    EXPECT_EQ(autoscaler.nodes(), 6u);

    // Confirmed on the second tick → shed back to min_nodes (1).
    const AutoscaleTick second = autoscaler.tick();
    EXPECT_EQ(second.confirmed_action, ScalingAction::ScaleDown);
    EXPECT_TRUE(second.acted);
    EXPECT_EQ(second.nodes_before, 6u);
    EXPECT_EQ(second.nodes_after, 1u);
    EXPECT_EQ(autoscaler.nodes(), 1u);
    EXPECT_EQ(autoscaler.total_scale_downs(), 1u);
    EXPECT_EQ(provisioner.teardown_calls(), 5);  // released 6 → 1
}

// ---- the cost cap is a hard constraint: shed immediately, no confirmation ---

TEST(JobAutoscaler, CostCapBreachShedsImmediately) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Running);
    FakeProvisioner provisioner(/*initial=*/8);

    // A runtime-target policy that WOULD otherwise scale up (it is behind)...
    JobObservation observation{/*progress=*/0.25, /*elapsed=*/50.0, /*cost=*/60.0};
    JobAutoscaler autoscaler(
        job, runtime_target(1, 16, /*target=*/100.0, /*cost_cap=*/50.0),
        provisioner, [&] { return observation; }, confirm(3));

    // ...but the cost cap is breached → shed to min_nodes on the FIRST tick,
    // bypassing the 3-tick confirmation window (the hard-constraint path).
    const AutoscaleTick tick = autoscaler.tick();
    EXPECT_FALSE(tick.decision.within_cost_cap);
    EXPECT_EQ(tick.confirmed_action, ScalingAction::ScaleDown);
    EXPECT_TRUE(tick.acted);
    EXPECT_EQ(autoscaler.nodes(), 1u);
    EXPECT_EQ(autoscaler.total_scale_downs(), 1u);
}

// ---- hysteresis prevents thrashing on an alternating signal ----------------

TEST(JobAutoscaler, HysteresisPreventsThrash) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Running);
    FakeProvisioner provisioner(/*initial=*/4);

    // Alternate between "behind" (ScaleUp) and "on pace" (Hold) every tick, so the
    // ScaleUp streak never reaches the 2-tick confirmation window.
    std::vector<JobObservation> script{
        {0.25, 50.0, 0.0},  // projected 200 > 100 → ScaleUp
        {0.50, 50.0, 0.0},  // projected 100 == 100 → Hold (resets the streak)
        {0.25, 50.0, 0.0},  // ScaleUp again, but streak was reset → still 1
        {0.50, 50.0, 0.0},  // Hold
    };
    std::size_t index = 0;
    JobAutoscaler autoscaler(job, runtime_target(1, 8, /*target=*/100.0),
                             provisioner,
                             [&] { return script[index++]; }, confirm(2));

    for (std::size_t step = 0; step < script.size(); ++step) {
        const AutoscaleTick tick = autoscaler.tick();
        EXPECT_FALSE(tick.acted) << "tick " << step << " should not move the fleet";
    }
    EXPECT_EQ(autoscaler.nodes(), 4u);            // never moved
    EXPECT_EQ(autoscaler.total_scale_ups(), 0u);
    EXPECT_EQ(autoscaler.total_scale_downs(), 0u);
    EXPECT_EQ(provisioner.provision_calls(), 0);  // never even asked the backend
}

// ---- given hardware (LocalProvisioner): best-effort, no release -------------

TEST(JobAutoscaler, GivenHardwareDegradesToBestEffort) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Running);

    // A fixed three-node cluster behind the given-hardware provisioner.
    std::vector<ProvisionedNode> fleet;
    for (int index = 0; index < 3; ++index) {
        ProvisionedNode node;
        node.node_id = "fixed-" + std::to_string(index);
        fleet.push_back(node);
    }
    LocalProvisioner provisioner([&] { return fleet; });

    // A cost objective wants to shed to 1, but given hardware cannot be released.
    JobObservation observation{0.5, 10.0, 10.0};
    JobAutoscaler autoscaler(job, minimize_cost(1, 8), provisioner,
                             [&] { return observation; }, confirm(1));

    const AutoscaleTick tick = autoscaler.tick();
    EXPECT_EQ(tick.confirmed_action, ScalingAction::ScaleDown);  // it tried
    EXPECT_FALSE(tick.acted);                                    // nothing released
    EXPECT_EQ(autoscaler.nodes(), 3u);  // max utilization of the fixed fleet
    EXPECT_EQ(autoscaler.total_scale_downs(), 0u);
    EXPECT_NE(tick.detail.find("given hardware"), std::string::npos);
}

// ---- a non-running job is a no-op -------------------------------------------

TEST(JobAutoscaler, NonRunningJobHolds) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Queued);
    FakeProvisioner provisioner(/*initial=*/4);

    JobObservation observation{0.5, 10.0, 99.0};  // would otherwise shed
    JobAutoscaler autoscaler(job, minimize_cost(1, 8), provisioner,
                             [&] { return observation; }, confirm(1));

    const AutoscaleTick tick = autoscaler.tick();
    EXPECT_FALSE(tick.job_running);
    EXPECT_FALSE(tick.acted);
    EXPECT_EQ(autoscaler.nodes(), 4u);
    EXPECT_EQ(provisioner.provision_calls(), 0);
    EXPECT_EQ(provisioner.teardown_calls(), 0);
}

// ---- elastic membership: re-provision toward the target after a node leaves -

TEST(JobAutoscaler, ReProvisionsAfterMembershipLoss) {
    TempDir tmp;
    const ComputeJob job = make_job(tmp, JobStatus::Running);
    FakeProvisioner provisioner(/*initial=*/4);

    // data_shape early in the job → size toward the ceiling (10).
    JobObservation observation{/*progress=*/0.1, /*elapsed=*/5.0, /*cost=*/0.0};
    JobAutoscaler autoscaler(job, data_shape(2, 10), provisioner,
                             [&] { return observation; }, confirm(1));

    // First tick scales up to the data-shaped target.
    const AutoscaleTick grown = autoscaler.tick();
    EXPECT_EQ(grown.confirmed_action, ScalingAction::ScaleUp);
    EXPECT_EQ(autoscaler.nodes(), 10u);

    // Three nodes leave the cluster mid-job (a crash / autoscale-out elsewhere).
    provisioner.drop(3);
    EXPECT_EQ(autoscaler.nodes(), 7u);  // the loop sees live membership

    // Next tick re-reads membership and re-provisions back toward the target.
    const AutoscaleTick healed = autoscaler.tick();
    EXPECT_EQ(healed.confirmed_action, ScalingAction::ScaleUp);
    EXPECT_EQ(healed.nodes_before, 7u);
    EXPECT_EQ(healed.nodes_after, 10u);
    EXPECT_EQ(autoscaler.nodes(), 10u);
    EXPECT_EQ(autoscaler.total_scale_ups(), 2u);
}

// ---- AutoscalerConfig JSON boundary: round-trip ----------------------------

TEST(JobAutoscaler, ConfigJsonRoundTrip) {
    AutoscalerConfig config;
    config.node_spec.cores = 8;
    config.node_spec.memory_bytes = 16ull << 30;
    config.confirm_ticks = 3;
    config.shed_on_cost_breach = false;

    const nlohmann::json json = config.to_json();
    EXPECT_EQ(json["confirm_ticks"], 3);
    EXPECT_EQ(json["shed_on_cost_breach"], false);
    EXPECT_EQ(json["node_spec"]["cores"], 8);

    const auto parsed = AutoscalerConfig::from_json(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, config);

    // Omitted fields take their defaults.
    const auto minimal = AutoscalerConfig::from_json(nlohmann::json::object());
    ASSERT_TRUE(minimal);
    EXPECT_EQ(minimal->confirm_ticks, 2u);
    EXPECT_TRUE(minimal->shed_on_cost_breach);
}

// ---- AutoscalerConfig JSON boundary: rejects malformed input ----------------

TEST(JobAutoscaler, ConfigFromJsonRejectsMalformed) {
    using nlohmann::json;

    EXPECT_FALSE(AutoscalerConfig::from_json(json::array()));  // not an object

    // confirm_ticks must be an integer >= 1.
    EXPECT_FALSE(AutoscalerConfig::from_json(json{{"confirm_ticks", 0}}));
    EXPECT_FALSE(AutoscalerConfig::from_json(json{{"confirm_ticks", -2}}));
    EXPECT_FALSE(AutoscalerConfig::from_json(json{{"confirm_ticks", "two"}}));

    // shed_on_cost_breach must be a boolean.
    EXPECT_FALSE(AutoscalerConfig::from_json(json{{"shed_on_cost_breach", 1}}));

    // an invalid node_spec is rejected by NodeCapability's boundary.
    EXPECT_FALSE(AutoscalerConfig::from_json(
        json{{"node_spec", {{"cores", -1}, {"memory_bytes", 0}}}}));

    // a well-formed config parses.
    EXPECT_TRUE(AutoscalerConfig::from_json(
        json{{"confirm_ticks", 1}, {"shed_on_cost_breach", true}}));
}
