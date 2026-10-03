// The keyed record store (#1231, docs/data/record-store.md): conditional writes,
// secondary-index lists, retention, the journal, the revision register, the
// two-node linearizable path, and the latency budget.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/record_store.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

namespace {

using zetta::cluster::ReplicationManager;
using zetta::data::RecordCollection;
using zetta::data::RecordCondition;
using zetta::data::RecordListRequest;
using zetta::data::RecordOutcome;
using zetta::data::RecordStatus;
using zetta::data::RecordStore;
using zetta::data::RecordWrite;
using zetta::test::TempDir;

std::string base64(std::string_view text) {
    static constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    std::size_t position = 0;
    while (position + 2 < text.size()) {
        const unsigned triple = (static_cast<unsigned char>(text[position]) << 16) |
                                (static_cast<unsigned char>(text[position + 1]) << 8) |
                                static_cast<unsigned char>(text[position + 2]);
        for (int shift = 18; shift >= 0; shift -= 6) out.push_back(kAlphabet[(triple >> shift) & 63]);
        position += 3;
    }
    const std::size_t remaining = text.size() - position;
    if (remaining > 0) {
        unsigned triple = static_cast<unsigned char>(text[position]) << 16;
        if (remaining == 2) triple |= static_cast<unsigned char>(text[position + 1]) << 8;
        out.push_back(kAlphabet[(triple >> 18) & 63]);
        out.push_back(kAlphabet[(triple >> 12) & 63]);
        out.push_back(remaining == 2 ? kAlphabet[(triple >> 6) & 63] : '=');
        out.push_back('=');
    }
    return out;
}

std::string unbase64(std::string_view text) {
    std::string out;
    unsigned accumulator = 0;
    int bits = 0;
    for (const char character : text) {
        int value = -1;
        if (character >= 'A' && character <= 'Z') value = character - 'A';
        if (character >= 'a' && character <= 'z') value = character - 'a' + 26;
        if (character >= '0' && character <= '9') value = character - '0' + 52;
        if (character == '+') value = 62;
        if (character == '/') value = 63;
        if (value < 0) continue;
        accumulator = (accumulator << 6) | static_cast<unsigned>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((accumulator >> bits) & 0xff));
        }
    }
    return out;
}

RecordWrite put(std::string collection, std::string key, std::string value,
                std::map<std::string, std::string> index = {}, std::string order = {}) {
    RecordWrite write;
    write.collection = std::move(collection);
    write.key = std::move(key);
    write.value = base64(value);
    write.index = std::move(index);
    write.order = std::move(order);
    return write;
}

RecordCollection collection(std::string name, std::vector<std::string> indexes = {},
                            std::optional<std::int64_t> retention_ms = std::nullopt) {
    RecordCollection definition;
    definition.name = std::move(name);
    definition.indexes = std::move(indexes);
    definition.retention_ms = retention_ms;
    return definition;
}

std::vector<std::string> keys_of(const zetta::data::RecordPage& page) {
    std::vector<std::string> keys;
    for (const auto& record : page.records) keys.push_back(record.key);
    return keys;
}

struct StoreFixture : ::testing::Test {
    TempDir dir;
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore store{replication, replication.node_id(), dir.path() / "records.journal"};
};

}  // namespace

TEST_F(StoreFixture, ConditionalWritesFollowTheLiveRevision) {
    ASSERT_TRUE(store.create_collection(collection("sessions")).ok());

    RecordWrite first = put("sessions", "s-1", "one");
    first.condition = RecordCondition::IfAbsent;
    const RecordOutcome created = store.write(first);
    ASSERT_TRUE(created.ok()) << created.message;
    const std::uint64_t revision = created.record->revision;
    EXPECT_GT(revision, 0u);
    EXPECT_EQ(created.owner, replication.node_id());

    // A second if_absent is a conflict that names the live revision.
    const RecordOutcome again = store.write(first);
    EXPECT_EQ(again.status, RecordStatus::Conflict);
    ASSERT_TRUE(again.record);
    EXPECT_EQ(again.record->revision, revision);

    RecordWrite stale = put("sessions", "s-1", "two");
    stale.condition = RecordCondition::IfRevision;
    stale.revision = revision + 1;
    EXPECT_EQ(store.write(stale).status, RecordStatus::Conflict);

    RecordWrite current = put("sessions", "s-1", "two");
    current.condition = RecordCondition::IfRevision;
    current.revision = revision;
    const RecordOutcome updated = store.write(current);
    ASSERT_TRUE(updated.ok()) << updated.message;
    EXPECT_GT(updated.record->revision, revision);
    // The same revision cannot win twice: a one-use code is consumed once.
    EXPECT_EQ(store.write(current).status, RecordStatus::Conflict);

    const RecordOutcome read = store.get("sessions", "s-1");
    ASSERT_TRUE(read.ok());
    EXPECT_EQ(unbase64(read.record->value), "two");
    EXPECT_EQ(read.record->revision, updated.record->revision);

    // Delete at a stale revision is refused; at the live one it removes.
    RecordWrite remove;
    remove.collection = "sessions";
    remove.key = "s-1";
    remove.remove = true;
    remove.condition = RecordCondition::IfRevision;
    remove.revision = revision;
    EXPECT_EQ(store.write(remove).status, RecordStatus::Conflict);
    remove.revision = updated.record->revision;
    const RecordOutcome deleted = store.write(remove);
    ASSERT_TRUE(deleted.ok()) << deleted.message;
    EXPECT_EQ(store.get("sessions", "s-1").status, RecordStatus::NotFound);
    remove.condition = RecordCondition::None;
    EXPECT_EQ(store.write(remove).status, RecordStatus::NotFound);

    // Re-creating the key never reuses a revision a caller may still hold.
    const RecordOutcome recreated = store.write(first);
    ASSERT_TRUE(recreated.ok()) << recreated.message;
    EXPECT_GT(recreated.record->revision, deleted.record->revision);
}

TEST_F(StoreFixture, RefusesBadInputAndUnknownCollectionsAndIndexes) {
    EXPECT_EQ(store.write(put("missing", "k", "v")).status, RecordStatus::NotFound);
    EXPECT_EQ(store.get("missing", "k").status, RecordStatus::NotFound);
    ASSERT_TRUE(store.create_collection(collection("things", {"owner"})).ok());
    EXPECT_EQ(store.write(put("things", "k", "v", {{"colour", "red"}})).status,
              RecordStatus::InvalidInput);
    RecordWrite bad_value = put("things", "k", "v");
    bad_value.value = "not base64!";
    EXPECT_EQ(store.write(bad_value).status, RecordStatus::InvalidInput);
    EXPECT_EQ(store.write(put("things", std::string("a\nb"), "v")).status,
              RecordStatus::InvalidInput);
    EXPECT_EQ(store.write(put("Things", "k", "v")).status, RecordStatus::InvalidInput);

    // Declaring the same collection again is idempotent; differently, a conflict.
    const RecordOutcome same = store.create_collection(collection("things", {"owner"}));
    ASSERT_TRUE(same.ok());
    EXPECT_FALSE(same.created);
    EXPECT_EQ(store.create_collection(collection("things", {"other"})).status,
              RecordStatus::Conflict);
    EXPECT_EQ(store.create_collection(collection("bad", {"dup", "dup"})).status,
              RecordStatus::InvalidInput);
}

TEST_F(StoreFixture, ListsOneIndexValueInOrderAPageAtATime) {
    ASSERT_TRUE(store.create_collection(collection("messages", {"conversation"})).ok());
    // Two conversations; written out of order so the order column does the work.
    for (const int at : {5, 1, 4, 2, 3}) {
        const std::string order = "000" + std::to_string(at);
        ASSERT_TRUE(store.write(put("messages", "a-" + std::to_string(at), "m",
                                    {{"conversation", "a"}}, order))
                        .ok());
        ASSERT_TRUE(store.write(put("messages", "b-" + std::to_string(at), "m",
                                    {{"conversation", "b"}}, order))
                        .ok());
    }
    RecordListRequest request;
    request.collection = "messages";
    request.index = "conversation";
    request.value = "a";
    request.limit = 2;
    auto page = store.list(request);
    ASSERT_TRUE(page.ok()) << page.message;
    EXPECT_EQ(keys_of(page), (std::vector<std::string>{"a-1", "a-2"}));
    ASSERT_FALSE(page.next_after.empty());
    request.after = page.next_after;
    page = store.list(request);
    EXPECT_EQ(keys_of(page), (std::vector<std::string>{"a-3", "a-4"}));
    request.after = page.next_after;
    page = store.list(request);
    EXPECT_EQ(keys_of(page), (std::vector<std::string>{"a-5"}));
    EXPECT_TRUE(page.next_after.empty());

    request.after.clear();
    request.descending = true;
    request.limit = 3;
    page = store.list(request);
    EXPECT_EQ(keys_of(page), (std::vector<std::string>{"a-5", "a-4", "a-3"}));
    request.after = page.next_after;
    page = store.list(request);
    EXPECT_EQ(keys_of(page), (std::vector<std::string>{"a-2", "a-1"}));

    // Moving a record to another index value moves it between listings, and the
    // stale entry is never returned.
    ASSERT_TRUE(store.write(put("messages", "a-3", "moved", {{"conversation", "b"}}, "0003")).ok());
    request = {};
    request.collection = "messages";
    request.index = "conversation";
    request.value = "a";
    EXPECT_EQ(keys_of(store.list(request)),
              (std::vector<std::string>{"a-1", "a-2", "a-4", "a-5"}));
    request.value = "b";
    const auto b = store.list(request);
    ASSERT_EQ(b.records.size(), 6u);
    EXPECT_EQ(b.records[2].key, "a-3");  // order 0003 sorts before b-3 by key
    EXPECT_EQ(b.records[3].key, "b-3");

    // Without an index: every record by key, paged.
    RecordListRequest all;
    all.collection = "messages";
    all.limit = 4;
    std::vector<std::string> seen;
    for (int pages = 0; pages < 10; ++pages) {
        const auto chunk = store.list(all);
        ASSERT_TRUE(chunk.ok());
        for (const auto& record : chunk.records) seen.push_back(record.key);
        if (chunk.next_after.empty()) break;
        all.after = chunk.next_after;
    }
    EXPECT_EQ(seen.size(), 10u);
    EXPECT_TRUE(std::ranges::is_sorted(seen));

    request.index = "nope";
    EXPECT_EQ(store.list(request).status, RecordStatus::InvalidInput);
}

TEST_F(StoreFixture, RetentionHidesThenSweepRemovesExpiredRecords) {
    ASSERT_TRUE(store.create_collection(collection("codes", {"user"}, 40)).ok());
    ASSERT_TRUE(store.write(put("codes", "c-1", "x", {{"user", "u"}})).ok());
    const RecordOutcome fresh = store.get("codes", "c-1");
    ASSERT_TRUE(fresh.ok());
    ASSERT_TRUE(fresh.record->expires_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    EXPECT_EQ(store.get("codes", "c-1").status, RecordStatus::NotFound);
    RecordListRequest request;
    request.collection = "codes";
    request.index = "user";
    request.value = "u";
    EXPECT_TRUE(store.list(request).records.empty());

    // An expired key is free for if_absent again before any sweep.
    RecordWrite again = put("codes", "c-2", "x", {{"user", "u"}});
    ASSERT_TRUE(store.write(again).ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    again.condition = RecordCondition::IfAbsent;
    ASSERT_TRUE(store.write(again).ok());

    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    store.sweep(now + 1000);
    const auto view = replication.shared_snapshot();
    for (const auto& [key, value] : view->items()) {
        EXPECT_FALSE(key.starts_with("record:codes:")) << key;
        EXPECT_FALSE(key.starts_with("record_index:codes:")) << key;
    }
}

TEST_F(StoreFixture, DeletionTombstonesArePurgedAfterTheGracePeriod) {
    ASSERT_TRUE(store.create_collection(collection("things", {"owner"})).ok());
    ASSERT_TRUE(store.write(put("things", "t", "x", {{"owner", "o"}})).ok());
    RecordWrite remove;
    remove.collection = "things";
    remove.key = "t";
    remove.remove = true;
    ASSERT_TRUE(store.write(remove).ok());
    EXPECT_TRUE(replication.shared_snapshot()->contains("record:things:t"));
    const std::int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::system_clock::now().time_since_epoch())
                                 .count();
    store.sweep(now);  // inside the grace period: kept
    EXPECT_TRUE(replication.shared_snapshot()->contains("record:things:t"));
    store.sweep(now + RecordStore::kTombstoneGraceMs + 1000);
    EXPECT_FALSE(replication.shared_snapshot()->contains("record:things:t"));
}

TEST_F(StoreFixture, ConcurrentConditionalIncrementsNeverLoseAnUpdate) {
    ASSERT_TRUE(store.create_collection(collection("counters")).ok());
    ASSERT_TRUE(store.write(put("counters", "c", "0")).ok());
    constexpr int kThreads = 8;
    constexpr int kIncrements = 40;
    std::atomic<int> conflicts{0};
    std::mutex mutex;
    std::vector<std::uint64_t> revisions;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&] {
            for (int done = 0; done < kIncrements;) {
                const RecordOutcome current = store.get("counters", "c");
                ASSERT_TRUE(current.ok());
                RecordWrite next =
                    put("counters", "c",
                        std::to_string(std::stoi(unbase64(current.record->value)) + 1));
                next.condition = RecordCondition::IfRevision;
                next.revision = current.record->revision;
                const RecordOutcome written = store.write(next);
                if (written.status == RecordStatus::Conflict) {
                    ++conflicts;
                    continue;
                }
                ASSERT_TRUE(written.ok()) << written.message;
                const std::lock_guard<std::mutex> lock(mutex);
                revisions.push_back(written.record->revision);
                ++done;
            }
        });
    }
    for (auto& thread : threads) thread.join();
    const RecordOutcome final_value = store.get("counters", "c");
    ASSERT_TRUE(final_value.ok());
    EXPECT_EQ(unbase64(final_value.record->value), std::to_string(kThreads * kIncrements));
    // Every acknowledged write got its own revision.
    std::set<std::uint64_t> unique(revisions.begin(), revisions.end());
    EXPECT_EQ(unique.size(), revisions.size());
    std::printf("[record store] %d conditional increments, %d conflicts retried\n",
                kThreads * kIncrements, conflicts.load());
}

TEST_F(StoreFixture, TheJournalRestoresAcknowledgedWritesAfterACrash) {
    ASSERT_TRUE(store.create_collection(collection("sessions", {"user"})).ok());
    ASSERT_TRUE(store.write(put("sessions", "s-1", "one", {{"user", "u"}})).ok());
    ASSERT_TRUE(store.write(put("sessions", "s-2", "two", {{"user", "u"}})).ok());
    const std::uint64_t mark = store.journal_mark();
    EXPECT_GT(mark, 0u);

    // A crash before the document was saved: a fresh document, the same journal.
    ReplicationManager restarted{replication.node_id()};
    RecordStore recovered{restarted, restarted.node_id(), dir.path() / "records.journal"};
    EXPECT_EQ(recovered.replay_journal(), 3u);  // the collection and two records
    const RecordOutcome read = recovered.get("sessions", "s-2");
    ASSERT_TRUE(read.ok()) << read.message;
    EXPECT_EQ(unbase64(read.record->value), "two");
    RecordListRequest request;
    request.collection = "sessions";
    request.index = "user";
    request.value = "u";
    EXPECT_EQ(keys_of(recovered.list(request)), (std::vector<std::string>{"s-1", "s-2"}));
    // Replaying twice changes nothing.
    EXPECT_EQ(recovered.replay_journal(), 0u);

    // Once the document holding them is saved, the journal drops those entries
    // and keeps anything appended after the mark.
    ASSERT_TRUE(store.write(put("sessions", "s-3", "three")).ok());
    store.journal_truncate(mark);
    ReplicationManager later{replication.node_id()};
    RecordStore tail{later, later.node_id(), dir.path() / "records.journal"};
    EXPECT_EQ(tail.replay_journal(), 1u);
}

TEST(RecordStoreNode, PersistingTheDocumentEmptiesTheJournalAndSurvivesRestart) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    {
        ReplicationManager replication{node_id};
        zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                         &replication};
        ASSERT_TRUE(node.start());
        ASSERT_NE(node.record_store(), nullptr);
        ASSERT_TRUE(node.record_store()->create_collection(collection("sessions")).ok());
        ASSERT_TRUE(node.record_store()->write(put("sessions", "s", "kept")).ok());
        EXPECT_GT(std::filesystem::file_size(dir.path() / "records.journal"), 0u);
        node.stop();  // persists the document, then trims the journal
        EXPECT_EQ(std::filesystem::file_size(dir.path() / "records.journal"), 0u);
    }
    ReplicationManager replication{node_id};
    zetta::Node node{zetta::NodeConfig{dir.path().string(), /*preflight=*/false},
                     &replication};
    ASSERT_TRUE(node.start());
    const RecordOutcome read = node.record_store()->get("sessions", "s");
    ASSERT_TRUE(read.ok()) << read.message;
    EXPECT_EQ(unbase64(read.record->value), "kept");
    node.stop();
}

// The revision register: whichever value the CRDT would pick, every replica
// resolves concurrent copies of a record to the highest revision.
TEST(RecordRevisionRegister, ConcurrentCopiesResolveToTheHighestRevision) {
    for (const bool first_is_newer : {false, true}) {
        ReplicationManager left{zetta::util::new_uuid()};
        ReplicationManager right{zetta::util::new_uuid()};
        const nlohmann::json older{{"rev", 5}, {"updated", 1}, {"value", "b2xk"}};
        const nlohmann::json newer{{"rev", 9}, {"updated", 2}, {"value", "bmV3"}};
        left.set("record:c:k", first_is_newer ? newer : older);
        right.set("record:c:k", first_is_newer ? older : newer);
        // An unrelated key keeps the CRDT's own winner rule.
        left.set("other:k", nlohmann::json{{"rev", 9}});
        right.set("other:k", nlohmann::json{{"rev", 5}});
        left.merge(right.save());
        right.merge(left.save());
        EXPECT_EQ(left.shared_snapshot()->at("record:c:k").at("rev"), 9);
        EXPECT_EQ(right.shared_snapshot()->at("record:c:k").at("rev"), 9);
        EXPECT_EQ(left.shared_snapshot()->at("other:k"), right.shared_snapshot()->at("other:k"));
        // After a restore from the saved bytes, too.
        ReplicationManager restored{left.node_id()};
        ASSERT_TRUE(restored.restore(left.save()));
        EXPECT_EQ(restored.shared_snapshot()->at("record:c:k").at("rev"), 9);
    }
}

TEST(RecordRevisionRegister, UpdateIsAtomicAgainstConcurrentWriters) {
    ReplicationManager replication{zetta::util::new_uuid()};
    replication.set("counter", nlohmann::json{{"n", 0}});
    constexpr int kThreads = 8;
    constexpr int kIncrements = 500;
    std::vector<std::thread> threads;
    for (int thread = 0; thread < kThreads; ++thread) {
        threads.emplace_back([&] {
            for (int done = 0; done < kIncrements; ++done) {
                replication.update(
                    "counter",
                    [](const nlohmann::json* current)
                        -> std::optional<std::vector<zetta::cluster::ReplicationWrite>> {
                        const int value = current ? current->at("n").get<int>() : 0;
                        return std::vector<zetta::cluster::ReplicationWrite>{
                            {"counter", nlohmann::json{{"n", value + 1}}}};
                    });
            }
        });
    }
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(replication.shared_snapshot()->at("counter").at("n"), kThreads * kIncrements);
}

namespace {

// One node of a real two-node loopback cluster with a replicated catalogue.
struct RecordClusterNode {
    RecordClusterNode(std::uint16_t port, std::vector<std::string> seeds)
        : node_id(zetta::util::new_uuid()),
          replication(node_id),
          node(zetta::NodeConfig{dir.path().string(), /*preflight=*/false}, &replication),
          cluster(node_id, config(port, std::move(seeds)), &replication) {
        node.start();
    }
    ~RecordClusterNode() {
        node.stop();
        cluster.stop();
    }

    static zetta::cluster::ClusterConfig config(std::uint16_t port,
                                                std::vector<std::string> seeds) {
        zetta::cluster::ClusterConfig config;
        config.listen_host = "127.0.0.1";
        config.listen_port = port;
        config.port_range = 1;
        config.seeds = std::move(seeds);
        return config;
    }

    RecordStore& store() { return *node.record_store(); }

    TempDir dir;
    std::string node_id;
    ReplicationManager replication;
    zetta::Node node;
    zetta::cluster::Cluster cluster;
};

}  // namespace

// Two nodes, one key, conditional increments from threads on BOTH nodes: every
// write is ordered by the key's owner (a write on the other node is forwarded),
// none is lost, and an acknowledged write is readable on the other node at once.
TEST(RecordStoreCluster, ConditionalWritesAreLinearizableAcrossTwoNodes) {
    auto first = std::make_unique<RecordClusterNode>(48731, std::vector<std::string>{});
    auto second = std::make_unique<RecordClusterNode>(
        48732, std::vector<std::string>{"127.0.0.1:48731"});
    first->cluster.start();
    second->cluster.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((first->cluster.peer_count() != 1 || second->cluster.peer_count() != 1) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    ASSERT_EQ(first->cluster.peer_count(), 1u);
    ASSERT_EQ(second->cluster.peer_count(), 1u);
    first->node.attach_cluster(first->cluster, /*worker_count=*/1);
    second->node.attach_cluster(second->cluster, /*worker_count=*/1);

    // The collection created on one node is on the other before it returns.
    const RecordOutcome created = first->store().create_collection(collection("counters"));
    ASSERT_TRUE(created.ok()) << created.message;
    EXPECT_EQ(created.replicas_expected, 1u);
    EXPECT_EQ(created.replicas, 1u);
    ASSERT_TRUE(second->store().find_collection("counters"));

    // Both nodes agree on who owns the key.
    EXPECT_EQ(first->store().owner_of("counters", "c"), second->store().owner_of("counters", "c"));

    const RecordOutcome seeded = second->store().write(put("counters", "c", "0"));
    ASSERT_TRUE(seeded.ok()) << seeded.message;
    EXPECT_EQ(seeded.replicas, 1u);
    ASSERT_TRUE(first->store().get("counters", "c").ok());

    constexpr int kThreadsPerNode = 3;
    constexpr int kIncrements = 15;
    std::atomic<int> conflicts{0};
    std::atomic<int> stale_reads{0};
    std::vector<std::thread> threads;
    std::vector<RecordClusterNode*> nodes{first.get(), second.get()};
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        for (int thread = 0; thread < kThreadsPerNode; ++thread) {
            threads.emplace_back([&, index] {
                RecordStore& mine = nodes[index]->store();
                RecordStore& other = nodes[1 - index]->store();
                for (int done = 0; done < kIncrements;) {
                    const RecordOutcome current = mine.get("counters", "c");
                    ASSERT_TRUE(current.ok()) << current.message;
                    RecordWrite next = put(
                        "counters", "c",
                        std::to_string(std::stoi(unbase64(current.record->value)) + 1));
                    next.condition = RecordCondition::IfRevision;
                    next.revision = current.record->revision;
                    const RecordOutcome written = mine.write(next);
                    if (written.status == RecordStatus::Conflict) {
                        ++conflicts;
                        continue;
                    }
                    ASSERT_TRUE(written.ok()) << written.message;
                    ++done;
                    // Read-after-acknowledge on the OTHER node.
                    const RecordOutcome there = other.get("counters", "c");
                    if (!there.ok() || there.record->revision < written.record->revision) {
                        ++stale_reads;
                    }
                }
            });
        }
    }
    for (auto& thread : threads) thread.join();
    constexpr int kTotal = 2 * kThreadsPerNode * kIncrements;
    for (RecordClusterNode* node : nodes) {
        const RecordOutcome final_value = node->store().get("counters", "c");
        ASSERT_TRUE(final_value.ok());
        EXPECT_EQ(unbase64(final_value.record->value), std::to_string(kTotal));
    }
    EXPECT_EQ(stale_reads.load(), 0);
    std::printf("[record store] two nodes: %d increments, %d conflicts retried\n", kTotal,
                conflicts.load());
    second.reset();
    first.reset();
}

// The latency budget (#1231: get_record and list_records within 10 ms at
// realistic size). A workspace's message log: 500 conversations of 100 messages,
// each a ~1 KiB sealed body — 50,000 records and 50,000 index entries in the
// replicated catalogue.
TEST(RecordStoreBenchmark, GetAndListStayInsideTheTenMillisecondBudget) {
    ReplicationManager replication{zetta::util::new_uuid()};
    RecordStore store{replication, replication.node_id(), {}};
    ASSERT_TRUE(store.create_collection(collection("ask_messages", {"conversation"})).ok());
    constexpr int kConversations = 500;
    constexpr int kMessages = 100;
    const std::string body = base64(std::string(768, 'x'));  // 1 KiB of base64
    const auto load_started = std::chrono::steady_clock::now();
    for (int conversation = 0; conversation < kConversations; ++conversation) {
        for (int message = 0; message < kMessages; ++message) {
            RecordWrite write;
            write.collection = "ask_messages";
            write.key = zetta::util::new_uuid();
            write.value = body;
            write.index = {{"conversation", "conversation-" + std::to_string(conversation)}};
            char order[24];
            std::snprintf(order, sizeof(order), "%016d", message);
            write.order = order;
            ASSERT_TRUE(store.write(write).ok());
        }
    }
    const double load_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - load_started).count();

    std::vector<std::string> keys;
    {
        RecordListRequest request;
        request.collection = "ask_messages";
        request.index = "conversation";
        request.value = "conversation-7";
        for (const auto& record : store.list(request).records) keys.push_back(record.key);
    }
    ASSERT_EQ(keys.size(), 100u);

    const auto time_ms = [](auto&& work) {
        const auto started = std::chrono::steady_clock::now();
        work();
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                         started)
            .count();
    };
    std::vector<double> gets;
    std::vector<double> lists;
    for (int sample = 0; sample < 400; ++sample) {
        const std::string& key = keys[static_cast<std::size_t>(sample) % keys.size()];
        gets.push_back(time_ms([&] { ASSERT_TRUE(store.get("ask_messages", key).ok()); }));
        RecordListRequest request;
        request.collection = "ask_messages";
        request.index = "conversation";
        request.value = "conversation-" + std::to_string((sample * 37) % kConversations);
        request.limit = 50;
        request.descending = sample % 2 == 1;
        lists.push_back(time_ms([&] { ASSERT_EQ(store.list(request).records.size(), 50u); }));
    }
    // Four concurrent readers against the same catalogue.
    std::vector<double> concurrent(4, 0.0);
    {
        std::vector<std::thread> readers;
        for (std::size_t reader = 0; reader < concurrent.size(); ++reader) {
            readers.emplace_back([&, reader] {
                for (int sample = 0; sample < 100; ++sample) {
                    RecordListRequest request;
                    request.collection = "ask_messages";
                    request.index = "conversation";
                    request.value = "conversation-" + std::to_string(sample);
                    request.limit = 100;
                    const double elapsed = time_ms([&] {
                        store.list(request);
                        store.get("ask_messages", keys[static_cast<std::size_t>(sample)]);
                    });
                    concurrent[reader] = std::max(concurrent[reader], elapsed);
                }
            });
        }
        for (auto& reader : readers) reader.join();
    }
    const auto percentile = [](std::vector<double> samples, double fraction) {
        std::ranges::sort(samples);
        return samples[static_cast<std::size_t>(fraction * static_cast<double>(samples.size() - 1))];
    };
    const double worst_concurrent = *std::ranges::max_element(concurrent);
    std::printf(
        "[record store] %d records loaded in %.1f s; get median %.3f ms p99 %.3f ms; "
        "list(50) median %.3f ms p99 %.3f ms; worst list(100)+get across 4 readers %.3f ms\n",
        kConversations * kMessages, load_seconds, percentile(gets, 0.5),
        percentile(gets, 0.99), percentile(lists, 0.5), percentile(lists, 0.99),
        worst_concurrent);
    EXPECT_LT(percentile(gets, 0.5), 10.0);
    EXPECT_LT(percentile(gets, 0.99), 10.0);
    EXPECT_LT(percentile(lists, 0.5), 10.0);
    EXPECT_LT(percentile(lists, 0.99), 10.0);
    EXPECT_LT(worst_concurrent, 50.0);
}
