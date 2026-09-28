#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <vector>

#include "zetta/ml/anomaly.hpp"

namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<zetta::ml::AnomalyPartition> tabular_fixture(bool labels) {
  std::vector<zetta::ml::AnomalyPartition> partitions(4);
  for (std::size_t partition_index = 0; partition_index < partitions.size();
       ++partition_index) {
    zetta::ml::AnomalyPartition &partition = partitions[partition_index];
    partition.id = "partition-" + std::to_string(partition_index);
    partition.columns.assign(3, {});
    for (std::size_t local_row = 0; local_row < 50; ++local_row) {
      const std::size_t row = partition_index * 50 + local_row;
      partition.ordinals.push_back(row);
      partition.row_keys.push_back(static_cast<std::int64_t>(1000 + row));
      partition.columns[0].push_back(std::sin(static_cast<double>(row) * 0.17));
      partition.columns[1].push_back(std::cos(static_cast<double>(row) * 0.11));
      partition.columns[2].push_back(static_cast<double>(row % 9) * 0.1);
      if (labels) {
        partition.labels.push_back(0.0);
      }
    }
  }
  const std::vector<std::pair<std::size_t, std::size_t>> outliers = {
      {0, 48}, {1, 48}, {2, 48}};
  for (std::size_t index = 0; index < outliers.size(); ++index) {
    const auto [partition, row] = outliers[index];
    partitions[partition].columns[index][row] = 40.0 + index * 10.0;
    if (labels) {
      partitions[partition].labels[row] = 1.0;
    }
  }
  return partitions;
}

zetta::ml::AnomalyParams tabular_params() {
  zetta::ml::AnomalyParams params;
  params.tree_count = 64;
  params.subsample_size = 32;
  params.seed = 1046;
  params.threshold.contamination.reset();
  params.threshold.alert_budget = 3;
  return params;
}

std::vector<std::uint64_t>
flagged_ordinals(const std::vector<zetta::ml::AnomalyRecord> &records) {
  std::vector<std::uint64_t> ordinals;
  for (const zetta::ml::AnomalyRecord &record : records) {
    if (record.flagged) {
      ordinals.push_back(record.ordinal);
    }
  }
  std::sort(ordinals.begin(), ordinals.end());
  return ordinals;
}

std::vector<double> days(std::size_t count) {
  std::vector<double> periods(count);
  std::iota(periods.begin(), periods.end(), 0.0);
  return periods;
}

std::vector<double> seasonal_kpi(std::size_t count) {
  std::vector<double> values(count);
  for (std::size_t day = 0; day < count; ++day) {
    values[day] = 100.0 + 0.2 * static_cast<double>(day) +
                  18.0 * std::sin(2.0 * kPi * static_cast<double>(day) / 7.0);
  }
  return values;
}

zetta::ml::AnomalyParams kpi_params() {
  zetta::ml::AnomalyParams params;
  params.mode = zetta::ml::AnomalyMode::KpiTimeSeries;
  params.seasonal_period = 7;
  params.trend_window = 28;
  params.residual_window = 56;
  params.seasonal_cycles = 8;
  params.threshold.contamination.reset();
  params.threshold.alert_budget = 1;
  return params;
}

} // namespace

TEST(IsolationForestAnomaly, PlantedTabularOutliersRankAtTheTopWithoutLabels) {
  const zetta::ml::AnomalyScoreResult result =
      zetta::ml::score_tabular_anomalies(tabular_fixture(false),
                                         {"latency", "errors", "saturation"},
                                         tabular_params());
  ASSERT_TRUE(result.ok()) << result.status.message;
  EXPECT_EQ(result.model.trees.size(), 64u);
  EXPECT_EQ(result.report.flagged, 3u);
  EXPECT_DOUBLE_EQ(result.report.alert_rate, 3.0 / 200.0);
  EXPECT_EQ(flagged_ordinals(result.records),
            (std::vector<std::uint64_t>{48, 98, 148}));
  for (const zetta::ml::AnomalyRecord &record : result.records) {
    ASSERT_FALSE(record.contributions.empty());
    EXPECT_FALSE(record.label.has_value());
  }
}

TEST(AnomalyThreshold, AlertRateMatchesTheExplicitContaminationBudget) {
  zetta::ml::AnomalyParams params = tabular_params();
  params.threshold.alert_budget.reset();
  params.threshold.contamination = 0.05;
  const zetta::ml::AnomalyScoreResult result =
      zetta::ml::score_tabular_anomalies(
          tabular_fixture(false), {"latency", "errors", "saturation"}, params);
  ASSERT_TRUE(result.ok()) << result.status.message;
  EXPECT_EQ(result.report.flagged, 10u);
  EXPECT_DOUBLE_EQ(result.report.alert_rate, 0.05);
  std::vector<double> flagged_scores;
  for (const zetta::ml::AnomalyRecord &record : result.records) {
    if (record.flagged) {
      flagged_scores.push_back(record.score);
    }
  }
  ASSERT_EQ(flagged_scores.size(), 10u);
  EXPECT_DOUBLE_EQ(
      result.report.threshold,
      *std::min_element(flagged_scores.begin(), flagged_scores.end()));
}

TEST(AnomalyEvaluation, OptionalLabelsOnlyMeasurePrecisionAtK) {
  const zetta::ml::AnomalyScoreResult labelled =
      zetta::ml::score_tabular_anomalies(tabular_fixture(true),
                                         {"latency", "errors", "saturation"},
                                         tabular_params());
  const zetta::ml::AnomalyScoreResult unlabelled =
      zetta::ml::score_tabular_anomalies(tabular_fixture(false),
                                         {"latency", "errors", "saturation"},
                                         tabular_params());
  ASSERT_TRUE(labelled.ok()) << labelled.status.message;
  ASSERT_TRUE(unlabelled.ok()) << unlabelled.status.message;
  ASSERT_TRUE(labelled.report.precision_at_k.has_value());
  EXPECT_DOUBLE_EQ(*labelled.report.precision_at_k, 1.0);
  EXPECT_FALSE(unlabelled.report.precision_at_k.has_value());
  ASSERT_EQ(labelled.records.size(), unlabelled.records.size());
  for (std::size_t index = 0; index < labelled.records.size(); ++index) {
    EXPECT_DOUBLE_EQ(labelled.records[index].score,
                     unlabelled.records[index].score);
  }
}

TEST(SeasonalAnomaly, WeeklyPeaksAreExpectedAndThePlantedSpikeIsFlagged) {
  std::vector<double> values = seasonal_kpi(140);
  constexpr std::size_t kSpike = 100;
  values[kSpike] += 90.0;
  const zetta::ml::AnomalyScoreResult result = zetta::ml::score_kpi_anomalies(
      days(values.size()), {values}, {"requests"}, {}, kpi_params());
  ASSERT_TRUE(result.ok()) << result.status.message;
  const std::vector<std::uint64_t> flagged = flagged_ordinals(result.records);
  ASSERT_EQ(flagged.size(), 1u);
  EXPECT_EQ(flagged.front(), kSpike);

  const double spike_score = result.records[kSpike].score;
  for (std::size_t day = 65; day < values.size(); day += 7) {
    if (day == kSpike) {
      continue;
    }
    EXPECT_LT(result.records[day].score, spike_score);
    EXPECT_FALSE(result.records[day].flagged);
  }
}

TEST(SeasonalAnomaly, AFuturePointNeverChangesAPastScore) {
  std::vector<double> full_values = seasonal_kpi(150);
  full_values[100] += 90.0;
  full_values[149] += 500.0;
  std::vector<double> prefix_values(full_values.begin(),
                                    full_values.begin() + 120);
  const zetta::ml::AnomalyScoreResult prefix = zetta::ml::score_kpi_anomalies(
      days(prefix_values.size()), {prefix_values}, {"requests"}, {},
      kpi_params());
  const zetta::ml::AnomalyScoreResult full = zetta::ml::score_kpi_anomalies(
      days(full_values.size()), {full_values}, {"requests"}, {}, kpi_params());
  ASSERT_TRUE(prefix.ok()) << prefix.status.message;
  ASSERT_TRUE(full.ok()) << full.status.message;
  ASSERT_EQ(prefix.records.size(), 120u);
  for (std::size_t index = 0; index < prefix.records.size(); ++index) {
    EXPECT_DOUBLE_EQ(prefix.records[index].score, full.records[index].score)
        << "past score changed at period " << index;
  }
}

TEST(AnomalyThreshold, RefusesAmbiguousOrMissingThresholdPolicies) {
  std::vector<zetta::ml::AnomalyRecord> records(2);
  records[0].score = 1.0;
  records[1].score = 2.0;
  zetta::ml::AnomalyReport report;
  zetta::ml::AnomalyThresholdPolicy both;
  both.alert_budget = 1;
  EXPECT_FALSE(zetta::ml::apply_anomaly_threshold(records, both, report).ok());
  zetta::ml::AnomalyThresholdPolicy neither;
  neither.contamination.reset();
  EXPECT_FALSE(
      zetta::ml::apply_anomaly_threshold(records, neither, report).ok());
}

TEST(SeasonalAnomaly, RefusesDuplicateAndNonFiniteObservationTimes) {
  const auto duplicate = zetta::ml::score_kpi_anomalies(
      {1, 1, 2}, {{4, 5, 6}}, {"metric"}, {}, kpi_params());
  EXPECT_FALSE(duplicate.ok());
  EXPECT_NE(duplicate.status.message.find("unique"), std::string::npos);
  const auto nonfinite = zetta::ml::score_kpi_anomalies(
      {1, std::numeric_limits<double>::quiet_NaN(), 2}, {{4, 5, 6}}, {"metric"},
      {}, kpi_params());
  EXPECT_FALSE(nonfinite.ok());
}
