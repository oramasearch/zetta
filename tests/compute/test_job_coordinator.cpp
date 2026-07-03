#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

using namespace std::chrono_literals;

using zetta::governance::AuditAction;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::CollectiveOp;
using zetta::compute::ComputeJob;
using zetta::compute::JobKind;
using zetta::compute::JobStatus;
using zetta::Node;
using zetta::NodeConfig;
using zetta::compute::JobCoordinator;
using zetta::compute::JobRound;
using zetta::test::TempDir;

namespace {

void sleep_ms(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) return true;
        sleep_ms(25);
    }
    return false;
}

// One node of a loopback compute cluster: a database Node (for the ComputeJob
// record), a Cluster (the compute plane), and the JobCoordinator over it. The
// data Node is standalone (no replication) — the test asserts the recorded job
// locally, not its cross-node replication (which test_compute_job covers).
struct ComputeNode {
    ComputeNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds, unsigned workers)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)),
          coordinator(cluster, workers) {
        node.start();
    }

    static NodeConfig node_config(const TempDir& dir) {
        NodeConfig cfg;
        cfg.data_dir = dir.path().string();
        return cfg;
    }

    static ClusterConfig cluster_config(std::uint16_t port,
                                        const std::vector<std::string>& seeds) {
        ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = seeds;
        return config;
    }

    // Declared first so it outlives `node` (which writes under its path).
    TempDir temp_dir;
    Node node;
    Cluster cluster;
    JobCoordinator coordinator;
};

// The squared-value executor for the cross-node map path: a stolen task must
// compute identically wherever it lands.
std::string square_executor(std::string_view /*method*/,
                            std::string_view payload) {
    const long value = std::stol(std::string(payload));
    return std::to_string(value * value);
}

}  // namespace

// --- The acceptance scenario -------------------------------------------------

// A trivial distributed-sum job runs end-to-end through the coordinator on a
// multi-node loopback cluster and records a ComputeJob. This is the headline
// SPMD path: every node sums its own shard across the local work-stealing pool
// (parallel_map), then a collective (allreduce) combines the per-node partials
// at the round boundary — so every node ends with the same global sum, and each
// records its own job as Succeeded.
TEST(JobCoordinator, DistributedSumRecordsJobOnEveryNode) {
    // Controlled cluster ids so "node-a" is the deterministic collective root.
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    constexpr std::uint16_t kBasePort = 46200;

    std::vector<std::unique_ptr<ComputeNode>> nodes;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" +
                            std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<ComputeNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds,
            /*workers=*/2));
    }

    for (auto& node : nodes) node->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& node : nodes) {
            if (node->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    }));
    for (auto& node : nodes) node->coordinator.start();

    // Each node owns a different shard; the global sum is 6 + 60 + 600 = 666.
    const std::vector<std::vector<long>> shards = {
        {1, 2, 3}, {10, 20, 30}, {100, 200, 300}};
    constexpr long kExpectedSum = 666;

    // Each node creates + drives its own ComputeJob through its coordinator on
    // its own thread (collectives need every node participating concurrently).
    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes.size());
    std::vector<JobStatus> statuses(nodes.size(), JobStatus::Queued);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Ddf);
        threads.emplace_back([&, index] {
            const std::vector<long>& shard = shards[index];
            statuses[index] = nodes[index]->coordinator.run(
                *jobs[index], [&shard](JobRound& round) -> JobCoordinator::Outcome {
                    // Split the shard into tasks on the local work-stealing pool.
                    const std::vector<long> values =
                        round.parallel_map(shard, [](long value) { return value; });
                    const double partial = static_cast<double>(
                        std::accumulate(values.begin(), values.end(), 0L));
                    // Round boundary: combine the per-node partials.
                    const auto total = round.allreduce(
                        "sum-round", {partial}, CollectiveOp::Sum, 5s);
                    if (!total || total->size() != 1) {
                        return {.ok = false, .detail = "allreduce failed"};
                    }
                    return {.ok = true,
                            .detail = "sum=" +
                                      std::to_string(static_cast<long>((*total)[0]))};
                });
        });
    }
    for (auto& thread : threads) thread.join();

    // Every node finished Succeeded with the same global sum recorded.
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        EXPECT_EQ(statuses[index], JobStatus::Succeeded) << "node " << index;
        EXPECT_EQ(jobs[index]->status(), JobStatus::Succeeded) << "node " << index;
        EXPECT_EQ(jobs[index]->detail(), "sum=" + std::to_string(kExpectedSum))
            << "node " << index;
    }

    // The job is durably recorded: it survives a registry lookup, and the
    // lifecycle is audited (job_created on create, job_finished on Succeeded).
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id()));
        int created = 0;
        int finished = 0;
        for (const auto& event : nodes[index]->node.audit_events()) {
            if (event->target() != jobs[index]->id()) continue;
            if (event->action() == AuditAction::JobCreated) ++created;
            if (event->action() == AuditAction::JobFinished) ++finished;
        }
        EXPECT_EQ(created, 1) << "node " << index;
        EXPECT_EQ(finished, 1) << "node " << index;
    }

    for (auto& node : nodes) node->coordinator.stop();
    for (auto& node : nodes) node->cluster.stop();
}

// --- The cross-node map path through the coordinator --------------------------

// The coordinator also exposes dynamic cross-node task stealing (map). The owner
// node has zero local workers (a pure source), so if its job succeeds at all,
// every task crossed the wire: the idle helper stole them, ran them, and
// returned the results — and the job is recorded Succeeded.
TEST(JobCoordinator, MapDistributesTasksToIdlePeer) {
    ComputeNode owner{"owner", 46210, {}, /*workers=*/0};
    ComputeNode helper{"helper", 46211, {"127.0.0.1:46210"}, /*workers=*/4};
    owner.coordinator.set_executor(square_executor);
    helper.coordinator.set_executor(square_executor);

    owner.cluster.start();
    sleep_ms(100);
    helper.cluster.start();
    ASSERT_TRUE(wait_for([&] {
        return owner.cluster.peer_count() == 1 && helper.cluster.peer_count() == 1;
    }));
    owner.coordinator.start();
    helper.coordinator.start();

    constexpr int kTasks = 16;
    std::vector<std::string> inputs;
    for (int value = 0; value < kTasks; ++value) {
        inputs.push_back(std::to_string(value));
    }
    // Sum of squares 0..15 = 1240.
    long expected = 0;
    for (int value = 0; value < kTasks; ++value) expected += long(value) * value;

    auto job = owner.node.create_job(JobKind::Score);
    long observed = 0;
    const JobStatus status = owner.coordinator.run(
        *job, [&](JobRound& round) -> JobCoordinator::Outcome {
            const std::vector<std::string> results = round.map("square", inputs);
            if (results.size() != inputs.size()) {
                return {.ok = false, .detail = "wrong result count"};
            }
            for (const std::string& result : results) {
                observed += std::stol(result);
            }
            return {.ok = true, .detail = "sum_of_squares=" + std::to_string(observed)};
        });

    EXPECT_EQ(status, JobStatus::Succeeded);
    EXPECT_EQ(observed, expected);
    EXPECT_EQ(job->status(), JobStatus::Succeeded);
    EXPECT_EQ(job->detail(), "sum_of_squares=" + std::to_string(expected));

    // The owner ran nothing itself; the helper stole and ran every task.
    EXPECT_EQ(owner.coordinator.scheduler().stats().local_executed, 0u);
    EXPECT_EQ(owner.coordinator.scheduler().stats().handed_out,
              static_cast<std::size_t>(kTasks));
    EXPECT_EQ(helper.coordinator.scheduler().stats().stolen_executed,
              static_cast<std::size_t>(kTasks));

    owner.coordinator.stop();
    helper.coordinator.stop();
    helper.cluster.stop();
    owner.cluster.stop();
}

// --- Lifecycle edges ---------------------------------------------------------

// A single-node cluster degenerates cleanly: the collective reduces locally, so
// the coordinator still drives the job to Succeeded with the node's own sum.
TEST(JobCoordinator, SingleNodeDrivesJobToSucceeded) {
    ComputeNode solo{zetta::util::new_uuid(), 46220, {}, /*workers=*/2};
    solo.cluster.start();
    solo.coordinator.start();

    const std::vector<long> shard = {4, 5, 6};
    auto job = solo.node.create_job(JobKind::Featurize);
    const JobStatus status = solo.coordinator.run(
        *job, [&](JobRound& round) -> JobCoordinator::Outcome {
            const auto values =
                round.parallel_map(shard, [](long value) { return value; });
            const double partial = static_cast<double>(
                std::accumulate(values.begin(), values.end(), 0L));
            const auto total =
                round.allreduce("solo", {partial}, CollectiveOp::Sum, 2s);
            if (!total) return {.ok = false, .detail = "no result"};
            return {.ok = true,
                    .detail = "sum=" +
                              std::to_string(static_cast<long>((*total)[0]))};
        });

    EXPECT_EQ(status, JobStatus::Succeeded);
    EXPECT_EQ(job->detail(), "sum=15");

    solo.coordinator.stop();
    solo.cluster.stop();
}

// A job body that reports failure marks the job Failed and records the reason —
// the coordinator never leaves a job stuck in Running.
TEST(JobCoordinator, FailedAlgorithmMarksJobFailed) {
    ComputeNode solo{zetta::util::new_uuid(), 46230, {}, /*workers=*/1};
    solo.cluster.start();
    solo.coordinator.start();

    auto job = solo.node.create_job(JobKind::Train);
    const JobStatus status = solo.coordinator.run(
        *job, [](JobRound&) -> JobCoordinator::Outcome {
            return {.ok = false, .detail = "model diverged"};
        });

    EXPECT_EQ(status, JobStatus::Failed);
    EXPECT_EQ(job->status(), JobStatus::Failed);
    EXPECT_EQ(job->detail(), "model diverged");

    solo.coordinator.stop();
    solo.cluster.stop();
}

// --- Cross-node trace propagation (#640) --------------------------------------

// One trace per distributed job: the coordinator stamps the job's trace id,
// records the job root span + the collective-round child spans on this node,
// and a helper that steals tasks records its execution span joining the
// OWNER's trace — so reading the trace shows where the work actually ran.
TEST(JobCoordinator, DistributedJobSpansJoinOneTraceAcrossNodes) {
    ComputeNode owner{"trace-owner", 46240, {}, /*workers=*/0};
    ComputeNode helper{"trace-helper", 46241, {"127.0.0.1:46240"}, /*workers=*/2};
    owner.coordinator.set_executor(square_executor);
    helper.coordinator.set_executor(square_executor);
    owner.coordinator.set_span_recorder(&owner.node.span_recorder());
    helper.coordinator.set_span_recorder(&helper.node.span_recorder());

    owner.cluster.start();
    sleep_ms(100);
    helper.cluster.start();
    ASSERT_TRUE(wait_for([&] {
        return owner.cluster.peer_count() == 1 && helper.cluster.peer_count() == 1;
    }));
    owner.coordinator.start();
    helper.coordinator.start();

    std::vector<std::string> inputs;
    for (int value = 0; value < 8; ++value) {
        inputs.push_back(std::to_string(value));
    }
    auto job = owner.node.create_job(JobKind::Score);
    EXPECT_TRUE(job->value().trace_id.empty());  // stamped by run() below
    const JobStatus status = owner.coordinator.run(
        *job, [&](JobRound& round) -> JobCoordinator::Outcome {
            const std::vector<std::string> results = round.map("square", inputs);
            return {.ok = results.size() == inputs.size(),
                    .detail = "mapped"};
        });
    ASSERT_EQ(status, JobStatus::Succeeded);

    // The job record carries the trace id (replicated state).
    const std::string trace_id = job->value().trace_id;
    ASSERT_EQ(trace_id.size(), 32u);

    // Owner: the job root span + the map child span, all in ONE trace.
    bool owner_has_job_span = false;
    bool owner_has_map_span = false;
    for (const auto& span : owner.node.span_recorder().trace(trace_id)) {
        if (span.name == "job:score") owner_has_job_span = true;
        if (span.name == "map:square") owner_has_map_span = true;
    }
    EXPECT_TRUE(owner_has_job_span);
    EXPECT_TRUE(owner_has_map_span);

    // Helper: executed the stolen tasks under the OWNER's trace — the
    // cross-node acceptance of #640 (spans from other nodes of one run).
    ASSERT_TRUE(wait_for([&] {
        return !helper.node.span_recorder().trace(trace_id).empty();
    }));
    const auto helper_spans = helper.node.span_recorder().trace(trace_id);
    bool helper_ran_stolen_work = false;
    for (const auto& span : helper_spans) {
        if (span.name == "steal:square") helper_ran_stolen_work = true;
    }
    EXPECT_TRUE(helper_ran_stolen_work);

    owner.coordinator.stop();
    helper.coordinator.stop();
    helper.cluster.stop();
    owner.cluster.stop();
}

// The SPMD path records the collective rounds as spans of the job's trace.
TEST(JobCoordinator, CollectiveRoundsBecomeSpansOfTheJobTrace) {
    ComputeNode solo{zetta::util::new_uuid(), 46250, {}, /*workers=*/2};
    solo.coordinator.set_span_recorder(&solo.node.span_recorder());
    solo.cluster.start();
    solo.coordinator.start();

    auto job = solo.node.create_job(JobKind::Ddf);
    const JobStatus status = solo.coordinator.run(
        *job, [&](JobRound& round) -> JobCoordinator::Outcome {
            const auto total =
                round.allreduce("trace-round", {21.0}, CollectiveOp::Sum,
                                std::chrono::seconds(5));
            return {.ok = total.has_value(), .detail = "reduced"};
        });
    ASSERT_EQ(status, JobStatus::Succeeded);

    const auto spans = solo.node.span_recorder().trace(job->value().trace_id);
    bool has_round_span = false;
    bool round_parents_on_job_span = false;
    std::string job_span_id;
    for (const auto& span : spans) {
        if (span.name == "job:ddf") job_span_id = span.span_id;
    }
    for (const auto& span : spans) {
        if (span.name == "collective:allreduce") {
            has_round_span = true;
            round_parents_on_job_span = span.parent_span_id == job_span_id;
            EXPECT_EQ(span.attributes.at("tag"), "trace-round");
        }
    }
    EXPECT_TRUE(has_round_span);
    EXPECT_TRUE(round_parents_on_job_span);

    solo.coordinator.stop();
    solo.cluster.stop();
}
