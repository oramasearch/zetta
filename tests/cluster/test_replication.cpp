#include <gtest/gtest.h>

#include "zetta/cluster/replication.hpp"
#include "zetta/util/uuid.hpp"

using zetta::ReplicationManager;
using zetta::ReplicationPolicy;

TEST(Replication, DefaultPolicyReplicatesToAllNodes) {
    ReplicationManager manager{zetta::new_uuid()};
    EXPECT_EQ(manager.policy(), ReplicationPolicy::ReplicateToAllNodes);
}

TEST(Replication, MergePropagatesState) {
    ReplicationManager source{zetta::new_uuid()};
    source.set("title", "hello");
    source.set("count", 3);
    source.set("nested", nlohmann::json{{"a", 1}, {"b", 2}});

    ReplicationManager replica{zetta::new_uuid()};
    replica.merge(source.save());

    const nlohmann::json snapshot = replica.snapshot();
    EXPECT_EQ(snapshot["title"], "hello");
    EXPECT_EQ(snapshot["count"], 3);
    EXPECT_EQ(snapshot["nested"]["b"], 2);
}

TEST(Replication, EntryCountAndVersionTrackTheDocument) {
    ReplicationManager manager{zetta::new_uuid()};
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
    ReplicationManager a{zetta::new_uuid()};
    ReplicationManager b{zetta::new_uuid()};
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
    ReplicationManager a{zetta::new_uuid()};
    ReplicationManager b{zetta::new_uuid()};
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
