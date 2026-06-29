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
#include "zetta/dataset_partition.hpp"
#include "zetta/large_object.hpp"
#include "zetta/node.hpp"
#include "zetta/replicated_blob_store.hpp"
#include "zetta/replication.hpp"
#include "zetta/sha256.hpp"
#include "zetta/uuid.hpp"

using zetta::Cluster;
using zetta::ClusterConfig;
using zetta::DatasetPartitionValue;
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

// ---- partition-aware blob placement (issue #515) ---------------------------

namespace {

bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// Seed a large-object's metadata + a dataset partition whose body it is, with the
// given owner set, into a (replicate-to-all) document. `blob_ref` is recorded in
// the `lo:<uuid>` catalog-key form unless `bare_ref` is set.
void seed_partition_blob(ReplicationManager& replication,
                         const std::string& lo_id, const std::string& db_id,
                         const std::string& body,
                         const std::vector<std::string>& owner_nodes,
                         bool bare_ref = false) {
    replication.set("lo:" + lo_id, nlohmann::json{{"db", db_id},
                                                  {"name", "part.bin"},
                                                  {"size", body.size()},
                                                  {"sha256", sha256_hex(body)}});
    DatasetPartitionValue partition;
    partition.table = "tbl:" + zetta::new_uuid();
    partition.blob_ref = bare_ref ? lo_id : ("lo:" + lo_id);
    partition.row_range = {0, 100};
    partition.owner_nodes = owner_nodes;
    partition.created = "2026-06-29T00:00:00Z";
    replication.set("part:" + zetta::new_uuid(), zetta::to_json(partition));
}

}  // namespace

TEST(ReplicatedBlobStore, PartitionBodyFetchedAndHeldOnlyByOwners) {
    // A partition's body is fetched + stored only by its owner node(s); a
    // non-owner neither lists it as missing nor accepts a pushed copy. Both
    // stores see the same replicate-to-all catalog (so seed both documents).
    TempDir owner_dir, other_dir;
    const std::string owner_id = zetta::new_uuid();
    const std::string other_id = zetta::new_uuid();
    const std::string lo_id = zetta::new_uuid();
    const std::string db_id = zetta::new_uuid();
    const std::string body = sample_payload();

    ReplicationManager repl_owner{owner_id};
    ReplicationManager repl_other{other_id};
    seed_partition_blob(repl_owner, lo_id, db_id, body, {owner_id});
    seed_partition_blob(repl_other, lo_id, db_id, body, {owner_id});
    ReplicatedBlobStore blobs_owner{owner_dir.path(), repl_owner};
    ReplicatedBlobStore blobs_other{other_dir.path(), repl_other};

    // Only the owner would fetch the body.
    EXPECT_TRUE(contains(blobs_owner.missing_blobs(), lo_id));
    EXPECT_FALSE(contains(blobs_other.missing_blobs(), lo_id));

    // A non-owner refuses the body even on an (unsolicited) push; the owner
    // accepts it and then no longer reports it missing.
    EXPECT_FALSE(blobs_other.store_blob(lo_id, body));
    EXPECT_FALSE(blobs_other.read_blob(lo_id).has_value());
    EXPECT_TRUE(blobs_owner.store_blob(lo_id, body));
    EXPECT_EQ(blobs_owner.read_blob(lo_id).value_or(""), body);
    EXPECT_FALSE(contains(blobs_owner.missing_blobs(), lo_id));
}

TEST(ReplicatedBlobStore, ReplicationFactorPlacesBodyOnExactlyRfNodes) {
    // With owner_nodes of size rf, exactly rf nodes fetch + hold the body.
    TempDir dirs[3];
    const std::string ids[3] = {zetta::new_uuid(), zetta::new_uuid(),
                                zetta::new_uuid()};
    const std::string lo_id = zetta::new_uuid();
    const std::string db_id = zetta::new_uuid();
    const std::string body = "partition body, rf=2";
    const std::vector<std::string> owners = {ids[0], ids[1]};  // rf = 2

    int fetchers = 0;
    for (int node = 0; node < 3; ++node) {
        ReplicationManager replication{ids[node]};
        seed_partition_blob(replication, lo_id, db_id, body, owners);
        ReplicatedBlobStore blobs{dirs[node].path(), replication};
        const bool would_fetch = contains(blobs.missing_blobs(), lo_id);
        const bool is_owner = contains(owners, ids[node]);
        EXPECT_EQ(would_fetch, is_owner);
        EXPECT_EQ(blobs.store_blob(lo_id, body), is_owner);
        if (would_fetch) ++fetchers;
    }
    EXPECT_EQ(fetchers, 2);  // exactly rf nodes hold the body
}

TEST(ReplicatedBlobStore, UnplacedPartitionBodyReplicatesToAll) {
    // A partition with no recorded owners (not yet placed) must not be stranded:
    // its body falls back to replicate-to-all so any node still fetches it.
    TempDir dir;
    ReplicationManager replication{zetta::new_uuid()};
    const std::string lo_id = zetta::new_uuid();
    const std::string body = "unplaced partition body";
    seed_partition_blob(replication, lo_id, zetta::new_uuid(), body,
                        /*owner_nodes=*/{});
    ReplicatedBlobStore blobs{dir.path(), replication};
    EXPECT_TRUE(contains(blobs.missing_blobs(), lo_id));
    EXPECT_TRUE(blobs.store_blob(lo_id, body));
}

TEST(ReplicatedBlobStore, OwnerCheckToleratesBareBlobRef) {
    // blob_ref may be recorded as a bare uuid (no "lo:" prefix); placement must
    // still resolve it to the right owner set.
    TempDir owner_dir, other_dir;
    const std::string owner_id = zetta::new_uuid();
    const std::string other_id = zetta::new_uuid();
    const std::string lo_id = zetta::new_uuid();
    const std::string db_id = zetta::new_uuid();
    const std::string body = "bare-ref partition body";

    ReplicationManager repl_owner{owner_id};
    ReplicationManager repl_other{other_id};
    seed_partition_blob(repl_owner, lo_id, db_id, body, {owner_id},
                        /*bare_ref=*/true);
    seed_partition_blob(repl_other, lo_id, db_id, body, {owner_id},
                        /*bare_ref=*/true);
    ReplicatedBlobStore blobs_owner{owner_dir.path(), repl_owner};
    ReplicatedBlobStore blobs_other{other_dir.path(), repl_other};

    EXPECT_TRUE(contains(blobs_owner.missing_blobs(), lo_id));
    EXPECT_FALSE(contains(blobs_other.missing_blobs(), lo_id));
}
