#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "temp_dir.hpp"
#include "zetta/cluster/replication.hpp"
#include "zetta/governance/audit_event.hpp"
#include "zetta/node.hpp"
#include "zetta/util/uuid.hpp"

using zetta::cluster::ReplicationManager;
using zetta::cluster::ReplicationPolicy;

namespace {

zetta::NodeConfig config_for(const zetta::test::TempDir& temporary_directory) {
    zetta::NodeConfig config;
    config.data_dir = temporary_directory.path().string();
    return config;
}

nlohmann::json audit_value(std::string target) {
    zetta::governance::AuditEventValue value;
    value.target = std::move(target);
    value.time = "2026-09-27T00:00:00Z";
    return zetta::governance::to_json(value);
}

}  // namespace

TEST(Replication, DefaultPolicyReplicatesToAllNodes) {
    ReplicationManager manager{zetta::util::new_uuid()};
    EXPECT_EQ(manager.policy(), ReplicationPolicy::ReplicateToAllNodes);
}

TEST(Replication, MergePropagatesState) {
    ReplicationManager source{zetta::util::new_uuid()};
    source.set("title", "hello");
    source.set("count", 3);
    source.set("nested", nlohmann::json{{"a", 1}, {"b", 2}});

    ReplicationManager replica{zetta::util::new_uuid()};
    replica.merge(source.save());

    const nlohmann::json snapshot = replica.snapshot();
    EXPECT_EQ(snapshot["title"], "hello");
    EXPECT_EQ(snapshot["count"], 3);
    EXPECT_EQ(snapshot["nested"]["b"], 2);
}

TEST(Replication, SharedSnapshotsAreImmutableAcrossWrites) {
    ReplicationManager manager{zetta::util::new_uuid()};
    manager.set("one", 1);
    const auto before = manager.shared_snapshot();

    manager.set("two", 2);
    const auto after = manager.shared_snapshot();

    ASSERT_TRUE(before);
    ASSERT_TRUE(after);
    EXPECT_EQ(before->size(), 1u);
    EXPECT_FALSE(before->contains("two"));
    EXPECT_EQ(after->at("two"), 2);
}

TEST(Replication, ChangesTrackLocalWritesAndRemovals) {
    ReplicationManager manager{zetta::util::new_uuid()};
    const auto initial = manager.changes_since(0);
    ASSERT_TRUE(initial.full_resync);
    ASSERT_TRUE(initial.snapshot);

    manager.set("one", 1);
    manager.set("two", 2);
    const auto writes = manager.changes_since(initial.next_token);
    EXPECT_FALSE(writes.full_resync);
    EXPECT_EQ(writes.keys, (std::vector<std::string>{"one", "two"}));
    ASSERT_TRUE(writes.snapshot);
    EXPECT_EQ(writes.snapshot->size(), 2u);

    manager.remove("one");
    const auto removal = manager.changes_since(writes.next_token);
    EXPECT_FALSE(removal.full_resync);
    EXPECT_EQ(removal.keys, (std::vector<std::string>{"one"}));
    ASSERT_TRUE(removal.snapshot);
    EXPECT_FALSE(removal.snapshot->contains("one"));
}

TEST(Replication, ChangesTrackPeerMerges) {
    ReplicationManager source{zetta::util::new_uuid()};
    ReplicationManager replica{zetta::util::new_uuid()};
    const auto baseline = replica.changes_since(0);

    source.set("peer", nlohmann::json{{"value", 1}});
    replica.merge(source.save());
    const auto merged = replica.changes_since(baseline.next_token);

    EXPECT_FALSE(merged.full_resync);
    EXPECT_EQ(merged.keys, (std::vector<std::string>{"peer"}));
    ASSERT_TRUE(merged.snapshot);
    EXPECT_EQ(merged.snapshot->at("peer").at("value"), 1);
}

TEST(Replication, EntryCountAndVersionTrackTheDocument) {
    ReplicationManager manager{zetta::util::new_uuid()};
    EXPECT_EQ(manager.entry_count(), 0u);
    EXPECT_EQ(manager.version(), "empty");

    manager.set("one", 1);
    EXPECT_EQ(manager.entry_count(), 1u);
    const std::string after_one = manager.version();
    EXPECT_NE(after_one, "empty");

    manager.set("two", 2);
    EXPECT_EQ(manager.entry_count(), 2u);
    EXPECT_NE(manager.version(), after_one);  // version moves with the document
}

TEST(Replication, ConvergedDocumentsShareAVersion) {
    ReplicationManager a{zetta::util::new_uuid()};
    ReplicationManager b{zetta::util::new_uuid()};
    a.set("from_a", "alpha");
    b.set("from_b", "beta");

    // Diverged: different content, different versions.
    EXPECT_NE(a.version(), b.version());

    // After a full cross-merge both hold the same heads → identical version.
    const auto a_blob = a.save();
    const auto b_blob = b.save();
    a.merge(b_blob);
    b.merge(a_blob);
    EXPECT_EQ(a.entry_count(), 2u);
    EXPECT_EQ(b.entry_count(), 2u);
    EXPECT_EQ(a.version(), b.version());
}

TEST(Replication, ConcurrentEditsMergeConflictFree) {
    ReplicationManager a{zetta::util::new_uuid()};
    ReplicationManager b{zetta::util::new_uuid()};
    a.set("shared", 1);
    b.merge(a.save());  // both start with shared=1

    // Each makes an independent change to a different key.
    a.set("from_a", "alpha");
    b.set("from_b", "beta");

    // Cross-merge using snapshots taken before the merges.
    const auto a_blob = a.save();
    const auto b_blob = b.save();
    a.merge(b_blob);
    b.merge(a_blob);

    // Both converge to the union — no change is lost.
    for (const ReplicationManager* node : {&a, &b}) {
        const nlohmann::json snapshot = node->snapshot();
        EXPECT_EQ(snapshot["shared"], 1);
        EXPECT_EQ(snapshot["from_a"], "alpha");
        EXPECT_EQ(snapshot["from_b"], "beta");
    }
}

// A three-node mesh gossiping whole documents, driven entirely in-process — no
// sockets, no threads, no timing. Each round every node writes locally and a few
// one-way merges fire, which is exactly the shape the cluster's 500 ms SYNC
// produces; then mutation stops and the nodes gossip to a fixed point.
//
// Convergence after quiescence is the CRDT's defining promise, and it is what
// ClusterSecurity.MeshConvergesOverMutualTlsUnderSustainedLoad asserts over real
// mTLS. This is the same property with the transport removed, so a failure here
// is unambiguously a merge bug rather than a networking or timing one (#835).
TEST(Replication, AMeshOfThreeConvergesAfterConcurrentWrites) {
    constexpr int kNodes = 3;
    std::vector<std::unique_ptr<ReplicationManager>> nodes;
    for (int n = 0; n < kNodes; ++n) {
        nodes.push_back(std::make_unique<ReplicationManager>(zetta::util::new_uuid()));
    }

    // Deterministic: a fixed seed, so a failure is reproducible rather than a
    // story about a busy machine.
    std::mt19937 rng(20250718u);
    const std::string blob(512, 'x');

    for (int round = 0; round < 300; ++round) {
        for (int n = 0; n < kNodes; ++n) {
            nodes[n]->set("k" + std::to_string(rng() % 4) + ":" + std::to_string(n),
                          nlohmann::json{{"n", round}, {"blob", blob}});
        }
        // Independent link timers: a handful of one-way merges in arbitrary
        // directions, so nodes routinely see partial views of each other.
        for (int fired = 0; fired < kNodes; ++fired) {
            const int to = static_cast<int>(rng() % kNodes);
            const int from = (to + 1 + static_cast<int>(rng() % (kNodes - 1))) % kNodes;
            nodes[to]->merge(nodes[from]->save());
        }
    }

    // Quiesce: no more local writes, gossip until it settles.
    for (int round = 0; round < 20; ++round) {
        for (int to = 0; to < kNodes; ++to) {
            for (int from = 0; from < kNodes; ++from) {
                if (from != to) nodes[to]->merge(nodes[from]->save());
            }
        }
    }

    for (int n = 1; n < kNodes; ++n) {
        EXPECT_EQ(nodes[n]->version(), nodes[0]->version())
            << "node " << n << " did not converge with node 0 after quiescence";
        EXPECT_EQ(nodes[n]->snapshot(), nodes[0]->snapshot())
            << "node " << n << " holds different content than node 0";
    }
}

TEST(IncrementalReconcile, AppliesPeerLocalAndRemovalChangesInOneTick) {
    zetta::test::TempDir temporary_directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    for (int index = 0; index < 16; ++index) {
        replication.set("audit:seed-" + std::to_string(index),
                        audit_value("seed"));
    }
    replication.set("db:removed", nlohmann::json{{"name", "removed"}});

    zetta::Node node{config_for(temporary_directory), &replication};
    ASSERT_TRUE(node.start());
    ASSERT_TRUE(node.find_database("removed"));

    ReplicationManager peer{zetta::util::new_uuid()};
    peer.merge(replication.save());
    peer.set("db:peer", nlohmann::json{{"name", "peer"}});
    replication.merge(peer.save());
    replication.set("db:local", nlohmann::json{{"name", "local"}});
    replication.remove("db:removed");

    node.tick();

    EXPECT_TRUE(node.find_database("peer"));
    EXPECT_TRUE(node.find_database("local"));
    EXPECT_FALSE(node.find_database("removed"));
    node.stop();
}

TEST(IncrementalReconcile, OneChangeStaysFastWithOneHundredThousandAuditKeys) {
    zetta::test::TempDir temporary_directory;
    ReplicationManager replication{zetta::util::new_uuid()};
    constexpr int kExistingEvents = 100000;
    constexpr int kTicks = 40;
    for (int index = 0; index < kExistingEvents; ++index) {
        replication.set("audit:existing-" + std::to_string(index),
                        audit_value("existing"));
    }

    zetta::Node node{config_for(temporary_directory), &replication};
    ASSERT_TRUE(node.start());
    ASSERT_EQ(node.audit_event_count(),
              static_cast<std::size_t>(kExistingEvents));

    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < kTicks; ++index) {
        replication.set("audit:tick-" + std::to_string(index),
                        audit_value("tick"));
        node.tick();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    std::printf(
        "zetta-test: %d one-change ticks with %d existing audit keys: %lld ms\n",
        kTicks, kExistingEvents,
        static_cast<long long>(elapsed_milliseconds.count()));
    EXPECT_LT(elapsed, std::chrono::seconds(4));
    EXPECT_EQ(node.audit_event_count(),
              static_cast<std::size_t>(kExistingEvents + kTicks));
    node.stop();
}
