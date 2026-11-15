// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/autograd/safetensors.hpp"
#include "zetta/cluster/cluster.hpp"
#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/ml/vision.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"

namespace {

zetta::ml::VitConfig tiny_config() {
  return {.image_width = 4,
          .image_height = 4,
          .patch_size = 2,
          .channels = 3,
          .embedding_size = 4,
          .attention_heads = 2,
          .encoder_layers = 1,
          .mlp_size = 6};
}

std::string one_pixel_png() {
  const unsigned char bytes[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D,
      'I',  'H',  'D',  'R',  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00,
      0x0D, 'I',  'D',  'A',  'T',  0x08, 0xD7, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
      0x1F, 0x00, 0x05, 0x00, 0x01, 0xFF, 0x89, 0x99, 0xD1, 0x8D, 0x00, 0x00,
      0x00, 0x00, 'I',  'E',  'N',  'D',  0xAE, 0x42, 0x60, 0x82};
  return {reinterpret_cast<const char *>(bytes), sizeof(bytes)};
}

void append_marker(std::string &bytes, unsigned char marker,
                   const std::vector<unsigned char> &body) {
  bytes.push_back(static_cast<char>(0xFF));
  bytes.push_back(static_cast<char>(marker));
  const std::size_t length = body.size() + 2;
  bytes.push_back(static_cast<char>((length >> 8) & 0xFF));
  bytes.push_back(static_cast<char>(length & 0xFF));
  bytes.append(reinterpret_cast<const char *>(body.data()), body.size());
}

std::string constant_gray_jpeg() {
  std::string bytes{"\xFF\xD8", 2};
  std::vector<unsigned char> quantization(65, 1);
  quantization[0] = 0;
  append_marker(bytes, 0xDB, quantization);
  append_marker(bytes, 0xC0, {8, 0, 8, 0, 8, 1, 1, 0x11, 0});
  std::vector<unsigned char> dc = {0};
  dc.resize(17, 0);
  dc[1] = 1;
  dc.push_back(0);
  append_marker(bytes, 0xC4, dc);
  std::vector<unsigned char> ac = {0x10};
  ac.resize(17, 0);
  ac[1] = 1;
  ac.push_back(0);
  append_marker(bytes, 0xC4, ac);
  append_marker(bytes, 0xDA, {1, 1, 0, 0, 63, 0});
  bytes.push_back(static_cast<char>(0x3F));
  bytes.append("\xFF\xD9", 2);
  return bytes;
}

} // namespace

TEST(ImageDecode, ReadsNativePngAndBaselineJpeg) {
  const zetta::ml::ImageDecodeResult png =
      zetta::ml::decode_image(one_pixel_png());
  ASSERT_TRUE(png.ok()) << png.error;
  EXPECT_EQ(png.image.width, 1u);
  EXPECT_EQ(png.image.height, 1u);
  EXPECT_EQ(png.image.channels, 3u);

  const zetta::ml::ImageDecodeResult jpeg =
      zetta::ml::decode_image(constant_gray_jpeg());
  ASSERT_TRUE(jpeg.ok()) << jpeg.error;
  EXPECT_EQ(jpeg.image.width, 8u);
  EXPECT_EQ(jpeg.image.height, 8u);
  ASSERT_EQ(jpeg.image.pixels.size(), 8u * 8u * 3u);
  EXPECT_NEAR(jpeg.image.pixels.front(), 128.0 / 255.0, 0.01);
}

TEST(ImageResize, ProducesNormalizedChwPixels) {
  const zetta::ml::Image image = {1, 1, 3, {1.0F, 0.5F, 0.0F}};
  const std::vector<float> resized = zetta::ml::resize_normalize(
      image, 2, 2, {0.5F, 0.5F, 0.5F}, {0.5F, 0.5F, 0.5F});
  ASSERT_EQ(resized.size(), 12u);
  EXPECT_FLOAT_EQ(resized[0], 1.0F);
  EXPECT_FLOAT_EQ(resized[4], 0.0F);
  EXPECT_FLOAT_EQ(resized[8], -1.0F);
}

TEST(ViT, TinyRandomBackboneEmbedsDeterministically) {
  const zetta::ml::VitConfig config = tiny_config();
  const zetta::ml::VitBackbone first =
      zetta::ml::random_vit_backbone(config, 42);
  const zetta::ml::VitBackbone second =
      zetta::ml::random_vit_backbone(config, 42);
  std::vector<float> input(3 * 4 * 4);
  for (std::size_t index = 0; index < input.size(); ++index) {
    input[index] = static_cast<float>(index) / input.size();
  }
  std::string error;
  const std::vector<float> first_embedding =
      zetta::ml::vit_embed(first, input, &error);
  ASSERT_TRUE(error.empty()) << error;
  const std::vector<float> second_embedding =
      zetta::ml::vit_embed(second, input, &error);
  ASSERT_TRUE(error.empty()) << error;
  EXPECT_EQ(first_embedding, second_embedding);
  EXPECT_EQ(first_embedding.size(), config.embedding_size);
}

TEST(ViT, LoadsItsArchitectureFromSafetensors) {
  const zetta::ml::VitConfig config = tiny_config();
  const zetta::ml::VitBackbone generated =
      zetta::ml::random_vit_backbone(config, 9);
  const std::string bytes =
      zetta::autograd::serialize_safetensors(generated.tensors);
  std::string error;
  const std::optional<zetta::ml::VitBackbone> loaded =
      zetta::ml::load_vit_backbone(config, bytes, &error);
  ASSERT_TRUE(loaded) << error;
  EXPECT_EQ(loaded->tensors.size(), generated.tensors.size());
}

TEST(VisionSplit, NeverPlacesOneSourceInTrainAndTest) {
  std::vector<zetta::ml::VisionExample> examples;
  for (int source = 0; source < 8; ++source) {
    for (int frame = 0; frame < 3; ++frame) {
      examples.push_back({"image",
                          "source-" + std::to_string(source),
                          static_cast<std::size_t>(source % 2),
                          {},
                          false});
    }
  }
  std::string error;
  ASSERT_TRUE(
      zetta::ml::assign_source_disjoint_split(examples, 0.25, 73, &error))
      << error;
  std::set<std::string> train;
  std::set<std::string> test;
  for (const zetta::ml::VisionExample &example : examples) {
    (example.held_out ? test : train).insert(example.source);
  }
  EXPECT_FALSE(train.empty());
  EXPECT_FALSE(test.empty());
  for (const std::string &source : train) {
    EXPECT_FALSE(test.contains(source));
  }
}

TEST(VisionHead, LearnsAPlantedSeparableSignal) {
  std::vector<zetta::ml::VisionExample> examples;
  for (int index = 0; index < 80; ++index) {
    const std::size_t label = index % 2;
    const float signal = label == 0 ? -2.0F : 2.0F;
    examples.push_back({"image-" + std::to_string(index),
                        "source-" + std::to_string(index / 4),
                        label,
                        {signal, signal * 0.5F, 0.1F},
                        index % 5 == 0});
  }
  const zetta::ml::VisionAllReduce identity =
      [](const std::string &, const std::vector<double> &values) {
        return values;
      };
  const zetta::ml::VisionTrainResult result = zetta::ml::train_vision_head(
      examples, {.classes = 2, .epochs = 40, .learning_rate = 0.2, .seed = 11},
      identity);
  ASSERT_TRUE(result.ok()) << result.error;
  EXPECT_GT(result.metrics.accuracy, 0.95);
  EXPECT_GT(result.metrics.macro_f1, 0.95);
}

TEST(VisionRunner, RefusesAnUnauthorizedImageBeforeReadingItsBytes) {
  zetta::test::TempDir temporary;
  zetta::cluster::ClusterConfig cluster_config;
  cluster_config.listen_host = "127.0.0.1";
  cluster_config.listen_port = 47930;
  cluster_config.port_range = 4;
  zetta::cluster::Cluster cluster("vision-auth", cluster_config);
  zetta::Node node({temporary.path().string(), false});
  ASSERT_TRUE(node.start());
  cluster.start();
  node.attach_cluster(cluster, 2);

  const auto database = node.create_database("images");
  ASSERT_TRUE(database);
  const auto object =
      database->large_objects().import_bytes("frame.png", one_pixel_png());
  ASSERT_TRUE(object);
  const auto table = database->create_table("frames");
  ASSERT_TRUE(table);
  table->add_row({{"image", object->id}, {"label", 0}, {"source", "camera-a"}});

  const zetta::ml::VitConfig config = tiny_config();
  const zetta::ml::VitBackbone backbone =
      zetta::ml::random_vit_backbone(config, 2);
  zetta::ml::ModelValue value;
  value.name = "tiny-vit";
  value.kind = "vision_backbone";
  value.feature_schema = {{"vit_config", zetta::ml::to_json(config)},
                          {"weights_origin", "random"}};
  const auto model = node.create_model(std::move(value));
  ASSERT_TRUE(model);
  ASSERT_TRUE(model->set_weights(
      zetta::autograd::serialize_safetensors(backbone.tensors)));

  zetta::data::ResolvedDataset dataset;
  dataset.table_id = table->id();
  dataset.database = "images";
  dataset.table = "frames";
  dataset.rows = 1;
  zetta::ml::VisionRunParams params;
  params.backbone_model = model->id();
  std::size_t authorization_checks = 0;
  const zetta::ml::VisionRunResult result = zetta::ml::run_vision_on_table(
      node, dataset, params, [&](std::string_view, std::string &reason) {
        ++authorization_checks;
        reason = "denied by test policy";
        return false;
      });
  EXPECT_FALSE(result.ok());
  EXPECT_NE(result.error.find("was refused"), std::string::npos);
  EXPECT_EQ(authorization_checks, 1u);
  EXPECT_EQ(result.images_read, 0u);
  const auto run = node.find_run(result.run_id);
  ASSERT_TRUE(run);
  EXPECT_EQ(run->status(), zetta::pipeline::RunStatus::Failed);
  node.stop();
}

TEST(ViT, RealCheckpointLoadsWhenExplicitlyConfigured) {
  const char *checkpoint = std::getenv("ZETTA_VIT_CHECKPOINT");
  const char *config_path = std::getenv("ZETTA_VIT_CONFIG");
  if (checkpoint == nullptr || config_path == nullptr) {
    GTEST_SKIP() << "set ZETTA_VIT_CHECKPOINT and ZETTA_VIT_CONFIG";
  }
  std::ifstream config_file(config_path);
  const nlohmann::json config_json =
      nlohmann::json::parse(config_file, nullptr, false);
  std::string error;
  const std::optional<zetta::ml::VitConfig> config =
      zetta::ml::vit_config_from_json(config_json, &error);
  ASSERT_TRUE(config) << error;
  const std::optional<zetta::autograd::SafeTensors> tensors =
      zetta::autograd::read_safetensors_file(checkpoint, &error);
  ASSERT_TRUE(tensors) << error;
  const std::string serialized =
      zetta::autograd::serialize_safetensors(*tensors);
  EXPECT_TRUE(zetta::ml::load_vit_backbone(*config, serialized, &error))
      << error;
}

// #1418: a second frame after the scan would clear the decoded planes while the scan stays
// complete; the decoder refuses the image instead of converting empty planes.
TEST(ImageDecode, ASecondJpegFrameIsRefused) {
  std::string bytes = constant_gray_jpeg();
  bytes.resize(bytes.size() - 2); // drop the EOI
  append_marker(bytes, 0xC0, {8, 0, 16, 0, 16, 1, 1, 0x11, 0});
  bytes.append("\xFF\xD9", 2);
  const zetta::ml::ImageDecodeResult decoded = zetta::ml::decode_image(bytes);
  EXPECT_FALSE(decoded.ok());
  EXPECT_NE(decoded.error.find("more than one frame"), std::string::npos) << decoded.error;
}
