// SPDX-License-Identifier: MIT

#include "zetta/ml/vision.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

std::vector<double> softmax(const std::vector<double> &logits) {
  if (logits.empty()) {
    return {};
  }
  const double maximum = *std::max_element(logits.begin(), logits.end());
  std::vector<double> probabilities(logits.size());
  double total = 0.0;
  for (std::size_t index = 0; index < logits.size(); ++index) {
    probabilities[index] = std::exp(logits[index] - maximum);
    total += probabilities[index];
  }
  if (total <= 0.0 || !std::isfinite(total)) {
    return std::vector<double>(logits.size(),
                               1.0 / static_cast<double>(logits.size()));
  }
  for (double &probability : probabilities) {
    probability /= total;
  }
  return probabilities;
}

std::size_t predicted_class(const LinearVisionHead &head,
                            const VisionExample &example) {
  const std::vector<double> probabilities =
      head.probabilities(example.embedding);
  return static_cast<std::size_t>(std::distance(
      probabilities.begin(),
      std::max_element(probabilities.begin(), probabilities.end())));
}

VisionMetrics measure(const LinearVisionHead &head,
                      const std::vector<VisionExample> &examples,
                      const VisionAllReduce &allreduce,
                      const std::string &round_tag) {
  const std::size_t classes = head.classes;
  std::vector<double> local(classes * classes + 2, 0.0);
  for (const VisionExample &example : examples) {
    if (!example.held_out || example.label >= classes) {
      continue;
    }
    const std::size_t predicted = predicted_class(head, example);
    local[example.label * classes + predicted] += 1.0;
    local[classes * classes] += 1.0;
  }
  for (const VisionExample &example : examples) {
    local[classes * classes + 1] += example.held_out ? 0.0 : 1.0;
  }
  const std::vector<double> global = allreduce(round_tag + ".metrics", local);

  VisionMetrics metrics;
  if (global.size() != local.size()) {
    return metrics;
  }
  metrics.confusion_matrix.resize(classes * classes);
  double correct = 0.0;
  for (std::size_t index = 0; index < classes * classes; ++index) {
    const double count = std::max(0.0, global[index]);
    metrics.confusion_matrix[index] =
        static_cast<std::uint64_t>(std::llround(count));
    if (index / classes == index % classes) {
      correct += count;
    }
  }
  const double held_out = global[classes * classes];
  metrics.held_out_rows =
      static_cast<std::uint64_t>(std::llround(std::max(0.0, held_out)));
  metrics.train_rows = static_cast<std::uint64_t>(
      std::llround(std::max(0.0, global[classes * classes + 1])));
  if (held_out > 0.0) {
    metrics.accuracy = correct / held_out;
  }

  double f1_total = 0.0;
  for (std::size_t label = 0; label < classes; ++label) {
    const double true_positive = global[label * classes + label];
    double false_positive = 0.0;
    double false_negative = 0.0;
    for (std::size_t other = 0; other < classes; ++other) {
      if (other == label) {
        continue;
      }
      false_positive += global[other * classes + label];
      false_negative += global[label * classes + other];
    }
    const double denominator =
        2.0 * true_positive + false_positive + false_negative;
    if (denominator > 0.0) {
      f1_total += 2.0 * true_positive / denominator;
    }
  }
  metrics.macro_f1 = f1_total / static_cast<double>(classes);
  return metrics;
}

} // namespace

std::vector<double>
LinearVisionHead::probabilities(const std::vector<float> &embedding) const {
  if (classes == 0 || embedding.size() != embedding_size ||
      weights.size() != classes * embedding_size || bias.size() != classes) {
    return {};
  }
  std::vector<double> logits = bias;
  for (std::size_t label = 0; label < classes; ++label) {
    for (std::size_t feature = 0; feature < embedding_size; ++feature) {
      logits[label] += weights[label * embedding_size + feature] *
                       static_cast<double>(embedding[feature]);
    }
  }
  return softmax(logits);
}

bool assign_source_disjoint_split(std::vector<VisionExample> &examples,
                                  double holdout_fraction, std::uint64_t seed,
                                  std::string *error) {
  if (!(holdout_fraction > 0.0 && holdout_fraction < 1.0)) {
    if (error != nullptr) {
      *error = "vision split: holdout_fraction must be between 0 and 1";
    }
    return false;
  }
  std::set<std::string> unique_sources;
  for (const VisionExample &example : examples) {
    if (example.source.empty()) {
      if (error != nullptr) {
        *error = "vision split: every image must name a non-empty source";
      }
      return false;
    }
    unique_sources.insert(example.source);
  }
  if (unique_sources.size() < 2) {
    if (error != nullptr) {
      *error = "vision split: at least two sources are required for a "
               "source-disjoint holdout";
    }
    return false;
  }

  std::vector<std::string> sources(unique_sources.begin(),
                                   unique_sources.end());
  std::mt19937_64 generator(seed);
  std::shuffle(sources.begin(), sources.end(), generator);
  std::size_t held_out_sources = static_cast<std::size_t>(
      std::llround(static_cast<double>(sources.size()) * holdout_fraction));
  held_out_sources =
      std::clamp<std::size_t>(held_out_sources, 1, sources.size() - 1);
  const std::set<std::string> held_out(sources.begin(),
                                       sources.begin() + held_out_sources);
  for (VisionExample &example : examples) {
    example.held_out = held_out.contains(example.source);
  }
  return true;
}

VisionTrainResult
train_vision_head(const std::vector<VisionExample> &local_examples,
                  const VisionTrainParams &params,
                  const VisionAllReduce &allreduce,
                  const std::string &round_tag) {
  VisionTrainResult result;
  if (!allreduce) {
    result.error = "vision train: no allreduce was supplied";
    return result;
  }
  if (params.classes < 2) {
    result.error = "vision train: at least two classes are required";
    return result;
  }
  std::size_t embedding_size = 0;
  for (const VisionExample &example : local_examples) {
    if (!example.embedding.empty()) {
      embedding_size = example.embedding.size();
      break;
    }
  }
  const std::vector<double> global_size =
      allreduce(round_tag + ".embedding_size",
                {static_cast<double>(embedding_size),
                 static_cast<double>(embedding_size * embedding_size),
                 embedding_size == 0 ? 0.0 : 1.0});
  if (global_size.size() != 3 || global_size[2] <= 0.0) {
    result.error = "vision train: no participant supplied embeddings";
    return result;
  }
  const double mean_size = global_size[0] / global_size[2];
  const double variance =
      global_size[1] / global_size[2] - mean_size * mean_size;
  if (std::abs(mean_size - std::round(mean_size)) > 1.0e-9 ||
      variance > 1.0e-9) {
    result.error =
        "vision train: participants supplied different embedding sizes";
    return result;
  }
  embedding_size = static_cast<std::size_t>(std::llround(mean_size));
  for (const VisionExample &example : local_examples) {
    if (example.embedding.size() != embedding_size) {
      result.error = "vision train: embedding sizes differ";
      return result;
    }
    if (example.label >= params.classes) {
      result.error = "vision train: label exceeds the configured class count";
      return result;
    }
  }

  result.head.classes = params.classes;
  result.head.embedding_size = embedding_size;
  result.head.weights.assign(params.classes * embedding_size, 0.0);
  result.head.bias.assign(params.classes, 0.0);
  std::mt19937_64 generator(params.seed);
  std::normal_distribution<double> initial(0.0, 0.01);
  for (double &weight : result.head.weights) {
    weight = initial(generator);
  }

  const std::size_t parameter_count =
      result.head.weights.size() + result.head.bias.size();
  for (std::size_t epoch = 0; epoch < params.epochs; ++epoch) {
    std::vector<double> local(parameter_count + 1, 0.0);
    for (const VisionExample &example : local_examples) {
      if (example.held_out) {
        continue;
      }
      const std::vector<double> probabilities =
          result.head.probabilities(example.embedding);
      if (probabilities.size() != params.classes) {
        result.error = "vision train: head produced no probabilities";
        return result;
      }
      for (std::size_t label = 0; label < params.classes; ++label) {
        const double error_value =
            probabilities[label] - (label == example.label ? 1.0 : 0.0);
        for (std::size_t feature = 0; feature < embedding_size; ++feature) {
          local[label * embedding_size + feature] +=
              error_value * static_cast<double>(example.embedding[feature]);
        }
        local[result.head.weights.size() + label] += error_value;
      }
      local.back() += 1.0;
    }
    const std::vector<double> global =
        allreduce(round_tag + ".epoch." + std::to_string(epoch), local);
    if (global.size() != local.size() || global.back() <= 0.0) {
      result.error = "vision train: gradient allreduce failed or no training "
                     "rows remained";
      return result;
    }
    const double scale = params.learning_rate / global.back();
    for (std::size_t index = 0; index < result.head.weights.size(); ++index) {
      result.head.weights[index] -= scale * global[index];
    }
    for (std::size_t label = 0; label < params.classes; ++label) {
      result.head.bias[label] -=
          scale * global[result.head.weights.size() + label];
    }
  }

  result.metrics = measure(result.head, local_examples, allreduce, round_tag);
  if (result.metrics.held_out_rows == 0) {
    result.error = "vision train: no held-out rows remained after the "
                   "source-disjoint split";
  }
  return result;
}

} // namespace zetta::ml
