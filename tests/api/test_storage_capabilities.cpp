// storage_usage on the control surface (docs/data/storage-census.md, lake#658): a node answers
// its disk by kind, through the registry like every capability, and a byte added to a bucket
// shows in that bucket.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
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
