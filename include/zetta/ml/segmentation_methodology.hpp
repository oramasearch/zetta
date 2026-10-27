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
namespace zetta::cluster {
class Cluster;
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

// A landed table resolved the way M-3 training resolves it, with no label:
// `config` names `entity_column` (else it is inferred by name),
// `feature_columns` and `categorical_columns`; a text entity column gets a
// derived key mapping. `score` and `explain` on a segmentation model read a
// table through it (#1497); the supervised resolver would demand a label.
zetta::data::ResolvedDataset
resolve_segmentation_table(Node &node, const zetta::data::DatasetPath &path,
                           nlohmann::json config,
                           const zetta::cluster::Cluster *cluster);
} // namespace zetta::ml
