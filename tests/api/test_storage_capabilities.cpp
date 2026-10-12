// storage_usage on the control surface (docs/data/storage-census.md, lake#658): a node answers
// its disk by kind, through the registry like every capability, and a byte added to a bucket
// shows in that bucket.
#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <thread>
#include <vector>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/storage_census.hpp"
#include "zetta/node.hpp"
#include "zetta/observability/trace.hpp"
#include "zetta/util/uuid.hpp"

TEST(StorageCapabilities, ANodeAnswersItsDiskByKind) {
    zetta::test::TempDir dir;
    zetta::cluster::ReplicationManager replication{zetta::util::new_uuid()};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication};
    ASSERT_TRUE(node.start());
    node.ensure_builtin_roles();
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    auto trace = zetta::observability::make_root_trace();
    const auto call = [&] {
        zetta::api::ApiContext context{node, nullptr, "", trace, &node.span_recorder()};
        return registry.call("storage_usage", nlohmann::json::object(), context);
    };

    const auto before = call();
    ASSERT_TRUE(before.ok) << before.error.message;
    EXPECT_EQ(before.value["scope"], "node");
    EXPECT_EQ(before.value["node_id"], node.node_id());
    for (const std::string_view kind : zetta::data::storage_kinds()) {
        EXPECT_TRUE(before.value["kinds"].contains(std::string(kind))) << kind;
    }

    {
        std::filesystem::create_directories(dir.path() / "objects" / "photos");
        std::ofstream(dir.path() / "objects" / "photos" / "extra", std::ios::binary)
            << std::string(4096, 'p');
    }
    const auto after = call();
    ASSERT_TRUE(after.ok) << after.error.message;
    EXPECT_EQ(after.value["kinds"]["buckets"].get<std::uint64_t>(),
              before.value["kinds"]["buckets"].get<std::uint64_t>() + 4096);
    bool listed = false;
    for (const auto& bucket : after.value["buckets"]) {
        listed = listed || (bucket["bucket"] == "photos" && bucket["bytes"].get<std::uint64_t>() >= 4096);
    }
    EXPECT_TRUE(listed) << after.value["buckets"].dump();
    EXPECT_GE(after.value["total_bytes"].get<std::uint64_t>(),
              before.value["total_bytes"].get<std::uint64_t>() + 4096);
    EXPECT_TRUE(registry.find("storage_usage")->blocks) << "walks the disk off the owner loop";
    node.stop();
}

// zetta#1259: `scope: "cluster"` answers a whole cluster's disk. Three nodes on loopback, each
// measuring its own census on its pool and carrying it in its telemetry: the answer on node-a
// lists all three, sums their kinds (a byte added on node-b's disk shows in the total) and says
// the bytes are physical.
TEST(StorageCapabilities, AClusterAnswersEveryMembersDiskSummed) {
    struct Member {
        Member(std::string id, std::uint16_t port, std::vector<std::string> seeds)
            : node(config(dir)), cluster(std::move(id), cluster_config(port, std::move(seeds))) {
            node.start();
        }
        ~Member() {
            node.stop();
            cluster.stop();
        }
        static zetta::NodeConfig config(const zetta::test::TempDir& dir) {
            zetta::NodeConfig node_config;
            node_config.data_dir = dir.path().string();
            return node_config;
        }
        static zetta::cluster::ClusterConfig cluster_config(std::uint16_t port,
                                                            std::vector<std::string> seeds) {
            zetta::cluster::ClusterConfig cluster_config;
            cluster_config.listen_host = "127.0.0.1";
            cluster_config.listen_port = port;
            cluster_config.port_range = 1;
            cluster_config.seeds = std::move(seeds);
            return cluster_config;
        }
        zetta::test::TempDir dir;
        zetta::Node node;
        zetta::cluster::Cluster cluster;
    };
    constexpr std::uint16_t kBasePort = 46870;
    const std::vector<std::string> ids{"node-a", "node-b", "node-c"};
    std::vector<std::unique_ptr<Member>> members;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        std::vector<std::string> seeds;
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            seeds.push_back("127.0.0.1:" + std::to_string(kBasePort + earlier));
        }
        members.push_back(std::make_unique<Member>(
            ids[index], static_cast<std::uint16_t>(kBasePort + index), seeds));
    }
    {
        const auto bucket = members[1]->dir.path() / "objects" / "photos";
        std::filesystem::create_directories(bucket);
        std::ofstream(bucket / "extra", std::ios::binary) << std::string(4096, 'p');
    }
    for (auto& member : members) member->cluster.start();
    for (auto& member : members) member->node.attach_cluster(member->cluster, 1);

    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    auto trace = zetta::observability::make_root_trace();
    zetta::Node& first = members[0]->node;
    const auto call = [&] {
        zetta::api::ApiContext context{first, nullptr, "", trace, &first.span_recorder()};
        return registry.call("storage_usage", {{"scope", "cluster"}}, context);
    };
    nlohmann::json answer;
    for (int attempt = 0; attempt < 400; ++attempt) {
        for (auto& member : members) member->node.tick();
        const auto result = call();
        ASSERT_TRUE(result.ok) << result.error.message;
        answer = result.value;
        if (answer["nodes"].size() == ids.size()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_EQ(answer["nodes"].size(), ids.size()) << answer.dump();
    EXPECT_EQ(answer["scope"], "cluster");
    EXPECT_EQ(answer["bytes"], "physical");
    EXPECT_EQ(answer["node_id"], first.node_id());
    EXPECT_TRUE(answer["missing"].empty()) << answer.dump();
    std::uint64_t sum = 0;
    std::uint64_t buckets = 0;
    int local = 0;
    for (const auto& member : answer["nodes"]) {
        sum += member["total_bytes"].get<std::uint64_t>();
        buckets += member["kinds"]["buckets"].get<std::uint64_t>();
        local += member["local"].get<bool>() ? 1 : 0;
        EXPECT_FALSE(member["measured_at"].get<std::string>().empty());
    }
    EXPECT_EQ(local, 1);
    EXPECT_EQ(answer["total_bytes"].get<std::uint64_t>(), sum);
    EXPECT_EQ(answer["kinds"]["buckets"].get<std::uint64_t>(), buckets);
    EXPECT_GE(buckets, 4096u);
    bool listed = false;
    for (const auto& bucket : answer["buckets"]) {
        listed = listed || (bucket["bucket"] == "photos" && bucket["bytes"].get<std::uint64_t>() >= 4096);
    }
    EXPECT_TRUE(listed) << answer.dump();

    // The default stays the node that answers.
    zetta::api::ApiContext context{first, nullptr, "", trace, &first.span_recorder()};
    const auto node_scope = registry.call("storage_usage", nlohmann::json::object(), context);
    ASSERT_TRUE(node_scope.ok);
    EXPECT_EQ(node_scope.value["scope"], "node");
    EXPECT_FALSE(node_scope.value.contains("nodes"));
}
