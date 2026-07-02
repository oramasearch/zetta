#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "zetta/data/replication_strategy.hpp"

using zetta::data::kDefaultReplicationFactor;
using zetta::data::ReplicationStrategy;
using zetta::data::ReplicationStrategyKind;

TEST(ReplicationStrategy, DefaultIsReplicateToAll) {
    ReplicationStrategy strategy;
    EXPECT_EQ(strategy.kind(), ReplicationStrategyKind::ReplicateToAll);
    EXPECT_TRUE(strategy.is_default());
    EXPECT_EQ(strategy, ReplicationStrategy::replicate_to_all());
}

TEST(ReplicationStrategy, Factories) {
    EXPECT_EQ(ReplicationStrategy::partitioned().kind(),
              ReplicationStrategyKind::Partitioned);
    EXPECT_EQ(ReplicationStrategy::partitioned().replication_factor(), 1);
    EXPECT_FALSE(ReplicationStrategy::partitioned().is_default());

    const auto replicas = ReplicationStrategy::partitioned_with_replicas();
    EXPECT_EQ(replicas.kind(), ReplicationStrategyKind::PartitionedWithReplicas);
    EXPECT_EQ(replicas.replication_factor(), kDefaultReplicationFactor);  // 3
    EXPECT_FALSE(replicas.is_default());

    EXPECT_EQ(ReplicationStrategy::partitioned_with_replicas(5).replication_factor(),
              5);
    // A factor below 1 is clamped to a single copy.
    EXPECT_EQ(ReplicationStrategy::partitioned_with_replicas(0).replication_factor(),
              1);
}

TEST(ReplicationStrategy, KindNames) {
    EXPECT_EQ(to_string(ReplicationStrategyKind::ReplicateToAll),
              "replicate_to_all");
    EXPECT_EQ(to_string(ReplicationStrategyKind::Partitioned), "partitioned");
    EXPECT_EQ(to_string(ReplicationStrategyKind::PartitionedWithReplicas),
              "partitioned_with_replicas");

    ReplicationStrategyKind kind{};
    EXPECT_TRUE(zetta::data::replication_strategy_kind_from_string("partitioned", kind));
    EXPECT_EQ(kind, ReplicationStrategyKind::Partitioned);
    EXPECT_FALSE(zetta::data::replication_strategy_kind_from_string("nope", kind));
}

TEST(ReplicationStrategy, JsonRoundTrip) {
    for (const ReplicationStrategy original :
         {ReplicationStrategy::replicate_to_all(),
          ReplicationStrategy::partitioned(),
          ReplicationStrategy::partitioned_with_replicas(4)}) {
        const auto parsed = ReplicationStrategy::from_json(original.to_json());
        ASSERT_TRUE(parsed);
        EXPECT_EQ(*parsed, original);
    }
}

TEST(ReplicationStrategy, JsonShape) {
    EXPECT_EQ(ReplicationStrategy::replicate_to_all().to_json(),
              (nlohmann::json{{"kind", "replicate_to_all"}}));
    EXPECT_EQ(ReplicationStrategy::partitioned().to_json(),
              (nlohmann::json{{"kind", "partitioned"}}));
    // replication_factor is present only for partitioned_with_replicas.
    EXPECT_EQ(ReplicationStrategy::partitioned_with_replicas(3).to_json(),
              (nlohmann::json{{"kind", "partitioned_with_replicas"},
                              {"replication_factor", 3}}));
}

TEST(ReplicationStrategy, PartitionedWithReplicasDefaultsFactorWhenAbsent) {
    const auto parsed = ReplicationStrategy::from_json(
        nlohmann::json{{"kind", "partitioned_with_replicas"}});
    ASSERT_TRUE(parsed);
    EXPECT_EQ(parsed->replication_factor(), kDefaultReplicationFactor);  // 3
}

TEST(ReplicationStrategy, FromJsonRejectsMalformed) {
    // Not an object.
    EXPECT_FALSE(ReplicationStrategy::from_json(nlohmann::json("partitioned")));
    EXPECT_FALSE(ReplicationStrategy::from_json(nlohmann::json::array()));
    // Missing / wrong-typed / unknown kind.
    EXPECT_FALSE(ReplicationStrategy::from_json(nlohmann::json::object()));
    EXPECT_FALSE(ReplicationStrategy::from_json(nlohmann::json{{"kind", 7}}));
    EXPECT_FALSE(ReplicationStrategy::from_json(nlohmann::json{{"kind", "quorum"}}));
    // A non-positive / non-integer replication_factor.
    EXPECT_FALSE(ReplicationStrategy::from_json(
        nlohmann::json{{"kind", "partitioned_with_replicas"},
                       {"replication_factor", 0}}));
    EXPECT_FALSE(ReplicationStrategy::from_json(
        nlohmann::json{{"kind", "partitioned_with_replicas"},
                       {"replication_factor", 2.5}}));
}

TEST(ReplicationStrategy, FromMetaDefaultsWhenAbsentOrMalformed) {
    // Absent "replication" field → default.
    EXPECT_TRUE(
        zetta::data::replication_strategy_from_meta(nlohmann::json{{"name", "t"}})
            .is_default());
    // Present-but-malformed → safe default (full replication), never a throw.
    EXPECT_TRUE(zetta::data::replication_strategy_from_meta(
                    nlohmann::json{{"replication", nlohmann::json{{"kind", "x"}}}})
                    .is_default());
    // Present + valid → parsed.
    EXPECT_EQ(zetta::data::replication_strategy_from_meta(nlohmann::json{
                  {"replication", ReplicationStrategy::partitioned().to_json()}}),
              ReplicationStrategy::partitioned());
}
