#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/node.hpp"
#include "zetta/compute/pipeline_instrumentation.hpp"
#include "zetta/query/service.hpp"
#include "zetta/util/uuid.hpp"

// Live pipeline instrumentation as observable read views (#589, epic #575 area 5).
// The distributed pipeline's instrumentation — the per-job near-data counters
// (bytes read near data vs moved over the wire) and the live collective-plane
// progress — is queryable as ephemeral, observed `system.<table>`(s), so the GUI's
// algorithm-at-work animation reads from system tables rather than bespoke hooks:
//   - system.pipeline_jobs    — one row per recorded near-data job (Node::pipeline_jobs)
//   - system.collective_rounds — the live coordinator's round + wire-byte meters
// Both are ephemeral / observed: NOT replicated, no CRDT backing (like
// system.node_metrics / system.checkpoints). The end-to-end "instrumentation of a
// live interactive run is queryable" acceptance over a real loopback cluster lives
// in test_ddf_local_partitions.cpp (the near-data group-by run).

using namespace std::chrono_literals;

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::CollectiveOp;
using zetta::JobKind;
using zetta::JobStatus;
using zetta::NearDataJobKind;
using zetta::Node;
using zetta::NodeConfig;
using zetta::PipelineJobInstrumentation;
using zetta::compute::JobCoordinator;
using zetta::query::QueryService;
using zetta::compute::JobRound;
using zetta::test::TempDir;

namespace {

NodeConfig node_config(const TempDir& dir) {
    NodeConfig cfg;
    cfg.data_dir = dir.path().string();
    return cfg;
}

PipelineJobInstrumentation sample_record(std::string job_id, NearDataJobKind kind,
                                         const std::string& node_id) {
    return PipelineJobInstrumentation{
        .job_id = std::move(job_id),
        .kind = kind,
        .node = node_id,
        .partitions_read = 4,
        .rows_read = 300,
        .local_bytes_read = 12000,
        .wire_bytes_moved = 240,
        .status = JobStatus::Succeeded,
        .recorded_at = "2026-06-30T00:00:00Z",
    };
}

}  // namespace

// A recorded near-data job surfaces in system.pipeline_jobs with its local-vs-wire
// counters, in record order. Projection + WHERE work like any system table.
TEST(PipelineInstrumentation, PipelineJobsViewExposesRecordedCounters) {
    TempDir dir;
    Node node{node_config(dir)};
    ASSERT_TRUE(node.start());

    node.record_pipeline_job(
        sample_record("job-gb", NearDataJobKind::GroupBy, node.node_id()));
    node.record_pipeline_job(
        sample_record("job-train", NearDataJobKind::Train, node.node_id()));

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.pipeline_jobs;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 2u);

    const auto& first = sel.rows[0];
    EXPECT_EQ(first["job_id"].get<std::string>(), "job-gb");
    EXPECT_EQ(first["kind"].get<std::string>(), "group_by");
    EXPECT_EQ(first["node"].get<std::string>(), node.node_id());
    EXPECT_EQ(first["partitions_read"].get<std::uint64_t>(), 4u);
    EXPECT_EQ(first["rows_read"].get<std::uint64_t>(), 300u);
    EXPECT_EQ(first["local_bytes_read"].get<std::uint64_t>(), 12000u);
    EXPECT_EQ(first["wire_bytes_moved"].get<std::uint64_t>(), 240u);
    EXPECT_EQ(first["status"].get<std::string>(), "succeeded");
    EXPECT_EQ(first["recorded_at"].get<std::string>(), "2026-06-30T00:00:00Z");
    // Bytes moved over the wire are a small fraction of what was read near data.
    EXPECT_LT(first["wire_bytes_moved"].get<std::uint64_t>(),
              first["local_bytes_read"].get<std::uint64_t>());

    EXPECT_EQ(sel.rows[1]["kind"].get<std::string>(), "train");

    // Projection + WHERE.
    auto filtered = svc.run(
        "SELECT job_id FROM system.pipeline_jobs WHERE kind = 'train';", "");
    ASSERT_TRUE(filtered.ok()) << filtered.error;
    ASSERT_EQ(filtered.rows.size(), 1u);
    EXPECT_EQ(filtered.rows[0]["job_id"].get<std::string>(), "job-train");

    node.stop();
}

// The registry is bounded: only the most-recent kMaxPipelineJobs records are kept
// (oldest dropped), so the ephemeral view can't grow without limit over a
// long-lived node.
TEST(PipelineInstrumentation, PipelineJobsRegistryIsBounded) {
    TempDir dir;
    Node node{node_config(dir)};
    ASSERT_TRUE(node.start());

    const std::size_t over = Node::kMaxPipelineJobs + 25;
    for (std::size_t i = 0; i < over; ++i) {
        node.record_pipeline_job(sample_record(
            "job-" + std::to_string(i), NearDataJobKind::Featurize, node.node_id()));
    }

    const auto jobs = node.pipeline_jobs();
    ASSERT_EQ(jobs.size(), Node::kMaxPipelineJobs);
    // The oldest 25 were dropped; the front is now job-25 and the back is the last.
    EXPECT_EQ(jobs.front().job_id, "job-25");
    EXPECT_EQ(jobs.back().job_id, "job-" + std::to_string(over - 1));

    node.stop();
}

// An empty registry yields an empty view (no rows until a near-data job runs),
// like system.checkpoints.
TEST(PipelineInstrumentation, PipelineJobsEmptyByDefault) {
    TempDir dir;
    Node node{node_config(dir)};
    ASSERT_TRUE(node.start());

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.pipeline_jobs;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    EXPECT_EQ(sel.rows.size(), 0u);

    node.stop();
}

// A standalone node (no cluster attached → no compute plane) reports exactly one
// collective-rounds row with attached=false and zeroed live meters — always one
// row, like system.node_metrics.
TEST(PipelineInstrumentation, CollectiveRoundsWithoutCoordinator) {
    TempDir dir;
    Node node{node_config(dir)};
    ASSERT_TRUE(node.start());
    ASSERT_EQ(node.job_coordinator(), nullptr);

    QueryService svc{node};
    auto sel = svc.run("SELECT * FROM system.collective_rounds;", "");
    ASSERT_TRUE(sel.ok()) << sel.error;
    ASSERT_EQ(sel.rows.size(), 1u);

    const auto& row = sel.rows[0];
    EXPECT_EQ(row["node"].get<std::string>(), node.node_id());
    EXPECT_FALSE(row["attached"].get<bool>());
    EXPECT_EQ(row["participants"].get<int>(), 0);
    EXPECT_EQ(row["rounds_completed"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(row["wire_bytes_moved"].get<std::uint64_t>(), 0u);

    node.stop();
}

// A node with its real cluster attached (the #579 spine) reports the live
// coordinator's collective-plane progress: after running a job whose body does an
// allreduce through the node-owned coordinator, system.collective_rounds shows
// attached=true and rounds_completed advanced. A single-node cluster reduces
// locally, so no bytes cross the wire (one participant) — proving the meter is
// wired to the live coordinator, not a stub.
TEST(PipelineInstrumentation, CollectiveRoundsTracksLiveCoordinator) {
    TempDir dir;
    Node node{node_config(dir)};
    ClusterConfig cluster_config;
    cluster_config.listen_host = "127.0.0.1";
    cluster_config.listen_port = 46540;
    cluster_config.port_range = 1;
    Cluster cluster{zetta::new_uuid(), cluster_config};
    ASSERT_TRUE(node.start());
    cluster.start();
    node.attach_cluster(cluster, /*worker_count=*/2);
    ASSERT_NE(node.job_coordinator(), nullptr);

    QueryService svc{node, &cluster};

    // Before any round, the coordinator is attached but idle.
    {
        auto before = svc.run("SELECT * FROM system.collective_rounds;", "");
        ASSERT_TRUE(before.ok()) << before.error;
        ASSERT_EQ(before.rows.size(), 1u);
        EXPECT_TRUE(before.rows[0]["attached"].get<bool>());
        EXPECT_EQ(before.rows[0]["participants"].get<int>(), 1);
        EXPECT_EQ(before.rows[0]["rounds_completed"].get<std::uint64_t>(), 0u);
    }

    // Run a job that performs two collective rounds (an allreduce + a barrier).
    auto job = node.create_job(JobKind::Ddf);
    const std::vector<long> shard = {2, 3, 5};
    const JobStatus status = node.job_coordinator()->run(
        *job, [&](JobRound& round) -> JobCoordinator::Outcome {
            const auto values =
                round.parallel_map(shard, [](long value) { return value; });
            const double partial =
                static_cast<double>(std::accumulate(values.begin(), values.end(), 0L));
            const auto total =
                round.allreduce("instr-sum", {partial}, CollectiveOp::Sum, 2s);
            if (!total) return {.ok = false, .detail = "allreduce failed"};
            if (!round.barrier("instr-barrier", 2s)) {
                return {.ok = false, .detail = "barrier failed"};
            }
            return {.ok = true, .detail = "ok"};
        });
    ASSERT_EQ(status, JobStatus::Succeeded) << job->detail();

    auto after = svc.run("SELECT * FROM system.collective_rounds;", "");
    ASSERT_TRUE(after.ok()) << after.error;
    ASSERT_EQ(after.rows.size(), 1u);
    const auto& row = after.rows[0];
    EXPECT_EQ(row["node"].get<std::string>(), node.node_id());
    EXPECT_TRUE(row["attached"].get<bool>());
    EXPECT_EQ(row["participants"].get<int>(), 1);
    // The two rounds the job body ran both completed.
    EXPECT_GE(row["rounds_completed"].get<std::uint64_t>(), 2u);

    node.stop();
    cluster.stop();
}
