// SPDX-License-Identifier: MIT

#include "zetta/ml/vision.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace zetta::ml {
namespace {

using zetta::autograd::SafeTensor;
using zetta::autograd::SafeTensors;

std::optional<std::string> require_tensor(const SafeTensors &tensors,
                                          const std::string &name,
                                          const std::vector<int> &shape) {
  const auto found = tensors.find(name);
  if (found == tensors.end()) {
    return "vision checkpoint: missing tensor '" + name + "'";
  }
  if (found->second.shape != shape) {
    return "vision checkpoint: tensor '" + name + "' has the wrong shape";
  }
  return std::nullopt;
}

const SafeTensor &tensor(const VitBackbone &backbone, const std::string &name) {
  return backbone.tensors.at(name);
}

std::vector<float> linear(const std::vector<float> &input, std::size_t rows,
                          std::size_t input_size, const SafeTensor &weight,
                          const SafeTensor &bias) {
  const std::size_t output_size = bias.data.size();
  std::vector<float> output(rows * output_size, 0.0F);
  for (std::size_t row = 0; row < rows; ++row) {
    for (std::size_t out = 0; out < output_size; ++out) {
      float value = bias.data[out];
      for (std::size_t in = 0; in < input_size; ++in) {
        value +=
            input[row * input_size + in] * weight.data[out * input_size + in];
      }
      output[row * output_size + out] = value;
    }
  }
  return output;
}

std::vector<float> layer_norm(const std::vector<float> &input, std::size_t rows,
                              std::size_t columns, const SafeTensor &weight,
                              const SafeTensor &bias, float epsilon) {
  std::vector<float> output(input.size());
  for (std::size_t row = 0; row < rows; ++row) {
    double mean = 0.0;
    for (std::size_t column = 0; column < columns; ++column) {
      mean += input[row * columns + column];
    }
    mean /= static_cast<double>(columns);
    double variance = 0.0;
    for (std::size_t column = 0; column < columns; ++column) {
      const double centered = input[row * columns + column] - mean;
      variance += centered * centered;
    }
    variance /= static_cast<double>(columns);
    const double inverse = 1.0 / std::sqrt(variance + epsilon);
    for (std::size_t column = 0; column < columns; ++column) {
      output[row * columns + column] =
          static_cast<float>((input[row * columns + column] - mean) * inverse *
                                 weight.data[column] +
                             bias.data[column]);
    }
  }
  return output;
}

void add_in_place(std::vector<float> &destination,
                  const std::vector<float> &source) {
  for (std::size_t index = 0; index < destination.size(); ++index) {
    destination[index] += source[index];
  }
}

std::vector<float> attention(const VitBackbone &backbone,
                             const std::vector<float> &input, std::size_t layer,
                             std::size_t tokens) {
  const VitConfig &config = backbone.config;
  const std::string prefix = "encoder." + std::to_string(layer) + ".";
  const SafeTensor &qkv_weight = tensor(backbone, prefix + "qkv.weight");
  const SafeTensor &qkv_bias = tensor(backbone, prefix + "qkv.bias");
  const std::vector<float> qkv =
      linear(input, tokens, config.embedding_size, qkv_weight, qkv_bias);
  const std::size_t head_size = config.embedding_size / config.attention_heads;
  const float scale = 1.0F / std::sqrt(static_cast<float>(head_size));
  std::vector<float> combined(tokens * config.embedding_size, 0.0F);

  for (std::size_t head = 0; head < config.attention_heads; ++head) {
    for (std::size_t query = 0; query < tokens; ++query) {
      std::vector<float> scores(tokens);
      float maximum = -std::numeric_limits<float>::infinity();
      for (std::size_t key = 0; key < tokens; ++key) {
        float score = 0.0F;
        for (std::size_t dimension = 0; dimension < head_size; ++dimension) {
          const std::size_t channel = head * head_size + dimension;
          score += qkv[query * 3 * config.embedding_size + channel] *
                   qkv[key * 3 * config.embedding_size + config.embedding_size +
                       channel];
        }
        scores[key] = score * scale;
        maximum = std::max(maximum, scores[key]);
      }
      float total = 0.0F;
      for (float &score : scores) {
        score = std::exp(score - maximum);
        total += score;
      }
      for (float &score : scores) {
        score /= total;
      }
      for (std::size_t dimension = 0; dimension < head_size; ++dimension) {
        float value = 0.0F;
        const std::size_t channel = head * head_size + dimension;
        for (std::size_t key = 0; key < tokens; ++key) {
          value += scores[key] * qkv[key * 3 * config.embedding_size +
                                     2 * config.embedding_size + channel];
        }
        combined[query * config.embedding_size + channel] = value;
      }
    }
  }
  return linear(combined, tokens, config.embedding_size,
                tensor(backbone, prefix + "projection.weight"),
                tensor(backbone, prefix + "projection.bias"));
}

std::vector<float> patch_embed(const VitBackbone &backbone,
                               const std::vector<float> &input) {
  const VitConfig &config = backbone.config;
  const SafeTensor &weight = tensor(backbone, "patch_embed.weight");
  const SafeTensor &bias = tensor(backbone, "patch_embed.bias");
  const std::size_t patches_x = config.image_width / config.patch_size;
  const std::size_t patches_y = config.image_height / config.patch_size;
  std::vector<float> output((config.patch_count() + 1) * config.embedding_size);
  std::copy(tensor(backbone, "cls_token").data.begin(),
            tensor(backbone, "cls_token").data.end(), output.begin());

  for (std::size_t patch_y = 0; patch_y < patches_y; ++patch_y) {
    for (std::size_t patch_x = 0; patch_x < patches_x; ++patch_x) {
      const std::size_t token = 1 + patch_y * patches_x + patch_x;
      for (std::size_t embedding = 0; embedding < config.embedding_size;
           ++embedding) {
        float value = bias.data[embedding];
        std::size_t kernel_index = 0;
        for (std::size_t channel = 0; channel < config.channels; ++channel) {
          for (std::size_t y = 0; y < config.patch_size; ++y) {
            for (std::size_t x = 0; x < config.patch_size; ++x) {
              const std::size_t image_x = patch_x * config.patch_size + x;
              const std::size_t image_y = patch_y * config.patch_size + y;
              const std::size_t image_index =
                  channel * config.image_width * config.image_height +
                  image_y * config.image_width + image_x;
              const std::size_t weight_index = embedding * config.channels *
                                                   config.patch_size *
                                                   config.patch_size +
                                               kernel_index;
              value += input[image_index] * weight.data[weight_index];
              ++kernel_index;
            }
          }
        }
        output[token * config.embedding_size + embedding] = value;
      }
    }
  }
  const SafeTensor &position = tensor(backbone, "position_embedding");
  add_in_place(output, position.data);
  return output;
}

SafeTensor random_tensor(const std::vector<int> &shape,
                         std::mt19937_64 &generator, float scale,
                         float offset = 0.0F) {
  std::size_t count = 1;
  for (const int dimension : shape) {
    count *= static_cast<std::size_t>(dimension);
  }
  std::normal_distribution<float> distribution(offset, scale);
  SafeTensor result;
  result.shape = shape;
  result.data.resize(count);
  for (float &value : result.data) {
    value = distribution(generator);
  }
  return result;
}

} // namespace

std::size_t VitConfig::patch_count() const {
  if (patch_size == 0) {
    return 0;
  }
  return (image_width / patch_size) * (image_height / patch_size);
}

bool VitConfig::valid(std::string *error) const {
  const auto fail = [error](std::string reason) {
    if (error != nullptr) {
      *error = std::move(reason);
    }
    return false;
  };
  if (image_width == 0 || image_height == 0 || patch_size == 0 ||
      channels == 0 || embedding_size == 0 || attention_heads == 0 ||
      encoder_layers == 0 || mlp_size == 0) {
    return fail("vision config: every dimension must be positive");
  }
  if (image_width % patch_size != 0 || image_height % patch_size != 0) {
    return fail(
        "vision config: image dimensions must be divisible by patch_size");
  }
  if (embedding_size % attention_heads != 0) {
    return fail(
        "vision config: embedding_size must be divisible by attention_heads");
  }
  return true;
}

nlohmann::json to_json(const VitConfig &config) {
  return {{"image_width", config.image_width},
          {"image_height", config.image_height},
          {"patch_size", config.patch_size},
          {"channels", config.channels},
          {"embedding_size", config.embedding_size},
          {"attention_heads", config.attention_heads},
          {"encoder_layers", config.encoder_layers},
          {"mlp_size", config.mlp_size},
          {"layer_norm_epsilon", config.layer_norm_epsilon}};
}

std::optional<VitConfig> vit_config_from_json(const nlohmann::json &value,
                                              std::string *error) {
  if (!value.is_object()) {
    if (error != nullptr) {
      *error = "vision config: expected an object";
    }
    return std::nullopt;
  }
  VitConfig config;
  const auto dimension = [&](const char *name, std::size_t &output) {
    const auto found = value.find(name);
    if (found == value.end() || !found->is_number_integer() ||
        found->get<std::int64_t>() <= 0) {
      return false;
    }
    output = found->get<std::size_t>();
    return true;
  };
  if (!dimension("image_width", config.image_width) ||
      !dimension("image_height", config.image_height) ||
      !dimension("patch_size", config.patch_size) ||
      !dimension("channels", config.channels) ||
      !dimension("embedding_size", config.embedding_size) ||
      !dimension("attention_heads", config.attention_heads) ||
      !dimension("encoder_layers", config.encoder_layers) ||
      !dimension("mlp_size", config.mlp_size)) {
    if (error != nullptr) {
      *error = "vision config: missing or non-integer dimension";
    }
    return std::nullopt;
  }
  if (const auto epsilon = value.find("layer_norm_epsilon");
      epsilon != value.end()) {
    if (!epsilon->is_number()) {
      if (error != nullptr) {
        *error = "vision config: layer_norm_epsilon must be numeric";
      }
      return std::nullopt;
    }
    config.layer_norm_epsilon = epsilon->get<float>();
  }
  if (!config.valid(error)) {
    return std::nullopt;
  }
  return config;
}

std::optional<VitBackbone> load_vit_backbone(const VitConfig &config,
                                             std::string_view safetensors,
                                             std::string *error) {
  if (!config.valid(error)) {
    return std::nullopt;
  }
  std::optional<SafeTensors> tensors =
      zetta::autograd::parse_safetensors(safetensors, error);
  if (!tensors) {
    return std::nullopt;
  }
  const int embedding = static_cast<int>(config.embedding_size);
  const int mlp = static_cast<int>(config.mlp_size);
  const int patch = static_cast<int>(config.patch_size);
  const int channels = static_cast<int>(config.channels);
  const int tokens = static_cast<int>(config.patch_count() + 1);
  std::vector<std::pair<std::string, std::vector<int>>> required = {
      {"patch_embed.weight", {embedding, channels, patch, patch}},
      {"patch_embed.bias", {embedding}},
      {"cls_token", {embedding}},
      {"position_embedding", {tokens, embedding}},
  };
  for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
    const std::string prefix = "encoder." + std::to_string(layer) + ".";
    required.insert(required.end(),
                    {
                        {prefix + "norm1.weight", {embedding}},
                        {prefix + "norm1.bias", {embedding}},
                        {prefix + "qkv.weight", {3 * embedding, embedding}},
                        {prefix + "qkv.bias", {3 * embedding}},
                        {prefix + "projection.weight", {embedding, embedding}},
                        {prefix + "projection.bias", {embedding}},
                        {prefix + "norm2.weight", {embedding}},
                        {prefix + "norm2.bias", {embedding}},
                        {prefix + "mlp1.weight", {mlp, embedding}},
                        {prefix + "mlp1.bias", {mlp}},
                        {prefix + "mlp2.weight", {embedding, mlp}},
                        {prefix + "mlp2.bias", {embedding}},
                    });
  }
  for (const auto &[name, shape] : required) {
    if (const std::optional<std::string> reason =
            require_tensor(*tensors, name, shape)) {
      if (error != nullptr) {
        *error = *reason;
      }
      return std::nullopt;
    }
  }
  return VitBackbone{config, std::move(*tensors)};
}

VitBackbone random_vit_backbone(const VitConfig &config, std::uint64_t seed) {
  std::mt19937_64 generator(seed);
  SafeTensors tensors;
  const int embedding = static_cast<int>(config.embedding_size);
  const int mlp = static_cast<int>(config.mlp_size);
  const int patch = static_cast<int>(config.patch_size);
  const int channels = static_cast<int>(config.channels);
  const int tokens = static_cast<int>(config.patch_count() + 1);
  tensors["patch_embed.weight"] =
      random_tensor({embedding, channels, patch, patch}, generator, 0.08F);
  tensors["patch_embed.bias"] = random_tensor({embedding}, generator, 0.01F);
  tensors["cls_token"] = random_tensor({embedding}, generator, 0.02F);
  tensors["position_embedding"] =
      random_tensor({tokens, embedding}, generator, 0.02F);
  for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
    const std::string prefix = "encoder." + std::to_string(layer) + ".";
    tensors[prefix + "norm1.weight"] = SafeTensor{
        {embedding}, std::vector<float>(config.embedding_size, 1.0F)};
    tensors[prefix + "norm1.bias"] = SafeTensor{
        {embedding}, std::vector<float>(config.embedding_size, 0.0F)};
    tensors[prefix + "qkv.weight"] =
        random_tensor({3 * embedding, embedding}, generator, 0.08F);
    tensors[prefix + "qkv.bias"] =
        random_tensor({3 * embedding}, generator, 0.01F);
    tensors[prefix + "projection.weight"] =
        random_tensor({embedding, embedding}, generator, 0.08F);
    tensors[prefix + "projection.bias"] =
        random_tensor({embedding}, generator, 0.01F);
    tensors[prefix + "norm2.weight"] = SafeTensor{
        {embedding}, std::vector<float>(config.embedding_size, 1.0F)};
    tensors[prefix + "norm2.bias"] = SafeTensor{
        {embedding}, std::vector<float>(config.embedding_size, 0.0F)};
    tensors[prefix + "mlp1.weight"] =
        random_tensor({mlp, embedding}, generator, 0.08F);
    tensors[prefix + "mlp1.bias"] = random_tensor({mlp}, generator, 0.01F);
    tensors[prefix + "mlp2.weight"] =
        random_tensor({embedding, mlp}, generator, 0.08F);
    tensors[prefix + "mlp2.bias"] =
        random_tensor({embedding}, generator, 0.01F);
  }
  return VitBackbone{config, std::move(tensors)};
}

std::vector<float> vit_embed(const VitBackbone &backbone,
                             const std::vector<float> &normalized_chw,
                             std::string *error) {
  const VitConfig &config = backbone.config;
  const std::size_t expected =
      config.channels * config.image_width * config.image_height;
  if (normalized_chw.size() != expected) {
    if (error != nullptr) {
      *error = "vision embed: normalized image has the wrong size";
    }
    return {};
  }
  std::vector<float> hidden = patch_embed(backbone, normalized_chw);
  const std::size_t tokens = config.patch_count() + 1;
  for (std::size_t layer = 0; layer < config.encoder_layers; ++layer) {
    const std::string prefix = "encoder." + std::to_string(layer) + ".";
    const std::vector<float> normalized = layer_norm(
        hidden, tokens, config.embedding_size,
        tensor(backbone, prefix + "norm1.weight"),
        tensor(backbone, prefix + "norm1.bias"), config.layer_norm_epsilon);
    add_in_place(hidden, attention(backbone, normalized, layer, tokens));

    const std::vector<float> normalized_mlp = layer_norm(
        hidden, tokens, config.embedding_size,
        tensor(backbone, prefix + "norm2.weight"),
        tensor(backbone, prefix + "norm2.bias"), config.layer_norm_epsilon);
    std::vector<float> expanded =
        linear(normalized_mlp, tokens, config.embedding_size,
               tensor(backbone, prefix + "mlp1.weight"),
               tensor(backbone, prefix + "mlp1.bias"));
    constexpr float kSqrtTwo = 1.4142135623730951F;
    for (float &value : expanded) {
      value = 0.5F * value * (1.0F + std::erf(value / kSqrtTwo));
    }
    add_in_place(hidden, linear(expanded, tokens, config.mlp_size,
                                tensor(backbone, prefix + "mlp2.weight"),
                                tensor(backbone, prefix + "mlp2.bias")));
  }
  return std::vector<float>(hidden.begin(),
                            hidden.begin() + config.embedding_size);
}

} // namespace zetta::ml
