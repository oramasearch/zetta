// SPDX-License-Identifier: MIT

#include "zetta/ml/speech.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

using zetta::autograd::SafeTensor;
using zetta::autograd::SafeTensors;

struct Matrix {
  std::size_t rows = 0;
  std::size_t columns = 0;
  std::vector<float> values;

  float &at(std::size_t row, std::size_t column) {
    return values[row * columns + column];
  }

  float at(std::size_t row, std::size_t column) const {
    return values[row * columns + column];
  }
};

std::optional<std::string> read_file(const std::filesystem::path &path,
                                     bool binary = false) {
  std::ifstream input(path, binary ? std::ios::binary : std::ios::in);
  if (!input) {
    return std::nullopt;
  }
  std::ostringstream buffer;
  buffer << input.rdbuf();
  return buffer.str();
}

std::optional<std::size_t> positive_size(const nlohmann::json &value,
                                         const char *primary,
                                         const char *alternate,
                                         std::string *error) {
  const auto first = value.find(primary);
  const auto second =
      alternate == nullptr ? value.end() : value.find(alternate);
  const auto found = first != value.end() ? first : second;
  if (found == value.end() || !found->is_number_integer()) {
    if (error != nullptr) {
      *error = std::string("whisper config: missing integer '") + primary + "'";
    }
    return std::nullopt;
  }
  const std::int64_t number = found->get<std::int64_t>();
  if (number <= 0) {
    if (error != nullptr) {
      *error =
          std::string("whisper config: '") + primary + "' must be positive";
    }
    return std::nullopt;
  }
  return static_cast<std::size_t>(number);
}

bool permissive_license(std::string_view license) {
  return license == "MIT" || license == "Apache-2.0" ||
         license == "BSD-2-Clause" || license == "BSD-3-Clause";
}

bool valid_pretrained_metadata(const WhisperCheckpointMetadata &metadata,
                               std::string *error) {
  std::string_view pinned;
  if (metadata.repository == "openai/whisper-tiny") {
    pinned = kWhisperTinyRevision;
  } else if (metadata.repository == "openai/whisper-base") {
    pinned = kWhisperBaseRevision;
  } else {
    if (error != nullptr) {
      *error = "whisper checkpoint: only the pinned openai/whisper-tiny "
               "and openai/whisper-base checkpoints are accepted";
    }
    return false;
  }
  if (metadata.revision != pinned) {
    if (error != nullptr) {
      *error = "whisper checkpoint: revision '" + metadata.revision +
               "' is not the pinned revision for " + metadata.repository;
    }
    return false;
  }
  if (!permissive_license(metadata.weights_license) ||
      !permissive_license(metadata.tokenizer_license) ||
      !permissive_license(metadata.config_license)) {
    if (error != nullptr) {
      *error = "whisper checkpoint: weights, tokenizer, and config must "
               "each have an MIT, Apache-2.0, or BSD license";
    }
    return false;
  }
  return true;
}

const SafeTensor *tensor(const SafeTensors &tensors, const std::string &name) {
  const auto found = tensors.find(name);
  return found == tensors.end() ? nullptr : &found->second;
}

std::size_t element_count(const std::vector<int> &shape) {
  std::size_t count = 1;
  for (const int dimension : shape) {
    if (dimension <= 0) {
      return 0;
    }
    count *= static_cast<std::size_t>(dimension);
  }
  return count;
}

bool tensor_shape(const SafeTensors &tensors, const std::string &name,
                  const std::vector<int> &shape, std::string *error,
                  bool optional = false) {
  const SafeTensor *found = tensor(tensors, name);
  if (found == nullptr) {
    if (optional) {
      return true;
    }
    if (error != nullptr) {
      *error = "whisper checkpoint: missing tensor '" + name + "'";
    }
    return false;
  }
  if (found->shape != shape || found->data.size() != element_count(shape)) {
    if (error != nullptr) {
      *error = "whisper checkpoint: tensor '" + name + "' has the wrong shape";
    }
    return false;
  }
  return true;
}

bool validate_attention(const SafeTensors &tensors, const std::string &prefix,
                        std::size_t width, std::string *error) {
  const std::vector<int> matrix_shape = {static_cast<int>(width),
                                         static_cast<int>(width)};
  const std::vector<int> bias_shape = {static_cast<int>(width)};
  for (const std::string_view projection :
       {"q_proj", "k_proj", "v_proj", "out_proj"}) {
    const std::string base = prefix + std::string(projection);
    if (!tensor_shape(tensors, base + ".weight", matrix_shape, error)) {
      return false;
    }
    const bool optional_bias = projection == "k_proj";
    if (!tensor_shape(tensors, base + ".bias", bias_shape, error,
                      optional_bias)) {
      return false;
    }
  }
  return true;
}

bool validate_checkpoint(const WhisperCheckpoint &checkpoint,
                         std::string *error) {
  if (!checkpoint.config.valid(error)) {
    return false;
  }
  const WhisperConfig &config = checkpoint.config;
  const std::vector<int> conv1 = {static_cast<int>(config.model_width),
                                  static_cast<int>(config.mel_bins), 3};
  const std::vector<int> conv2 = {static_cast<int>(config.model_width),
                                  static_cast<int>(config.model_width), 3};
  const std::vector<int> width = {static_cast<int>(config.model_width)};
  if (!tensor_shape(checkpoint.tensors, "model.encoder.conv1.weight", conv1,
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.encoder.conv1.bias", width,
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.encoder.conv2.weight", conv2,
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.encoder.conv2.bias", width,
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.encoder.embed_positions.weight",
                    {static_cast<int>(config.max_source_positions),
                     static_cast<int>(config.model_width)},
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.encoder.layer_norm.weight",
                    width, error) ||
      !tensor_shape(checkpoint.tensors, "model.encoder.layer_norm.bias", width,
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.decoder.embed_tokens.weight",
                    {static_cast<int>(config.vocabulary_size),
                     static_cast<int>(config.model_width)},
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.decoder.embed_positions.weight",
                    {static_cast<int>(config.max_target_positions),
                     static_cast<int>(config.model_width)},
                    error) ||
      !tensor_shape(checkpoint.tensors, "model.decoder.layer_norm.weight",
                    width, error) ||
      !tensor_shape(checkpoint.tensors, "model.decoder.layer_norm.bias", width,
                    error)) {
    return false;
  }

  for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
    const std::string prefix =
        "model.encoder.layers." + std::to_string(layer) + ".";
    if (!validate_attention(checkpoint.tensors, prefix + "self_attn.",
                            config.model_width, error) ||
        !tensor_shape(checkpoint.tensors,
                      prefix + "self_attn_layer_norm.weight", width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "self_attn_layer_norm.bias",
                      width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc1.weight",
                      {static_cast<int>(config.encoder_feed_forward),
                       static_cast<int>(config.model_width)},
                      error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc1.bias",
                      {static_cast<int>(config.encoder_feed_forward)}, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc2.weight",
                      {static_cast<int>(config.model_width),
                       static_cast<int>(config.encoder_feed_forward)},
                      error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc2.bias", width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "final_layer_norm.weight",
                      width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "final_layer_norm.bias",
                      width, error)) {
      return false;
    }
  }
  for (std::size_t layer = 0; layer < config.decoder_layers; ++layer) {
    const std::string prefix =
        "model.decoder.layers." + std::to_string(layer) + ".";
    if (!validate_attention(checkpoint.tensors, prefix + "self_attn.",
                            config.model_width, error) ||
        !validate_attention(checkpoint.tensors, prefix + "encoder_attn.",
                            config.model_width, error) ||
        !tensor_shape(checkpoint.tensors,
                      prefix + "self_attn_layer_norm.weight", width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "self_attn_layer_norm.bias",
                      width, error) ||
        !tensor_shape(checkpoint.tensors,
                      prefix + "encoder_attn_layer_norm.weight", width,
                      error) ||
        !tensor_shape(checkpoint.tensors,
                      prefix + "encoder_attn_layer_norm.bias", width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc1.weight",
                      {static_cast<int>(config.decoder_feed_forward),
                       static_cast<int>(config.model_width)},
                      error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc1.bias",
                      {static_cast<int>(config.decoder_feed_forward)}, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc2.weight",
                      {static_cast<int>(config.model_width),
                       static_cast<int>(config.decoder_feed_forward)},
                      error) ||
        !tensor_shape(checkpoint.tensors, prefix + "fc2.bias", width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "final_layer_norm.weight",
                      width, error) ||
        !tensor_shape(checkpoint.tensors, prefix + "final_layer_norm.bias",
                      width, error)) {
      return false;
    }
  }
  return tensor_shape(checkpoint.tensors, "proj_out.weight",
                      {static_cast<int>(config.vocabulary_size),
                       static_cast<int>(config.model_width)},
                      error, true);
}

SafeTensor random_tensor(const std::vector<int> &shape, std::mt19937 &generator,
                         bool ones = false) {
  SafeTensor result;
  result.shape = shape;
  result.data.resize(element_count(shape), ones ? 1.0F : 0.0F);
  if (ones) {
    return result;
  }
  std::normal_distribution<float> distribution(0.0F, 0.02F);
  for (float &value : result.data) {
    value = distribution(generator);
  }
  return result;
}

void add_attention(SafeTensors &tensors, const std::string &prefix,
                   std::size_t width, std::mt19937 &generator) {
  const std::vector<int> matrix = {static_cast<int>(width),
                                   static_cast<int>(width)};
  const std::vector<int> bias = {static_cast<int>(width)};
  for (const std::string_view projection :
       {"q_proj", "k_proj", "v_proj", "out_proj"}) {
    const std::string base = prefix + std::string(projection);
    tensors[base + ".weight"] = random_tensor(matrix, generator);
    tensors[base + ".bias"] = SafeTensor{bias, std::vector<float>(width)};
  }
}

void add_layer_norm(SafeTensors &tensors, const std::string &prefix,
                    std::size_t width, std::mt19937 &generator) {
  const std::vector<int> shape = {static_cast<int>(width)};
  tensors[prefix + ".weight"] = random_tensor(shape, generator, true);
  tensors[prefix + ".bias"] = SafeTensor{shape, std::vector<float>(width)};
}

Matrix layer_norm(const Matrix &input, const SafeTensor &weight,
                  const SafeTensor &bias) {
  Matrix result = input;
  constexpr double epsilon = 1.0e-5;
  for (std::size_t row = 0; row < input.rows; ++row) {
    double mean = 0.0;
    for (std::size_t column = 0; column < input.columns; ++column) {
      mean += input.at(row, column);
    }
    mean /= static_cast<double>(input.columns);
    double variance = 0.0;
    for (std::size_t column = 0; column < input.columns; ++column) {
      const double centered = input.at(row, column) - mean;
      variance += centered * centered;
    }
    variance /= static_cast<double>(input.columns);
    const double inverse = 1.0 / std::sqrt(variance + epsilon);
    for (std::size_t column = 0; column < input.columns; ++column) {
      result.at(row, column) = static_cast<float>(
          (input.at(row, column) - mean) * inverse * weight.data[column] +
          bias.data[column]);
    }
  }
  return result;
}

Matrix linear(const Matrix &input, const SafeTensor &weight,
              const SafeTensor *bias) {
  const std::size_t outputs = static_cast<std::size_t>(weight.shape[0]);
  const std::size_t inputs = static_cast<std::size_t>(weight.shape[1]);
  Matrix result{input.rows, outputs,
                std::vector<float>(input.rows * outputs, 0.0F)};
  for (std::size_t row = 0; row < input.rows; ++row) {
    for (std::size_t output = 0; output < outputs; ++output) {
      double sum = bias == nullptr ? 0.0 : bias->data[output];
      for (std::size_t input_column = 0; input_column < inputs;
           ++input_column) {
        sum += static_cast<double>(input.at(row, input_column)) *
               weight.data[output * inputs + input_column];
      }
      result.at(row, output) = static_cast<float>(sum);
    }
  }
  return result;
}

Matrix add(const Matrix &left, const Matrix &right) {
  Matrix result = left;
  for (std::size_t index = 0; index < result.values.size(); ++index) {
    result.values[index] += right.values[index];
  }
  return result;
}

Matrix gelu(Matrix input) {
  constexpr double inverse_sqrt_two = 0.70710678118654752440;
  for (float &value : input.values) {
    const double number = value;
    value = static_cast<float>(0.5 * number *
                               (1.0 + std::erf(number * inverse_sqrt_two)));
  }
  return input;
}

Matrix convolution(const Matrix &input, const SafeTensor &weight,
                   const SafeTensor &bias, std::size_t stride) {
  const std::size_t output_channels = static_cast<std::size_t>(weight.shape[0]);
  const std::size_t input_channels = static_cast<std::size_t>(weight.shape[1]);
  const std::size_t kernel = static_cast<std::size_t>(weight.shape[2]);
  const std::size_t output_rows = (input.rows + 1) / stride;
  Matrix result{output_rows, output_channels,
                std::vector<float>(output_rows * output_channels, 0.0F)};
  for (std::size_t output = 0; output < output_rows; ++output) {
    const std::ptrdiff_t center = static_cast<std::ptrdiff_t>(output * stride);
    for (std::size_t channel = 0; channel < output_channels; ++channel) {
      double sum = bias.data[channel];
      for (std::size_t input_channel = 0; input_channel < input_channels;
           ++input_channel) {
        for (std::size_t tap = 0; tap < kernel; ++tap) {
          const std::ptrdiff_t row =
              center + static_cast<std::ptrdiff_t>(tap) - 1;
          if (row < 0 || row >= static_cast<std::ptrdiff_t>(input.rows)) {
            continue;
          }
          const std::size_t weight_index =
              (channel * input_channels + input_channel) * kernel + tap;
          sum += input.at(static_cast<std::size_t>(row), input_channel) *
                 weight.data[weight_index];
        }
      }
      result.at(output, channel) = static_cast<float>(sum);
    }
  }
  return result;
}

Matrix attention(const Matrix &query_input, const Matrix &key_value_input,
                 const SafeTensors &tensors, const std::string &prefix,
                 std::size_t heads, bool causal) {
  const Matrix queries =
      linear(query_input, *tensor(tensors, prefix + "q_proj.weight"),
             tensor(tensors, prefix + "q_proj.bias"));
  const Matrix keys =
      linear(key_value_input, *tensor(tensors, prefix + "k_proj.weight"),
             tensor(tensors, prefix + "k_proj.bias"));
  const Matrix values =
      linear(key_value_input, *tensor(tensors, prefix + "v_proj.weight"),
             tensor(tensors, prefix + "v_proj.bias"));
  const std::size_t head_width = query_input.columns / heads;
  const double scale = 1.0 / std::sqrt(static_cast<double>(head_width));
  Matrix combined{
      query_input.rows, query_input.columns,
      std::vector<float>(query_input.rows * query_input.columns, 0.0F)};
  std::vector<double> scores(key_value_input.rows, 0.0);
  for (std::size_t head = 0; head < heads; ++head) {
    const std::size_t offset = head * head_width;
    for (std::size_t query = 0; query < query_input.rows; ++query) {
      double maximum = -std::numeric_limits<double>::infinity();
      for (std::size_t key = 0; key < key_value_input.rows; ++key) {
        if (causal && key > query) {
          scores[key] = -std::numeric_limits<double>::infinity();
          continue;
        }
        double score = 0.0;
        for (std::size_t column = 0; column < head_width; ++column) {
          score += queries.at(query, offset + column) *
                   keys.at(key, offset + column);
        }
        scores[key] = score * scale;
        maximum = std::max(maximum, scores[key]);
      }
      double denominator = 0.0;
      for (std::size_t key = 0; key < key_value_input.rows; ++key) {
        if (!std::isfinite(scores[key])) {
          scores[key] = 0.0;
        } else {
          scores[key] = std::exp(scores[key] - maximum);
        }
        denominator += scores[key];
      }
      denominator = std::max(denominator, 1.0e-20);
      for (std::size_t column = 0; column < head_width; ++column) {
        double context = 0.0;
        for (std::size_t key = 0; key < key_value_input.rows; ++key) {
          context +=
              scores[key] / denominator * values.at(key, offset + column);
        }
        combined.at(query, offset + column) = static_cast<float>(context);
      }
    }
  }
  return linear(combined, *tensor(tensors, prefix + "out_proj.weight"),
                tensor(tensors, prefix + "out_proj.bias"));
}

Matrix encode(const WhisperCheckpoint &checkpoint,
              const LogMelSpectrogram &mel) {
  const WhisperConfig &config = checkpoint.config;
  const std::size_t frames =
      std::min(mel.frames, config.max_source_positions * 2);
  Matrix features{frames, config.mel_bins,
                  std::vector<float>(frames * config.mel_bins, 0.0F)};
  for (std::size_t frame = 0; frame < frames; ++frame) {
    for (std::size_t bin = 0; bin < config.mel_bins; ++bin) {
      features.at(frame, bin) = mel.at(bin, frame);
    }
  }
  Matrix hidden = gelu(convolution(
      features, *tensor(checkpoint.tensors, "model.encoder.conv1.weight"),
      *tensor(checkpoint.tensors, "model.encoder.conv1.bias"), 1));
  hidden = gelu(convolution(
      hidden, *tensor(checkpoint.tensors, "model.encoder.conv2.weight"),
      *tensor(checkpoint.tensors, "model.encoder.conv2.bias"), 2));
  const SafeTensor &positions =
      *tensor(checkpoint.tensors, "model.encoder.embed_positions.weight");
  if (hidden.rows > config.max_source_positions) {
    hidden.rows = config.max_source_positions;
    hidden.values.resize(hidden.rows * hidden.columns);
  }
  for (std::size_t row = 0; row < hidden.rows; ++row) {
    for (std::size_t column = 0; column < hidden.columns; ++column) {
      hidden.at(row, column) += positions.data[row * hidden.columns + column];
    }
  }

  for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
    const std::string prefix =
        "model.encoder.layers." + std::to_string(layer) + ".";
    Matrix normalized = layer_norm(
        hidden,
        *tensor(checkpoint.tensors, prefix + "self_attn_layer_norm.weight"),
        *tensor(checkpoint.tensors, prefix + "self_attn_layer_norm.bias"));
    hidden = add(hidden,
                 attention(normalized, normalized, checkpoint.tensors,
                           prefix + "self_attn.", config.encoder_heads, false));
    normalized = layer_norm(
        hidden, *tensor(checkpoint.tensors, prefix + "final_layer_norm.weight"),
        *tensor(checkpoint.tensors, prefix + "final_layer_norm.bias"));
    Matrix feed_forward = gelu(
        linear(normalized, *tensor(checkpoint.tensors, prefix + "fc1.weight"),
               tensor(checkpoint.tensors, prefix + "fc1.bias")));
    feed_forward =
        linear(feed_forward, *tensor(checkpoint.tensors, prefix + "fc2.weight"),
               tensor(checkpoint.tensors, prefix + "fc2.bias"));
    hidden = add(hidden, feed_forward);
  }
  return layer_norm(
      hidden, *tensor(checkpoint.tensors, "model.encoder.layer_norm.weight"),
      *tensor(checkpoint.tensors, "model.encoder.layer_norm.bias"));
}

Matrix decode_hidden(const WhisperCheckpoint &checkpoint, const Matrix &encoded,
                     const std::vector<int> &tokens) {
  const WhisperConfig &config = checkpoint.config;
  const SafeTensor &embeddings =
      *tensor(checkpoint.tensors, "model.decoder.embed_tokens.weight");
  const SafeTensor &positions =
      *tensor(checkpoint.tensors, "model.decoder.embed_positions.weight");
  Matrix hidden{tokens.size(), config.model_width,
                std::vector<float>(tokens.size() * config.model_width, 0.0F)};
  for (std::size_t row = 0; row < tokens.size(); ++row) {
    const std::size_t token_id = static_cast<std::size_t>(std::clamp(
        tokens[row], 0, static_cast<int>(config.vocabulary_size - 1)));
    for (std::size_t column = 0; column < config.model_width; ++column) {
      hidden.at(row, column) =
          embeddings.data[token_id * config.model_width + column] +
          positions.data[row * config.model_width + column];
    }
  }

  for (std::size_t layer = 0; layer < config.decoder_layers; ++layer) {
    const std::string prefix =
        "model.decoder.layers." + std::to_string(layer) + ".";
    Matrix normalized = layer_norm(
        hidden,
        *tensor(checkpoint.tensors, prefix + "self_attn_layer_norm.weight"),
        *tensor(checkpoint.tensors, prefix + "self_attn_layer_norm.bias"));
    hidden = add(hidden,
                 attention(normalized, normalized, checkpoint.tensors,
                           prefix + "self_attn.", config.decoder_heads, true));
    normalized = layer_norm(
        hidden,
        *tensor(checkpoint.tensors, prefix + "encoder_attn_layer_norm.weight"),
        *tensor(checkpoint.tensors, prefix + "encoder_attn_layer_norm.bias"));
    hidden = add(hidden, attention(normalized, encoded, checkpoint.tensors,
                                   prefix + "encoder_attn.",
                                   config.decoder_heads, false));
    normalized = layer_norm(
        hidden, *tensor(checkpoint.tensors, prefix + "final_layer_norm.weight"),
        *tensor(checkpoint.tensors, prefix + "final_layer_norm.bias"));
    Matrix feed_forward = gelu(
        linear(normalized, *tensor(checkpoint.tensors, prefix + "fc1.weight"),
               tensor(checkpoint.tensors, prefix + "fc1.bias")));
    feed_forward =
        linear(feed_forward, *tensor(checkpoint.tensors, prefix + "fc2.weight"),
               tensor(checkpoint.tensors, prefix + "fc2.bias"));
    hidden = add(hidden, feed_forward);
  }
  return layer_norm(
      hidden, *tensor(checkpoint.tensors, "model.decoder.layer_norm.weight"),
      *tensor(checkpoint.tensors, "model.decoder.layer_norm.bias"));
}

std::pair<int, double> next_token(const WhisperCheckpoint &checkpoint,
                                  const Matrix &hidden) {
  const SafeTensor *projection = tensor(checkpoint.tensors, "proj_out.weight");
  if (projection == nullptr) {
    projection =
        tensor(checkpoint.tensors, "model.decoder.embed_tokens.weight");
  }
  const std::size_t row = hidden.rows - 1;
  std::vector<double> logits(checkpoint.config.vocabulary_size, 0.0);
  for (std::size_t token_id = 0; token_id < checkpoint.config.vocabulary_size;
       ++token_id) {
    double logit = 0.0;
    for (std::size_t column = 0; column < checkpoint.config.model_width;
         ++column) {
      logit +=
          hidden.at(row, column) *
          projection->data[token_id * checkpoint.config.model_width + column];
    }
    logits[token_id] = logit;
  }
  for (const int suppressed : checkpoint.config.suppressed_tokens) {
    if (suppressed >= 0 && suppressed < static_cast<int>(logits.size())) {
      logits[static_cast<std::size_t>(suppressed)] =
          -std::numeric_limits<double>::infinity();
    }
  }
  const auto best = std::max_element(logits.begin(), logits.end());
  const int id = static_cast<int>(best - logits.begin());
  double denominator = 0.0;
  for (const double logit : logits) {
    denominator += std::isfinite(logit) ? std::exp(logit - *best) : 0.0;
  }
  const double probability = denominator == 0.0 ? 0.0 : 1.0 / denominator;
  return {id, probability};
}

std::string fallback_decode(const WhisperCheckpoint &checkpoint,
                            const std::vector<int> &tokens) {
  std::string text;
  for (const int token_id : tokens) {
    if (token_id < 0 ||
        token_id >= static_cast<int>(checkpoint.test_vocabulary.size()) ||
        token_id >= checkpoint.config.timestamp_begin) {
      continue;
    }
    const std::string &token =
        checkpoint.test_vocabulary[static_cast<std::size_t>(token_id)];
    if (token.starts_with("<|")) {
      continue;
    }
    text += token;
  }
  return text;
}

} // namespace

bool WhisperConfig::valid(std::string *error) const {
  const bool positive = mel_bins > 0 && model_width > 0 && encoder_layers > 0 &&
                        encoder_heads > 0 && encoder_feed_forward > 0 &&
                        decoder_layers > 0 && decoder_heads > 0 &&
                        decoder_feed_forward > 0 && max_source_positions > 0 &&
                        max_target_positions > 0 && vocabulary_size > 0;
  if (!positive || model_width % encoder_heads != 0 ||
      model_width % decoder_heads != 0) {
    if (error != nullptr) {
      *error = "whisper config: dimensions must be positive and model_width "
               "must divide evenly across encoder and decoder heads";
    }
    return false;
  }
  const auto valid_token = [&](int token_id) {
    return token_id >= 0 && token_id < static_cast<int>(vocabulary_size);
  };
  if (!valid_token(decoder_start_token) || !valid_token(end_token) ||
      timestamp_begin < 0 ||
      timestamp_begin > static_cast<int>(vocabulary_size)) {
    if (error != nullptr) {
      *error = "whisper config: token ids are outside the vocabulary";
    }
    return false;
  }
  return true;
}

nlohmann::json to_json(const WhisperConfig &config) {
  return {{"num_mel_bins", config.mel_bins},
          {"d_model", config.model_width},
          {"encoder_layers", config.encoder_layers},
          {"encoder_attention_heads", config.encoder_heads},
          {"encoder_ffn_dim", config.encoder_feed_forward},
          {"decoder_layers", config.decoder_layers},
          {"decoder_attention_heads", config.decoder_heads},
          {"decoder_ffn_dim", config.decoder_feed_forward},
          {"max_source_positions", config.max_source_positions},
          {"max_target_positions", config.max_target_positions},
          {"vocab_size", config.vocabulary_size},
          {"decoder_start_token_id", config.decoder_start_token},
          {"eos_token_id", config.end_token},
          {"timestamp_begin", config.timestamp_begin},
          {"decoder_prompt", config.decoder_prompt},
          {"suppress_tokens", config.suppressed_tokens}};
}

std::optional<WhisperConfig>
whisper_config_from_json(const nlohmann::json &value, std::string *error) {
  if (!value.is_object()) {
    if (error != nullptr) {
      *error = "whisper config: expected an object";
    }
    return std::nullopt;
  }
  WhisperConfig config;
  const auto mel_bins = positive_size(value, "num_mel_bins", "mel_bins", error);
  const auto model_width =
      positive_size(value, "d_model", "model_width", error);
  const auto encoder_layers =
      positive_size(value, "encoder_layers", nullptr, error);
  const auto encoder_heads =
      positive_size(value, "encoder_attention_heads", "encoder_heads", error);
  const auto encoder_feed_forward =
      positive_size(value, "encoder_ffn_dim", "encoder_feed_forward", error);
  const auto decoder_layers =
      positive_size(value, "decoder_layers", nullptr, error);
  const auto decoder_heads =
      positive_size(value, "decoder_attention_heads", "decoder_heads", error);
  const auto decoder_feed_forward =
      positive_size(value, "decoder_ffn_dim", "decoder_feed_forward", error);
  const auto max_source_positions =
      positive_size(value, "max_source_positions", nullptr, error);
  const auto max_target_positions =
      positive_size(value, "max_target_positions", nullptr, error);
  const auto vocabulary_size =
      positive_size(value, "vocab_size", "vocabulary_size", error);
  if (!mel_bins || !model_width || !encoder_layers || !encoder_heads ||
      !encoder_feed_forward || !decoder_layers || !decoder_heads ||
      !decoder_feed_forward || !max_source_positions || !max_target_positions ||
      !vocabulary_size) {
    return std::nullopt;
  }
  config.mel_bins = *mel_bins;
  config.model_width = *model_width;
  config.encoder_layers = *encoder_layers;
  config.encoder_heads = *encoder_heads;
  config.encoder_feed_forward = *encoder_feed_forward;
  config.decoder_layers = *decoder_layers;
  config.decoder_heads = *decoder_heads;
  config.decoder_feed_forward = *decoder_feed_forward;
  config.max_source_positions = *max_source_positions;
  config.max_target_positions = *max_target_positions;
  config.vocabulary_size = *vocabulary_size;
  config.decoder_start_token = value.value(
      "decoder_start_token_id", value.value("decoder_start_token", 1));
  config.end_token = value.value("eos_token_id", value.value("end_token", 2));
  config.timestamp_begin = value.value(
      "timestamp_begin",
      config.vocabulary_size > 50364 ? 50364 : config.timestamp_begin);
  if (const auto prompt = value.find("decoder_prompt");
      prompt != value.end() && prompt->is_array()) {
    config.decoder_prompt = prompt->get<std::vector<int>>();
  } else {
    config.decoder_prompt = {config.decoder_start_token};
    if (const auto forced = value.find("forced_decoder_ids");
        forced != value.end() && forced->is_array()) {
      for (const nlohmann::json &entry : *forced) {
        if (!entry.is_array() || entry.size() != 2 ||
            !entry[1].is_number_integer()) {
          continue;
        }
        const int token_id = entry[1].get<int>();
        if (token_id != config.timestamp_begin - 1) {
          config.decoder_prompt.push_back(token_id);
        }
      }
    }
  }
  if (const auto suppressed = value.find("suppress_tokens");
      suppressed != value.end() && suppressed->is_array()) {
    config.suppressed_tokens = suppressed->get<std::vector<int>>();
  }
  if (!config.valid(error)) {
    return std::nullopt;
  }
  return config;
}

WhisperCheckpoint random_whisper_checkpoint(const WhisperConfig &config,
                                            std::uint64_t seed) {
  WhisperCheckpoint checkpoint;
  checkpoint.config = config;
  checkpoint.metadata.origin = "random";
  std::mt19937 generator(static_cast<std::mt19937::result_type>(seed));
  const std::size_t width = config.model_width;
  checkpoint.tensors["model.encoder.conv1.weight"] = random_tensor(
      {static_cast<int>(width), static_cast<int>(config.mel_bins), 3},
      generator);
  checkpoint.tensors["model.encoder.conv1.bias"] =
      SafeTensor{{static_cast<int>(width)}, std::vector<float>(width)};
  checkpoint.tensors["model.encoder.conv2.weight"] = random_tensor(
      {static_cast<int>(width), static_cast<int>(width), 3}, generator);
  checkpoint.tensors["model.encoder.conv2.bias"] =
      SafeTensor{{static_cast<int>(width)}, std::vector<float>(width)};
  checkpoint.tensors["model.encoder.embed_positions.weight"] = random_tensor(
      {static_cast<int>(config.max_source_positions), static_cast<int>(width)},
      generator);
  for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
    const std::string prefix =
        "model.encoder.layers." + std::to_string(layer) + ".";
    add_attention(checkpoint.tensors, prefix + "self_attn.", width, generator);
    add_layer_norm(checkpoint.tensors, prefix + "self_attn_layer_norm", width,
                   generator);
    checkpoint.tensors[prefix + "fc1.weight"] =
        random_tensor({static_cast<int>(config.encoder_feed_forward),
                       static_cast<int>(width)},
                      generator);
    checkpoint.tensors[prefix + "fc1.bias"] =
        SafeTensor{{static_cast<int>(config.encoder_feed_forward)},
                   std::vector<float>(config.encoder_feed_forward)};
    checkpoint.tensors[prefix + "fc2.weight"] =
        random_tensor({static_cast<int>(width),
                       static_cast<int>(config.encoder_feed_forward)},
                      generator);
    checkpoint.tensors[prefix + "fc2.bias"] =
        SafeTensor{{static_cast<int>(width)}, std::vector<float>(width)};
    add_layer_norm(checkpoint.tensors, prefix + "final_layer_norm", width,
                   generator);
  }
  add_layer_norm(checkpoint.tensors, "model.encoder.layer_norm", width,
                 generator);

  checkpoint.tensors["model.decoder.embed_tokens.weight"] = random_tensor(
      {static_cast<int>(config.vocabulary_size), static_cast<int>(width)},
      generator);
  checkpoint.tensors["model.decoder.embed_positions.weight"] = random_tensor(
      {static_cast<int>(config.max_target_positions), static_cast<int>(width)},
      generator);
  for (std::size_t layer = 0; layer < config.decoder_layers; ++layer) {
    const std::string prefix =
        "model.decoder.layers." + std::to_string(layer) + ".";
    add_attention(checkpoint.tensors, prefix + "self_attn.", width, generator);
    add_attention(checkpoint.tensors, prefix + "encoder_attn.", width,
                  generator);
    add_layer_norm(checkpoint.tensors, prefix + "self_attn_layer_norm", width,
                   generator);
    add_layer_norm(checkpoint.tensors, prefix + "encoder_attn_layer_norm",
                   width, generator);
    checkpoint.tensors[prefix + "fc1.weight"] =
        random_tensor({static_cast<int>(config.decoder_feed_forward),
                       static_cast<int>(width)},
                      generator);
    checkpoint.tensors[prefix + "fc1.bias"] =
        SafeTensor{{static_cast<int>(config.decoder_feed_forward)},
                   std::vector<float>(config.decoder_feed_forward)};
    checkpoint.tensors[prefix + "fc2.weight"] =
        random_tensor({static_cast<int>(width),
                       static_cast<int>(config.decoder_feed_forward)},
                      generator);
    checkpoint.tensors[prefix + "fc2.bias"] =
        SafeTensor{{static_cast<int>(width)}, std::vector<float>(width)};
    add_layer_norm(checkpoint.tensors, prefix + "final_layer_norm", width,
                   generator);
  }
  add_layer_norm(checkpoint.tensors, "model.decoder.layer_norm", width,
                 generator);
  checkpoint.tensors["proj_out.weight"] = random_tensor(
      {static_cast<int>(config.vocabulary_size), static_cast<int>(width)},
      generator);
  checkpoint.test_vocabulary.resize(config.vocabulary_size);
  for (std::size_t token_id = 0; token_id < config.vocabulary_size;
       ++token_id) {
    checkpoint.test_vocabulary[token_id] =
        token_id < static_cast<std::size_t>(config.timestamp_begin)
            ? " token" + std::to_string(token_id)
            : "<|" + std::to_string((token_id - config.timestamp_begin) * 20) +
                  "ms|>";
  }
  if (config.decoder_start_token >= 0 &&
      config.decoder_start_token <
          static_cast<int>(checkpoint.test_vocabulary.size())) {
    checkpoint.test_vocabulary[config.decoder_start_token] = "<|start|>";
  }
  if (config.end_token >= 0 &&
      config.end_token < static_cast<int>(checkpoint.test_vocabulary.size())) {
    checkpoint.test_vocabulary[config.end_token] = "<|end|>";
  }
  return checkpoint;
}

std::optional<WhisperCheckpoint>
load_whisper_checkpoint(const std::string &directory, std::string *error) {
  const std::filesystem::path root(directory);
  const std::optional<std::string> config_bytes =
      read_file(root / "config.json");
  if (!config_bytes) {
    if (error != nullptr) {
      *error = "whisper checkpoint: cannot read '" +
               (root / "config.json").string() + "'";
    }
    return std::nullopt;
  }
  const nlohmann::json config_json =
      nlohmann::json::parse(*config_bytes, nullptr, false);
  if (config_json.is_discarded()) {
    if (error != nullptr) {
      *error = "whisper checkpoint: config.json is not valid JSON";
    }
    return std::nullopt;
  }
  const std::optional<WhisperConfig> config =
      whisper_config_from_json(config_json, error);
  if (!config) {
    return std::nullopt;
  }
  const std::optional<std::string> manifest_bytes =
      read_file(root / "zetta-checkpoint.json");
  if (!manifest_bytes) {
    if (error != nullptr) {
      *error = "whisper checkpoint: zetta-checkpoint.json is required "
               "to pin revision and licenses";
    }
    return std::nullopt;
  }
  const nlohmann::json manifest =
      nlohmann::json::parse(*manifest_bytes, nullptr, false);
  if (manifest.is_discarded() || !manifest.is_object()) {
    if (error != nullptr) {
      *error = "whisper checkpoint: zetta-checkpoint.json is invalid";
    }
    return std::nullopt;
  }
  WhisperCheckpoint checkpoint;
  checkpoint.config = *config;
  checkpoint.metadata.repository = manifest.value("repository", "");
  checkpoint.metadata.revision = manifest.value("revision", "");
  checkpoint.metadata.weights_license = manifest.value("weights_license", "");
  checkpoint.metadata.tokenizer_license =
      manifest.value("tokenizer_license", "");
  checkpoint.metadata.config_license = manifest.value("config_license", "");
  checkpoint.metadata.origin = manifest.value("origin", "pretrained");
  if (checkpoint.metadata.origin != "random" &&
      !valid_pretrained_metadata(checkpoint.metadata, error)) {
    return std::nullopt;
  }
  std::string weights_error;
  const std::optional<SafeTensors> weights =
      zetta::autograd::read_safetensors_file(
          (root / "model.safetensors").string(), &weights_error);
  if (!weights) {
    if (error != nullptr) {
      *error = "whisper checkpoint: " + weights_error;
    }
    return std::nullopt;
  }
  checkpoint.tensors = *weights;
  std::string tokenizer_error;
  checkpoint.tokenizer = zetta::tokenizer::BpeTokenizer::from_directory(
      directory, &tokenizer_error);
  if (!checkpoint.tokenizer) {
    const std::optional<std::string> vocabulary =
        read_file(root / "zetta-vocabulary.json");
    if (!vocabulary) {
      if (error != nullptr) {
        *error = "whisper checkpoint: " + tokenizer_error;
      }
      return std::nullopt;
    }
    const nlohmann::json parsed =
        nlohmann::json::parse(*vocabulary, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_array()) {
      if (error != nullptr) {
        *error = "whisper checkpoint: zetta-vocabulary.json must be "
                 "a string array";
      }
      return std::nullopt;
    }
    checkpoint.test_vocabulary = parsed.get<std::vector<std::string>>();
  }
  if (!validate_checkpoint(checkpoint, error)) {
    return std::nullopt;
  }
  return checkpoint;
}

WhisperDecodeResult whisper_decode(const WhisperCheckpoint &checkpoint,
                                   const LogMelSpectrogram &mel,
                                   std::size_t maximum_new_tokens) {
  WhisperDecodeResult result;
  if (!validate_checkpoint(checkpoint, &result.error)) {
    return result;
  }
  if (mel.mel_bins != checkpoint.config.mel_bins || mel.frames == 0) {
    result.error = "whisper decode: log-mel shape does not match the model";
    return result;
  }
  const Matrix encoded = encode(checkpoint, mel);
  std::vector<int> tokens = checkpoint.config.decoder_prompt;
  if (tokens.empty()) {
    tokens.push_back(checkpoint.config.decoder_start_token);
  }
  std::vector<int> generated;
  generated.reserve(maximum_new_tokens);
  double probability_sum = 0.0;
  for (std::size_t produced = 0;
       produced < maximum_new_tokens &&
       tokens.size() < checkpoint.config.max_target_positions;
       ++produced) {
    const Matrix hidden = decode_hidden(checkpoint, encoded, tokens);
    const auto [token_id, probability] = next_token(checkpoint, hidden);
    result.tokens.push_back({token_id, probability});
    probability_sum += probability;
    tokens.push_back(token_id);
    generated.push_back(token_id);
    if (token_id >= checkpoint.config.timestamp_begin) {
      const double timestamp =
          0.02 * (token_id - checkpoint.config.timestamp_begin);
      if (!result.first_timestamp) {
        result.first_timestamp = timestamp;
      }
      result.last_timestamp = timestamp;
    }
    if (token_id == checkpoint.config.end_token) {
      break;
    }
  }
  result.confidence =
      result.tokens.empty() ? 0.0 : probability_sum / result.tokens.size();
  if (checkpoint.tokenizer) {
    std::vector<int> text_tokens;
    for (const int token_id : generated) {
      if (token_id < checkpoint.config.timestamp_begin &&
          token_id != checkpoint.config.end_token) {
        text_tokens.push_back(token_id);
      }
    }
    result.text = checkpoint.tokenizer->decode(text_tokens);
  } else {
    result.text = fallback_decode(checkpoint, generated);
  }
  return result;
}

} // namespace zetta::ml
