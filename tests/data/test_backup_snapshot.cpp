// Backup snapshots and their restore (zetta#1518, docs/data/backup-snapshot.md):
// a snapshot of every catalogue family it keeps, every record and every object
// version, restored into an empty node with the same summary; a snapshot taken
// under writes is one cut across the stores; an unfinished table is left out
// whole; a non-empty target, a damaged and a truncated archive are refused; and
// discard empties only a restored restore target that nothing has written since.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/backup_snapshot.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/data/replicated_blob_store.hpp"
#include "zetta/util/sha256.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using nlohmann::json;
using zetta::cluster::ReplicationManager;
using zetta::data::BackupFreeze;
using zetta::data::BackupOutcome;
using zetta::data::BackupSnapshots;
using zetta::data::BackupStatus;
using zetta::data::BackupStores;
using zetta::data::BackupSummary;
using zetta::data::DiscardRequest;
using zetta::data::ObjectBucket;
using zetta::data::ObjectReader;
using zetta::data::ObjectStatus;
using zetta::data::ObjectStore;
using zetta::data::ObjectWrite;
using zetta::data::RecordCollection;
using zetta::data::RecordStatus;
using zetta::data::RecordStore;
using zetta::data::RecordWrite;
using zetta::data::ReplicatedBlobStore;
using zetta::data::SnapshotRestore;
using zetta::data::SnapshotWriter;
using zetta::test::TempDir;

std::string pattern(std::size_t size, std::uint64_t seed) {
    std::string bytes(size, '\0');
    std::mt19937_64 random(seed);
    for (char& byte : bytes) byte = static_cast<char>(random() & 0xFF);
    return bytes;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// One node's stores, without a Node: the catalogue, the record log, the object
// logs and the bodies on disk.
struct Side {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore records{replication, replication.node_id(), dir.path() / "records"};
    ObjectStore objects{replication, replication.node_id(), dir.path() / "objects"};
    ReplicatedBlobStore bodies{dir.path(), replication};
    BackupStores stores{replication, records, objects, bodies, replication.node_id()};

    Side() {
        EXPECT_TRUE(records.open());
        EXPECT_TRUE(objects.open());
    }

    std::filesystem::path backups() const { return dir.path() / "backups"; }
    std::filesystem::path restore_record() const {
        return zetta::data::restore_record_path(dir.path());
    }

    void create_collection(const std::string& name) {
        RecordCollection collection;
        collection.name = name;
        ASSERT_TRUE(records.create_collection(collection).ok());
    }

    zetta::data::RecordOutcome put_record(const std::string& collection, const std::string& key) {
        RecordWrite write;
        write.collection = collection;
        write.key = key;
        write.value = "c2VhbGVk";
        return records.write(write);
    }

    void create_bucket(const std::string& name) {
        ObjectBucket bucket;
        bucket.name = name;
        ASSERT_TRUE(objects.create_bucket(bucket).ok());
    }

    std::optional<std::string> object(const std::string& bucket, const std::string& key) {
        std::shared_ptr<ObjectReader> reader;
        if (!objects.open_read(bucket, key, 0, std::nullopt, reader).ok()) return std::nullopt;
        std::string bytes;
        if (!reader->read(0, reader->length(), bytes)) return std::nullopt;
        return bytes;
    }

    // Take a snapshot the way the route does: freeze, capture, thaw, finish.
    BackupOutcome snapshot() {
        std::optional<BackupFreeze> freeze = BackupFreeze::begin(records, objects);
        if (!freeze) return BackupOutcome{BackupStatus::Busy, "frozen already"};
        while (!freeze->drained()) std::this_thread::yield();
        SnapshotWriter writer(stores, backups(), zetta::util::new_uuid());
        BackupOutcome captured = writer.capture(replication.shared_snapshot());
        freeze.reset();
        if (!captured.ok()) return captured;
        return writer.finish();
    }

    // Restore an archive fed `chunk` bytes at a time.
    BackupOutcome restore(const std::string& archive, std::size_t chunk = 777'777) {
        SnapshotRestore restore(stores, restore_record());
        BackupOutcome outcome = restore.begin();
        if (!outcome.ok()) return outcome;
        for (std::size_t at = 0; at < archive.size(); at += chunk) {
            outcome = restore.feed(std::string_view(archive).substr(at, chunk));
            if (!outcome.ok()) return outcome;
        }
        return restore.finish();
    }
};

// A table of 12 catalogue rows, a finished partition-only table of 5 rows with
// its body, an unfinished one, a bucket of three objects (one past a slice), a
// collection of 2,500 records, and an identity record and a membership entry
// that a snapshot leaves out.
void seed(Side& side) {
    const std::string node = side.replication.node_id();
    side.replication.set("node:" + node, json{{"address", ""}});
    side.replication.set("user:u1", json{{"name", "someone"}});
    side.replication.set("db:d1", json{{"name", "lake"}});
    side.replication.set("tbl:t1", json{{"db", "d1"}, {"name", "events"}});
    for (int row = 0; row < 12; ++row) {
        side.replication.set("row:r" + std::to_string(row),
                             json{{"tbl", "t1"}, {"data", {{"n", row}}}});
    }
    const std::string body = pattern(4096, 7);
    side.replication.set("tbl:t2", json{{"db", "d1"}, {"name", "facts"}, {"partition_only", true}});
    side.replication.set("mfst:t2", json{{"table", "t2"}, {"partitions", 1}, {"rows", 5}});
    side.replication.set("lo:b1", json{{"db", "d1"},
                                       {"name", "facts-0"},
                                       {"size", body.size()},
                                       {"sha256", zetta::util::sha256_hex(body)}});
    side.replication.set("part:p1", json{{"table", "t2"},
                                         {"blob_ref", "b1"},
                                         {"row_range", {{"start", 0}, {"end", 5}}},
                                         {"owner_nodes", {node}}});
    ASSERT_TRUE(side.bodies.store_blob("b1", body));
    // A load still running: no manifest.
    const std::string partial = pattern(512, 9);
    side.replication.set("tbl:t3", json{{"db", "d1"}, {"name", "loading"}, {"partition_only", true}});
    side.replication.set("lo:b2", json{{"db", "d1"},
                                       {"name", "loading-0"},
                                       {"size", partial.size()},
                                       {"sha256", zetta::util::sha256_hex(partial)}});
    side.replication.set("part:p2", json{{"table", "t3"},
                                         {"blob_ref", "b2"},
                                         {"row_range", {{"start", 0}, {"end", 3}}},
                                         {"owner_nodes", {node}}});
    ASSERT_TRUE(side.bodies.store_blob("b2", partial));

    side.create_bucket("drive");
    ASSERT_TRUE(side.objects.put("drive", "a", "alpha", ObjectWrite{}).ok());
    ASSERT_TRUE(side.objects.put("drive", "b/c", pattern(1000, 3), ObjectWrite{}).ok());
    ASSERT_TRUE(side.objects
                    .put("drive", "large", pattern(ObjectStore::kSliceBytes + 123'456, 5),
                         ObjectWrite{})
                    .ok());
    side.create_collection("lake_sessions");
    for (int record = 0; record < 2500; ++record) {
        ASSERT_TRUE(side.put_record("lake_sessions", "s" + std::to_string(record)).ok());
    }
}

}  // namespace

TEST(BackupSnapshot, RestoresIntoAnEmptyNodeWithTheSameSummary) {
    Side source;
    seed(source);
    const BackupOutcome taken = source.snapshot();
    ASSERT_TRUE(taken.ok()) << taken.message;
    EXPECT_EQ(taken.summary.at("table:lake.events"), 12);
    EXPECT_EQ(taken.summary.at("table:lake.facts"), 5);
    EXPECT_FALSE(taken.summary.contains("table:lake.loading"));  // left out whole
    EXPECT_EQ(taken.summary.at("bucket:drive"), 3);
    EXPECT_EQ(taken.summary.at("collection:lake_sessions"), 2500);
    EXPECT_EQ(taken.summary.at("bodies"), 1);
    EXPECT_FALSE(taken.summary.contains("catalogue:user"));
    EXPECT_FALSE(taken.summary.contains("catalogue:node"));
    EXPECT_FALSE(taken.summary.contains("bodies:missing"));

    BackupSnapshots snapshots(source.backups());
    const BackupOutcome found = snapshots.find(taken.snapshot);
    ASSERT_TRUE(found.ok()) << found.message;
    EXPECT_EQ(found.size_bytes, taken.size_bytes);
    EXPECT_EQ(found.summary, taken.summary);
    const std::string archive = read_file(found.archive);
    ASSERT_EQ(archive.size(), taken.size_bytes);

    Side target;
    const BackupOutcome restored = target.restore(archive);
    ASSERT_TRUE(restored.ok()) << restored.message << " " << restored.details.dump();
    EXPECT_EQ(restored.summary, taken.summary);
    EXPECT_EQ(restored.snapshot, taken.snapshot);

    // What was left out is not there, and partitions live on the target now.
    const auto catalogue = target.replication.shared_snapshot();
    EXPECT_FALSE(catalogue->contains("user:u1"));
    EXPECT_FALSE(catalogue->contains("node:" + source.replication.node_id()));
    EXPECT_FALSE(catalogue->contains("tbl:t3"));
    EXPECT_FALSE(catalogue->contains("part:p2"));
    ASSERT_TRUE(catalogue->contains("part:p1"));
    EXPECT_EQ((*catalogue)["part:p1"]["owner_nodes"], json::array({target.replication.node_id()}));
    EXPECT_EQ(target.bodies.read_blob("b1"), source.bodies.read_blob("b1"));
    for (const char* key : {"a", "b/c", "large"}) {
        EXPECT_EQ(target.object("drive", key), source.object("drive", key)) << key;
    }
    const auto record = target.records.get("lake_sessions", "s2499");
    ASSERT_TRUE(record.ok()) << record.message;
    EXPECT_EQ(record.record->value, "c2VhbGVk");
    EXPECT_EQ(record.record->revision, source.records.get("lake_sessions", "s2499").record->revision);

    // The restore is recorded, with what it counted.
    std::ifstream record_file(target.restore_record());
    const json recorded = json::parse(record_file, nullptr, false);
    ASSERT_TRUE(recorded.is_object());
    EXPECT_EQ(recorded["snapshot"], taken.snapshot);
    EXPECT_EQ(recorded["state"], "restored");
    EXPECT_EQ(zetta::data::summary_from_json(recorded["summary"]), taken.summary);
}

// A writer writes record i, then object i, without pause. In any one moment the
// records number the objects or one more, so a snapshot that is one cut keeps
// that; one that read the stores at different moments would not.
TEST(BackupSnapshot, ASnapshotTakenUnderWritesIsOneCut) {
    Side source;
    source.create_collection("lake_events");
    source.create_bucket("drive");
    std::atomic<bool> stop{false};
    std::atomic<int> refused{0};
    std::thread writer([&] {
        for (int i = 0; !stop; ++i) {
            const std::string key = "k" + std::to_string(i);
            for (;;) {
                const auto written = source.put_record("lake_events", key);
                if (written.ok()) break;
                ASSERT_EQ(written.status, RecordStatus::Unavailable) << written.message;
                ++refused;
                std::this_thread::yield();
            }
            for (;;) {
                const auto put = source.objects.put("drive", key, key, ObjectWrite{});
                if (put.ok()) break;
                ASSERT_EQ(put.status, ObjectStatus::Unavailable) << put.message;
                ++refused;
                std::this_thread::yield();
            }
        }
    });
    std::vector<BackupOutcome> taken;
    for (int round = 0; round < 5; ++round) {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        taken.push_back(source.snapshot());
    }
    stop = true;
    writer.join();
    for (const BackupOutcome& outcome : taken) {
        ASSERT_TRUE(outcome.ok()) << outcome.message;
        const std::int64_t records = outcome.summary.contains("collection:lake_events")
                                         ? outcome.summary.at("collection:lake_events")
                                         : 0;
        const std::int64_t objects = outcome.summary.at("bucket:drive");
        EXPECT_GE(records, objects);
        EXPECT_LE(records, objects + 1);
    }
    // The last one restores whole.
    Side target;
    const BackupOutcome restored =
        target.restore(read_file(BackupSnapshots(source.backups()).find(taken.back().snapshot).archive));
    ASSERT_TRUE(restored.ok()) << restored.message;
    EXPECT_EQ(restored.summary, taken.back().summary);
}

TEST(BackupSnapshot, TheWriteGateRefusesWritesOnlyWhileFrozen) {
    Side side;
    side.create_collection("lake_events");
    side.create_bucket("drive");
    {
        std::optional<BackupFreeze> freeze = BackupFreeze::begin(side.records, side.objects);
        ASSERT_TRUE(freeze);
        EXPECT_FALSE(BackupFreeze::begin(side.records, side.objects));  // one at a time
        EXPECT_TRUE(freeze->drained());
        EXPECT_EQ(side.put_record("lake_events", "k").status, RecordStatus::Unavailable);
        EXPECT_EQ(side.objects.put("drive", "k", "v", ObjectWrite{}).status,
                  ObjectStatus::Unavailable);
    }
    EXPECT_TRUE(side.put_record("lake_events", "k").ok());
    EXPECT_TRUE(side.objects.put("drive", "k", "v", ObjectWrite{}).ok());
}

TEST(BackupSnapshot, ANodeHoldingUserDataIsRefusedAsATarget) {
    Side source;
    seed(source);
    const BackupOutcome taken = source.snapshot();
    ASSERT_TRUE(taken.ok()) << taken.message;
    const std::string archive = read_file(BackupSnapshots(source.backups()).find(taken.snapshot).archive);

    Side target;
    target.create_bucket("photos");
    ASSERT_TRUE(target.objects.put("photos", "p", "x", ObjectWrite{}).ok());
    SnapshotRestore restore(target.stores, target.restore_record());
    const BackupOutcome refused = restore.begin();
    EXPECT_EQ(refused.status, BackupStatus::Conflict);
    EXPECT_FALSE(restore.wrote());
    EXPECT_FALSE(std::filesystem::exists(target.restore_record()));
    EXPECT_FALSE(target.replication.shared_snapshot()->contains("db:d1"));

    // The source itself is not empty either.
    EXPECT_EQ(source.restore(archive).status, BackupStatus::Conflict);
}

TEST(BackupSnapshot, ADamagedOrTruncatedArchiveIsRefused) {
    Side source;
    seed(source);
    const BackupOutcome taken = source.snapshot();
    ASSERT_TRUE(taken.ok()) << taken.message;
    const std::string archive = read_file(BackupSnapshots(source.backups()).find(taken.snapshot).archive);

    Side truncated;
    const BackupOutcome cut = truncated.restore(archive.substr(0, archive.size() - 40));
    EXPECT_EQ(cut.status, BackupStatus::InvalidInput) << cut.message;
    EXPECT_NE(cut.message.find("discard"), std::string::npos) << cut.message;

    // One byte of the large object's payload flipped: the trailer's checksum catches it.
    std::string damaged = archive;
    const std::size_t at = damaged.find("\"key\":\"large\"");
    ASSERT_NE(at, std::string::npos);
    damaged[damaged.find('\n', at) + 1000] ^= 0x01;
    Side flipped;
    const BackupOutcome refused = flipped.restore(damaged);
    EXPECT_EQ(refused.status, BackupStatus::InvalidInput);
    EXPECT_NE(refused.message.find("checksum"), std::string::npos) << refused.message;

    Side garbage;
    EXPECT_EQ(garbage.restore("not an archive\n").status, BackupStatus::InvalidInput);
}

TEST(BackupSnapshot, DiscardEmptiesOnlyARestoredRestoreTargetLeftAsRestored) {
    Side source;
    seed(source);
    const BackupOutcome taken = source.snapshot();
    ASSERT_TRUE(taken.ok()) << taken.message;
    const std::string archive = read_file(BackupSnapshots(source.backups()).find(taken.snapshot).archive);

    // Never on a node whose data did not come from a restore, flag or not.
    EXPECT_EQ(zetta::data::discard_restored(source.stores, source.restore_record(), true,
                                            {source.replication.node_id(), taken.snapshot})
                  .status,
              BackupStatus::Conflict);
    EXPECT_TRUE(source.replication.shared_snapshot()->contains("db:d1"));

    Side target;
    ASSERT_TRUE(target.restore(archive).ok());
    const DiscardRequest right{target.replication.node_id(), taken.snapshot};
    // Not started as a restore target.
    EXPECT_EQ(zetta::data::discard_restored(target.stores, target.restore_record(), false, right)
                  .status,
              BackupStatus::Conflict);
    // Another node, another snapshot.
    EXPECT_EQ(zetta::data::discard_restored(target.stores, target.restore_record(), true,
                                            {source.replication.node_id(), taken.snapshot})
                  .status,
              BackupStatus::InvalidInput);
    EXPECT_EQ(zetta::data::discard_restored(target.stores, target.restore_record(), true,
                                            {right.node_id, zetta::util::new_uuid()})
                  .status,
              BackupStatus::InvalidInput);
    // Written since the restore: refused, and the write survives.
    ASSERT_TRUE(target.put_record("lake_sessions", "written-after").ok());
    EXPECT_EQ(zetta::data::discard_restored(target.stores, target.restore_record(), true, right)
                  .status,
              BackupStatus::Conflict);
    EXPECT_TRUE(target.records.get("lake_sessions", "written-after").ok());
    RecordWrite undo;
    undo.collection = "lake_sessions";
    undo.key = "written-after";
    undo.remove = true;
    ASSERT_TRUE(target.records.write(undo).ok());

    const BackupOutcome emptied =
        zetta::data::discard_restored(target.stores, target.restore_record(), true, right);
    ASSERT_TRUE(emptied.ok()) << emptied.message << " " << emptied.details.dump();
    EXPECT_FALSE(zetta::data::holds_user_data(zetta::data::summarize_node(target.stores)));
    EXPECT_FALSE(std::filesystem::exists(target.restore_record()));
    // A second discard has no restore to undo.
    EXPECT_EQ(zetta::data::discard_restored(target.stores, target.restore_record(), true, right)
                  .status,
              BackupStatus::Conflict);
    // The next rehearsal finds it empty.
    const BackupOutcome again = target.restore(archive);
    ASSERT_TRUE(again.ok()) << again.message << " " << again.details.dump();
    EXPECT_EQ(again.summary, taken.summary);
}

TEST(BackupSnapshot, ABodyHeldOnlyByAnotherMemberRefusesTheSnapshot) {
    Side source;
    const std::string body = pattern(64, 1);
    source.replication.set("db:d1", json{{"name", "lake"}});
    source.replication.set("tbl:t1", json{{"db", "d1"}, {"name", "facts"}, {"partition_only", true}});
    source.replication.set("mfst:t1", json{{"table", "t1"}, {"partitions", 1}, {"rows", 2}});
    source.replication.set("lo:b1", json{{"db", "d1"},
                                         {"name", "facts-0"},
                                         {"size", body.size()},
                                         {"sha256", zetta::util::sha256_hex(body)}});
    source.replication.set("part:p1", json{{"table", "t1"},
                                           {"blob_ref", "b1"},
                                           {"row_range", {{"start", 0}, {"end", 2}}},
                                           {"owner_nodes", {"another-member"}}});
    const BackupOutcome refused = source.snapshot();
    EXPECT_EQ(refused.status, BackupStatus::Conflict) << refused.message;
    EXPECT_EQ(refused.details["owners"], json::array({"another-member"}));
    // Nothing is left behind.
    std::error_code ec;
    EXPECT_TRUE(!std::filesystem::exists(source.backups(), ec) ||
                std::filesystem::is_empty(source.backups(), ec));
}

TEST(BackupSnapshot, SnapshotsAreKeptUntilReleasedOrExpired) {
    Side side;
    seed(side);
    const BackupOutcome first = side.snapshot();
    const BackupOutcome second = side.snapshot();
    ASSERT_TRUE(first.ok() && second.ok());
    BackupSnapshots snapshots(side.backups());
    EXPECT_TRUE(snapshots.release(first.snapshot).ok());
    EXPECT_EQ(snapshots.find(first.snapshot).status, BackupStatus::NotFound);
    EXPECT_EQ(snapshots.release(first.snapshot).status, BackupStatus::NotFound);
    EXPECT_EQ(snapshots.find("../../etc").status, BackupStatus::NotFound);
    EXPECT_EQ(snapshots.expire(std::chrono::system_clock::now()), 0u);
    EXPECT_TRUE(snapshots.find(second.snapshot).ok());
    EXPECT_EQ(snapshots.expire(std::chrono::system_clock::now() + std::chrono::hours(7)), 1u);
    EXPECT_EQ(snapshots.find(second.snapshot).status, BackupStatus::NotFound);
}

TEST(BackupSnapshot, TheSummaryNamesWhatItCounts) {
    const json catalogue{
        {"db:d1", {{"name", "lake"}}},
        {"tbl:t1", {{"db", "d1"}, {"name", "events"}}},
        {"row:r1", {{"tbl", "t1"}}},
        {"row:r2", {{"tbl", "t1"}}},
        {"tbl:t2", {{"db", "d1"}, {"name", "facts"}, {"partition_only", true}}},
        {"part:p1", {{"table", "t2"}, {"blob_ref", "lo:b1"}, {"row_range", {{"start", 10}, {"end", 15}}}}},
        {"part:p2", {{"table", "t2"}, {"blob_ref", "b2"}, {"row_range", {{"start", 15}, {"end", 20}}}}},
        {"lo:b1", {{"db", "d1"}}},
        {"lo:b2", {{"db", "d1"}}},
        {"object_bucket:drive", {{"name", "drive"}}},
        {"record_collection:lake_sessions", {{"name", "lake_sessions"}}},
        {"user:u1", {{"name", "x"}}},
        {"project:x", {{"name", "churn"}}},
    };
    const BackupSummary summary = zetta::data::summarize(
        catalogue, [](const std::string& id) { return id == "b1"; }, {{"drive", 4}},
        {{"lake_sessions", 0}, {"_entity_keys", 3}});
    EXPECT_EQ(summary.at("table:lake.events"), 2);
    EXPECT_EQ(summary.at("table:lake.facts"), 5);  // only the partition whose body is present
    EXPECT_EQ(summary.at("bodies"), 1);
    EXPECT_EQ(summary.at("bodies:missing"), 1);
    EXPECT_EQ(summary.at("bucket:drive"), 4);
    EXPECT_EQ(summary.at("collection:_entity_keys"), 3);
    EXPECT_FALSE(summary.contains("collection:lake_sessions"));  // holds none
    EXPECT_EQ(summary.at("catalogue:project"), 1);
    EXPECT_EQ(summary.at("catalogue:row"), 2);
    EXPECT_FALSE(summary.contains("catalogue:user"));
    EXPECT_FALSE(summary.contains("catalogue:object_bucket"));
    EXPECT_FALSE(summary.contains("catalogue:record_collection"));
    EXPECT_TRUE(zetta::data::holds_user_data(summary));
    EXPECT_FALSE(zetta::data::holds_user_data({{"bodies", 0}, {"bucket:drive", 0}}));
    const json differences = zetta::data::summary_differences({{"a", 1}, {"b", 2}}, {{"b", 3}});
    EXPECT_EQ(differences["a"]["observed"], 0);
    EXPECT_EQ(differences["b"]["expected"], 2);
    EXPECT_FALSE(differences.contains("c"));
}
