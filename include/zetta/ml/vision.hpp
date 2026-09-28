#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/autograd/safetensors.hpp"
#include "zetta/data/dataset_path.hpp"

namespace zetta {
class Node;
}

// Native computer-vision transfer learning for M-13 (#1052).
//
// The image decoder, resize/normalization path, ViT forward pass,
// source-grouped split, distributed linear-head trainer, metrics, and table
// runner are all implemented in zetta-core. The ViT backbone is frozen; only
// the classification head is trained. Checkpoint tensors use the existing
// safetensors reader.
//
// See docs/ml/vision.md.
namespace zetta::ml {

struct Image {
  std::size_t width = 0;
  std::size_t height = 0;
  std::size_t channels = 0;
  std::vector<float> pixels;

  bool empty() const {
    return width == 0 || height == 0 || channels == 0 || pixels.empty();
  }
};

struct ImageDecodeResult {
  Image image;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Decode an 8-bit PNG or baseline sequential JPEG into RGB floats in [0, 1].
// The format is detected from the bytes. Malformed or unsupported input fails
// with a path-qualified reason and never returns a partial image.
ImageDecodeResult decode_image(std::string_view bytes);

// Bilinear resize to RGB CHW order, followed by per-channel normalization.
std::vector<float>
resize_normalize(const Image &image, std::size_t width, std::size_t height,
                 const std::vector<float> &mean,
                 const std::vector<float> &standard_deviation);

struct VitConfig {
  std::size_t image_width = 0;
  std::size_t image_height = 0;
  std::size_t patch_size = 0;
  std::size_t channels = 3;
  std::size_t embedding_size = 0;
  std::size_t attention_heads = 0;
  std::size_t encoder_layers = 0;
  std::size_t mlp_size = 0;
  float layer_norm_epsilon = 1.0e-5F;

  std::size_t patch_count() const;
  bool valid(std::string *error = nullptr) const;
};

struct VitBackbone {
  VitConfig config;
  zetta::autograd::SafeTensors tensors;
};

nlohmann::json to_json(const VitConfig &config);
std::optional<VitConfig> vit_config_from_json(const nlohmann::json &value,
                                              std::string *error = nullptr);

// Load and validate every frozen-backbone tensor from safetensors bytes.
std::optional<VitBackbone> load_vit_backbone(const VitConfig &config,
                                             std::string_view safetensors,
                                             std::string *error = nullptr);

// Deterministic tiny weights for unit tests and locally installed test models.
// Production runs load a registered backbone model instead.
VitBackbone random_vit_backbone(const VitConfig &config, std::uint64_t seed);

// Frozen ViT forward pass. Returns one class-token embedding per image.
std::vector<float> vit_embed(const VitBackbone &backbone,
                             const std::vector<float> &normalized_chw,
                             std::string *error = nullptr);

struct VisionExample {
  std::string image_ref;
  std::string source;
  std::size_t label = 0;
  std::vector<float> embedding;
  bool held_out = false;
};

// Assign whole source groups to train or held-out sets. At least two distinct
// sources are required. The result is deterministic under seed and never puts
// one source in both sets.
bool assign_source_disjoint_split(std::vector<VisionExample> &examples,
                                  double holdout_fraction, std::uint64_t seed,
                                  std::string *error = nullptr);

struct LinearVisionHead {
  std::size_t classes = 0;
  std::size_t embedding_size = 0;
  std::vector<double> weights;
  std::vector<double> bias;

  std::vector<double> probabilities(const std::vector<float> &embedding) const;
};

using VisionAllReduce = std::function<std::vector<double>(
    const std::string &tag, const std::vector<double> &local)>;

struct VisionTrainParams {
  std::size_t classes = 2;
  std::size_t epochs = 80;
  double learning_rate = 0.15;
  std::uint64_t seed = 1;
};

struct VisionMetrics {
  double accuracy = 0.0;
  double macro_f1 = 0.0;
  std::uint64_t train_rows = 0;
  std::uint64_t held_out_rows = 0;
  std::vector<std::uint64_t> confusion_matrix;
};

struct VisionTrainResult {
  LinearVisionHead head;
  VisionMetrics metrics;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Synchronous data-parallel head training. Each epoch allreduces summed
// gradients and the local training-row count, so single-member and multi-member
// runs apply the same update to identical starting weights.
VisionTrainResult
train_vision_head(const std::vector<VisionExample> &local_examples,
                  const VisionTrainParams &params,
                  const VisionAllReduce &allreduce,
                  const std::string &round_tag = "ml.vision");

struct VisionRunParams {
  std::string project;
  std::string graph;
  std::string image_column = "image";
  std::string label_column = "label";
  std::string source_column = "source";
  std::string backbone_model;
  std::string model_name = "vision_model";
  double holdout_fraction = 0.25;
  std::size_t epochs = 80;
  double learning_rate = 0.15;
  std::uint64_t seed = 1;
};

using VisionReadAuthorizer =
    std::function<bool(std::string_view image_ref, std::string &reason)>;

struct VisionRunResult {
  std::string run_id;
  std::string model_id;
  std::string prediction_table;
  VisionMetrics metrics;
  std::uint64_t images_read = 0;
  std::uint64_t images_excluded = 0;
  std::string error;

  bool ok() const { return error.empty(); }
};

// Run M-13 over a resolved table, register the trained model, and write the
// per-image prediction table plus lineage. The authorizer is called before each
// large-object read; one refusal fails the run before that object's bytes are
// accessed.
VisionRunResult
run_vision_on_table(Node &node, const zetta::data::ResolvedDataset &dataset,
                    const VisionRunParams &params,
                    const VisionReadAuthorizer &authorizer = {});

} // namespace zetta::ml
