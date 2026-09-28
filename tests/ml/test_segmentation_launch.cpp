#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/api/background.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/compute/coordinated_launch.hpp"
#include "zetta/data/add_dataset.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/dataset_partition.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/segmentation.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/uuid.hpp"

namespace {
using namespace std::chrono_literals;
using Json = nlohmann::json;

struct SegmentationMember {
    zetta::test::TempDir directory;
    std::string id = zetta::util::new_uuid();
    zetta::cluster::ReplicationManager replication{id};
    zetta::Node node{zetta::NodeConfig{directory.path().string(), false},
                     &replication};
    zetta::data::ReplicatedBlobStore blobs{directory.path(), replication};
    zetta::cluster::Cluster cluster;

    SegmentationMember(std::uint16_t port,
                       const std::vector<std::string> &seeds)
        : cluster(id, config(port, seeds), &replication, &blobs) {
        node.start();
        cluster.start();
        replication.set("node:" + id,
                        Json{{"address", cluster.listen_address()}});
        node.attach_cluster(cluster, 2);
    }

    ~SegmentationMember() {
        node.stop();
    }

    static zetta::cluster::ClusterConfig
    config(std::uint16_t port, const std::vector<std::string> &seeds) {
        zetta::cluster::ClusterConfig value;
        value.listen_host = "127.0.0.1";
        value.listen_port = port;
        value.port_range = 4;
        value.seeds = seeds;
        return value;
    }
};

bool table_ready(SegmentationMember &member, const std::string &table_id) {
    auto database = member.node.find_database("crm");
    auto table = database ? database->find_table("customers") : nullptr;
    if (!table || table->id() != table_id || !table->partition_manifest()) {
        return false;
    }
    std::size_t count = 0;
    for (const auto &partition : member.node.dataset_partitions()) {
        if (partition->table() != table_id) {
            continue;
        }
        ++count;
        const auto &owners = partition->owner_nodes();
        if (std::find(owners.begin(), owners.end(), member.id) !=
                owners.end() &&
            !member.blobs.read_blob(partition->blob_ref())) {
            return false;
        }
    }
    return count == 3;
}

template <class Predicate>
bool wait_for(const std::vector<std::unique_ptr<SegmentationMember>> &members,
              Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 45s;
    while (std::chrono::steady_clock::now() < deadline) {
        for (const auto &member : members) {
            member->node.tick();
        }
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return false;
}
} // namespace

TEST(SegmentationMethodology, OffRootSubmissionLaunchesThreeOwnedPartitions) {
    std::vector<std::unique_ptr<SegmentationMember>> members;
    std::vector<std::string> seeds;
    for (int index = 0; index < 3; ++index) {
        members.push_back(
            std::make_unique<SegmentationMember>(47820 + index * 4, seeds));
        seeds.push_back(members.back()->cluster.listen_address());
    }
    ASSERT_TRUE(wait_for(members, [&] {
        return std::all_of(members.begin(), members.end(),
                           [](const auto &member) {
                               return member->cluster.peer_count() == 2;
                           });
    }));
    std::sort(members.begin(), members.end(),
              [](const auto &left, const auto &right) {
                  return left->id < right->id;
              });
    auto &writer = *members.back();
    std::vector<Json> rows;
    std::vector<zetta::ml::SegmentationRow> oracle_rows;
    for (int group = 0; group < 3; ++group) {
        for (int row = 0; row < 20; ++row) {
            Json values = {{"spend", group * 100.0 + row * 0.01},
                           {"tenure", 80.0 - group * 30.0 + row * 0.01}};
            const auto entity = group * 20 + row + 1;
            oracle_rows.push_back({entity, values});
            values["customer_id"] = entity;
            rows.push_back(values);
        }
    }
    zetta::data::AddDatasetOptions landing;
    landing.database = "crm";
    landing.table_name = "customers";
    landing.partition_count = 3;
    landing.partition_only = true;
    landing.replication_factor = 1;
    for (const auto &member : members) {
        landing.members.push_back(member->id);
    }
    const auto added =
        zetta::data::add_dataset_rows(writer.node, rows, landing);
    ASSERT_TRUE(added.ok()) << added.error;
    ASSERT_TRUE(wait_for(members, [&] {
        return std::all_of(members.begin(), members.end(),
                           [&](const auto &member) {
                               return table_ready(*member, added.table->id());
                           });
    }));
    zetta::ml::SegmentationParams params;
    params.minimum_k = 3;
    params.maximum_k = 3;
    params.restarts = 2;
    params.feature_columns = {"spend", "tenure"};
    const auto oracle = zetta::ml::train_segmentation(oracle_rows, params);
    ASSERT_TRUE(oracle.ok()) << oracle.status.message;

    auto project = writer.node.create_project("distributed segments");
    ASSERT_TRUE(project);
    zetta::ml::ShapeRequest shape;
    shape.hints = {{"source", "crm"},
                   {"table", "customers"},
                   {"minimum_k", 3},
                   {"maximum_k", 3},
                   {"restarts", 2}};
    ASSERT_TRUE(project->create_graph(
        "segments", zetta::ml::find_methodology("M-3")->build_shape(shape)));
    zetta::api::Registry registry;
    zetta::api::register_core_capabilities(registry);
    zetta::api::BackgroundActions background;
    zetta::api::ApiContext context{writer.node,
                                   &writer.cluster,
                                   {},
                                   zetta::observability::make_root_trace(),
                                   &writer.node.span_recorder(),
                                   &background};

    std::vector<std::jthread> peers;
    for (std::size_t index = 0; index < 2; ++index) {
        auto *member = members[index].get();
        peers.emplace_back([member](std::stop_token stop) {
            while (!stop.stop_requested()) {
                member->node.tick();
                std::this_thread::sleep_for(5ms);
            }
        });
    }
    const auto submitted = registry.call(
        "run", {{"project", project->id()}, {"graph", "segments"}}, context);
    ASSERT_TRUE(submitted.ok) << submitted.error.message;
    const auto run = writer.node.find_run(submitted.value.at("run_id"));
    ASSERT_TRUE(run);
    const auto deadline = std::chrono::steady_clock::now() + 90s;
    while (std::chrono::steady_clock::now() < deadline &&
           run->status() == zetta::pipeline::RunStatus::Running) {
        writer.node.tick();
        background.prune();
        std::this_thread::sleep_for(5ms);
    }
    for (auto &peer : peers) {
        peer.request_stop();
    }
    peers.clear();
    ASSERT_EQ(run->status(), zetta::pipeline::RunStatus::Succeeded)
        << run->metadata().dump();
    const auto report =
        registry.call("get_run_report", {{"run_id", run->id()}}, context);
    ASSERT_TRUE(report.ok) << report.error.message;
    EXPECT_EQ(report.value.at("dataset").at("read_rows"), 60);
    EXPECT_EQ(report.value.at("dataset").at("clustered_rows"), 60);
    EXPECT_EQ(report.value.at("model").at("metrics").at("chosen_k"), 3);
    EXPECT_NEAR(
        report.value.at("model").at("metrics").at("silhouette").get<double>(),
        oracle.metrics.silhouette, 1e-9);
    EXPECT_EQ(report.value.at("output_tables").at(0).at("rows"), 60);
    EXPECT_EQ(report.value.at("segments").size(), 3u);
    for (const auto &member : members) {
        EXPECT_FALSE(member->node.jobs().empty());
    }
}

TEST(SegmentationMethodology, RefusesMalformedPeerPayloadBeforeReadingData) {
    SegmentationMember member(47840, {});
    zetta::compute::CoordinatedLaunchRequest request;
    request.id = zetta::util::new_uuid();
    request.kind = "segmentation";
    request.initiator = member.id;
    request.participants = {member.id};
    request.payload = {{"config", {{"seed", "not an integer"}}}};
    zetta::cluster::Signal signal;
    signal.kind = zetta::cluster::SignalKind::Task;
    signal.method = "run.launch";
    signal.from = member.id;
    signal.payload = zetta::compute::encode_coordinated_launch(request);
    member.node.coordinated_launch()->on_task(signal);
    EXPECT_EQ(member.node.coordinated_launch()->drain(), 1u);
    EXPECT_TRUE(member.node.jobs().empty());
    EXPECT_TRUE(member.node.runs().empty());
    bool refused = false;
    for (const auto &span : member.node.span_recorder().snapshot()) {
        if (span.name == "run.launch:segmentation") {
            refused = !span.ok && span.error.find("segmentation launch:") !=
                                      std::string::npos;
        }
    }
    EXPECT_TRUE(refused);
}
