// The object capability (#1005, docs/data/object-store.md): every operation's
// contract, conditional create under a race, fencing, crash safety of appends and
// puts, compaction and restart, quota, and placement on rf owners of a real
// three-node cluster with a node lost, repaired and replaced.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <unistd.h>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/object_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::cluster::ReplicationManager;
using zetta::data::ObjectBucket;
using zetta::data::ObjectOutcome;
using zetta::data::ObjectReader;
using zetta::data::ObjectStatus;
using zetta::data::ObjectStore;
using zetta::data::ObjectWrite;
using zetta::test::TempDir;

ObjectBucket bucket(std::string name, std::optional<std::uint64_t> quota = std::nullopt,
                    std::optional<std::uint32_t> rf = std::nullopt) {
    ObjectBucket definition;
    definition.name = std::move(name);
    definition.quota_bytes = quota;
    definition.rf = rf;
    return definition;
}

ObjectWrite fenced(std::uint64_t fence) {
    ObjectWrite options;
    options.fence = fence;
    return options;
}

std::string pattern(std::size_t size, std::uint64_t seed) {
    std::string bytes(size, '\0');
    std::mt19937_64 random(seed);
    for (std::size_t index = 0; index < size; index += 8) {
        const std::uint64_t value = random();
        std::memcpy(bytes.data() + index, &value, std::min<std::size_t>(8, size - index));
    }
    return bytes;
}

// Read [offset, offset + length) (to the end when absent), or the failure.
struct Read {
    ObjectStatus status = ObjectStatus::Ok;
    std::string bytes;
    std::uint64_t size = 0;
};

Read read(ObjectStore& store, const std::string& name, const std::string& key,
          std::uint64_t offset = 0, std::optional<std::uint64_t> length = std::nullopt) {
    std::shared_ptr<ObjectReader> reader;
    const ObjectOutcome opened = store.open_read(name, key, offset, length, reader);
    Read result;
    result.status = opened.status;
    if (opened.object) result.size = opened.object->size;
    if (!opened.ok()) return result;
    if (!reader->read(0, reader->length(), result.bytes)) result.status = ObjectStatus::Unavailable;
    return result;
}

struct ObjectStoreFixture : ::testing::Test {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    std::unique_ptr<ObjectStore> store;

    void SetUp() override { reopen(); }

    void reopen(std::uint64_t segment_bytes = 0) {
        store.reset();
        store = std::make_unique<ObjectStore>(replication, replication.node_id(),
                                              dir.path() / "objects", segment_bytes);
        ASSERT_TRUE(store->open());
    }
};

std::filesystem::path last_segment(const std::filesystem::path& directory) {
    std::vector<std::filesystem::path> segments;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        segments.push_back(entry.path());
    }
    std::ranges::sort(segments);
    return segments.back();
}

void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::filesystem::remove_all(to);
    std::filesystem::copy(from, to, std::filesystem::copy_options::recursive);
}

}  // namespace

TEST_F(ObjectStoreFixture, BucketsAreIdempotentAndRefuseADifferentDefinition) {
    const ObjectOutcome created = store->create_bucket(bucket("lake_ws1", 1 << 20));
    ASSERT_TRUE(created.ok()) << created.message;
    EXPECT_TRUE(created.created);
    const ObjectOutcome again = store->create_bucket(bucket("lake_ws1", 1 << 20));
    ASSERT_TRUE(again.ok());
    EXPECT_FALSE(again.created);
    const ObjectOutcome different = store->create_bucket(bucket("lake_ws1", 2 << 20));
    EXPECT_EQ(different.status, ObjectStatus::Conflict);
    ASSERT_TRUE(different.bucket);
    EXPECT_EQ(different.bucket->quota_bytes, 1u << 20);
    // The definition is in the catalogue, the bytes never are.
    EXPECT_TRUE(replication.shared_snapshot()->contains("object_bucket:lake_ws1"));

    EXPECT_EQ(store->create_bucket(bucket("Lake")).status, ObjectStatus::InvalidInput);
    EXPECT_EQ(store->create_bucket(bucket("")).status, ObjectStatus::InvalidInput);
    EXPECT_EQ(store->put("nope", "k", "x", {}).status, ObjectStatus::NotFound);
    EXPECT_EQ(store->list("nope", "", "", 10).status, ObjectStatus::NotFound);
    EXPECT_EQ(store->put("lake_ws1", std::string(1025, 'k'), "x", {}).status,
              ObjectStatus::InvalidInput);
    EXPECT_EQ(store->put("lake_ws1", "bad\nkey", "x", {}).status, ObjectStatus::InvalidInput);
    EXPECT_TRUE(store->put("lake_ws1", std::string(1024, 'k'), "x", {}).ok());
}

TEST_F(ObjectStoreFixture, PutReplacesTheWholeObjectAndReadsExactRanges) {
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    const ObjectOutcome first = store->put("store", "index/run-1", "hello world", {});
    ASSERT_TRUE(first.ok()) << first.message;
    EXPECT_EQ(first.object->size, 11u);
    EXPECT_EQ(read(*store, "store", "index/run-1").bytes, "hello world");
    EXPECT_EQ(read(*store, "store", "index/run-1", 6, 5).bytes, "world");
    EXPECT_EQ(read(*store, "store", "index/run-1", 11, 0).bytes, "");
    // Outside the object: 416 with the size; absent: 404.
    const Read past = read(*store, "store", "index/run-1", 6, 6);
    EXPECT_EQ(past.status, ObjectStatus::OutOfRange);
    EXPECT_EQ(past.size, 11u);
    EXPECT_EQ(read(*store, "store", "index/run-1", 12).status, ObjectStatus::OutOfRange);
    EXPECT_EQ(read(*store, "store", "missing").status, ObjectStatus::NotFound);

    const ObjectOutcome second = store->put("store", "index/run-1", "x", {});
    ASSERT_TRUE(second.ok());
    EXPECT_GT(second.object->generation, first.object->generation);
    EXPECT_EQ(read(*store, "store", "index/run-1").bytes, "x");
    ASSERT_TRUE(store->put("store", "empty", "", {}).ok());
    const Read empty = read(*store, "store", "empty");
    EXPECT_EQ(empty.status, ObjectStatus::Ok);
    EXPECT_EQ(empty.bytes, "");

    // put if_absent decides atomically.
    EXPECT_TRUE(store->put("store", "LIQUID_STORE", "v1", {}, /*if_absent=*/true).ok());
    const ObjectOutcome taken = store->put("store", "LIQUID_STORE", "v2", {}, true);
    EXPECT_EQ(taken.status, ObjectStatus::Conflict);
    EXPECT_EQ(read(*store, "store", "LIQUID_STORE").bytes, "v1");
}

TEST_F(ObjectStoreFixture, PutRangeAppendsAtTheExactOffsetAndARetryChangesNothing) {
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    ASSERT_TRUE(store->put_range("store", "packs/1", 0, "abc", {}).ok());
    const ObjectOutcome appended = store->put_range("store", "packs/1", 3, "def", {});
    ASSERT_TRUE(appended.ok()) << appended.message;
    EXPECT_FALSE(appended.unchanged);
    EXPECT_EQ(appended.object->size, 6u);
    // A retry of an acknowledged append succeeds and changes nothing.
    const ObjectOutcome retried = store->put_range("store", "packs/1", 3, "def", {});
    ASSERT_TRUE(retried.ok()) << retried.message;
    EXPECT_TRUE(retried.unchanged);
    EXPECT_TRUE(store->put_range("store", "packs/1", 1, "bcd", {}).ok());
    // Anything else is a conflict naming the size.
    const ObjectOutcome different = store->put_range("store", "packs/1", 3, "xyz", {});
    EXPECT_EQ(different.status, ObjectStatus::Conflict);
    EXPECT_EQ(different.object->size, 6u);
    EXPECT_EQ(store->put_range("store", "packs/1", 9, "z", {}).status, ObjectStatus::Conflict);
    EXPECT_EQ(store->put_range("store", "packs/1", 5, "fg", {}).status, ObjectStatus::Conflict);
    const ObjectOutcome absent = store->put_range("store", "packs/2", 4, "z", {});
    EXPECT_EQ(absent.status, ObjectStatus::Conflict);
    EXPECT_EQ(absent.object->size, 0u);
    EXPECT_EQ(read(*store, "store", "packs/1").bytes, "abcdef");

    // A range larger than one slice is applied a slice at a time.
    const std::string big = pattern(9 * 1024 * 1024 + 17, 7);
    ASSERT_TRUE(store->put_range("store", "packs/1", 6, big, {}).ok());
    const Read whole = read(*store, "store", "packs/1");
    ASSERT_EQ(whole.bytes.size(), 6 + big.size());
    EXPECT_EQ(whole.bytes.substr(6), big);
    EXPECT_EQ(read(*store, "store", "packs/1", 4 * 1024 * 1024 + 3, 100).bytes,
              big.substr(4 * 1024 * 1024 - 3, 100));
    EXPECT_TRUE(store->put_range("store", "packs/1", 6, big, {}).unchanged);
}

TEST_F(ObjectStoreFixture, ListRemoveUsageAndBarrier) {
    ASSERT_TRUE(store->create_bucket(bucket("store", 1 << 20)).ok());
    for (const char* key : {"packs/a", "packs/b", "packs/c", "index/a", "leases/x"}) {
        ASSERT_TRUE(store->put("store", key, std::string(10, 'k'), {}).ok());
    }
    zetta::data::ObjectPage page = store->list("store", "packs/", "", 2);
    ASSERT_TRUE(page.ok());
    ASSERT_EQ(page.objects.size(), 2u);
    EXPECT_EQ(page.objects[0].key, "packs/a");
    EXPECT_EQ(page.objects[0].size, 10u);
    EXPECT_EQ(page.next_after, "packs/b");
    page = store->list("store", "packs/", page.next_after, 2);
    ASSERT_EQ(page.objects.size(), 1u);
    EXPECT_EQ(page.objects[0].key, "packs/c");
    EXPECT_TRUE(page.next_after.empty());
    EXPECT_EQ(store->list("store", "", "", 1000).objects.size(), 5u);

    ASSERT_TRUE(store->remove("store", "packs/b", {}).ok());
    EXPECT_EQ(store->remove("store", "packs/b", {}).status, ObjectStatus::NotFound);
    EXPECT_EQ(read(*store, "store", "packs/b").status, ObjectStatus::NotFound);
    EXPECT_EQ(store->list("store", "packs/", "", 10).objects.size(), 2u);

    const zetta::data::ObjectUsage usage = store->usage("store");
    ASSERT_TRUE(usage.ok());
    EXPECT_EQ(usage.objects, 4u);
    EXPECT_EQ(usage.bytes, 40u);
    EXPECT_EQ(usage.quota_bytes, 1u << 20);
    const ObjectOutcome barrier = store->barrier("store");
    ASSERT_TRUE(barrier.ok());
    EXPECT_EQ(barrier.nodes, 1u);
    // A removed key re-created gets a higher generation.
    ASSERT_TRUE(store->put("store", "packs/b", "again", {}).ok());
    EXPECT_EQ(read(*store, "store", "packs/b").bytes, "again");
}

TEST_F(ObjectStoreFixture, QuotaRefusesWritesPastIt) {
    ASSERT_TRUE(store->create_bucket(bucket("small", 1000)).ok());
    ASSERT_TRUE(store->put("small", "a", std::string(600, 'a'), {}).ok());
    EXPECT_EQ(store->put("small", "b", std::string(600, 'b'), {}).status,
              ObjectStatus::QuotaExceeded);
    // Replacing an object frees its old bytes.
    EXPECT_TRUE(store->put("small", "a", std::string(900, 'a'), {}).ok());
    EXPECT_EQ(store->put_range("small", "a", 900, std::string(101, 'a'), {}).status,
              ObjectStatus::QuotaExceeded);
    EXPECT_TRUE(store->put_range("small", "a", 900, std::string(100, 'a'), {}).ok());
    ASSERT_TRUE(store->remove("small", "a", {}).ok());
    EXPECT_TRUE(store->put("small", "b", std::string(600, 'b'), {}).ok());
    // A staged upload is checked as its parts arrive.
    ASSERT_TRUE(store->create_bucket(bucket("tiny", 5 * 1024 * 1024)).ok());
    EXPECT_EQ(store->put("tiny", "big", pattern(6 * 1024 * 1024, 1), {}).status,
              ObjectStatus::QuotaExceeded);
    EXPECT_EQ(read(*store, "tiny", "big").status, ObjectStatus::NotFound);
    EXPECT_EQ(store->usage("tiny").bytes, 0u);
}

// Eight threads race a conditional create of the same key, twenty times: exactly
// one wins each time and the object holds the winner's bytes.
TEST_F(ObjectStoreFixture, ConditionalCreateUnderARaceHasExactlyOneWinner) {
    ASSERT_TRUE(store->create_bucket(bucket("race")).ok());
    for (int round = 0; round < 20; ++round) {
        const std::string key = "repo/" + std::to_string(round);
        std::atomic<int> winners{0};
        std::atomic<int> winner{-1};
        std::vector<std::thread> threads;
        for (int thread = 0; thread < 8; ++thread) {
            threads.emplace_back([&, thread] {
                const ObjectOutcome outcome =
                    store->put("race", key, "writer-" + std::to_string(thread), {}, true);
                if (outcome.ok()) {
                    ++winners;
                    winner = thread;
                } else {
                    EXPECT_EQ(outcome.status, ObjectStatus::Conflict);
                }
            });
        }
        for (auto& thread : threads) thread.join();
        EXPECT_EQ(winners.load(), 1);
        EXPECT_EQ(read(*store, "race", key).bytes, "writer-" + std::to_string(winner.load()));
    }
}

// A put larger than one slice is staged and committed: a reader running beside
// the replacements sees all of the old object or all of the new, never a mix,
// and an abandoned upload leaves the old object.
TEST_F(ObjectStoreFixture, LargePutsAreAtomicForReaders) {
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    const std::string a(10 * 1024 * 1024 + 5, 'a');
    const std::string b(10 * 1024 * 1024 + 5, 'b');
    ASSERT_TRUE(store->put("store", "index/big", a, {}).ok());
    std::atomic<bool> done{false};
    std::atomic<int> mixed{0};
    std::atomic<int> reads{0};
    std::thread reader([&] {
        while (!done) {
            const Read got = read(*store, "store", "index/big");
            ASSERT_EQ(got.status, ObjectStatus::Ok);
            if (got.bytes != a && got.bytes != b) ++mixed;
            ++reads;
        }
    });
    for (int round = 0; round < 6; ++round) {
        ASSERT_TRUE(store->put("store", "index/big", round % 2 == 0 ? b : a, {}).ok());
    }
    done = true;
    reader.join();
    EXPECT_EQ(mixed.load(), 0);
    EXPECT_GT(reads.load(), 0);

    zetta::data::ObjectUpload upload;
    ASSERT_TRUE(store->begin_upload("store", "index/big", {}, std::nullopt, upload).ok());
    ASSERT_TRUE(store->upload_part(upload, std::string(1024, 'z')).ok());
    store->abort_upload(upload);
    EXPECT_EQ(read(*store, "store", "index/big").bytes, a);  // the last put, round 5
}

// Fencing: a stale holder's writes are refused after a takeover; acquire is held
// while another holder's lease is live and succeeds on release and on expiry; a
// renewal keeps the lease.
TEST_F(ObjectStoreFixture, FencingRefusesAStaleHolderAfterATakeover) {
    ASSERT_TRUE(store->create_bucket(bucket("lake_ws")).ok());
    // A bucket that never had a holder takes writes without a fence.
    ASSERT_TRUE(store->put("lake_ws", "k", "0", {}).ok());

    const ObjectOutcome first = store->acquire("lake_ws", "lake-pod-1", 60'000);
    ASSERT_TRUE(first.ok()) << first.message;
    EXPECT_EQ(first.lease->fence, 1u);
    const ObjectOutcome unfenced = store->put("lake_ws", "k", "1", {});
    EXPECT_EQ(unfenced.status, ObjectStatus::Fenced);
    EXPECT_EQ(unfenced.fence, 1u);
    EXPECT_TRUE(store->put("lake_ws", "k", "1", fenced(1)).ok());
    // The same holder re-acquiring keeps its fence.
    EXPECT_EQ(store->acquire("lake_ws", "lake-pod-1", 60'000).lease->fence, 1u);

    const ObjectOutcome held = store->acquire("lake_ws", "lake-pod-2", 60'000);
    EXPECT_EQ(held.status, ObjectStatus::Held);
    ASSERT_TRUE(held.lease);
    EXPECT_EQ(held.lease->holder, "lake-pod-1");

    // A watcher is woken by the release.
    std::atomic<bool> woken{false};
    store->watch_lease("lake_ws", [&] { woken = true; });
    ASSERT_TRUE(store->release("lake_ws", "lake-pod-1", 1).ok());
    EXPECT_TRUE(woken.load());
    const ObjectOutcome second = store->acquire("lake_ws", "lake-pod-2", 60'000);
    ASSERT_TRUE(second.ok()) << second.message;
    EXPECT_EQ(second.lease->fence, 2u);

    // The predecessor can never write again, nor renew.
    const ObjectOutcome stale = store->put_range("lake_ws", "k", 1, "x", fenced(1));
    EXPECT_EQ(stale.status, ObjectStatus::Fenced);
    EXPECT_EQ(stale.fence, 2u);
    EXPECT_EQ(store->remove("lake_ws", "k", fenced(1)).status, ObjectStatus::Fenced);
    EXPECT_EQ(store->renew("lake_ws", "lake-pod-1", 1, 60'000).status, ObjectStatus::Fenced);
    EXPECT_TRUE(store->put_range("lake_ws", "k", 1, "x", fenced(2)).ok());
    EXPECT_EQ(read(*store, "lake_ws", "k").bytes, "1x");

    // A renewal keeps the lease past its first expiry.
    const ObjectOutcome shortened = store->renew("lake_ws", "lake-pod-2", 2, 300);
    ASSERT_TRUE(shortened.ok()) << shortened.message;
    for (int step = 0; step < 4; ++step) {
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ASSERT_TRUE(store->renew("lake_ws", "lake-pod-2", 2, 300).ok());
        EXPECT_EQ(store->acquire("lake_ws", "lake-pod-3", 60'000).status, ObjectStatus::Held);
    }
    // Then it lapses, and the next holder takes over with a higher fence.
    std::this_thread::sleep_for(std::chrono::milliseconds(350));
    const ObjectOutcome third = store->acquire("lake_ws", "lake-pod-3", 60'000);
    ASSERT_TRUE(third.ok()) << third.message;
    EXPECT_EQ(third.lease->fence, 3u);
    EXPECT_EQ(store->put("lake_ws", "k", "late", fenced(2)).status, ObjectStatus::Fenced);

    // The fence is in the catalogue: a restarted store still enforces it.
    reopen();
    EXPECT_EQ(store->lease("lake_ws")->fence, 3u);
    EXPECT_EQ(store->put("lake_ws", "k", "late", fenced(2)).status, ObjectStatus::Fenced);
    EXPECT_TRUE(store->put("lake_ws", "k", "on time", fenced(3)).ok());
}

TEST_F(ObjectStoreFixture, ObjectsSurviveARestartAndStayOutOfTheCatalogue) {
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    const std::string pack = pattern(3 * 1024 * 1024, 3);
    ASSERT_TRUE(store->put_range("store", "packs/1", 0, pack.substr(0, 1000), {}).ok());
    ASSERT_TRUE(store->put_range("store", "packs/1", 1000, pack.substr(1000), {}).ok());
    ASSERT_TRUE(store->put("store", "index/1", "run", {}).ok());
    ASSERT_TRUE(store->put("store", "gone", "x", {}).ok());
    ASSERT_TRUE(store->remove("store", "gone", {}).ok());
    ASSERT_TRUE(store->put("store", "big", pattern(9 * 1024 * 1024, 9), {}).ok());
    ASSERT_TRUE(store->barrier("store").ok());
    // A restart is a new incarnation (lake#543): 32 random hex digits each time.
    const std::string before = store->incarnation();
    ASSERT_EQ(before.size(), 32u);
    EXPECT_EQ(before.find_first_not_of("0123456789abcdef"), std::string::npos);
    EXPECT_EQ(store->incarnation("store"), before + ".0");
    reopen();
    EXPECT_NE(store->incarnation(), before);
    EXPECT_EQ(store->incarnation("store"), store->incarnation() + ".0");
    EXPECT_EQ(read(*store, "store", "packs/1").bytes, pack);
    EXPECT_EQ(read(*store, "store", "index/1").bytes, "run");
    EXPECT_EQ(read(*store, "store", "gone").status, ObjectStatus::NotFound);
    EXPECT_EQ(read(*store, "store", "big").bytes, pattern(9 * 1024 * 1024, 9));
    // Appending continues the same version after the restart.
    ASSERT_TRUE(store->put_range("store", "packs/1", pack.size(), "tail", {}).ok());
    EXPECT_EQ(read(*store, "store", "packs/1", pack.size()).bytes, "tail");
    const std::string catalogue = replication.shared_snapshot()->dump();
    EXPECT_EQ(catalogue.find("packs/1"), std::string::npos);
    EXPECT_LT(catalogue.size(), 4096u);
}

// Crash safety: after a kill, the tail of the log may be torn anywhere past what
// a barrier made durable. An append is then absent or complete, never partial,
// and every acknowledged-and-barriered byte is there; a put is never partial.
TEST_F(ObjectStoreFixture, ATornTailLeavesAppendsWholeUpToTheBarrierAndPutsNeverPartial) {
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    constexpr std::size_t kPiece = 64 * 1024;
    std::string expected;
    for (int piece = 0; piece < 10; ++piece) {
        const std::string bytes = pattern(kPiece, 100 + piece);
        ASSERT_TRUE(store->put_range("store", "packs/1", expected.size(), bytes, {}).ok());
        expected += bytes;
    }
    ASSERT_TRUE(store->put("store", "index/run", "old version", {}).ok());
    ASSERT_TRUE(store->barrier("store").ok());
    const std::size_t barriered = expected.size();
    const std::filesystem::path bucket_dir = dir.path() / "objects" / "store";
    const std::uintmax_t durable_size = std::filesystem::file_size(last_segment(bucket_dir));
    for (int piece = 10; piece < 15; ++piece) {
        const std::string bytes = pattern(kPiece, 100 + piece);
        ASSERT_TRUE(store->put_range("store", "packs/1", expected.size(), bytes, {}).ok());
        expected += bytes;
    }
    // A put of a large new version: staged parts, then the commit frame last.
    const std::string replacement(9 * 1024 * 1024, 'n');
    ASSERT_TRUE(store->put("store", "index/run", replacement, {}).ok());
    store.reset();  // the "kill": no barrier for any of the above
    const std::uintmax_t full_size = std::filesystem::file_size(last_segment(bucket_dir));
    ASSERT_GT(full_size, durable_size);

    std::mt19937_64 random(42);
    std::vector<std::uintmax_t> cuts{durable_size, full_size, full_size - 1, full_size - 20};
    for (int cut = 0; cut < 24; ++cut) cuts.push_back(durable_size + random() % (full_size - durable_size));
    const TempDir crash;
    for (const std::uintmax_t cut : cuts) {
        copy_tree(dir.path() / "objects", crash.path() / "objects");
        const std::filesystem::path segment = last_segment(crash.path() / "objects" / "store");
        std::filesystem::resize_file(segment, cut);
        if (cut % 3 == 0) {  // and garbage after the tear
            std::ofstream tail(segment, std::ios::binary | std::ios::app);
            tail << pattern(777, cut);
        }
        ObjectStore after(replication, replication.node_id(), crash.path() / "objects");
        ASSERT_TRUE(after.open());
        const Read pack = read(after, "store", "packs/1");
        ASSERT_EQ(pack.status, ObjectStatus::Ok) << "cut at " << cut;
        EXPECT_GE(pack.bytes.size(), barriered) << "cut at " << cut;
        EXPECT_EQ(pack.bytes.size() % kPiece, 0u) << "cut at " << cut;
        EXPECT_EQ(pack.bytes, expected.substr(0, pack.bytes.size())) << "cut at " << cut;
        const Read run = read(after, "store", "index/run");
        ASSERT_EQ(run.status, ObjectStatus::Ok) << "cut at " << cut;
        EXPECT_TRUE(run.bytes == "old version" || run.bytes == replacement)
            << "cut at " << cut << ": a partial put of " << run.bytes.size() << " bytes";
        if (cut == full_size) EXPECT_EQ(run.bytes, replacement);
        // The log keeps working after a torn tail.
        EXPECT_TRUE(after.put_range("store", "packs/1", pack.bytes.size(), "z", {}).ok());
    }
}

// A bucket whose log failed an fsync serves nothing from it (#1235): reads,
// listings, usage and barriers answer Unavailable (503 busy), not "absent",
// empty or zero, even while a reopen keeps failing part way. Health and the
// storage stats report it, and once the disk works the next reopen restores it.
TEST_F(ObjectStoreFixture, AFailedLogAnswersUnavailableUntilItIsReopened) {
    std::atomic<bool> failing{false};
    store->set_fsync([&](const std::filesystem::path&, int descriptor) {
        if (failing.load()) {
            errno = EIO;
            return -1;
        }
        return ::fsync(descriptor);
    });
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    ObjectWrite durable;
    durable.durable = true;
    for (int index = 0; index < 5; ++index) {
        ASSERT_TRUE(store->put("store", "k" + std::to_string(index), "value", durable).ok());
    }
    const std::string incarnation = store->incarnation("store");
    EXPECT_EQ(incarnation, store->incarnation() + ".0");
    failing = true;
    const ObjectOutcome failed = store->put("store", "k9", "lost", durable);
    EXPECT_EQ(failed.status, ObjectStatus::Unavailable);
    const auto expect_unavailable = [&](const std::string& when) {
        EXPECT_EQ(read(*store, "store", "k1").status, ObjectStatus::Unavailable) << when;
        EXPECT_EQ(read(*store, "store", "absent").status, ObjectStatus::Unavailable) << when;
        EXPECT_EQ(store->list("store", "", "", 100).status, ObjectStatus::Unavailable) << when;
        EXPECT_EQ(store->usage("store").status, ObjectStatus::Unavailable) << when;
        EXPECT_EQ(store->barrier("store").status, ObjectStatus::Unavailable) << when;
        EXPECT_EQ(store->put("store", "k1", "new", {}).status, ObjectStatus::Unavailable) << when;
        EXPECT_EQ(store->put("store", "k1", "new", {}, /*if_absent=*/true).status,
                  ObjectStatus::Unavailable)
            << when;
        const auto failures = store->failed_logs();
        ASSERT_EQ(failures.size(), 1u) << when;
        EXPECT_EQ(failures.front().bucket, "store");
        EXPECT_EQ(store->storage_stats().failed_logs, 1u) << when;
    };
    expect_unavailable("after the failed fsync");
    // The reopen fails part way (its startup fsync); nothing changes.
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    store->maintain(now);
    ASSERT_EQ(store->failed_logs().front().reopen_attempts, 1u);
    expect_unavailable("after a failed reopen");
    // Only a reopen that succeeds changes the bucket's incarnation.
    EXPECT_EQ(store->incarnation("store"), incarnation);
    failing = false;
    store->maintain(now + 60'000);
    EXPECT_TRUE(store->failed_logs().empty());
    // The reopen dropped what no fsync covered, so the bucket's incarnation
    // changes (lake#543), and the node's stays.
    EXPECT_EQ(store->incarnation("store"), store->incarnation() + ".1");
    EXPECT_EQ(store->storage_stats().failed_logs, 0u);
    EXPECT_EQ(read(*store, "store", "k1").bytes, "value");
    EXPECT_EQ(read(*store, "store", "k9").status, ObjectStatus::NotFound);
    EXPECT_EQ(store->usage("store").objects, 5u);
    EXPECT_TRUE(store->put("store", "k9", "kept", durable).ok());
}

// lake#543: the Zetta-Incarnation precondition and the write are one step. A
// reopen of the bucket's log (after a failed fsync) that lands after the
// precondition was read and before the write must refuse the write, which
// would otherwise land after the reopen dropped a write it depends on. The
// injection point runs between the store's checks and its log write.
TEST_F(ObjectStoreFixture, AReopenBetweenThePreconditionAndTheWriteRefusesIt) {
    std::atomic<bool> failing{false};
    store->set_fsync([&](const std::filesystem::path&, int descriptor) {
        if (failing.load()) {
            errno = EIO;
            return -1;
        }
        return ::fsync(descriptor);
    });
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    ObjectWrite durable;
    durable.durable = true;
    ASSERT_TRUE(store->put("store", "kept", "durable", durable).ok());
    ASSERT_TRUE(store->put_range("store", "pack", 0, "abc", durable).ok());
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    std::int64_t later = now;
    // A failed fsync (the barrier's) fails the log; maintenance reopens it,
    // cut back to the last fsync, which drops the unsynced write.
    const auto force_reopen = [&] {
        failing = true;
        EXPECT_FALSE(store->barrier("store").ok());
        failing = false;
        later += 60'000;
        store->maintain(later);
        EXPECT_TRUE(store->failed_logs().empty());
    };
    struct Case {
        std::string name;
        std::function<ObjectOutcome(const ObjectWrite&)> write;
        std::function<void()> unchanged;
    };
    const std::vector<Case> cases{
        {"put", [&](const ObjectWrite& options) { return store->put("store", "late", "x", options); },
         [&] { EXPECT_EQ(read(*store, "store", "late").status, ObjectStatus::NotFound); }},
        {"put range",
         [&](const ObjectWrite& options) { return store->put_range("store", "pack", 3, "def", options); },
         [&] { EXPECT_EQ(read(*store, "store", "pack").bytes, "abc"); }},
        {"remove", [&](const ObjectWrite& options) { return store->remove("store", "kept", options); },
         [&] { EXPECT_EQ(read(*store, "store", "kept").bytes, "durable"); }},
        {"upload",
         [&](const ObjectWrite& options) {
             return store->put("store", "large", std::string(ObjectStore::kSliceBytes + 10, 'l'), options);
         },
         [&] { EXPECT_EQ(read(*store, "store", "large").status, ObjectStatus::NotFound); }},
    };
    for (const Case& each : cases) {
        // Acknowledged without durability, then lost to the reopen.
        ASSERT_TRUE(store->put("store", "acknowledged", "unsynced", {}).ok()) << each.name;
        ObjectWrite options;
        options.incarnation = store->incarnation("store");
        bool fired = false;
        store->set_before_log_write([&] {
            if (std::exchange(fired, true)) return;
            force_reopen();
        });
        const ObjectOutcome refused = each.write(options);
        store->set_before_log_write({});
        ASSERT_TRUE(fired) << each.name;
        EXPECT_EQ(refused.status, ObjectStatus::IncarnationChanged) << each.name << ": " << refused.message;
        EXPECT_EQ(refused.incarnation, store->incarnation("store")) << each.name;
        EXPECT_NE(refused.incarnation, options.incarnation) << each.name;
        EXPECT_EQ(read(*store, "store", "acknowledged").status, ObjectStatus::NotFound) << each.name;
        each.unchanged();
        // With the current incarnation it goes through, and answers it.
        options.incarnation = store->incarnation("store");
        const ObjectOutcome accepted = each.write(options);
        EXPECT_TRUE(accepted.ok()) << each.name << ": " << accepted.message;
        EXPECT_EQ(accepted.incarnation, options.incarnation) << each.name;
        if (each.name == "remove") ASSERT_TRUE(store->put("store", "kept", "durable", durable).ok());
        if (each.name == "put range") ASSERT_TRUE(store->remove("store", "pack", durable).ok());
        if (each.name == "put range") ASSERT_TRUE(store->put_range("store", "pack", 0, "abc", durable).ok());
    }
    // Another start of the node is refused without touching the log.
    ObjectWrite other;
    other.incarnation = std::string(32, '0') + ".0";
    const ObjectOutcome restarted = store->put("store", "late2", "x", other);
    EXPECT_EQ(restarted.status, ObjectStatus::IncarnationChanged);
    EXPECT_EQ(restarted.incarnation, store->incarnation("store"));
    EXPECT_EQ(read(*store, "store", "late2").status, ObjectStatus::NotFound);
}

// lake#543: a barrier answers the incarnation read after its sync, and refuses
// when the bucket's log was reopened while it synced: the reopen may have
// dropped writes acknowledged before the barrier, which the sync of the
// reopened log never covered.
TEST_F(ObjectStoreFixture, ABarrierAcrossAReopenIsRefused) {
    std::atomic<bool> failing{false};
    store->set_fsync([&](const std::filesystem::path&, int descriptor) {
        if (failing.load()) {
            errno = EIO;
            return -1;
        }
        return ::fsync(descriptor);
    });
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    ASSERT_TRUE(store->put("store", "acknowledged", "unsynced", {}).ok());
    const std::string seen = store->incarnation("store");
    ObjectWrite durable;
    durable.durable = true;
    bool fired = false;
    store->set_before_barrier_sync([&] {
        if (std::exchange(fired, true)) return;
        failing = true;
        EXPECT_FALSE(store->put("store", "trigger", "x", durable).ok());  // fails the log
        failing = false;
        store->maintain(std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count() +
                        60'000);
        EXPECT_TRUE(store->failed_logs().empty());
    });
    const ObjectOutcome refused = store->barrier("store", seen);
    store->set_before_barrier_sync({});
    ASSERT_TRUE(fired);
    EXPECT_EQ(refused.status, ObjectStatus::IncarnationChanged) << refused.message;
    EXPECT_EQ(refused.incarnation, store->incarnation("store"));
    EXPECT_NE(refused.incarnation, seen);
    EXPECT_EQ(read(*store, "store", "acknowledged").status, ObjectStatus::NotFound);
    // The precondition alone refuses the old incarnation; the new one passes,
    // and the answer carries it.
    EXPECT_EQ(store->barrier("store", seen).status, ObjectStatus::IncarnationChanged);
    const ObjectOutcome synced = store->barrier("store", store->incarnation("store"));
    EXPECT_TRUE(synced.ok()) << synced.message;
    EXPECT_EQ(synced.incarnation, store->incarnation("store"));
}

// lake#543: a lease decision is on the deciding node's disk before it is
// answered. A crash can lose the replicated catalogue's last second; without
// the record, the lease would come back as it was before the acquire, the next
// acquirer would get the fence already handed out, and both holders could
// write. The bucket definition is restored here (it is not what is tested).
TEST_F(ObjectStoreFixture, ALeaseSurvivesACrashThatLosesTheCatalogue) {
    ASSERT_TRUE(store->create_bucket(bucket("lake_ws")).ok());
    const ObjectOutcome first = store->acquire("lake_ws", "lake-a", 50);
    ASSERT_TRUE(first.ok()) << first.message;
    ASSERT_TRUE(store->put("lake_ws", "k", "from a", fenced(first.lease->fence)).ok());
    // The crash: a new process whose catalogue lost the lease.
    store.reset();
    ReplicationManager lost{zetta::util::new_uuid()};
    store = std::make_unique<ObjectStore>(lost, lost.node_id(), dir.path() / "objects");
    ASSERT_TRUE(store->open());
    ASSERT_TRUE(store->create_bucket(bucket("lake_ws")).ok());
    const std::optional<zetta::data::ObjectLease> restored = store->lease("lake_ws");
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->holder, "lake-a");
    EXPECT_EQ(restored->fence, first.lease->fence);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));  // a's lease runs out
    const ObjectOutcome second = store->acquire("lake_ws", "lake-b", 60'000);
    ASSERT_TRUE(second.ok()) << second.message;
    EXPECT_GT(second.lease->fence, first.lease->fence);
    // The old holder never lands a write again.
    const ObjectOutcome stale = store->put("lake_ws", "k", "from a again", fenced(first.lease->fence));
    EXPECT_EQ(stale.status, ObjectStatus::Fenced);
    EXPECT_EQ(store->renew("lake_ws", "lake-a", first.lease->fence, 60'000).status, ObjectStatus::Fenced);
    EXPECT_TRUE(store->put("lake_ws", "k", "from b", fenced(second.lease->fence)).ok());
    EXPECT_EQ(read(*store, "lake_ws", "k").bytes, "from b");
    // A release is recorded too: after another crash the lease is free.
    ASSERT_TRUE(store->release("lake_ws", "lake-b", second.lease->fence).ok());
    store.reset();
    ReplicationManager lost_again{zetta::util::new_uuid()};
    store = std::make_unique<ObjectStore>(lost_again, lost_again.node_id(), dir.path() / "objects");
    ASSERT_TRUE(store->open());
    ASSERT_TRUE(store->create_bucket(bucket("lake_ws")).ok());
    const ObjectOutcome third = store->acquire("lake_ws", "lake-c", 60'000);
    ASSERT_TRUE(third.ok()) << third.message;
    EXPECT_GT(third.lease->fence, second.lease->fence);
    store.reset();
}

// Disk follows the live set: dead segments are deleted, live frames of a
// mostly-dead segment move forward, and everything reads back after a restart.
TEST_F(ObjectStoreFixture, CompactionKeepsDiskNearTheLiveSet) {
    reopen(/*segment_bytes=*/1024 * 1024);
    ASSERT_TRUE(store->create_bucket(bucket("store")).ok());
    std::map<std::string, std::string> live;
    std::mt19937_64 random(5);
    for (int round = 0; round < 400; ++round) {
        const std::string key = "k" + std::to_string(random() % 40);
        if (random() % 4 == 0 && live.contains(key)) {
            ASSERT_TRUE(store->remove("store", key, {}).ok());
            live.erase(key);
        } else {
            const std::string bytes = pattern(16 * 1024 + random() % 4096, random());
            ASSERT_TRUE(store->put("store", key, bytes, {}).ok());
            live[key] = bytes;
        }
    }
    std::uint64_t live_bytes = 0;
    for (const auto& [key, bytes] : live) live_bytes += bytes.size();
    const auto before = store->storage_stats();
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    for (int step = 0; step < 200; ++step) store->maintain(now);
    const auto after = store->storage_stats();
    std::printf("[object compaction] disk %.1f MiB -> %.1f MiB for %.1f MiB live, %llu -> %llu segments\n",
                before.disk_bytes / 1048576.0, after.disk_bytes / 1048576.0, live_bytes / 1048576.0,
                static_cast<unsigned long long>(before.segments),
                static_cast<unsigned long long>(after.segments));
    EXPECT_LT(after.disk_bytes, before.disk_bytes);
    EXPECT_LT(after.disk_bytes, 2 * live_bytes + 3 * 1024 * 1024);
    // Tombstones past their grace are purged, and stay purged across a restart.
    for (int step = 0; step < 50; ++step) store->maintain(now + ObjectStore::kTombstoneGraceMs + 1);
    EXPECT_EQ(store->storage_stats().tombstones, 0u);
    reopen(1024 * 1024);
    EXPECT_EQ(store->storage_stats().tombstones, 0u);
    for (const auto& [key, bytes] : live) {
        EXPECT_EQ(read(*store, "store", key).bytes, bytes) << key;
    }
    EXPECT_EQ(store->usage("store").objects, live.size());
}

namespace {

// One node of a real loopback cluster with a replicated catalogue.
struct ObjectClusterNode {
    ObjectClusterNode(std::uint16_t port, std::vector<std::string> seeds)
        : node_id(zetta::util::new_uuid()),
          replication(node_id),
          node(zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication),
          cluster(node_id, config(port, std::move(seeds)), &replication) {
        node.start();
    }
    ~ObjectClusterNode() {
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

    ObjectStore& store() { return *node.object_store(); }

    TempDir dir;
    std::string node_id;
    ReplicationManager replication;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
};

bool wait_for_mesh(const std::vector<ObjectClusterNode*>& nodes) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        const bool meshed = std::ranges::all_of(nodes, [&](ObjectClusterNode* node) {
            return node->cluster.peer_count() == nodes.size() - 1;
        });
        if (meshed) return true;
        for (ObjectClusterNode* node : nodes) node->node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

std::uint64_t held(const std::vector<ObjectClusterNode*>& nodes) {
    std::uint64_t total = 0;
    for (ObjectClusterNode* node : nodes) total += node->store().storage_stats().objects;
    return total;
}

}  // namespace

// rf = 2 on three nodes: every acknowledged write is on exactly two owners and
// readable from any node. A node is killed: the survivors repair to two copies
// each. A replacement joins: objects move to their new owners, old copies are
// dropped once the new owners hold them, and no acknowledged write is lost.
TEST(ObjectStoreCluster, RfTwoOnThreeNodesSurvivesALossAndAReplacement) {
    auto a = std::make_unique<ObjectClusterNode>(48851, std::vector<std::string>{});
    auto b = std::make_unique<ObjectClusterNode>(48852, std::vector<std::string>{"127.0.0.1:48851"});
    auto c = std::make_unique<ObjectClusterNode>(
        48853, std::vector<std::string>{"127.0.0.1:48851", "127.0.0.1:48852"});
    for (ObjectClusterNode* node : {a.get(), b.get(), c.get()}) node->cluster.start();
    ASSERT_TRUE(wait_for_mesh({a.get(), b.get(), c.get()}));
    for (ObjectClusterNode* node : {a.get(), b.get(), c.get()}) {
        node->node.attach_cluster(node->cluster, /*worker_count=*/1);
    }

    const ObjectOutcome created = a->store().create_bucket(bucket("lake_ws", std::nullopt, 2));
    ASSERT_TRUE(created.ok()) << created.message;
    EXPECT_EQ(created.replicas, 2u);
    ASSERT_TRUE(c->store().find_bucket("lake_ws"));

    // Writes through every node: puts, two-step appends, a large staged put.
    std::map<std::string, std::string> acknowledged;
    std::vector<ObjectClusterNode*> nodes{a.get(), b.get(), c.get()};
    for (int index = 0; index < 60; ++index) {
        ObjectStore& via = nodes[index % 3]->store();
        const std::string key = "packs/" + std::to_string(index);
        const std::string bytes = pattern(1000 + index * 37, index);
        if (index % 2 == 0) {
            const ObjectOutcome put = via.put("lake_ws", key, bytes, {});
            ASSERT_TRUE(put.ok()) << put.message;
            EXPECT_EQ(put.replicas, 2u);
        } else {
            ASSERT_TRUE(via.put_range("lake_ws", key, 0, bytes.substr(0, 500), {}).ok());
            const ObjectOutcome tail = nodes[(index + 1) % 3]->store().put_range(
                "lake_ws", key, 500, bytes.substr(500), {});
            ASSERT_TRUE(tail.ok()) << tail.message;
            EXPECT_EQ(tail.replicas, 2u);
        }
        acknowledged[key] = bytes;
    }
    const std::string large = pattern(9 * 1024 * 1024, 77);
    const ObjectOutcome staged = b->store().put("lake_ws", "index/large", large, {});
    ASSERT_TRUE(staged.ok()) << staged.message;
    acknowledged["index/large"] = large;
    EXPECT_EQ(held(nodes), 2 * acknowledged.size());
    for (ObjectClusterNode* node : nodes) {
        for (const auto& [key, bytes] : acknowledged) {
            EXPECT_EQ(read(node->store(), "lake_ws", key).bytes, bytes) << key;
        }
        EXPECT_EQ(node->store().list("lake_ws", "", "", 1000).objects.size(), acknowledged.size());
        EXPECT_EQ(node->store().usage("lake_ws").objects, acknowledged.size());
    }
    ASSERT_TRUE(c->store().barrier("lake_ws").ok());
    EXPECT_EQ(c->store().barrier("lake_ws").nodes, 3u);

    // Lose c. The survivors own everything between them and repair to two copies.
    c.reset();
    const auto alone = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((a->cluster.peer_count() != 1 || b->cluster.peer_count() != 1) &&
           std::chrono::steady_clock::now() < alone) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_EQ(a->cluster.peer_count(), 1u);
    for (int round = 0; round < 3; ++round) {
        a->store().anti_entropy();
        b->store().anti_entropy();
    }
    EXPECT_EQ(held({a.get(), b.get()}), 2 * acknowledged.size());
    // Writes keep working on the degraded cluster.
    for (int index = 60; index < 70; ++index) {
        const std::string key = "packs/" + std::to_string(index);
        const std::string bytes = pattern(800, index);
        const ObjectOutcome put = a->store().put("lake_ws", key, bytes, {});
        ASSERT_TRUE(put.ok()) << put.message;
        EXPECT_EQ(put.replicas, 2u);
        acknowledged[key] = bytes;
    }

    // A replacement joins; objects move to their new owners.
    auto d = std::make_unique<ObjectClusterNode>(
        48854, std::vector<std::string>{"127.0.0.1:48851", "127.0.0.1:48852"});
    d->cluster.start();
    ASSERT_TRUE(wait_for_mesh({a.get(), b.get(), d.get()}));
    const auto catalogue = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!d->store().find_bucket("lake_ws") && std::chrono::steady_clock::now() < catalogue) {
        d->node.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_TRUE(d->store().find_bucket("lake_ws"));
    d->node.attach_cluster(d->cluster, 1);
    nodes = {a.get(), b.get(), d.get()};
    // Two rounds: the new owner pulls, then the old copies go once it holds them.
    for (int round = 0; round < 2; ++round) {
        for (ObjectClusterNode* node : nodes) node->store().anti_entropy();
    }
    EXPECT_EQ(held(nodes), 2 * acknowledged.size());
    EXPECT_GT(d->store().storage_stats().objects, 0u);
    for (ObjectClusterNode* node : nodes) {
        for (const auto& [key, bytes] : acknowledged) {
            EXPECT_EQ(read(node->store(), "lake_ws", key).bytes, bytes) << key;
        }
    }

    // The lease fences writes cluster-wide, whichever node orders them.
    const ObjectOutcome lease = b->store().acquire("lake_ws", "lake-1", 60'000);
    ASSERT_TRUE(lease.ok()) << lease.message;
    for (ObjectClusterNode* node : nodes) {
        EXPECT_EQ(node->store().put("lake_ws", "packs/0", "x", {}).status, ObjectStatus::Fenced);
        EXPECT_TRUE(node->store().put("lake_ws", "packs/0", "x", fenced(lease.lease->fence)).ok());
    }
    const ObjectOutcome takeover = d->store().acquire("lake_ws", "lake-2", 60'000);
    EXPECT_EQ(takeover.status, ObjectStatus::Held);
    ASSERT_TRUE(a->store().release("lake_ws", "lake-1", lease.lease->fence).ok());
    const ObjectOutcome next = d->store().acquire("lake_ws", "lake-2", 60'000);
    ASSERT_TRUE(next.ok()) << next.message;
    for (ObjectClusterNode* node : nodes) {
        EXPECT_EQ(node->store().put("lake_ws", "packs/0", "y", fenced(lease.lease->fence)).status,
                  ObjectStatus::Fenced);
    }
    d.reset();
    b.reset();
    a.reset();
}
