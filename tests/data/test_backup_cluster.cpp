// Backup snapshots on a multi-node cluster (zetta#1520, docs/data/backup-snapshot.md):
// the snapshot's pause covers every member's record and object writes, refuses
// rather than blocks, is released on every path and lapses by itself; a member
// holding another snapshot refuses the pause and leaves nothing frozen; a pause
// that lapsed during the capture refuses the cut; and a partition body held only
// by another member is fetched from it into the archive.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/backup_snapshot.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using nlohmann::json;
using zetta::data::BackupFreeze;
using zetta::data::BackupOutcome;
using zetta::data::BackupStatus;
using zetta::data::RecordStatus;
using zetta::data::RecordWrite;

// One node of a real loopback cluster, its coordinator attached (so the
// backup peer is on the signal plane).
struct Member {
    Member(std::uint16_t port, std::vector<std::string> seeds)
        : node_id(zetta::util::new_uuid()),
          replication(node_id),
          node(zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication),
          cluster(node_id, config(port, std::move(seeds)), &replication) {
        node.start();
    }
    ~Member() {
        node.stop();
        cluster.stop();
    }

    static zetta::cluster::ClusterConfig config(std::uint16_t port, std::vector<std::string> seeds) {
        zetta::cluster::ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = std::move(seeds);
        config.reconnect_interval_seconds = 1;
        return config;
    }

    zetta::data::RecordStore& records() { return *node.record_store(); }
    zetta::data::ObjectStore& objects() { return *node.object_store(); }

    zetta::data::RecordOutcome put(const std::string& key) {
        RecordWrite write;
        write.collection = "lake_events";
        write.key = key;
        write.value = "c2VhbGVk";
        return records().write(write);
    }

    zetta::test::TempDir dir;
    std::string node_id;
    zetta::cluster::ReplicationManager replication;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
};

struct Pair {
    std::unique_ptr<Member> a;
    std::unique_ptr<Member> b;

    explicit Pair(std::uint16_t port) {
        a = std::make_unique<Member>(port, std::vector<std::string>{});
        b = std::make_unique<Member>(port + 1,
                                     std::vector<std::string>{"127.0.0.1:" + std::to_string(port)});
        a->cluster.start();
        b->cluster.start();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        while (std::chrono::steady_clock::now() < deadline &&
               (a->cluster.peer_count() != 1 || b->cluster.peer_count() != 1)) {
            a->node.tick();
            b->node.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        EXPECT_EQ(a->cluster.peer_count(), 1u);
        a->node.attach_cluster(a->cluster, /*worker_count=*/1);
        b->node.attach_cluster(b->cluster, /*worker_count=*/1);
        zetta::data::RecordCollection collection;
        collection.name = "lake_events";
        EXPECT_TRUE(a->records().create_collection(collection).ok());
        settle([&] { return b->records().find_collection("lake_events").has_value(); });
    }

    template <class Predicate>
    bool settle(Predicate done) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            a->node.tick();
            b->node.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        return true;
    }
};

}  // namespace

TEST(BackupCluster, ThePauseCoversEveryMembersWritesAndIsReleased) {
    Pair pair(48871);
    std::optional<BackupFreeze> freeze;
    const BackupOutcome paused = BackupFreeze::begin(
        pair.a->records(), pair.a->objects(), &pair.a->cluster, "snapshot-1", freeze);
    ASSERT_TRUE(paused.ok()) << paused.message;
    ASSERT_TRUE(freeze);
    EXPECT_EQ(freeze->members(), std::vector<std::string>{pair.b->node_id});
    // Writes entering either member are refused, never blocked.
    for (int i = 0; i < 4; ++i) {
        const auto started = std::chrono::steady_clock::now();
        EXPECT_EQ(pair.b->put("k" + std::to_string(i)).status, RecordStatus::Unavailable);
        EXPECT_EQ(pair.a->put("k" + std::to_string(i)).status, RecordStatus::Unavailable);
        EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
    }
    EXPECT_TRUE(pair.b->records().write_gate().frozen());
    EXPECT_TRUE(pair.b->objects().write_gate().frozen());
    bool drained = false;
    ASSERT_TRUE(freeze->members_drained(drained).ok());
    EXPECT_TRUE(drained);
    EXPECT_TRUE(freeze->held().ok());

    freeze.reset();  // the thaw is an event: it arrives shortly
    EXPECT_FALSE(pair.a->records().write_gate().frozen());
    EXPECT_TRUE(pair.settle([&] { return !pair.b->records().write_gate().frozen(); }));
    EXPECT_TRUE(pair.b->put("after").ok());
    EXPECT_TRUE(pair.a->put("after-a").ok());
}

TEST(BackupCluster, AMemberHoldingAnotherSnapshotRefusesAndNothingStaysFrozen) {
    Pair pair(48873);
    const auto until = zetta::data::WriteGate::Clock::now() + std::chrono::seconds(60);
    ASSERT_TRUE(pair.b->records().write_gate().freeze("other", until));
    std::optional<BackupFreeze> freeze;
    const BackupOutcome refused = BackupFreeze::begin(
        pair.a->records(), pair.a->objects(), &pair.a->cluster, "snapshot-2", freeze);
    EXPECT_EQ(refused.status, BackupStatus::Busy) << refused.message;
    EXPECT_FALSE(freeze);
    EXPECT_FALSE(pair.a->records().write_gate().frozen());
    EXPECT_FALSE(pair.a->objects().write_gate().frozen());
    // b's objects were not left frozen by the half-made freeze, and its records
    // are still the other snapshot's.
    EXPECT_FALSE(pair.b->objects().write_gate().frozen());
    EXPECT_TRUE(pair.b->records().write_gate().held("other"));
    pair.b->records().write_gate().thaw("other");
    EXPECT_TRUE(pair.a->put("free").ok());
}

TEST(BackupCluster, APauseThatLapsedDuringTheCaptureRefusesTheCut) {
    Pair pair(48875);
    std::optional<BackupFreeze> freeze;
    ASSERT_TRUE(BackupFreeze::begin(pair.a->records(), pair.a->objects(), &pair.a->cluster,
                                    "snapshot-3", freeze)
                    .ok());
    // As if b's lease ran out: its gate is no longer this snapshot's.
    pair.b->records().write_gate().thaw("snapshot-3");
    const BackupOutcome lapsed = freeze->held();
    EXPECT_EQ(lapsed.status, BackupStatus::Busy);
    EXPECT_NE(lapsed.message.find("lapsed"), std::string::npos) << lapsed.message;
}

TEST(BackupCluster, APartitionBodyHeldOnlyByAnotherMemberIsFetched) {
    Pair pair(48877);
    const std::string body(5 * 1024 * 1024 + 17, 'q');  // more than one slice
    pair.a->replication.set("db:d1", json{{"name", "lake"}});
    pair.a->replication.set("tbl:t1",
                            json{{"db", "d1"}, {"name", "facts"}, {"partition_only", true}});
    pair.a->replication.set("mfst:t1", json{{"table", "t1"}, {"partitions", 1}, {"rows", 2}});
    pair.a->replication.set("lo:b1", json{{"db", "d1"},
                                          {"name", "facts-0"},
                                          {"size", body.size()},
                                          {"sha256", zetta::util::sha256_hex(body)}});
    pair.a->replication.set("part:p1", json{{"table", "t1"},
                                            {"blob_ref", "b1"},
                                            {"row_range", {{"start", 0}, {"end", 2}}},
                                            {"owner_nodes", {pair.b->node_id}}});
    ASSERT_TRUE(pair.settle([&] { return pair.b->replication.shared_snapshot()->contains("part:p1"); }));
    zetta::data::ReplicatedBlobStore b_bodies(pair.b->dir.path(), pair.b->replication);
    ASSERT_TRUE(b_bodies.store_blob("b1", body));

    zetta::data::ReplicatedBlobStore a_bodies(pair.a->dir.path(), pair.a->replication);
    ASSERT_FALSE(a_bodies.read_blob("b1"));
    zetta::data::BackupStores stores{pair.a->replication, pair.a->records(), pair.a->objects(),
                                     a_bodies,            pair.a->node_id,   &pair.a->cluster};
    std::optional<BackupFreeze> freeze;
    ASSERT_TRUE(BackupFreeze::begin(pair.a->records(), pair.a->objects(), &pair.a->cluster,
                                    "snapshot-4", freeze)
                    .ok());
    zetta::data::SnapshotWriter writer(stores, pair.a->dir.path() / "backups", zetta::util::new_uuid());
    BackupOutcome outcome = writer.capture(pair.a->replication.shared_snapshot());
    ASSERT_TRUE(outcome.ok()) << outcome.message;
    ASSERT_TRUE(freeze->held().ok());
    freeze.reset();
    outcome = writer.finish();
    ASSERT_TRUE(outcome.ok()) << outcome.message;
    EXPECT_EQ(outcome.summary.at("bodies"), 1);
    EXPECT_FALSE(outcome.summary.contains("bodies:missing"));
    EXPECT_EQ(outcome.summary.at("table:lake.facts"), 2);

    // A body nobody gives whole (b no longer holds it): refused, never short.
    std::error_code ec;
    std::filesystem::remove_all(pair.b->dir.path() / "d1", ec);
    zetta::data::SnapshotWriter again(stores, pair.a->dir.path() / "backups", zetta::util::new_uuid());
    ASSERT_TRUE(again.capture(pair.a->replication.shared_snapshot()).ok());
    const BackupOutcome refused = again.finish();
    EXPECT_EQ(refused.status, BackupStatus::Conflict) << refused.message;
}

TEST(BackupCluster, TheGateLapsesByItself) {
    zetta::data::WriteGate gate;
    ASSERT_TRUE(gate.freeze("s", zetta::data::WriteGate::Clock::now() + std::chrono::milliseconds(50)));
    EXPECT_FALSE(gate.enter());
    EXPECT_FALSE(gate.freeze("other", zetta::data::WriteGate::Clock::now() + std::chrono::seconds(1)));
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_TRUE(gate.enter());
    EXPECT_FALSE(gate.held("s"));
    EXPECT_TRUE(gate.freeze("other", zetta::data::WriteGate::Clock::now() + std::chrono::seconds(1)));
    gate.thaw("s");  // not the holder: nothing changes
    EXPECT_TRUE(gate.held("other"));
    gate.thaw("other");
    EXPECT_TRUE(gate.enter());
}
