// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "zetta/ml/entity_resolution.hpp"

namespace {

using zetta::ml::EntityRecord;
using zetta::ml::EntityResolutionMode;
using zetta::ml::EntityResolutionParams;
using zetta::ml::EntityResolutionResult;

std::vector<EntityRecord> duplicate_fixture() {
  return {
      {"crm", "1", "Alice Johnson", "10 Pine Street", "alice@example.com",
       "604-555-0101", 0.0, false, true, true, "alice"},
      {"billing", "101", "Alyce Johnson", "10 Pine St", "ALICE@example.com ",
       "+1 604 555 0101", 0.0, false, true, true, "alice"},
      {"crm", "2", "Bob Martinez", "22 Oak Avenue", "bob@example.com",
       "604-555-0102", 0.0, false, true, true, "bob"},
      {"billing", "102", "Bbo Martinez", "22 Oak Ave", "bob@example.com",
       "6045550102", 0.0, false, true, true, "bob"},
      {"crm", "3", "Carol Singh", "9 Cedar Road", "carol@example.com",
       "604-555-0103", 0.0, false, true, true, "carol"},
      {"billing", "103", "Carol Sing", "9 Cedar Rd", "carol@example.com",
       "6045550103", 0.0, false, true, true, "carol"},
      {"crm", "4", "David Chen", "18 Birch Lane", "david@example.com",
       "604-555-0104", 0.0, false, true, true, "david"},
      {"billing", "104", "Dvaid Chen", "18 Birch Ln", "david@example.com",
       "6045550104", 0.0, false, true, true, "david"},
      {"crm", "5", "Eva Brown", "77 Lake Drive", "eva@example.com",
       "604-555-0105", 0.0, false, true, true, "eva"},
      {"billing", "105", "Eve Brown", "77 Lake Dr", "eva@example.com",
       "6045550105", 0.0, false, true, true, "eva"},
  };
}

std::vector<std::string> assignments(const EntityResolutionResult &result) {
  std::vector<std::string> values;
  for (const auto &entity : result.entities) {
    values.push_back(entity.source + ":" + entity.record_id + "=" +
                     entity.entity_id);
  }
  std::sort(values.begin(), values.end());
  return values;
}

} // namespace

TEST(JaroWinkler, ScoresTypographicalVariantsAboveUnrelatedNames) {
  const double typo =
      zetta::ml::jaro_winkler_similarity("alice johnson", "alyce johnson");
  const double unrelated =
      zetta::ml::jaro_winkler_similarity("alice johnson", "bob martinez");
  EXPECT_GT(typo, 0.9);
  EXPECT_LT(unrelated, 0.6);
}

TEST(Blocking, DeduplicatesRoutesAndReportsRecall) {
  const EntityResolutionResult result =
      zetta::ml::resolve_entities({duplicate_fixture()});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_DOUBLE_EQ(result.metrics.blocking_recall, 1.0);
  EXPECT_GT(result.metrics.candidate_pairs, 5u);
  const auto alice = std::find_if(
      result.pairs.begin(), result.pairs.end(), [](const auto &pair) {
        return (pair.left_record_id == "1" && pair.right_record_id == "101") ||
               (pair.left_record_id == "101" && pair.right_record_id == "1");
      });
  ASSERT_NE(alice, result.pairs.end());
  EXPECT_GE(alice->routes.size(), 2u);
}

TEST(EntityResolution, PlantedDuplicatesResolveWithHighPairwiseF1) {
  const EntityResolutionResult result =
      zetta::ml::resolve_entities({duplicate_fixture()});
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.scoring_method, "calibrated_gbdt");
  EXPECT_GT(result.metrics.pairwise_f1, 0.9);
  EXPECT_GT(result.metrics.bcubed_precision, 0.9);
  EXPECT_GT(result.metrics.bcubed_recall, 0.9);
  EXPECT_EQ(result.entities.size(), duplicate_fixture().size());
}

TEST(EntityResolution, KeyedModeCollapsesRepeatedObservationsWithoutPairs) {
  const std::vector<EntityRecord> observations = {
      {"datasets.customers", "c00001"},
      {"datasets.customers", "c00001"},
      {"datasets.customers", "c00002"},
      {"datasets.customers", "c00002"},
  };
  EntityResolutionParams params;
  params.mode = EntityResolutionMode::Keyed;

  const EntityResolutionResult result =
      zetta::ml::resolve_entities({observations}, params);

  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.scoring_method, "declared_entity_key");
  EXPECT_EQ(result.entities.size(), 2u);
  EXPECT_TRUE(result.pairs.empty());
  EXPECT_EQ(result.entities[0].entity_id, "c00001");
  EXPECT_EQ(result.entities[1].entity_id, "c00002");
}

TEST(EntityResolution, ConflictingVerifiedKeyStopsTransitiveBridgeMerge) {
  std::vector<EntityRecord> records = {
      {"crm", "a", "Jordan Lee", "1 Main", "first@example.com", "1111", 0.0,
       false, true, false, "first"},
      {"support", "bridge", "Jordan Lee", "1 Main", "", "", 0.0, false, false,
       false, "first"},
      {"billing", "b", "Jordan Lee", "1 Main", "second@example.com", "2222",
       0.0, false, true, false, "second"},
  };
  EntityResolutionParams params;
  params.match_threshold = 0.65;
  const EntityResolutionResult result =
      zetta::ml::resolve_entities({std::move(records)}, params);
  ASSERT_TRUE(result.ok()) << result.error;
  const auto first =
      std::find_if(result.entities.begin(), result.entities.end(),
                   [](const auto &entity) { return entity.record_id == "a"; });
  const auto second =
      std::find_if(result.entities.begin(), result.entities.end(),
                   [](const auto &entity) { return entity.record_id == "b"; });
  ASSERT_NE(first, result.entities.end());
  ASSERT_NE(second, result.entities.end());
  EXPECT_NE(first->entity_id, second->entity_id);
}

TEST(EntityResolution, ThreePartitionsProduceTheSameAssignments) {
  const std::vector<EntityRecord> records = duplicate_fixture();
  const EntityResolutionResult single = zetta::ml::resolve_entities({records});
  std::vector<std::vector<EntityRecord>> partitions(3);
  for (std::size_t index = 0; index < records.size(); ++index) {
    partitions[index % partitions.size()].push_back(records[index]);
  }
  const EntityResolutionResult distributed =
      zetta::ml::resolve_entities(partitions);
  ASSERT_TRUE(single.ok()) << single.error;
  ASSERT_TRUE(distributed.ok()) << distributed.error;
  EXPECT_EQ(assignments(single), assignments(distributed));
  EXPECT_DOUBLE_EQ(single.metrics.pairwise_f1, distributed.metrics.pairwise_f1);
  for (const auto &pair : distributed.pairs) {
    EXPECT_LT(pair.owner_partition, partitions.size());
  }
}

TEST(EntityResolution, FellegiSunterReducesSufficientStatistics) {
  std::vector<EntityRecord> records = duplicate_fixture();
  for (EntityRecord &record : records) {
    record.truth_entity.clear();
  }
  std::size_t reductions = 0;
  EntityResolutionParams params;
  params.allreduce = [&reductions](const std::string &round,
                                   const std::vector<double> &statistics) {
    EXPECT_EQ(round,
              "entity_resolution.fs_em." + std::to_string(reductions));
    ++reductions;
    return statistics;
  };

  const EntityResolutionResult result =
      zetta::ml::resolve_entities({std::move(records)}, params);
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_EQ(result.scoring_method, "fellegi_sunter_em");
  EXPECT_EQ(reductions, 30u);
  EXPECT_FALSE(result.metrics.labels_present);
}

TEST(EntityResolution, RefusesInvalidDecisionThresholds) {
  EntityResolutionParams params;
  params.review_threshold = params.match_threshold;

  const EntityResolutionResult result =
      zetta::ml::resolve_entities({duplicate_fixture()}, params);
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error.find("0 <= review < match <= 1"), std::string::npos);
}
