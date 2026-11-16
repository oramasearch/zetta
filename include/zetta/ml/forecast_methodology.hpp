// SPDX-License-Identifier: MIT

#pragma once

#include <optional>
#include <string>

#include "zetta/ml/forecast_runner.hpp"
#include "zetta/ml/methodology.hpp"

namespace zetta::compute {
class CoordinatedLaunchListener;
}

namespace zetta::ml {

// The validated table-backed M-4 pipeline. Shared by the gate, initiator and
// peer launcher so none can silently ignore a changed graph. See forecast.md.
struct ForecastPipeline {
  std::string database;
  std::string table;
  ForecastRunParams params;
};

const nlohmann::json &forecast_step_schema();
const nlohmann::json &forecast_launch_schema();
bool owns_forecast(const ir::ExecutionGraph &graph);
std::optional<ForecastPipeline>
parse_forecast_pipeline(const ir::ExecutionGraph &graph, std::string &error);
Methodology make_m4_methodology();
api::ApiResult run_forecast_methodology(RunRequest &request);
void install_forecast_launch_runner(
    compute::CoordinatedLaunchListener &listener, Node &node);

} // namespace zetta::ml
