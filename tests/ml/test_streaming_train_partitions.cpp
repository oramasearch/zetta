// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/compute/compute_job.hpp"
#include "zetta/compute/job_coordinator.hpp"
#include "zetta/ml/streaming.hpp"
#include "zetta/node.hpp"

namespace {

using namespace std::chrono_literals;

template <class Predicate>
bool wait_for(Predicate predicate) {
    for (int attempt = 0; attempt < 400; ++attempt) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(25ms);
    }
    return false;
}

struct StreamingMember {
    StreamingMember(std::string id, std::uint16_t port,
                    std::vector<std::string> seeds)
        : node(config(temp)),
          cluster(std::move(id), cluster_config(port, std::move(seeds))),
          coordinator(cluster, 2) {
        node.start();
    }

    static zetta::NodeConfig config(const zetta::test::TempDir& directory) {
        zetta::NodeConfig value;
        value.data_dir = directory.path().string();
        return value;
    }

    static zetta::cluster::ClusterConfig cluster_config(
        std::uint16_t port, std::vector<std::string> seeds) {
        zetta::cluster::ClusterConfig value;
        value.listen_host = "127.0.0.1";
        value.listen_port = port;
        value.port_range = 1;
        value.seeds = std::move(seeds);
        return value;
    }

    zetta::test::TempDir temp;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
    zetta::compute::JobCoordinator coordinator;
};

std::vector<zetta::ml::StreamingEvent> shard(std::size_t parity) {
    std::vector<zetta::ml::StreamingEvent> events;
    for (std::size_t index = parity; index < 200; index += 2) {
        const double signal = index % 4 < 2 ? 0.0 : 1.0;
        events.push_back({"event-" + std::to_string(index),
                          static_cast<std::int64_t>(index),
                          static_cast<double>(index),
                          {signal},
                          signal,
                          static_cast<double>(index) + 0.25,
                          index});
    }
    return events;
}

} // namespace

TEST(OnlineStreamingPartitions, AllreduceProducesOneModelAcrossMembers) {
    constexpr std::uint16_t kBasePort = 47720;
    std::vector<std::unique_ptr<StreamingMember>> members;
    members.push_back(
        std::make_unique<StreamingMember>("stream-a", kBasePort,
                                          std::vector<std::string>{}));
    members.push_back(std::make_unique<StreamingMember>(
        "stream-b", static_cast<std::uint16_t>(kBasePort + 1),
        std::vector<std::string>{"127.0.0.1:" +
                                 std::to_string(kBasePort)}));
    for (auto& member : members) {
        member->cluster.start();
    }
    ASSERT_TRUE(wait_for([&] {
        return members[0]->cluster.peer_count() == 1 &&
               members[1]->cluster.peer_count() == 1;
    }));
    for (auto& member : members) {
        member->coordinator.start();
    }

    zetta::ml::StreamingParams params;
    params.learning_rate = 0.2;
    params.window_duration = 0.1;
    params.allowed_lateness = 0.0;
    params.metric_window = 50;
    params.seed = 1060;
    const std::vector<std::vector<zetta::ml::StreamingEvent>> shards = {
        shard(0), shard(1)};
    std::vector<std::shared_ptr<zetta::compute::ComputeJob>> jobs;
    std::vector<zetta::ml::DistributedStreamingResult> results(2);
    std::vector<std::thread> workers;
    for (std::size_t index = 0; index < members.size(); ++index) {
        jobs.push_back(
            members[index]->node.create_job(zetta::compute::JobKind::Train));
        workers.emplace_back([&, index] {
            results[index] = zetta::ml::run_streaming_train_job(
                members[index]->coordinator, *jobs[index], shards[index],
                {"signal"}, params, {}, "streaming-partitions", 8s);
        });
    }
    for (std::thread& worker : workers) {
        worker.join();
    }

    ASSERT_TRUE(results[0].succeeded) << results[0].detail;
    ASSERT_TRUE(results[1].succeeded) << results[1].detail;
    EXPECT_EQ(results[0].training.model.to_json(),
              results[1].training.model.to_json());
    EXPECT_EQ(results[0].training.metrics.to_json(),
              results[1].training.metrics.to_json());
    EXPECT_EQ(results[0].training.metrics.events_read, 200u);
    EXPECT_EQ(results[0].training.metrics.events_scored, 200u);
    EXPECT_EQ(results[0].training.metrics.evaluated_labels, 200u);
    EXPECT_GT(results[0].training.metrics.accuracy, 0.9);
    EXPECT_NE(results[0].is_root, results[1].is_root);
    EXPECT_EQ(jobs[0]->status(), zetta::compute::JobStatus::Succeeded);
    EXPECT_EQ(jobs[1]->status(), zetta::compute::JobStatus::Succeeded);

    for (auto& member : members) {
        member->coordinator.stop();
        member->cluster.stop();
    }
}
