#include <gtest/gtest.h>

#include "zetta/replication.hpp"
#include "zetta/uuid.hpp"

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
