#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/uuid.hpp"

// The node-owned JobCoordinator (#579): a running Node, given its real Cluster
// via Node::attach_cluster, exposes a started JobCoordinator through
// Node::job_coordinator() — and a job submitted through it runs SPMD across the
// live cluster membership, exactly like the ad-hoc coordinators the per-op suites
// build, but owned and torn down in the node lifecycle.
//
// This complements test_job_coordinator.cpp (which constructs coordinators
// ad-hoc): here the coordinator is the one the *node* owns over its real cluster.

using namespace std::chrono_literals;

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::CollectiveOp;
using zetta::ComputeJob;
using zetta::JobKind;
using zetta::JobStatus;
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

// One node of a loopback compute cluster: a standalone database Node (for the
// ComputeJob record) plus its Cluster (the compute plane). Unlike the ad-hoc
// ComputeNode in test_job_coordinator.cpp, this one does NOT own a coordinator —
// it gets one from the Node via attach_cluster(), the path under test. The
// destructor tears the node down BEFORE the cluster, so the node-owned
// coordinator's signal-handler unwind never touches a dead cluster.
struct ClusterNode {
    ClusterNode(std::string cluster_id, std::uint16_t port,
                const std::vector<std::string>& seeds)
        : node(node_config(temp_dir)),
          cluster(std::move(cluster_id), cluster_config(port, seeds)) {
        node.start();
    }

    ~ClusterNode() {
        node.stop();  // releases the coordinator while `cluster` is still alive
        cluster.stop();
    }

    ClusterNode(const ClusterNode&) = delete;
    ClusterNode& operator=(const ClusterNode&) = delete;

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
};

}  // namespace

// --- The acceptance scenario -------------------------------------------------

// A Node started in a real (loopback) multi-node cluster exposes a *running*
// JobCoordinator after attach_cluster, and a job submitted through it runs SPMD
// across the live membership: every node sums its own shard on the coordinator's
// local pool (parallel_map), then a round-boundary allreduce combines the
// per-node partials — so every node ends with the same global sum and records its
// own ComputeJob Succeeded. This is exactly the #579 spine: the coordinator is
// the node's, over its real cluster.
TEST(NodeJobCoordinator, AttachedNodeRunsDistributedJobAcrossMembership) {
    const std::vector<std::string> ids = {"node-a", "node-b", "node-c"};
    constexpr std::uint16_t kBasePort = 46300;

    std::vector<std::unique_ptr<ClusterNode>> nodes;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        nodes.push_back(std::make_unique<ClusterNode>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds));
    }

    // Before any cluster is attached, the node has no compute plane.
    for (const auto& entry : nodes) {
        EXPECT_EQ(entry->node.job_coordinator(), nullptr);
    }

    for (auto& entry : nodes) entry->cluster.start();
    ASSERT_TRUE(wait_for([&] {
        for (const auto& entry : nodes) {
            if (entry->cluster.peer_count() != nodes.size() - 1) return false;
        }
        return true;
    }));

    // Attach the real cluster: each node now owns a started coordinator over it.
    for (auto& entry : nodes) {
        entry->node.attach_cluster(entry->cluster, /*worker_count=*/2);
        ASSERT_NE(entry->node.job_coordinator(), nullptr);
    }

    // Each node owns a different shard; the global sum is 6 + 60 + 600 = 666.
    const std::vector<std::vector<long>> shards = {
        {1, 2, 3}, {10, 20, 30}, {100, 200, 300}};
    constexpr long kExpectedSum = 666;

    std::vector<std::shared_ptr<ComputeJob>> jobs(nodes.size());
    std::vector<JobStatus> statuses(nodes.size(), JobStatus::Queued);
    std::vector<std::thread> threads;
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        jobs[index] = nodes[index]->node.create_job(JobKind::Ddf);
        threads.emplace_back([&, index] {
            const std::vector<long>& shard = shards[index];
            statuses[index] = nodes[index]->node.job_coordinator()->run(
                *jobs[index], [&shard](JobRound& round) -> JobCoordinator::Outcome {
                    const std::vector<long> values =
                        round.parallel_map(shard, [](long value) { return value; });
                    const double partial = static_cast<double>(
                        std::accumulate(values.begin(), values.end(), 0L));
                    const auto total = round.allreduce("node-spine-sum", {partial},
                                                       CollectiveOp::Sum, 5s);
                    if (!total || total->size() != 1) {
                        return {.ok = false, .detail = "allreduce failed"};
                    }
                    return {.ok = true,
                            .detail = "sum=" + std::to_string(
                                                   static_cast<long>((*total)[0]))};
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
        EXPECT_TRUE(nodes[index]->node.find_job(jobs[index]->id()));
    }
}

// --- Lifecycle edges ---------------------------------------------------------

// A single-node cluster degenerates cleanly through the node-owned coordinator
// (auto worker count): the collective reduces locally, so the job still reaches
// Succeeded with the node's own sum.
TEST(NodeJobCoordinator, SingleNodeRunsJobThroughOwnedCoordinator) {
    ClusterNode solo{zetta::new_uuid(), 46320, {}};
    solo.cluster.start();
    solo.node.attach_cluster(solo.cluster);  // worker_count = 0 -> auto
    ASSERT_NE(solo.node.job_coordinator(), nullptr);

    const std::vector<long> shard = {4, 5, 6};
    auto job = solo.node.create_job(JobKind::Featurize);
    const JobStatus status = solo.node.job_coordinator()->run(
        *job, [&](JobRound& round) -> JobCoordinator::Outcome {
            const auto values =
                round.parallel_map(shard, [](long value) { return value; });
            const double partial =
                static_cast<double>(std::accumulate(values.begin(), values.end(), 0L));
            const auto total =
                round.allreduce("solo", {partial}, CollectiveOp::Sum, 2s);
            if (!total) return {.ok = false, .detail = "no result"};
            return {.ok = true,
                    .detail = "sum=" + std::to_string(static_cast<long>((*total)[0]))};
        });

    EXPECT_EQ(status, JobStatus::Succeeded);
    EXPECT_EQ(job->detail(), "sum=15");
}

// stop() tears the coordinator down: after stopping, job_coordinator() is null,
// and the node can be restarted (the attachment is remembered) to get a fresh
// coordinator. This is the "torn down cleanly on stop" half of the acceptance.
TEST(NodeJobCoordinator, StopTearsDownCoordinatorAndRestartReattaches) {
    ClusterNode solo{zetta::new_uuid(), 46330, {}};
    solo.cluster.start();
    solo.node.attach_cluster(solo.cluster, /*worker_count=*/2);
    ASSERT_NE(solo.node.job_coordinator(), nullptr);

    solo.node.stop();
    EXPECT_EQ(solo.node.job_coordinator(), nullptr);

    // A restart re-establishes the compute plane from the remembered attachment.
    ASSERT_TRUE(solo.node.start());
    EXPECT_NE(solo.node.job_coordinator(), nullptr);
}

// A node that never attached a cluster (a standalone node) has no coordinator —
// job_coordinator() is null, and that is a stable, queryable state.
TEST(NodeJobCoordinator, UnattachedNodeHasNoCoordinator) {
    TempDir dir;
    NodeConfig cfg;
    cfg.data_dir = dir.path().string();
    Node node(cfg);
    ASSERT_TRUE(node.start());
    EXPECT_EQ(node.job_coordinator(), nullptr);
    node.stop();
}
