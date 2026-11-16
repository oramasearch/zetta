// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>

#include "zetta/ml/methodology.hpp"
#include "zetta/ml/next_best_action_table.hpp"

namespace zetta::compute {
class CoordinatedLaunchListener;
}

namespace zetta::ml {

// Validated M-8 graph contract. The gate, initiating run and peer launch all
// parse this same value so none can ignore a changed policy or column role.
struct NextBestActionPipeline {
  std::string database;
  std::string table;
  data::DatasetContract contract;
  NextBestActionTableParams params;
};

const nlohmann::json &next_best_action_step_schema();
const nlohmann::json &next_best_action_launch_schema();
bool owns_next_best_action(const ir::ExecutionGraph &graph);
std::optional<NextBestActionPipeline>
parse_next_best_action_pipeline(const ir::ExecutionGraph &graph,
                                std::string &error);
Methodology make_m8_methodology();
api::ApiResult run_next_best_action_methodology(RunRequest &request);
void install_next_best_action_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node);

} // namespace zetta::ml
