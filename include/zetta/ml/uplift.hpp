#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "zetta/compute/compute_status.hpp"
#include "zetta/ml/gbdt.hpp"

// Native causal uplift estimation for methodology M-6 (#1045).
//
// The T-learner fits one binary-outcome GBDT per treatment arm. The X-learner
// reuses those outcome models, imputes individual effects, and fits one
// squared-error effect GBDT per arm. A fifth GBDT estimates treatment
// propensity from pre-treatment features. Every GBDT receives the same
// injected AllReduce callback used by the distributed GBDT trainer, so local
// gradient histograms combine across owned partitions and single-member and
// multi-member runs grow equivalent models.
//
// See docs/ml/uplift.md.
namespace zetta::ml {

struct UpliftParams {
  GbdtParams outcome_model;
  double holdout_fraction = 0.3;
  double targeting_fraction = 0.1;
  double overlap_lower = 0.05;
  double overlap_upper = 0.95;
  std::size_t bootstrap_samples = 200;
  std::uint64_t seed = 1045;
};

struct UpliftDataset {
  std::vector<std::int64_t> entities;
  std::vector<std::string> feature_names;
  std::vector<std::vector<double>> features;
  std::vector<double> treatment;
  std::vector<double> outcome;

  std::size_t row_count() const noexcept { return entities.size(); }
};

enum class UpliftLearner {
  T,
  X,
};

std::string_view to_string(UpliftLearner learner);

struct UpliftModelBundle {
  GbdtModel propensity;
  GbdtModel treated_outcome;
  GbdtModel control_outcome;
  GbdtModel treated_effect;
  GbdtModel control_effect;
};

struct UpliftLearnerMetrics {
  double auuc = 0.0;
  double qini_coefficient = 0.0;
  double uplift_at_target = 0.0;
};

struct QiniPoint {
  double fraction = 0.0;
  double incremental_gain = 0.0;
  double uplift = 0.0;
};

struct TreatmentEffect {
  std::int64_t entity = 0;
  double estimated_uplift = 0.0;
  double propensity = 0.0;
  double treatment = 0.0;
  double outcome = 0.0;
  bool overlap_eligible = false;
  bool recommended = false;
};

struct PropensitySummary {
  double minimum = 0.0;
  double maximum = 0.0;
  double mean = 0.0;
  double standard_deviation = 0.0;
};

struct UpliftResult {
  UpliftModelBundle models;
  UpliftLearner selected_learner = UpliftLearner::T;
  UpliftLearnerMetrics t_learner;
  UpliftLearnerMetrics x_learner;
  std::vector<QiniPoint> qini_curve;
  std::vector<TreatmentEffect> treatment_effects;
  PropensitySummary propensity;
  double uplift_lower = 0.0;
  double uplift_upper = 0.0;
  std::uint64_t treated_rows = 0;
  std::uint64_t control_rows = 0;
  std::uint64_t held_out_rows = 0;
  std::uint64_t overlap_excluded = 0;
  bool observational = false;
  compute::ComputeStatus status;

  bool ok() const noexcept { return status.ok(); }
};

struct UpliftScoredRow {
  std::int64_t entity = 0;
  double treatment = 0.0;
  double outcome = 0.0;
  double propensity = 0.0;
  double t_uplift = 0.0;
  double x_uplift = 0.0;
  bool held_out = false;
};

struct UpliftModelFit {
  UpliftModelBundle models;
  compute::ComputeStatus status;

  bool ok() const noexcept { return status.ok(); }
};

// Fit all five component GBDTs over this member's local rows. `allreduce`
// combines per-partition statistics. The split is deterministic from entity,
// arm, and seed, and is applied independently within both arms.
UpliftModelFit fit_uplift_models(const UpliftDataset &local,
                                 const UpliftParams &params,
                                 const AllReduce &allreduce,
                                 const std::string &round_tag = "uplift");

// Score local rows with an already-fitted bundle. This is the row-level partial
// gathered by the distributed job before global evaluation and targeting.
std::vector<UpliftScoredRow> score_uplift_rows(const UpliftDataset &local,
                                               const UpliftModelBundle &models,
                                               const UpliftParams &params);

// Evaluate gathered rows, choose T versus X on held-out Qini/AUUC, construct
// the targeting policy, and compute a seeded bootstrap uncertainty interval.
UpliftResult evaluate_uplift(const UpliftModelBundle &models,
                             std::vector<UpliftScoredRow> rows,
                             const UpliftParams &params);

// Single-member convenience. The distributed job uses the three primitives
// above with JobRound allreduce/gather/broadcast.
UpliftResult train_uplift(const UpliftDataset &dataset,
                          const UpliftParams &params = {});

} // namespace zetta::ml
