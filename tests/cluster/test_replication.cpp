// SPDX-License-Identifier: MIT

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
        replication.set("fixture:seed-" + std::to_string(index),
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

TEST(IncrementalReconcile, OneChangeStaysFastWithOneHundredThousandMigratedAuditEvents) {
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
    for (const auto& [key, value] : replication.snapshot().items()) {
        EXPECT_FALSE(key.starts_with("audit:"));
    }

    const auto started = std::chrono::steady_clock::now();
    for (int index = 0; index < kTicks; ++index) {
        replication.set("fixture:tick", index);
        zetta::governance::AuditEventValue event;
        event.target = "tick";
        node.record_audit_event(event);
        node.tick();
    }
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const auto elapsed_milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    std::printf(
        "zetta-test: %d one-change ticks with %d existing audit events: %lld ms\n",
        kTicks, kExistingEvents,
        static_cast<long long>(elapsed_milliseconds.count()));
    EXPECT_LT(elapsed, std::chrono::seconds(4));
    EXPECT_EQ(node.audit_event_count(),
              static_cast<std::size_t>(kExistingEvents + kTicks));
    node.stop();
}

// --- Gossip deltas (#794) ---------------------------------------------------

TEST(ReplicationDelta, ShipsOnlyTheChangesThePeerLacks) {
    ReplicationManager a(zetta::util::new_uuid());
    ReplicationManager b(zetta::util::new_uuid());
    const std::string blob(256, 'x');
    for (int key = 0; key < 200; ++key) {
        a.set("bulk:" + std::to_string(key), {{"blob", blob}});
    }
    b.merge(a.save());
    ASSERT_EQ(b.version(), a.version());

    a.set("fresh:1", {{"n", 1}});
    a.set("fresh:2", {{"n", 2}});
    a.remove("bulk:7");

    const auto delta = a.save_since(b.clock());
    EXPECT_FALSE(delta.full);
    EXPECT_EQ(delta.changes, 3u);
    EXPECT_LT(delta.blob.size() * 20, a.save().size())
        << "a delta of three changes should be a small fraction of the document";
    b.merge(delta.blob);
    EXPECT_EQ(b.version(), a.version());
    EXPECT_EQ(b.snapshot(), a.snapshot());
    EXPECT_FALSE(b.snapshot().contains("bulk:7"));
}

TEST(ReplicationDelta, IsEmptyWhenThePeerLacksNothing) {
    ReplicationManager a(zetta::util::new_uuid());
    ReplicationManager b(zetta::util::new_uuid());
    a.set("k", {{"n", 1}});
    b.set("other", {{"n", 2}});
    b.merge(a.save());
    // b holds everything a holds (and more): a has nothing to send it.
    const auto delta = a.save_since(b.clock());
    EXPECT_TRUE(delta.blob.empty());
    EXPECT_FALSE(delta.full);
    // The reverse direction still owes a b's own change.
    const auto back = b.save_since(a.clock());
    EXPECT_FALSE(back.blob.empty());
    a.merge(back.blob);
    EXPECT_EQ(a.version(), b.version());
}

TEST(ReplicationDelta, FallsBackToTheWholeDocument) {
    ReplicationManager a(zetta::util::new_uuid());
    for (int key = 0; key < 10; ++key) a.set("k" + std::to_string(key), {{"n", key}});

    // No clock (a peer that advertises none, e.g. an older node): the document.
    const auto unknown = a.save_since("");
    EXPECT_TRUE(unknown.full);
    EXPECT_EQ(unknown.blob, a.save());

    // A clock that does not decode is treated exactly like no clock.
    for (const std::string garbage : {std::string("\x05", 1), std::string("not a clock"),
                                      std::string(17, '\xff')}) {
        const auto delta = a.save_since(garbage);
        EXPECT_TRUE(delta.full) << "a malformed clock must not produce a delta";
    }

    // A peer that holds nothing lacks every change: too far behind for a delta.
    ReplicationManager fresh(zetta::util::new_uuid());
    const auto behind = a.save_since(fresh.clock());
    EXPECT_TRUE(behind.full);
    fresh.merge(behind.blob);
    EXPECT_EQ(fresh.version(), a.version());
}

TEST(ReplicationDelta, ClockNamesEveryActorAndAdvancesWithEachChange) {
    ReplicationManager a(zetta::util::new_uuid());
    ReplicationManager b(zetta::util::new_uuid());
    const std::string empty = a.clock();
    a.set("k", {{"n", 1}});
    const std::string one = a.clock();
    EXPECT_NE(one, empty);
    a.set("k", {{"n", 2}});
    EXPECT_NE(a.clock(), one);
    b.set("j", {{"n", 1}});
    a.merge(b.save());
    b.merge(a.save());
    EXPECT_EQ(a.clock(), b.clock()) << "converged documents hold the same clock";
    // 1 count byte + 2 actors x (16 id bytes + 1 sequence byte).
    EXPECT_EQ(a.clock().size(), 1u + 2u * 17u);
}

// The in-process mesh of AMeshOfThreeConvergesAfterConcurrentWrites, gossiping
// deltas against each receiver's own clock instead of whole documents. Partial
// views routinely put a third node's changes on a peer before the author's own
// link delivers them — a delta must never leave a gap merge() would skip.
TEST(ReplicationDelta, AMeshGossipingDeltasConvergesWithPartialViews) {
    constexpr int kNodes = 4;
    std::vector<std::unique_ptr<ReplicationManager>> nodes;
    for (int n = 0; n < kNodes; ++n) {
        nodes.push_back(std::make_unique<ReplicationManager>(zetta::util::new_uuid()));
    }
    std::mt19937 rng(20261021u);
    const std::string blob(128, 'x');
    std::size_t delta_bytes = 0;
    std::size_t full_bytes = 0;
    std::vector<std::string> reported(kNodes);  // the last clock each node reported
    for (int round = 0; round < 300; ++round) {
        for (int n = 0; n < kNodes; ++n) {
            const auto key = "k" + std::to_string(rng() % 8) + ":" + std::to_string(n);
            if (rng() % 10 == 0) {
                nodes[n]->remove(key);
            } else {
                nodes[n]->set(key, nlohmann::json{{"n", round}, {"blob", blob}});
            }
        }
        for (int fired = 0; fired < kNodes; ++fired) {
            const int to = static_cast<int>(rng() % kNodes);
            const int from = (to + 1 + static_cast<int>(rng() % (kNodes - 1))) % kNodes;
            // The receiver's clock as of its last STATUS, which may be rounds
            // old (or absent, before its first STATUS): a stale clock only
            // makes the sender re-ship changes the receiver already holds.
            const auto delta = nodes[from]->save_since(reported[to]);
            delta_bytes += delta.blob.size();
            full_bytes += nodes[from]->save().size();
            if (!delta.blob.empty()) nodes[to]->merge(delta.blob);
        }
        for (int n = 0; n < kNodes; ++n) {
            if (rng() % 2 == 0) reported[n] = nodes[n]->clock();
        }
    }
    for (int round = 0; round < 10; ++round) {
        for (int to = 0; to < kNodes; ++to) {
            for (int from = 0; from < kNodes; ++from) {
                if (from == to) continue;
                const auto delta = nodes[from]->save_since(nodes[to]->clock());
                if (!delta.blob.empty()) nodes[to]->merge(delta.blob);
            }
        }
    }
    for (int n = 1; n < kNodes; ++n) {
        EXPECT_EQ(nodes[n]->version(), nodes[0]->version()) << "node " << n;
        EXPECT_EQ(nodes[n]->snapshot(), nodes[0]->snapshot()) << "node " << n;
        EXPECT_EQ(nodes[n]->clock(), nodes[0]->clock()) << "node " << n;
    }
    // A fresh replica built from any node's whole document is the same document:
    // no change was skipped anywhere along the way.
    ReplicationManager check(zetta::util::new_uuid());
    check.merge(nodes[0]->save());
    for (int n = 1; n < kNodes; ++n) {
        const auto before = check.version();
        check.merge(nodes[n]->save());
        EXPECT_EQ(check.version(), before) << "node " << n << " holds a change node 0 lacks";
    }
    EXPECT_LT(delta_bytes * 5, full_bytes)
        << "deltas should cost a small fraction of shipping whole documents";
}

TEST(ReplicationDelta, ReachesAPeerThatRetiredAudit) {
    ReplicationManager a(zetta::util::new_uuid());
    ReplicationManager b(zetta::util::new_uuid());
    for (int key = 0; key < 20; ++key) a.set("k" + std::to_string(key), {{"n", key}});
    b.merge(a.save());
    ASSERT_TRUE(b.retire_audit([](const std::string&, const nlohmann::json&) { return true; },
                               [] { return true; }));
    a.set("after", {{"n", 1}});
    const auto delta = a.save_since(b.clock());
    ASSERT_FALSE(delta.full);
    b.merge(delta.blob);
    EXPECT_EQ(b.snapshot().at("after").at("n"), 1);
    EXPECT_EQ(b.version(), a.version());
}
