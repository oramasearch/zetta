#include "zetta/ml/anomaly_job.hpp"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

namespace zetta::ml {
namespace {

constexpr std::size_t kWireContributionLimit = 3;
constexpr std::size_t kWireRecordFields = 10 + kWireContributionLimit * 2;

void append_uint64(std::vector<double> &wire, std::uint64_t value) {
  wire.push_back(static_cast<double>(value >> 32U));
  wire.push_back(static_cast<double>(value & 0xffffffffULL));
}

std::optional<std::uint64_t> read_uint64(const std::vector<double> &wire,
                                         std::size_t &offset) {
  if (offset + 2 > wire.size() || wire[offset] < 0.0 ||
      wire[offset + 1] < 0.0) {
    return std::nullopt;
  }
  const std::uint64_t high = static_cast<std::uint64_t>(wire[offset]);
  const std::uint64_t low = static_cast<std::uint64_t>(wire[offset + 1]);
  offset += 2;
  if (high > 0xffffffffULL || low > 0xffffffffULL) {
    return std::nullopt;
  }
  return (high << 32U) | low;
}

std::vector<double>
encode_records(const std::vector<AnomalyRecord> &records,
               const std::vector<std::string> &feature_names) {
  std::vector<double> wire;
  wire.reserve(1 + records.size() * kWireRecordFields);
  wire.push_back(static_cast<double>(records.size()));
  for (const AnomalyRecord &record : records) {
    append_uint64(wire, record.ordinal);
    append_uint64(wire, std::bit_cast<std::uint64_t>(record.row));
    wire.push_back(record.period ? 1.0 : 0.0);
    wire.push_back(record.period.value_or(0.0));
    wire.push_back(record.score);
    wire.push_back(record.label ? 1.0 : 0.0);
    wire.push_back(record.label.value_or(0.0));
    const std::size_t count =
        std::min(record.contributions.size(), kWireContributionLimit);
    wire.push_back(static_cast<double>(count));
    for (std::size_t index = 0; index < kWireContributionLimit; ++index) {
      if (index < count) {
        const AnomalyContribution &contribution = record.contributions[index];
        const auto found = std::find(feature_names.begin(), feature_names.end(),
                                     contribution.feature);
        const std::size_t feature =
            found == feature_names.end()
                ? feature_names.size()
                : static_cast<std::size_t>(found - feature_names.begin());
        wire.push_back(static_cast<double>(feature));
        wire.push_back(contribution.deviation);
      } else {
        wire.push_back(0.0);
        wire.push_back(0.0);
      }
    }
  }
  return wire;
}

std::optional<std::vector<AnomalyRecord>>
decode_records(const std::vector<double> &wire,
               const std::vector<std::string> &feature_names) {
  if (wire.empty() || wire[0] < 0.0) {
    return std::nullopt;
  }
  const std::size_t count = static_cast<std::size_t>(wire[0]);
  if (wire.size() != 1 + count * kWireRecordFields) {
    return std::nullopt;
  }
  std::size_t offset = 1;
  std::vector<AnomalyRecord> records;
  records.reserve(count);
  for (std::size_t record_index = 0; record_index < count; ++record_index) {
    const std::optional<std::uint64_t> ordinal = read_uint64(wire, offset);
    const std::optional<std::uint64_t> row_bits = read_uint64(wire, offset);
    if (!ordinal || !row_bits || offset + 6 > wire.size()) {
      return std::nullopt;
    }
    AnomalyRecord record;
    record.ordinal = *ordinal;
    record.row = std::bit_cast<std::int64_t>(*row_bits);
    const bool has_period = wire[offset++] != 0.0;
    const double period = wire[offset++];
    if (has_period) {
      record.period = period;
    }
    record.score = wire[offset++];
    const bool has_label = wire[offset++] != 0.0;
    const double label = wire[offset++];
    if (has_label) {
      record.label = label;
    }
    const std::size_t contribution_count =
        static_cast<std::size_t>(wire[offset++]);
    if (contribution_count > kWireContributionLimit) {
      return std::nullopt;
    }
    for (std::size_t index = 0; index < kWireContributionLimit; ++index) {
      const std::size_t feature = static_cast<std::size_t>(wire[offset++]);
      const double deviation = wire[offset++];
      if (index < contribution_count) {
        if (feature >= feature_names.size()) {
          return std::nullopt;
        }
        record.contributions.push_back({feature_names[feature], deviation});
      }
    }
    records.push_back(std::move(record));
  }
  return records;
}

std::vector<double>
encode_global_result(const std::vector<AnomalyRecord> &records,
                     const AnomalyReport &report,
                     const std::vector<std::string> &feature_names) {
  std::vector<double> wire = {
      report.threshold,
      report.contamination ? 1.0 : 0.0,
      report.contamination.value_or(0.0),
      report.alert_budget ? 1.0 : 0.0,
      static_cast<double>(report.alert_budget.value_or(0)),
      static_cast<double>(report.rows_scored),
      static_cast<double>(report.flagged),
      report.alert_rate,
      report.precision_at_k ? 1.0 : 0.0,
      report.precision_at_k.value_or(0.0)};
  const std::vector<double> encoded = encode_records(records, feature_names);
  wire.insert(wire.end(), encoded.begin(), encoded.end());
  return wire;
}

bool decode_global_result(const std::vector<double> &wire,
                          const std::vector<std::string> &feature_names,
                          std::vector<AnomalyRecord> &records,
                          AnomalyReport &report) {
  constexpr std::size_t kHeaderSize = 10;
  if (wire.size() < kHeaderSize + 1) {
    return false;
  }
  report.threshold = wire[0];
  if (wire[1] != 0.0) {
    report.contamination = wire[2];
  }
  if (wire[3] != 0.0) {
    report.alert_budget = static_cast<std::size_t>(wire[4]);
  }
  report.rows_scored = static_cast<std::size_t>(wire[5]);
  report.flagged = static_cast<std::size_t>(wire[6]);
  report.alert_rate = wire[7];
  if (wire[8] != 0.0) {
    report.precision_at_k = wire[9];
  }
  const std::vector<double> record_wire(wire.begin() + kHeaderSize, wire.end());
  std::optional<std::vector<AnomalyRecord>> decoded =
      decode_records(record_wire, feature_names);
  if (!decoded) {
    return false;
  }
  records = std::move(*decoded);
  return records.size() == report.rows_scored;
}

AnomalyScoreResult
score_local_kpi(const std::vector<AnomalyPartition> &partitions,
                const std::vector<std::string> &feature_names,
                const AnomalyParams &params) {
  std::vector<double> periods;
  std::vector<std::vector<double>> columns(feature_names.size());
  std::vector<double> labels;
  bool has_labels = false;
  for (const AnomalyPartition &partition : partitions) {
    periods.insert(periods.end(), partition.periods.begin(),
                   partition.periods.end());
    for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
      if (feature >= partition.columns.size()) {
        AnomalyScoreResult failure;
        failure.status = compute::ComputeStatus::invalid_argument(
            "anomaly: KPI partition feature count mismatch");
        return failure;
      }
      columns[feature].insert(columns[feature].end(),
                              partition.columns[feature].begin(),
                              partition.columns[feature].end());
    }
    if (partition.has_labels()) {
      has_labels = true;
      labels.insert(labels.end(), partition.labels.begin(),
                    partition.labels.end());
    } else if (has_labels) {
      AnomalyScoreResult failure;
      failure.status = compute::ComputeStatus::invalid_argument(
          "anomaly: labels are present for only some KPI partitions");
      return failure;
    }
  }
  return score_kpi_anomalies(periods, columns, feature_names, labels, params);
}

} // namespace

DistributedAnomalyResult
run_anomaly_job(compute::JobCoordinator &coordinator, compute::ComputeJob &job,
                const std::vector<AnomalyPartition> &local_partitions,
                const std::vector<std::string> &global_partition_ids,
                const std::vector<std::string> &feature_names,
                const AnomalyParams &params, const std::string &round_tag,
                std::chrono::milliseconds timeout) {
  DistributedAnomalyResult result;
  result.status = coordinator.run(
      job, [&](compute::JobRound &round) -> compute::JobCoordinator::Outcome {
        result.is_root = round.is_root();
        if (params.mode == AnomalyMode::KpiTimeSeries &&
            round.participants().size() != 1) {
          return {
              .ok = false,
              .detail =
                  "anomaly KPI mode is single-member in phase A; distributed "
                  "causal prefix state is not implemented"};
        }

        AnomalyScoreResult local;
        if (params.mode == AnomalyMode::Tabular) {
          const AnomalyAllReduce reduce = [&](const std::string &tag,
                                              const std::vector<double> &values,
                                              AnomalyReduce operation)
              -> std::optional<std::vector<double>> {
            static_cast<void>(operation);
            return round.allreduce(tag, values, cluster::CollectiveOp::Sum,
                                   timeout);
          };
          local =
              score_tabular_anomalies(local_partitions, global_partition_ids,
                                      feature_names, params, reduce, round_tag);
        } else {
          local = score_local_kpi(local_partitions, feature_names, params);
        }
        if (!local.ok()) {
          return {.ok = false, .detail = local.status.message};
        }
        result.model = std::move(local.model);

        const std::vector<double> local_wire =
            encode_records(local.records, feature_names);
        const std::optional<std::vector<std::vector<double>>> gathered =
            round.gather(round_tag + ".scores", local_wire, timeout);
        if (!gathered) {
          return {.ok = false, .detail = "anomaly score gather failed"};
        }

        std::vector<double> global_wire;
        if (!gathered->empty()) {
          std::vector<AnomalyRecord> global_records;
          for (const std::vector<double> &member_wire : *gathered) {
            std::optional<std::vector<AnomalyRecord>> member_records =
                decode_records(member_wire, feature_names);
            if (!member_records) {
              return {.ok = false,
                      .detail = "anomaly score gather was malformed"};
            }
            global_records.insert(
                global_records.end(),
                std::make_move_iterator(member_records->begin()),
                std::make_move_iterator(member_records->end()));
          }
          AnomalyReport report;
          const compute::ComputeStatus threshold_status =
              apply_anomaly_threshold(global_records, params.threshold, report);
          if (!threshold_status.ok()) {
            return {.ok = false, .detail = threshold_status.message};
          }
          std::sort(global_records.begin(), global_records.end(),
                    [](const AnomalyRecord &left, const AnomalyRecord &right) {
                      if (left.period && right.period &&
                          *left.period != *right.period) {
                        return *left.period < *right.period;
                      }
                      return left.ordinal < right.ordinal;
                    });
          global_wire =
              encode_global_result(global_records, report, feature_names);
        }
        const std::optional<std::vector<double>> broadcast =
            round.broadcast(round_tag + ".result", global_wire, timeout);
        if (!broadcast ||
            !decode_global_result(*broadcast, feature_names, result.records,
                                  result.report)) {
          return {.ok = false, .detail = "anomaly result broadcast failed"};
        }
        return {.ok = true,
                .detail = "anomaly over " +
                          std::to_string(round.participants().size()) +
                          " members: " + std::to_string(result.records.size()) +
                          " rows, " + std::to_string(result.report.flagged) +
                          " flagged"};
      });
  return result;
}

} // namespace zetta::ml
