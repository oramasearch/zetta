#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#include "zetta/ml/uplift.hpp"

namespace {

struct SyntheticData {
  zetta::ml::UpliftDataset dataset;
  std::vector<double> true_effect;
};

zetta::ml::UpliftParams test_params() {
  zetta::ml::UpliftParams params;
  params.outcome_model.num_trees = 24;
  params.outcome_model.max_depth = 3;
  params.outcome_model.max_bins = 16;
  params.outcome_model.learning_rate = 0.2;
  params.holdout_fraction = 0.3;
  params.targeting_fraction = 0.1;
  params.bootstrap_samples = 80;
  params.seed = 1045;
  return params;
}

SyntheticData heterogeneous_experiment(std::size_t rows,
                                       bool observational = false) {
  SyntheticData data;
  data.dataset.feature_names = {"segment_score", "baseline_score"};
  data.dataset.features.assign(2, {});
  std::mt19937_64 generator(9045);
  std::uniform_real_distribution<double> uniform(0.0, 1.0);
  for (std::size_t index = 0; index < rows; ++index) {
    const double segment_score = uniform(generator);
    const double baseline_score = uniform(generator);
    const bool segment = segment_score >= 0.5;
    const double propensity = observational ? (segment ? 0.98 : 0.02) : 0.5;
    const double treatment = uniform(generator) < propensity ? 1.0 : 0.0;
    const double planted_effect = segment ? 0.55 : 0.0;
    const double baseline_probability = 0.12 + 0.18 * baseline_score;
    const double outcome_probability =
        std::min(0.98, baseline_probability + treatment * planted_effect);
    const double outcome = uniform(generator) < outcome_probability ? 1.0 : 0.0;

    data.dataset.entities.push_back(static_cast<std::int64_t>(index));
    data.dataset.features[0].push_back(segment_score);
    data.dataset.features[1].push_back(baseline_score);
    data.dataset.treatment.push_back(treatment);
    data.dataset.outcome.push_back(outcome);
    data.true_effect.push_back(planted_effect);
  }
  return data;
}

double
average_true_effect(const std::vector<zetta::ml::TreatmentEffect> &ranked,
                    const std::vector<double> &truth, std::size_t begin,
                    std::size_t end) {
  double sum = 0.0;
  for (std::size_t index = begin; index < end; ++index) {
    sum += truth[static_cast<std::size_t>(ranked[index].entity)];
  }
  return sum / static_cast<double>(end - begin);
}

} // namespace

TEST(UpliftCausal, HeterogeneousEffectRanksTheResponsiveSegment) {
  const SyntheticData data = heterogeneous_experiment(1'200);
  const zetta::ml::UpliftResult result =
      zetta::ml::train_uplift(data.dataset, test_params());
  ASSERT_TRUE(result.ok()) << result.status.message;
  ASSERT_EQ(result.treatment_effects.size(), data.dataset.row_count());
  ASSERT_FALSE(result.qini_curve.empty());

  const std::size_t decile = result.treatment_effects.size() / 10;
  const double top = average_true_effect(result.treatment_effects,
                                         data.true_effect, 0, decile);
  const double bottom =
      average_true_effect(result.treatment_effects, data.true_effect,
                          result.treatment_effects.size() - decile,
                          result.treatment_effects.size());
  EXPECT_GT(top, bottom + 0.25);

  const zetta::ml::UpliftLearnerMetrics &selected =
      result.selected_learner == zetta::ml::UpliftLearner::T ? result.t_learner
                                                             : result.x_learner;
  EXPECT_GT(selected.auuc, 0.0);
  EXPECT_GT(selected.qini_coefficient, 0.0);
  EXPECT_GT(selected.uplift_at_target, 0.1);
  EXPECT_LE(result.uplift_lower, result.uplift_upper);
  EXPECT_GT(result.treated_rows, 0u);
  EXPECT_GT(result.control_rows, 0u);
  EXPECT_GT(result.held_out_rows, 0u);
  EXPECT_FALSE(result.observational);

  std::size_t persuadables = 0;
  std::size_t sleeping_dogs = 0;
  for (const zetta::ml::TreatmentEffect &effect : result.treatment_effects) {
    if (effect.recommended) {
      ++persuadables;
      EXPECT_GT(effect.estimated_uplift, 0.0);
    }
    if (effect.estimated_uplift <= 0.0) {
      ++sleeping_dogs;
      EXPECT_FALSE(effect.recommended);
    }
  }
  EXPECT_GT(persuadables, 0u);
  EXPECT_GT(sleeping_dogs, 0u);
}

TEST(UpliftCausal, TreatmentAndOutcomeMustBeBinary) {
  SyntheticData treatment = heterogeneous_experiment(300);
  treatment.dataset.treatment[3] = 2.0;
  const zetta::ml::UpliftResult bad_treatment =
      zetta::ml::train_uplift(treatment.dataset, test_params());
  EXPECT_FALSE(bad_treatment.ok());
  EXPECT_NE(
      bad_treatment.status.message.find("treatment column must be binary"),
      std::string::npos);

  SyntheticData outcome = heterogeneous_experiment(300);
  outcome.dataset.outcome[4] = -1.0;
  const zetta::ml::UpliftResult bad_outcome =
      zetta::ml::train_uplift(outcome.dataset, test_params());
  EXPECT_FALSE(bad_outcome.ok());
  EXPECT_NE(bad_outcome.status.message.find("outcome column must be binary"),
            std::string::npos);
}

TEST(UpliftCausal, PositivityViolationsAreExcludedFromRecommendations) {
  const SyntheticData data = heterogeneous_experiment(1'500, true);
  const zetta::ml::UpliftResult result =
      zetta::ml::train_uplift(data.dataset, test_params());
  ASSERT_TRUE(result.ok()) << result.status.message;
  EXPECT_GT(result.overlap_excluded, 0u);
  EXPECT_TRUE(result.observational);
  for (const zetta::ml::TreatmentEffect &effect : result.treatment_effects) {
    if (!effect.overlap_eligible) {
      EXPECT_FALSE(effect.recommended);
    }
  }
}

TEST(UpliftCausal, SeedMakesLearnerSelectionAndBootstrapDeterministic) {
  const SyntheticData data = heterogeneous_experiment(800);
  const zetta::ml::UpliftParams params = test_params();
  const zetta::ml::UpliftResult first =
      zetta::ml::train_uplift(data.dataset, params);
  const zetta::ml::UpliftResult second =
      zetta::ml::train_uplift(data.dataset, params);
  ASSERT_TRUE(first.ok()) << first.status.message;
  ASSERT_TRUE(second.ok()) << second.status.message;
  EXPECT_EQ(first.selected_learner, second.selected_learner);
  EXPECT_DOUBLE_EQ(first.uplift_lower, second.uplift_lower);
  EXPECT_DOUBLE_EQ(first.uplift_upper, second.uplift_upper);
  ASSERT_EQ(first.treatment_effects.size(), second.treatment_effects.size());
  for (std::size_t index = 0; index < first.treatment_effects.size(); ++index) {
    EXPECT_EQ(first.treatment_effects[index].entity,
              second.treatment_effects[index].entity);
    EXPECT_DOUBLE_EQ(first.treatment_effects[index].estimated_uplift,
                     second.treatment_effects[index].estimated_uplift);
  }
}
