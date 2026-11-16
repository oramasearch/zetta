// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string>

#include <nlohmann/json.hpp>

#include "zetta/ml/segmentation.hpp"

namespace zetta::compute {
class CoordinatedLaunchListener;
}
namespace zetta::pipeline {
class Run;
}

namespace zetta::ml {
class Model;

// The graph's segmentation contract and the peer-launch payload are validated
// before typed access. See docs/ml/segmentation.md.
const nlohmann::json &segmentation_config_schema();
const nlohmann::json &segmentation_launch_schema();
bool validate_segmentation_config(const nlohmann::json &config,
                                  std::string &error);
SegmentationParams segmentation_params(const nlohmann::json &config);
bool is_segmentation_kind(const std::string &kind);

void install_segmentation_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node);

nlohmann::json segmentation_run_report(Node &node, const pipeline::Run &run,
                                       const Model &model, std::size_t limit);
} // namespace zetta::ml
