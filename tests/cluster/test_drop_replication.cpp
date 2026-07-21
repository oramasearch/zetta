// Multi-node DROP convergence (#838).
//
// Single-node drop tests prove nothing about replication: a `remove` looks fine
// on the node that issued it right up until a peer's merge arrives. Until this
// file there was no multi-node drop test anywhere in the tree, so the behaviour
// of every removal path — `drop_table`, `drop_job`, `drop_index`, large-object
// `unlink`, dataset partitions — was simply unverified across nodes.
//
// The property under test is not "the key goes away", which the local call
// obviously achieves. It is:
//
//   1. the removal REACHES the peer, and
//   2. it STAYS removed on both nodes through subsequent gossip rounds, and
//   3. it survives a restart of a node whose disk projection still holds the
//      entity, and
//   4. it wins over a write the removing node had not yet seen.
//
// (4) is the case #838 was found on: a coordinator cleaning up a failed
// distributed ingest removes `part:`/`lo:` keys while participants are still
// writing their own, so the removal is concurrent with writes it cannot see.
// Under a CRDT map delete those writes survive the delete verbatim, and the key
// reappears — on the removing node too, before any tick.
//
// See docs/cluster/replication.md → "Removal replicates as a tombstone".

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "temp_dir.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

using zetta::Node;
using zetta::NodeConfig;
using zetta::cluster::Cluster;
using zetta::cluster::ClusterConfig;
using zetta::cluster::ReplicationManager;
using zetta::test::TempDir;

namespace {

NodeConfig config_for(const TempDir& tmp) {
    NodeConfig cfg;
    cfg.data_dir = tmp.path().string();
    return cfg;
}

// Patient (tests/CLAUDE.md): poll a seconds-long deadline, never a fixed small
// iteration count. Real Corosio TCP is in play, so the schedule is the machine's.
template <class Predicate>
bool converge(Node& a, Node& b, Predicate done) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        a.tick();
        b.tick();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    a.tick();
    b.tick();
    return done();
}

// Keep ticking for a while and require `holds` to be true at EVERY tick. This is
// the half of the test that matters: a resurrection shows up as a predicate that
// was true and then stopped being true, which a one-shot assertion cannot see.
template <class Predicate>
bool holds_through_gossip(Node& a, Node& b, Predicate holds) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        a.tick();
        b.tick();
        if (!holds()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return holds();
}

// The internal CRDT key scheme lives in src/core/catalog.hpp, which is not on the
// tests' include path — mirror the one key this file needs, as the other
// replication tests do.
std::string table_key(const std::string& table_id) { return "tbl:" + table_id; }

struct Pair {
    TempDir dir_a;
    TempDir dir_b;
    std::string id_a = zetta::util::new_uuid();
    std::string id_b = zetta::util::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};
    Node a{config_for(dir_a), &repl_a};
    Node b{config_for(dir_b), &repl_b};
    Cluster cluster_a;
    Cluster cluster_b;

    Pair(std::uint16_t port_a, std::uint16_t port_b)
        : cluster_a(id_a, make(port_a, 0), &repl_a),
          cluster_b(id_b, make(port_b, port_a), &repl_b) {
        a.start();
        b.start();
        cluster_a.start();
        cluster_b.start();
    }

    ~Pair() {
        cluster_b.stop();
        cluster_a.stop();
        a.stop();
        b.stop();
    }

    static ClusterConfig make(std::uint16_t port, std::uint16_t seed) {
        ClusterConfig cfg;
        cfg.listen_host = "127.0.0.1";
        cfg.listen_port = port;
        cfg.port_range = 1;
        if (seed != 0) cfg.seeds = {"127.0.0.1:" + std::to_string(seed)};
        return cfg;
    }
};

}  // namespace

// A table dropped on one node goes away on the other AND stays gone.
//
// Measured honestly: this one passes both before and after the tombstone change.
// The uncontended case — the peer holds exactly the write the remover could see —
// was already safe, because a map delete does name that write as a predecessor.
// It is a regression guard for the common path, not a reproduction of #838; the
// two tests below are the reproductions, and both fail without the fix.
TEST(DropReplication, ADroppedTableStaysDroppedOnBothNodes) {
    Pair pair(43350, 43351);

    auto db_a = pair.a.create_database("sales");
    ASSERT_TRUE(db_a);
    ASSERT_TRUE(db_a->create_table("orders"));

    ASSERT_TRUE(converge(pair.a, pair.b, [&] {
        auto db_b = pair.b.find_database("sales");
        return db_b && db_b->find_table("orders");
    })) << "the table never reached the peer";

    ASSERT_TRUE(db_a->drop_table("orders"));

    ASSERT_TRUE(converge(pair.a, pair.b, [&] {
        auto db_b = pair.b.find_database("sales");
        return db_b && !db_b->find_table("orders");
    })) << "the drop never reached the peer";

    // The real assertion: it does not come back on either node, and it is gone
    // from the replicated document itself — not merely from the projection.
    EXPECT_TRUE(holds_through_gossip(pair.a, pair.b, [&] {
        auto db_a_now = pair.a.find_database("sales");
        auto db_b_now = pair.b.find_database("sales");
        return db_a_now && db_b_now && !db_a_now->find_table("orders") &&
               !db_b_now->find_table("orders");
    })) << "the dropped table was resurrected by a peer's gossip merge";
}

// The #838 case itself: the removing node has NOT seen the peer's write of the
// key it is removing. A map delete carries only the predecessors it can see, so
// the unseen write survives it and the key returns everywhere. A tombstone is an
// ordinary value change, and removal wins over a concurrent write.
TEST(DropReplication, ARemovalWinsOverAWriteItNeverSaw) {
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};

    // Both nodes share a starting point, then B writes a key A never merges.
    repl_a.set("seed", nlohmann::json{{"v", 1}});
    repl_b.merge(repl_a.save());
    repl_b.set("part:one", nlohmann::json{{"table", "t"}, {"owner", id_b}});

    // A removes it without ever having seen it — exactly what a coordinator does
    // when it cleans up after a participant that is still writing.
    repl_a.remove("part:one");

    // Gossip both ways, repeatedly, in both orders.
    for (int round = 0; round < 3; ++round) {
        repl_a.merge(repl_b.save());
        repl_b.merge(repl_a.save());
    }

    EXPECT_FALSE(repl_a.snapshot().contains("part:one"))
        << "the key came back on the node that removed it";
    EXPECT_FALSE(repl_b.snapshot().contains("part:one"))
        << "the removal never took effect on the peer";
    EXPECT_EQ(repl_a.version(), repl_b.version());
}

// A write CAUSALLY AFTER the removal is a deliberate re-create, not a
// resurrection, and must be honoured — otherwise a tombstone would make a name
// unusable forever.
TEST(DropReplication, AWriteAfterTheRemovalRecreatesTheKey) {
    const std::string id_a = zetta::util::new_uuid();
    const std::string id_b = zetta::util::new_uuid();
    ReplicationManager repl_a{id_a};
    ReplicationManager repl_b{id_b};

    repl_a.set("lo:one", nlohmann::json{{"name", "body"}});
    repl_b.merge(repl_a.save());
    repl_a.remove("lo:one");
    repl_b.merge(repl_a.save());
    ASSERT_FALSE(repl_b.snapshot().contains("lo:one"));

    // B has seen the tombstone; its write is causally after it.
    repl_b.set("lo:one", nlohmann::json{{"name", "rebuilt"}});
    repl_a.merge(repl_b.save());

    EXPECT_TRUE(repl_a.snapshot().contains("lo:one"));
    EXPECT_EQ(repl_a.snapshot()["lo:one"]["name"], "rebuilt");
    EXPECT_TRUE(repl_b.snapshot().contains("lo:one"));
}

// A restart is the other resurrection route. migrate_to_replication re-captures
// local projections the document does not hold, and a removed key is absent from
// the snapshot by construction — so without knows_key() a node whose disk still
// held the entity would put it straight back on the next start.
TEST(DropReplication, ARestartDoesNotRecaptureADroppedEntity) {
    TempDir dir;
    const std::string node_id = zetta::util::new_uuid();
    std::string table_id;

    {
        ReplicationManager repl{node_id};
        Node node{config_for(dir), &repl};
        node.start();
        auto db = node.create_database("sales");
        ASSERT_TRUE(db);
        auto table = db->create_table("orders");
        ASSERT_TRUE(table);
        table_id = table->id();
        node.tick();

        // Drop through the document only, leaving the on-disk projection behind —
        // which is precisely the state a node is in when a peer dropped the table
        // and this node has not reconciled yet.
        repl.remove(table_key(table_id));
        node.stop();
    }

    ReplicationManager repl{node_id};
    Node node{config_for(dir), &repl};
    node.start();
    node.tick();

    EXPECT_FALSE(repl.snapshot().contains(table_key(table_id)))
        << "a restart re-captured a table that had been dropped";
    auto db = node.find_database("sales");
    ASSERT_TRUE(db);
    EXPECT_FALSE(db->find_table("orders"))
        << "the dropped table came back on disk after a restart";
    node.stop();
}
