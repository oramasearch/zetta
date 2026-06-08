#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster.hpp"
#include "zetta/database.hpp"
#include "zetta/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/replicated_blob_store.hpp"
#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::LargeObject;
using zetta::Node;
using zetta::NodeConfig;
using zetta::ReplicatedBlobStore;
using zetta::ReplicationManager;
using zetta::sha256_hex;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// A binary payload with non-trivial bytes (exercises binary-safe transfer).
std::string sample_payload() {
    std::string data;
    data.reserve(40000);
    for (int i = 0; i < 40000; ++i) {
        data.push_back(static_cast<char>((i * 37 + 11) & 0xff));
    }
    return data;
}

}  // namespace

TEST(LargeObjectReplication, BodyTransfersAndVerifiesAcrossNodes) {
    TempDir dir_a, dir_b, src_dir;
    const std::string payload = sample_payload();
    const std::filesystem::path src = src_dir.path() / "photo.bin";
    {
        std::ofstream out(src, std::ios::binary);
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
    }

    const std::string id_a = zetta::new_uuid();
    const std::string id_b = zetta::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    ReplicatedBlobStore blobs_a{dir_a.path(), repl_a};
    ReplicatedBlobStore blobs_b{dir_b.path(), repl_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    a.start();
    b.start();

    ClusterConfig cfg_a;
    cfg_a.listen_host = "127.0.0.1";
    cfg_a.listen_port = 43400;
    cfg_a.port_range = 1;
    Cluster cluster_a{id_a, cfg_a, &repl_a, &blobs_a};
    cluster_a.start();

    ClusterConfig cfg_b;
    cfg_b.listen_host = "127.0.0.1";
    cfg_b.listen_port = 43401;
    cfg_b.port_range = 1;
    cfg_b.seeds = {"127.0.0.1:43400"};
    Cluster cluster_b{id_b, cfg_b, &repl_b, &blobs_b};
    cluster_b.start();

    // Import a large object on node A only.
    auto media = a.create_database("media");
    ASSERT_TRUE(media);
    auto imported = media->large_objects().import_file(src);
    ASSERT_TRUE(imported.has_value());
    EXPECT_EQ(imported->size, payload.size());
    EXPECT_EQ(imported->sha256, sha256_hex(payload));
    const std::string lo_id = imported->id;

    // Node B should converge: the database, the LO metadata, AND the body.
    const auto b_has_body = [&] {
        auto db = b.find_database("media");
        if (!db) return false;
        const auto bytes = db->large_objects().read(lo_id);  // needs body present
        return bytes.has_value() && *bytes == payload;
    };
    bool synced = false;
    for (int attempt = 0; attempt < 600; ++attempt) {  // patient: body moves over TCP
        a.tick();
        b.tick();
        if (b_has_body()) {
            synced = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(synced);

    // Metadata replicated, body verified, and on B's disk.
    auto b_db = b.find_database("media");
    ASSERT_TRUE(b_db);
    const std::vector<LargeObject> objects = b_db->large_objects().list();
    ASSERT_EQ(objects.size(), 1u);
    EXPECT_EQ(objects[0].id, lo_id);
    EXPECT_EQ(objects[0].size, payload.size());
    EXPECT_EQ(objects[0].sha256, sha256_hex(payload));
    EXPECT_TRUE(objects[0].body_present);
    EXPECT_EQ(sha256_hex(*b_db->large_objects().read(lo_id)), sha256_hex(payload));

    cluster_b.stop();
    cluster_a.stop();
    a.stop();
    b.stop();
}

TEST(ReplicatedBlobStore, VerifiesHashAndRejectsMismatch) {
    TempDir dir;
    const std::string node_id = zetta::new_uuid();
    ReplicationManager replication{node_id};
    ReplicatedBlobStore blobs{dir.path(), replication};

    const std::string db_id = zetta::new_uuid();
    const std::string lo_id = zetta::new_uuid();
    const std::string body = "the real bytes";
    // Record metadata directly (key format mirrors src/core/catalog.hpp).
    replication.set("lo:" + lo_id,
                    nlohmann::json{{"db", db_id},
                                   {"name", "f.bin"},
                                   {"size", body.size()},
                                   {"sha256", sha256_hex(body)}});

    // Before transfer the body is missing.
    const std::vector<std::string> missing = blobs.missing_blobs();
    EXPECT_NE(std::find(missing.begin(), missing.end(), lo_id), missing.end());

    // Wrong bytes (and wrong length) are rejected; correct bytes are accepted.
    EXPECT_FALSE(blobs.store_blob(lo_id, "tampered"));
    EXPECT_FALSE(blobs.read_blob(lo_id).has_value());
    EXPECT_TRUE(blobs.store_blob(lo_id, body));
    ASSERT_TRUE(blobs.read_blob(lo_id).has_value());
    EXPECT_EQ(*blobs.read_blob(lo_id), body);
    EXPECT_TRUE(blobs.missing_blobs().empty());
}

TEST(ReplicatedBlobStore, AcceptsLegacyObjectWithEmptyHash) {
    // Objects imported before SHA-256 existed carry an empty hash; the body must
    // still transfer (verified by size only), not be rejected forever.
    TempDir dir;
    ReplicationManager replication{zetta::new_uuid()};
    ReplicatedBlobStore blobs{dir.path(), replication};
    const std::string lo_id = zetta::new_uuid();
    const std::string body = "legacy body with no recorded hash";
    replication.set("lo:" + lo_id, nlohmann::json{{"db", zetta::new_uuid()},
                                                  {"name", "legacy.bin"},
                                                  {"size", body.size()},
                                                  {"sha256", ""}});
    EXPECT_FALSE(blobs.store_blob(lo_id, "wrong length"));  // size still enforced
    EXPECT_TRUE(blobs.store_blob(lo_id, body));             // empty hash → accept
    EXPECT_EQ(blobs.read_blob(lo_id).value_or(""), body);
}
