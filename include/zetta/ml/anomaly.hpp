// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "zetta/compute/compute_status.hpp"

// Native anomaly detection (#1046, M-7).
//
// Tabular detection combines a distributed isolation forest with robust
// median/MAD feature deviations. A tree is assigned to one catalog partition,
// trained from a deterministic bounded subsample of that partition, and
// combined with the other members' trees through an injected allreduce. The
// assignment is based on the seed, tree number, and stable partition ids, so
// moving the same partitions between members does not change the forest.
//
// KPI detection uses causal trend and seasonal windows. A score at position N
// reads positions strictly before N; appending future observations cannot
// change an earlier score. All residual scaling uses medians and median
// absolute deviations.
//
// Labels are optional evaluation data. They are never read by either trainer.
// See docs/ml/anomaly.md.
namespace zetta::ml {

enum class AnomalyMode {
  Tabular,
  KpiTimeSeries,
};

struct AnomalyThresholdPolicy {
  // Exactly one policy must be set. The default is an explicit 1% budget by
  // contamination. AlertBudget counts rows; Contamination is a fraction in
  // (0, 1].
  std::optional<double> contamination = 0.01;
  std::optional<std::size_t> alert_budget;
};

struct AnomalyParams {
  AnomalyMode mode = AnomalyMode::Tabular;
  std::size_t tree_count = 100;
  std::size_t subsample_size = 256;
  std::uint64_t seed = 0x5a17d3c4ULL;
  AnomalyThresholdPolicy threshold;

  // KPI windows count observations in time order. A period of 7 models a
  // weekly pattern in daily data. Trend and residual windows read only history.
  std::size_t seasonal_period = 7;
  std::size_t trend_window = 28;
  std::size_t residual_window = 56;
  std::size_t seasonal_cycles = 8;
};

// One physical dataset partition. Columns are column-major in feature_names
// order. ordinal is the globally stable row position used for exact
// alert-budget tie-breaking; row_key is the entity key shown in tabular output.
// KPI batches additionally carry periods.
struct AnomalyPartition {
  std::string id;
  std::vector<std::uint64_t> ordinals;
  std::vector<std::int64_t> row_keys;
  std::vector<double> periods;
  std::vector<std::vector<double>> columns;
  std::vector<double> labels;

  std::size_t row_count() const noexcept { return ordinals.size(); }
  bool has_labels() const noexcept { return !labels.empty(); }
};

struct IsolationTreeNode {
  int feature = -1;
  double threshold = 0.0;
  int left = -1;
  int right = -1;
  std::size_t leaf_size = 0;

  bool is_leaf() const noexcept { return feature < 0; }
};

struct IsolationTree {
  std::string partition_id;
  std::size_t sample_size = 0;
  std::vector<IsolationTreeNode> nodes;
};

struct AnomalyModel {
  AnomalyMode mode = AnomalyMode::Tabular;
  std::vector<std::string> feature_names;
  std::vector<IsolationTree> trees;
  std::vector<double> robust_centers;
  std::vector<double> robust_scales;
  std::uint64_t seed = 0;
  std::size_t seasonal_period = 0;
  std::size_t trend_window = 0;
};

struct AnomalyContribution {
  std::string feature;
  double deviation = 0.0;
};

struct AnomalyRecord {
  std::uint64_t ordinal = 0;
  std::int64_t row = 0;
  std::optional<double> period;
  double score = 0.0;
  bool flagged = false;
  std::vector<AnomalyContribution> contributions;
  std::optional<double> label;
};

struct AnomalyReport {
  double threshold = 0.0;
  std::optional<double> contamination;
  std::optional<std::size_t> alert_budget;
  std::size_t rows_scored = 0;
  std::size_t flagged = 0;
  double alert_rate = 0.0;
  std::optional<double> precision_at_k;
};

enum class AnomalyReduce {
  Sum,
};

using AnomalyAllReduce = std::function<std::optional<std::vector<double>>(
    const std::string &tag, const std::vector<double> &local,
    AnomalyReduce operation)>;

struct AnomalyScoreResult {
  AnomalyModel model;
  std::vector<AnomalyRecord> records;
  AnomalyReport report;
  compute::ComputeStatus status;

  bool ok() const noexcept { return status.ok(); }
};

// Train and score this member's tabular partitions. global_partition_ids is the
// identical sorted catalog partition list on every participant. Exactly one
// participant supplies each partition (the table runner selects its primary
// owner); all others contribute zeros to that tree's reduction.
//
// The returned records are local and unflagged. A distributed runner applies
// the global budget after gathering only the compact score/explanation rows.
AnomalyScoreResult score_tabular_anomalies(
    const std::vector<AnomalyPartition> &local_partitions,
    const std::vector<std::string> &global_partition_ids,
    const std::vector<std::string> &feature_names, const AnomalyParams &params,
    const AnomalyAllReduce &allreduce, const std::string &tag = "anomaly");

// Single-member convenience over the complete partition set. It deliberately
// preserves the partition boundaries, so it is the oracle for a distributed run
// over those same partitions.
AnomalyScoreResult
score_tabular_anomalies(const std::vector<AnomalyPartition> &partitions,
                        const std::vector<std::string> &feature_names,
                        const AnomalyParams &params = {});

// Score KPI periods in increasing time order. columns is metric-major and
// labels is optional. Trend, seasonality, residual center, and residual scale
// for row N are fitted only from rows before N.
AnomalyScoreResult
score_kpi_anomalies(const std::vector<double> &periods,
                    const std::vector<std::vector<double>> &columns,
                    const std::vector<std::string> &metric_names,
                    const std::vector<double> &labels,
                    const AnomalyParams &params);

// Apply an exact global contamination/alert budget. Ties are resolved by stable
// ordinal, so the number flagged is exact and reproducible. Labels, when
// present, are used only for precision@k.
compute::ComputeStatus
apply_anomaly_threshold(std::vector<AnomalyRecord> &records,
                        const AnomalyThresholdPolicy &policy,
                        AnomalyReport &report);

} // namespace zetta::ml
