#pragma once

#include <optional>
#include <string>

#include "zetta/ml/anomaly_table.hpp"
#include "zetta/ml/methodology.hpp"

namespace zetta::compute {
class CoordinatedLaunchListener;
}

namespace zetta::ml {

class Model;

// M-7 registry adapter and validated plan, #1046. See docs/ml/anomaly.md.
struct AnomalyPlan {
  std::string database;
  std::string table;
  data::DatasetContract contract;
  AnomalyParams params;
};

bool is_anomaly_kind(std::string_view kind);
std::optional<AnomalyPlan> parse_anomaly_plan(const ir::ExecutionGraph &graph,
                                              std::string &error);
Methodology make_anomaly_methodology();
void install_anomaly_launch_runner(compute::CoordinatedLaunchListener &listener,
                                   Node &node);
nlohmann::json anomaly_run_report(Node &node, const pipeline::Run &run,
                                  const Model &model, std::size_t limit);

} // namespace zetta::ml
