// SPDX-License-Identifier: MIT

#include "zetta/ml/anomaly.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <string_view>
#include <utility>

namespace zetta::ml {
namespace {

constexpr double kMadConsistency = 1.4826;
constexpr double kEulerMascheroni = 0.5772156649015328606;
constexpr std::size_t kContributionLimit = 3;

compute::ComputeStatus invalid(std::string reason) {
  return compute::ComputeStatus::invalid_argument("anomaly: " +
                                                  std::move(reason));
}

double median(std::vector<double> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const std::size_t middle = values.size() / 2;
  if (values.size() % 2 != 0) {
    return values[middle];
  }
  return (values[middle - 1] + values[middle]) * 0.5;
}

double median_absolute_deviation(const std::vector<double> &values,
                                 double center) {
  std::vector<double> deviations;
  deviations.reserve(values.size());
  for (const double value : values) {
    deviations.push_back(std::abs(value - center));
  }
  return median(std::move(deviations));
}

std::uint64_t mix_seed(std::uint64_t value) {
  value += 0x9e3779b97f4a7c15ULL;
  value = (value ^ (value >> 30U)) * 0xbf58476d1ce4e5b9ULL;
  value = (value ^ (value >> 27U)) * 0x94d049bb133111ebULL;
  return value ^ (value >> 31U);
}

std::size_t isolation_depth_limit(std::size_t sample_size) {
  if (sample_size <= 1) {
    return 0;
  }
  return static_cast<std::size_t>(
      std::ceil(std::log2(static_cast<double>(sample_size))));
}

double expected_path_length(std::size_t sample_size) {
  if (sample_size <= 1) {
    return 0.0;
  }
  if (sample_size == 2) {
    return 1.0;
  }
  const double count = static_cast<double>(sample_size);
  const double harmonic = std::log(count - 1.0) + kEulerMascheroni;
  return 2.0 * harmonic - 2.0 * (count - 1.0) / count;
}

class IsolationTreeBuilder {
public:
  IsolationTreeBuilder(const AnomalyPartition &partition,
                       std::vector<std::size_t> sample, std::uint64_t seed)
      : partition_(partition), sample_(std::move(sample)), generator_(seed),
        depth_limit_(isolation_depth_limit(sample_.size())) {}

  IsolationTree build() {
    IsolationTree tree;
    tree.partition_id = partition_.id;
    tree.sample_size = sample_.size();
    tree.nodes.reserve(sample_.size() * 2);
    build_node(tree, sample_, 0);
    return tree;
  }

private:
  int build_node(IsolationTree &tree, const std::vector<std::size_t> &rows,
                 std::size_t depth) {
    const int node_index = static_cast<int>(tree.nodes.size());
    tree.nodes.push_back(IsolationTreeNode{});
    if (rows.size() <= 1 || depth >= depth_limit_) {
      tree.nodes[static_cast<std::size_t>(node_index)].leaf_size = rows.size();
      return node_index;
    }

    std::vector<std::size_t> candidates;
    std::vector<std::pair<double, double>> ranges(partition_.columns.size());
    for (std::size_t feature = 0; feature < partition_.columns.size();
         ++feature) {
      double minimum = std::numeric_limits<double>::infinity();
      double maximum = -std::numeric_limits<double>::infinity();
      for (const std::size_t row : rows) {
        const double value = partition_.columns[feature][row];
        minimum = std::min(minimum, value);
        maximum = std::max(maximum, value);
      }
      ranges[feature] = {minimum, maximum};
      if (minimum < maximum) {
        candidates.push_back(feature);
      }
    }
    if (candidates.empty()) {
      tree.nodes[static_cast<std::size_t>(node_index)].leaf_size = rows.size();
      return node_index;
    }

    std::uniform_int_distribution<std::size_t> feature_distribution(
        0, candidates.size() - 1);
    const std::size_t feature = candidates[feature_distribution(generator_)];
    const auto [minimum, maximum] = ranges[feature];
    std::uniform_real_distribution<double> split_distribution(minimum, maximum);
    double threshold = split_distribution(generator_);
    if (threshold <= minimum) {
      threshold = std::nextafter(minimum, maximum);
    }

    std::vector<std::size_t> left_rows;
    std::vector<std::size_t> right_rows;
    left_rows.reserve(rows.size());
    right_rows.reserve(rows.size());
    for (const std::size_t row : rows) {
      if (partition_.columns[feature][row] < threshold) {
        left_rows.push_back(row);
      } else {
        right_rows.push_back(row);
      }
    }
    if (left_rows.empty() || right_rows.empty()) {
      tree.nodes[static_cast<std::size_t>(node_index)].leaf_size = rows.size();
      return node_index;
    }

    const int left = build_node(tree, left_rows, depth + 1);
    const int right = build_node(tree, right_rows, depth + 1);
    IsolationTreeNode &node = tree.nodes[static_cast<std::size_t>(node_index)];
    node.feature = static_cast<int>(feature);
    node.threshold = threshold;
    node.left = left;
    node.right = right;
    return node_index;
  }

  const AnomalyPartition &partition_;
  std::vector<std::size_t> sample_;
  std::mt19937_64 generator_;
  std::size_t depth_limit_ = 0;
};

std::vector<std::size_t> sample_rows(std::size_t row_count,
                                     std::size_t sample_size,
                                     std::uint64_t seed) {
  std::vector<std::size_t> rows(row_count);
  std::iota(rows.begin(), rows.end(), 0);
  std::mt19937_64 generator(seed);
  std::shuffle(rows.begin(), rows.end(), generator);
  rows.resize(std::min(row_count, sample_size));
  return rows;
}

double tree_path_length(const IsolationTree &tree,
                        const std::vector<double> &row) {
  if (tree.nodes.empty()) {
    return 0.0;
  }
  int node_index = 0;
  std::size_t depth = 0;
  while (node_index >= 0 &&
         static_cast<std::size_t>(node_index) < tree.nodes.size()) {
    const IsolationTreeNode &node =
        tree.nodes[static_cast<std::size_t>(node_index)];
    if (node.is_leaf()) {
      return static_cast<double>(depth) + expected_path_length(node.leaf_size);
    }
    const std::size_t feature = static_cast<std::size_t>(node.feature);
    if (feature >= row.size()) {
      return static_cast<double>(depth);
    }
    node_index = row[feature] < node.threshold ? node.left : node.right;
    ++depth;
  }
  return static_cast<double>(depth);
}

double forest_score(const AnomalyModel &model, const std::vector<double> &row) {
  if (model.trees.empty()) {
    return 0.0;
  }
  double normalized_path = 0.0;
  std::size_t contributing = 0;
  for (const IsolationTree &tree : model.trees) {
    const double normalizer = expected_path_length(tree.sample_size);
    if (normalizer <= 0.0) {
      continue;
    }
    normalized_path += tree_path_length(tree, row) / normalizer;
    ++contributing;
  }
  if (contributing == 0) {
    return 0.0;
  }
  normalized_path /= static_cast<double>(contributing);
  return std::pow(2.0, -normalized_path);
}

std::vector<double> encode_tree(const IsolationTree *tree,
                                std::size_t maximum_nodes) {
  constexpr std::size_t kFieldsPerNode = 5;
  std::vector<double> wire(3 + maximum_nodes * kFieldsPerNode, 0.0);
  if (tree == nullptr) {
    return wire;
  }
  wire[0] = 1.0;
  wire[1] = static_cast<double>(tree->sample_size);
  wire[2] = static_cast<double>(tree->nodes.size());
  for (std::size_t index = 0; index < tree->nodes.size(); ++index) {
    const IsolationTreeNode &node = tree->nodes[index];
    const std::size_t offset = 3 + index * kFieldsPerNode;
    wire[offset] = static_cast<double>(node.feature);
    wire[offset + 1] = node.threshold;
    wire[offset + 2] = static_cast<double>(node.left);
    wire[offset + 3] = static_cast<double>(node.right);
    wire[offset + 4] = static_cast<double>(node.leaf_size);
  }
  return wire;
}

std::optional<IsolationTree> decode_tree(const std::vector<double> &wire,
                                         std::size_t maximum_nodes,
                                         std::string partition_id) {
  constexpr std::size_t kFieldsPerNode = 5;
  if (wire.size() != 3 + maximum_nodes * kFieldsPerNode ||
      std::abs(wire[0] - 1.0) > 1e-9) {
    return std::nullopt;
  }
  const std::size_t node_count = static_cast<std::size_t>(wire[2]);
  if (node_count == 0 || node_count > maximum_nodes) {
    return std::nullopt;
  }
  IsolationTree tree;
  tree.partition_id = std::move(partition_id);
  tree.sample_size = static_cast<std::size_t>(wire[1]);
  tree.nodes.resize(node_count);
  for (std::size_t index = 0; index < node_count; ++index) {
    const std::size_t offset = 3 + index * kFieldsPerNode;
    IsolationTreeNode &node = tree.nodes[index];
    node.feature = static_cast<int>(wire[offset]);
    node.threshold = wire[offset + 1];
    node.left = static_cast<int>(wire[offset + 2]);
    node.right = static_cast<int>(wire[offset + 3]);
    node.leaf_size = static_cast<std::size_t>(wire[offset + 4]);
    if (!node.is_leaf() &&
        (node.left < 0 || node.right < 0 ||
         static_cast<std::size_t>(node.left) >= node_count ||
         static_cast<std::size_t>(node.right) >= node_count)) {
      return std::nullopt;
    }
  }
  return tree;
}

const AnomalyPartition *
find_partition(const std::vector<AnomalyPartition> &partitions,
               std::string_view id) {
  const auto found = std::find_if(
      partitions.begin(), partitions.end(),
      [id](const AnomalyPartition &partition) { return partition.id == id; });
  return found == partitions.end() ? nullptr : &*found;
}

compute::ComputeStatus
validate_threshold(const AnomalyThresholdPolicy &policy) {
  const bool has_contamination = policy.contamination.has_value();
  const bool has_budget = policy.alert_budget.has_value();
  if (has_contamination == has_budget) {
    return invalid(
        "set exactly one threshold policy: contamination or alert_budget");
  }
  if (has_contamination &&
      (!std::isfinite(*policy.contamination) || *policy.contamination <= 0.0 ||
       *policy.contamination > 1.0)) {
    return invalid("contamination must be in (0, 1]");
  }
  if (has_budget && *policy.alert_budget == 0) {
    return invalid("alert_budget must be positive");
  }
  return compute::ComputeStatus::success();
}

compute::ComputeStatus
validate_partitions(const std::vector<AnomalyPartition> &partitions,
                    const std::vector<std::string> &feature_names) {
  for (const AnomalyPartition &partition : partitions) {
    if (partition.id.empty()) {
      return invalid("a partition has no stable id");
    }
    if (partition.columns.size() != feature_names.size()) {
      return invalid("partition '" + partition.id +
                     "' feature count does not match feature_names");
    }
    if (partition.row_keys.size() != partition.row_count()) {
      return invalid("partition '" + partition.id +
                     "' row key count does not match its rows");
    }
    if (!partition.periods.empty() &&
        partition.periods.size() != partition.row_count()) {
      return invalid("partition '" + partition.id +
                     "' period count does not match its rows");
    }
    if (partition.has_labels() &&
        partition.labels.size() != partition.row_count()) {
      return invalid("partition '" + partition.id +
                     "' label count does not match its rows");
    }
    for (const std::vector<double> &column : partition.columns) {
      if (column.size() != partition.row_count()) {
        return invalid("partition '" + partition.id +
                       "' has a ragged feature column");
      }
      if (std::any_of(column.begin(), column.end(),
                      [](double value) { return !std::isfinite(value); })) {
        return invalid("partition '" + partition.id +
                       "' contains a non-finite feature value");
      }
    }
  }
  return compute::ComputeStatus::success();
}

double weighted_median(std::vector<std::pair<double, double>> values) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end(),
            [](const auto &left, const auto &right) {
              return left.first < right.first;
            });
  double total_weight = 0.0;
  for (const auto &[value, weight] : values) {
    static_cast<void>(value);
    total_weight += weight;
  }
  double accumulated = 0.0;
  for (const auto &[value, weight] : values) {
    accumulated += weight;
    if (accumulated * 2.0 >= total_weight) {
      return value;
    }
  }
  return values.back().first;
}

compute::ComputeStatus
reduce_robust_statistics(const std::vector<AnomalyPartition> &local_partitions,
                         const std::vector<std::string> &global_partition_ids,
                         const std::vector<std::string> &feature_names,
                         const AnomalyAllReduce &allreduce,
                         const std::string &tag, AnomalyModel &model) {
  const std::size_t fields = 1 + feature_names.size();
  std::vector<double> local_centers(global_partition_ids.size() * fields, 0.0);
  for (std::size_t partition_index = 0;
       partition_index < global_partition_ids.size(); ++partition_index) {
    const AnomalyPartition *partition =
        find_partition(local_partitions, global_partition_ids[partition_index]);
    if (partition == nullptr || partition->row_count() == 0) {
      continue;
    }
    const std::size_t offset = partition_index * fields;
    local_centers[offset] = static_cast<double>(partition->row_count());
    for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
      local_centers[offset + 1 + feature] = median(partition->columns[feature]);
    }
  }
  const std::optional<std::vector<double>> center_wire =
      allreduce(tag + ".robust.centers", local_centers, AnomalyReduce::Sum);
  if (!center_wire || center_wire->size() != local_centers.size()) {
    return invalid("robust center allreduce failed");
  }

  model.robust_centers.assign(feature_names.size(), 0.0);
  for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
    std::vector<std::pair<double, double>> summaries;
    for (std::size_t partition_index = 0;
         partition_index < global_partition_ids.size(); ++partition_index) {
      const std::size_t offset = partition_index * fields;
      const double count = (*center_wire)[offset];
      if (count > 0.0) {
        summaries.emplace_back((*center_wire)[offset + 1 + feature], count);
      }
    }
    model.robust_centers[feature] = weighted_median(std::move(summaries));
  }

  std::vector<double> local_scales(global_partition_ids.size() * fields, 0.0);
  for (std::size_t partition_index = 0;
       partition_index < global_partition_ids.size(); ++partition_index) {
    const AnomalyPartition *partition =
        find_partition(local_partitions, global_partition_ids[partition_index]);
    if (partition == nullptr || partition->row_count() == 0) {
      continue;
    }
    const std::size_t offset = partition_index * fields;
    local_scales[offset] = static_cast<double>(partition->row_count());
    for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
      local_scales[offset + 1 + feature] = median_absolute_deviation(
          partition->columns[feature], model.robust_centers[feature]);
    }
  }
  const std::optional<std::vector<double>> scale_wire =
      allreduce(tag + ".robust.scales", local_scales, AnomalyReduce::Sum);
  if (!scale_wire || scale_wire->size() != local_scales.size()) {
    return invalid("robust scale allreduce failed");
  }

  model.robust_scales.assign(feature_names.size(), 0.0);
  for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
    std::vector<std::pair<double, double>> summaries;
    for (std::size_t partition_index = 0;
         partition_index < global_partition_ids.size(); ++partition_index) {
      const std::size_t offset = partition_index * fields;
      const double count = (*scale_wire)[offset];
      if (count > 0.0) {
        summaries.emplace_back((*scale_wire)[offset + 1 + feature], count);
      }
    }
    model.robust_scales[feature] =
        kMadConsistency * weighted_median(std::move(summaries));
  }
  return compute::ComputeStatus::success();
}

std::vector<AnomalyContribution>
tabular_contributions(const AnomalyModel &model,
                      const std::vector<double> &row) {
  std::vector<AnomalyContribution> contributions;
  contributions.reserve(model.feature_names.size());
  for (std::size_t feature = 0; feature < model.feature_names.size();
       ++feature) {
    const double floor =
        std::max(1e-12, std::abs(model.robust_centers[feature]) * 1e-12);
    const double scale = std::max(model.robust_scales[feature], floor);
    contributions.push_back(
        {model.feature_names[feature],
         std::abs(row[feature] - model.robust_centers[feature]) / scale});
  }
  std::sort(
      contributions.begin(), contributions.end(),
      [](const AnomalyContribution &left, const AnomalyContribution &right) {
        if (left.deviation != right.deviation) {
          return left.deviation > right.deviation;
        }
        return left.feature < right.feature;
      });
  if (contributions.size() > kContributionLimit) {
    contributions.resize(kContributionLimit);
  }
  return contributions;
}

double
robust_component_score(const std::vector<AnomalyContribution> &contributions) {
  if (contributions.empty()) {
    return 0.0;
  }
  return 1.0 - std::exp(-contributions.front().deviation / 3.0);
}

std::pair<double, double> causal_trend(const std::vector<double> &periods,
                                       const std::vector<double> &values,
                                       std::size_t end, std::size_t window) {
  const std::size_t begin = end > window ? end - window : 0;
  if (end <= begin + 1) {
    const double fallback = end == 0 ? 0.0 : values[end - 1];
    return {0.0, fallback};
  }
  std::vector<double> slopes;
  for (std::size_t left = begin; left < end; ++left) {
    for (std::size_t right = left + 1; right < end; ++right) {
      const double delta = periods[right] - periods[left];
      if (delta != 0.0) {
        slopes.push_back((values[right] - values[left]) / delta);
      }
    }
  }
  const double slope = median(std::move(slopes));
  std::vector<double> intercepts;
  intercepts.reserve(end - begin);
  for (std::size_t index = begin; index < end; ++index) {
    intercepts.push_back(values[index] - slope * periods[index]);
  }
  return {slope, median(std::move(intercepts))};
}

std::vector<double> causal_metric_deviations(const std::vector<double> &periods,
                                             const std::vector<double> &values,
                                             const AnomalyParams &params) {
  std::vector<double> detrended(values.size(), 0.0);
  std::vector<double> residuals(values.size(), 0.0);
  std::vector<double> deviations(values.size(), 0.0);
  for (std::size_t index = 0; index < values.size(); ++index) {
    const auto [slope, intercept] =
        causal_trend(periods, values, index, params.trend_window);
    const double trend = slope * periods[index] + intercept;
    detrended[index] = values[index] - trend;

    std::vector<double> seasonal_history;
    if (params.seasonal_period > 0) {
      const std::size_t maximum =
          params.seasonal_period * params.seasonal_cycles;
      for (std::size_t previous = index; previous >= params.seasonal_period;) {
        previous -= params.seasonal_period;
        seasonal_history.push_back(detrended[previous]);
        if (seasonal_history.size() >= params.seasonal_cycles ||
            index - previous >= maximum) {
          break;
        }
      }
    }
    const double seasonal = median(std::move(seasonal_history));
    residuals[index] = detrended[index] - seasonal;

    const std::size_t begin =
        index > params.residual_window ? index - params.residual_window : 0;
    std::vector<double> history;
    history.reserve(index - begin);
    for (std::size_t previous = begin; previous < index; ++previous) {
      history.push_back(residuals[previous]);
    }
    const std::size_t minimum_history =
        std::max<std::size_t>(4, params.seasonal_period * 2);
    if (history.size() < minimum_history) {
      continue;
    }
    const double center = median(history);
    const double mad = median_absolute_deviation(history, center);
    std::vector<double> magnitudes;
    magnitudes.reserve(history.size());
    for (const double value : history) {
      magnitudes.push_back(std::abs(value));
    }
    const double magnitude = median(std::move(magnitudes));
    const double scale =
        std::max(kMadConsistency * mad, std::max(1e-9, magnitude * 1e-9));
    deviations[index] = std::abs(residuals[index] - center) / scale;
  }
  return deviations;
}

} // namespace

AnomalyScoreResult score_tabular_anomalies(
    const std::vector<AnomalyPartition> &local_partitions,
    const std::vector<std::string> &global_partition_ids,
    const std::vector<std::string> &feature_names, const AnomalyParams &params,
    const AnomalyAllReduce &allreduce, const std::string &tag) {
  AnomalyScoreResult result;
  result.model.mode = AnomalyMode::Tabular;
  result.model.feature_names = feature_names;
  result.model.seed = params.seed;
  if (params.mode != AnomalyMode::Tabular) {
    result.status = invalid("tabular scorer received KPI mode");
    return result;
  }
  if (feature_names.empty()) {
    result.status = invalid("tabular mode needs at least one feature");
    return result;
  }
  if (params.tree_count == 0 || params.subsample_size < 2) {
    result.status =
        invalid("tree_count must be positive and subsample_size at least 2");
    return result;
  }
  if (global_partition_ids.empty()) {
    result.status = invalid("the global partition catalog is empty");
    return result;
  }
  if (!allreduce) {
    result.status = invalid("no allreduce was supplied");
    return result;
  }
  if (const compute::ComputeStatus status =
          validate_threshold(params.threshold);
      !status.ok()) {
    result.status = status;
    return result;
  }
  if (const compute::ComputeStatus status =
          validate_partitions(local_partitions, feature_names);
      !status.ok()) {
    result.status = status;
    return result;
  }

  const std::size_t maximum_nodes = params.subsample_size * 2 - 1;
  result.model.trees.reserve(params.tree_count);
  for (std::size_t tree_index = 0; tree_index < params.tree_count;
       ++tree_index) {
    const std::uint64_t tree_seed = mix_seed(params.seed ^ tree_index);
    const std::string &partition_id =
        global_partition_ids[tree_seed % global_partition_ids.size()];
    const AnomalyPartition *partition =
        find_partition(local_partitions, partition_id);
    std::optional<IsolationTree> local_tree;
    if (partition != nullptr && partition->row_count() > 0) {
      std::vector<std::size_t> sample =
          sample_rows(partition->row_count(), params.subsample_size, tree_seed);
      local_tree = IsolationTreeBuilder(*partition, std::move(sample),
                                        mix_seed(tree_seed))
                       .build();
    }
    const std::vector<double> local_wire =
        encode_tree(local_tree ? &*local_tree : nullptr, maximum_nodes);
    const std::optional<std::vector<double>> global_wire =
        allreduce(tag + ".tree." + std::to_string(tree_index), local_wire,
                  AnomalyReduce::Sum);
    if (!global_wire) {
      result.status =
          invalid("tree " + std::to_string(tree_index) + " allreduce failed");
      return result;
    }
    std::optional<IsolationTree> tree =
        decode_tree(*global_wire, maximum_nodes, partition_id);
    if (!tree) {
      result.status =
          invalid("tree " + std::to_string(tree_index) +
                  " was not supplied by exactly one primary partition owner");
      return result;
    }
    result.model.trees.push_back(std::move(*tree));
  }

  result.status =
      reduce_robust_statistics(local_partitions, global_partition_ids,
                               feature_names, allreduce, tag, result.model);
  if (!result.status.ok()) {
    return result;
  }

  for (const AnomalyPartition &partition : local_partitions) {
    std::vector<double> row(feature_names.size());
    for (std::size_t row_index = 0; row_index < partition.row_count();
         ++row_index) {
      for (std::size_t feature = 0; feature < feature_names.size(); ++feature) {
        row[feature] = partition.columns[feature][row_index];
      }
      AnomalyRecord record;
      record.ordinal = partition.ordinals[row_index];
      record.row = partition.row_keys[row_index];
      record.score = forest_score(result.model, row);
      record.contributions = tabular_contributions(result.model, row);
      record.score =
          std::max(record.score, robust_component_score(record.contributions));
      if (partition.has_labels()) {
        record.label = partition.labels[row_index];
      }
      result.records.push_back(std::move(record));
    }
  }
  result.status = compute::ComputeStatus::success();
  return result;
}

AnomalyScoreResult
score_tabular_anomalies(const std::vector<AnomalyPartition> &partitions,
                        const std::vector<std::string> &feature_names,
                        const AnomalyParams &params) {
  std::vector<std::string> partition_ids;
  partition_ids.reserve(partitions.size());
  for (const AnomalyPartition &partition : partitions) {
    partition_ids.push_back(partition.id);
  }
  std::sort(partition_ids.begin(), partition_ids.end());
  const AnomalyAllReduce identity =
      [](const std::string &tag, const std::vector<double> &local,
         AnomalyReduce operation) -> std::optional<std::vector<double>> {
    static_cast<void>(tag);
    static_cast<void>(operation);
    return local;
  };
  AnomalyScoreResult result = score_tabular_anomalies(
      partitions, partition_ids, feature_names, params, identity);
  if (result.ok()) {
    result.status = apply_anomaly_threshold(result.records, params.threshold,
                                            result.report);
  }
  return result;
}

AnomalyScoreResult
score_kpi_anomalies(const std::vector<double> &periods,
                    const std::vector<std::vector<double>> &columns,
                    const std::vector<std::string> &metric_names,
                    const std::vector<double> &labels,
                    const AnomalyParams &params) {
  AnomalyScoreResult result;
  result.model.mode = AnomalyMode::KpiTimeSeries;
  result.model.feature_names = metric_names;
  result.model.seed = params.seed;
  result.model.seasonal_period = params.seasonal_period;
  result.model.trend_window = params.trend_window;
  if (params.mode != AnomalyMode::KpiTimeSeries) {
    result.status = invalid("KPI scorer received tabular mode");
    return result;
  }
  if (periods.empty() || metric_names.empty() ||
      columns.size() != metric_names.size()) {
    result.status =
        invalid("KPI mode needs periods and one or more named metrics");
    return result;
  }
  if (params.seasonal_period < 2 || params.trend_window < 2 ||
      params.residual_window < 2 || params.seasonal_cycles == 0) {
    result.status = invalid("KPI windows and seasonal cycles are too small");
    return result;
  }
  if (const compute::ComputeStatus status =
          validate_threshold(params.threshold);
      !status.ok()) {
    result.status = status;
    return result;
  }
  if (!labels.empty() && labels.size() != periods.size()) {
    result.status = invalid("KPI labels do not match the period count");
    return result;
  }
  for (const std::vector<double> &column : columns) {
    if (column.size() != periods.size()) {
      result.status = invalid("KPI metric columns are ragged");
      return result;
    }
  }

  std::vector<std::size_t> order(periods.size());
  if (std::ranges::any_of(
          periods, [](double period) { return !std::isfinite(period); })) {
    result.status = invalid("KPI periods must be finite");
    return result;
  }
  std::iota(order.begin(), order.end(), 0);
  std::stable_sort(order.begin(), order.end(),
                   [&](std::size_t left, std::size_t right) {
                     return periods[left] < periods[right];
                   });
  std::vector<double> sorted_periods(periods.size());
  std::vector<std::vector<double>> sorted_columns(
      columns.size(), std::vector<double>(periods.size()));
  for (std::size_t position = 0; position < order.size(); ++position) {
    sorted_periods[position] = periods[order[position]];
    if (position > 0 &&
        sorted_periods[position] == sorted_periods[position - 1]) {
      result.status = invalid(
          "KPI periods must be unique; aggregate duplicate periods first");
      return result;
    }
    if (!std::isfinite(sorted_periods[position])) {
      result.status = invalid("KPI periods must be finite");
      return result;
    }
    for (std::size_t metric = 0; metric < columns.size(); ++metric) {
      sorted_columns[metric][position] = columns[metric][order[position]];
      if (!std::isfinite(sorted_columns[metric][position])) {
        result.status = invalid("KPI metrics must be finite");
        return result;
      }
    }
  }

  std::vector<std::vector<double>> deviations;
  deviations.reserve(columns.size());
  for (const std::vector<double> &column : sorted_columns) {
    deviations.push_back(
        causal_metric_deviations(sorted_periods, column, params));
  }
  result.records.reserve(periods.size());
  for (std::size_t position = 0; position < sorted_periods.size(); ++position) {
    AnomalyRecord record;
    record.ordinal = position;
    record.row = static_cast<std::int64_t>(order[position]);
    record.period = sorted_periods[position];
    for (std::size_t metric = 0; metric < metric_names.size(); ++metric) {
      record.contributions.push_back(
          {metric_names[metric], deviations[metric][position]});
    }
    std::sort(
        record.contributions.begin(), record.contributions.end(),
        [](const AnomalyContribution &left, const AnomalyContribution &right) {
          if (left.deviation != right.deviation) {
            return left.deviation > right.deviation;
          }
          return left.feature < right.feature;
        });
    if (record.contributions.size() > kContributionLimit) {
      record.contributions.resize(kContributionLimit);
    }
    if (!record.contributions.empty()) {
      record.score = record.contributions.front().deviation;
    }
    if (!labels.empty()) {
      record.label = labels[order[position]];
    }
    result.records.push_back(std::move(record));
  }
  result.status =
      apply_anomaly_threshold(result.records, params.threshold, result.report);
  return result;
}

compute::ComputeStatus
apply_anomaly_threshold(std::vector<AnomalyRecord> &records,
                        const AnomalyThresholdPolicy &policy,
                        AnomalyReport &report) {
  if (const compute::ComputeStatus status = validate_threshold(policy);
      !status.ok()) {
    return status;
  }
  if (records.empty()) {
    return invalid("cannot choose a threshold for zero rows");
  }
  std::vector<std::size_t> ranking(records.size());
  std::iota(ranking.begin(), ranking.end(), 0);
  std::sort(ranking.begin(), ranking.end(),
            [&](std::size_t left, std::size_t right) {
              if (records[left].score != records[right].score) {
                return records[left].score > records[right].score;
              }
              return records[left].ordinal < records[right].ordinal;
            });
  std::size_t budget = 0;
  if (policy.contamination) {
    budget = static_cast<std::size_t>(
        std::ceil(*policy.contamination * static_cast<double>(records.size())));
  } else {
    budget = *policy.alert_budget;
  }
  budget = std::min(budget, records.size());
  for (AnomalyRecord &record : records) {
    record.flagged = false;
  }
  std::size_t true_positives = 0;
  bool has_labels = false;
  for (std::size_t position = 0; position < budget; ++position) {
    AnomalyRecord &record = records[ranking[position]];
    record.flagged = true;
    if (record.label) {
      has_labels = true;
      if (*record.label > 0.5) {
        ++true_positives;
      }
    }
  }
  report.threshold = records[ranking[budget - 1]].score;
  report.contamination = policy.contamination;
  report.alert_budget = policy.alert_budget;
  report.rows_scored = records.size();
  report.flagged = budget;
  report.alert_rate =
      static_cast<double>(budget) / static_cast<double>(records.size());
  report.precision_at_k.reset();
  if (has_labels) {
    report.precision_at_k =
        static_cast<double>(true_positives) / static_cast<double>(budget);
  }
  return compute::ComputeStatus::success();
}

} // namespace zetta::ml
