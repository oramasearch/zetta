#include "zetta/ml/uplift_job.hpp"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "zetta/cluster/collectives.hpp"

#include "feature_columns.hpp"

namespace zetta::ml {
namespace {

using compute::JobCoordinator;
using compute::JobRound;
using Outcome = JobCoordinator::Outcome;

zetta::cluster::CollectiveOp collective_op(GbdtReduce operation) {
  switch (operation) {
  case GbdtReduce::Sum:
    return zetta::cluster::CollectiveOp::Sum;
  case GbdtReduce::Min:
    return zetta::cluster::CollectiveOp::Minimum;
  case GbdtReduce::Max:
    return zetta::cluster::CollectiveOp::Maximum;
  }
  return zetta::cluster::CollectiveOp::Sum;
}

std::vector<double> encode_rows(const std::vector<UpliftScoredRow> &rows) {
  std::vector<double> wire;
  wire.reserve(rows.size() * 8);
  for (const UpliftScoredRow &row : rows) {
    const std::uint64_t entity = std::bit_cast<std::uint64_t>(row.entity);
    wire.push_back(static_cast<double>(entity >> 32));
    wire.push_back(static_cast<double>(entity & 0xffffffffull));
    wire.push_back(row.treatment);
    wire.push_back(row.outcome);
    wire.push_back(row.propensity);
    wire.push_back(row.t_uplift);
    wire.push_back(row.x_uplift);
    wire.push_back(row.held_out ? 1.0 : 0.0);
  }
  return wire;
}

std::vector<UpliftScoredRow>
decode_rows(const std::vector<std::vector<double>> &gathered) {
  std::vector<UpliftScoredRow> rows;
  for (const std::vector<double> &member : gathered) {
    for (std::size_t offset = 0; offset + 7 < member.size(); offset += 8) {
      const std::uint64_t entity =
          (static_cast<std::uint64_t>(member[offset]) << 32) |
          static_cast<std::uint64_t>(member[offset + 1]);
      rows.push_back(UpliftScoredRow{
          std::bit_cast<std::int64_t>(entity), member[offset + 2],
          member[offset + 3], member[offset + 4], member[offset + 5],
          member[offset + 6], member[offset + 7] != 0.0});
    }
  }
  return rows;
}

std::vector<double> encode_result(const UpliftResult &result) {
  std::vector<double> wire = {
      1.0,
      result.selected_learner == UpliftLearner::X ? 1.0 : 0.0,
      result.t_learner.auuc,
      result.t_learner.qini_coefficient,
      result.t_learner.uplift_at_target,
      result.x_learner.auuc,
      result.x_learner.qini_coefficient,
      result.x_learner.uplift_at_target,
      result.uplift_lower,
      result.uplift_upper,
      static_cast<double>(result.treated_rows),
      static_cast<double>(result.control_rows),
      static_cast<double>(result.held_out_rows),
      static_cast<double>(result.overlap_excluded),
      result.propensity.minimum,
      result.propensity.maximum,
      result.propensity.mean,
      result.propensity.standard_deviation,
      result.observational ? 1.0 : 0.0,
      static_cast<double>(result.qini_curve.size()),
      static_cast<double>(result.treatment_effects.size()),
  };
  for (const QiniPoint &point : result.qini_curve) {
    wire.push_back(point.fraction);
    wire.push_back(point.incremental_gain);
    wire.push_back(point.uplift);
  }
  for (const TreatmentEffect &effect : result.treatment_effects) {
    const std::uint64_t entity = std::bit_cast<std::uint64_t>(effect.entity);
    wire.push_back(static_cast<double>(entity >> 32));
    wire.push_back(static_cast<double>(entity & 0xffffffffull));
    wire.push_back(effect.estimated_uplift);
    wire.push_back(effect.propensity);
    wire.push_back(effect.treatment);
    wire.push_back(effect.outcome);
    wire.push_back(effect.overlap_eligible ? 1.0 : 0.0);
    wire.push_back(effect.recommended ? 1.0 : 0.0);
  }
  return wire;
}

bool decode_result(const std::vector<double> &wire, UpliftResult &result) {
  constexpr std::size_t kHeader = 21;
  if (wire.size() < kHeader || wire[0] != 1.0) {
    return false;
  }
  result.selected_learner =
      wire[1] == 1.0 ? UpliftLearner::X : UpliftLearner::T;
  result.t_learner = {wire[2], wire[3], wire[4]};
  result.x_learner = {wire[5], wire[6], wire[7]};
  result.uplift_lower = wire[8];
  result.uplift_upper = wire[9];
  result.treated_rows = static_cast<std::uint64_t>(wire[10]);
  result.control_rows = static_cast<std::uint64_t>(wire[11]);
  result.held_out_rows = static_cast<std::uint64_t>(wire[12]);
  result.overlap_excluded = static_cast<std::uint64_t>(wire[13]);
  result.propensity = {wire[14], wire[15], wire[16], wire[17]};
  result.observational = wire[18] != 0.0;
  const std::size_t qini_count = static_cast<std::size_t>(wire[19]);
  const std::size_t effect_count = static_cast<std::size_t>(wire[20]);
  const std::size_t expected = kHeader + qini_count * 3 + effect_count * 8;
  if (wire.size() != expected) {
    return false;
  }
  std::size_t offset = kHeader;
  result.qini_curve.clear();
  result.qini_curve.reserve(qini_count);
  for (std::size_t index = 0; index < qini_count; ++index) {
    result.qini_curve.push_back(
        QiniPoint{wire[offset], wire[offset + 1], wire[offset + 2]});
    offset += 3;
  }
  result.treatment_effects.clear();
  result.treatment_effects.reserve(effect_count);
  for (std::size_t index = 0; index < effect_count; ++index) {
    const std::uint64_t entity =
        (static_cast<std::uint64_t>(wire[offset]) << 32) |
        static_cast<std::uint64_t>(wire[offset + 1]);
    result.treatment_effects.push_back(
        TreatmentEffect{std::bit_cast<std::int64_t>(entity), wire[offset + 2],
                        wire[offset + 3], wire[offset + 4], wire[offset + 5],
                        wire[offset + 6] != 0.0, wire[offset + 7] != 0.0});
    offset += 8;
  }
  result.status = compute::ComputeStatus::success();
  return true;
}

bool binary_column(const std::vector<double> &values) {
  return std::ranges::all_of(
      values, [](double value) { return value == 0.0 || value == 1.0; });
}

} // namespace

DistributedUpliftResult run_uplift_job(
    compute::JobCoordinator &coordinator, compute::ComputeJob &job,
    const ddf::DataFrame &local_shard, const std::string &entity_column,
    const std::string &treatment_column, const std::string &outcome_column,
    const std::vector<std::string> &feature_columns, const UpliftParams &params,
    const std::string &round_tag, std::chrono::milliseconds timeout) {
  DistributedUpliftResult output;
  const std::string tag =
      round_tag.empty() ? "uplift:" + treatment_column + ":" + outcome_column
                        : round_tag;

  output.status = coordinator.run(job, [&](JobRound &round) -> Outcome {
    output.is_root = round.is_root();
    UpliftDataset local;
    local.feature_names = feature_columns;
    local.features.resize(feature_columns.size());
    for (std::size_t index = 0; index < feature_columns.size(); ++index) {
      const compute::ComputeStatus status = detail::read_column_as_double(
          local_shard, feature_columns[index], local.features[index]);
      if (!status.ok()) {
        return {.ok = false, .detail = status.message};
      }
    }
    if (const compute::ComputeStatus status = detail::read_column_as_double(
            local_shard, treatment_column, local.treatment);
        !status.ok()) {
      return {.ok = false, .detail = status.message};
    }
    if (const compute::ComputeStatus status = detail::read_column_as_double(
            local_shard, outcome_column, local.outcome);
        !status.ok()) {
      return {.ok = false, .detail = status.message};
    }
    if (const compute::ComputeStatus status =
            local_shard.to_host<std::int64_t>(entity_column, local.entities);
        !status.ok()) {
      return {.ok = false, .detail = status.message};
    }

    const double invalid_treatment = binary_column(local.treatment) ? 0.0 : 1.0;
    const double invalid_outcome = binary_column(local.outcome) ? 0.0 : 1.0;
    const std::optional<std::vector<double>> validation =
        round.allreduce(tag + ".validate",
                        {static_cast<double>(local.row_count()),
                         invalid_treatment, invalid_outcome},
                        zetta::cluster::CollectiveOp::Sum, timeout);
    if (!validation || validation->size() != 3) {
      return {.ok = false, .detail = "uplift validation collective failed"};
    }
    if ((*validation)[0] <= 0.0) {
      return {.ok = false, .detail = "uplift: the dataset has no rows"};
    }
    if ((*validation)[1] > 0.0) {
      return {.ok = false,
              .detail = "uplift: treatment column must be binary {0,1}"};
    }
    if ((*validation)[2] > 0.0) {
      return {.ok = false,
              .detail = "uplift: outcome column must be binary {0,1}"};
    }

    const AllReduce allreduce = [&](const std::string &name,
                                    const std::vector<double> &values,
                                    GbdtReduce operation) {
      const std::optional<std::vector<double>> reduced =
          round.allreduce(name, values, collective_op(operation), timeout);
      return reduced.value_or(std::vector<double>{});
    };
    UpliftModelFit fit =
        fit_uplift_models(local, params, allreduce, tag + ".models");
    if (!fit.ok()) {
      return {.ok = false, .detail = fit.status.message};
    }

    const std::vector<UpliftScoredRow> local_rows =
        score_uplift_rows(local, fit.models, params);
    const std::optional<std::vector<std::vector<double>>> gathered =
        round.gather(tag + ".rows", encode_rows(local_rows), timeout);
    if (!gathered) {
      return {.ok = false, .detail = "uplift held-out gather failed"};
    }
    std::vector<double> result_wire;
    if (!gathered->empty()) {
      output.uplift =
          evaluate_uplift(fit.models, decode_rows(*gathered), params);
      if (!output.uplift.ok()) {
        return {.ok = false, .detail = output.uplift.status.message};
      }
      result_wire = encode_result(output.uplift);
    }
    const std::optional<std::vector<double>> broadcast =
        round.broadcast(tag + ".result", result_wire, timeout);
    if (!broadcast) {
      return {.ok = false, .detail = "uplift result broadcast failed"};
    }
    output.uplift.models = std::move(fit.models);
    if (!decode_result(*broadcast, output.uplift)) {
      return {.ok = false, .detail = "uplift result broadcast was malformed"};
    }
    return {.ok = true,
            .detail = "uplift over " +
                      std::to_string(round.participants().size()) +
                      " members; selected " +
                      std::string(to_string(output.uplift.selected_learner))};
  });
  if (output.status != compute::JobStatus::Succeeded &&
      output.uplift.status.ok()) {
    output.uplift.status = compute::ComputeStatus::backend_error(job.detail());
  }
  return output;
}

} // namespace zetta::ml
