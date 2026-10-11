#include "zetta/pipeline/graph_checks.hpp"

#include "zetta/ml/forecast_methodology.hpp"

namespace zetta::pipeline {

std::string ForecastGraphCheck::name() const {
  return "forecast_correctness";
}

CheckResult
ForecastGraphCheck::evaluate(const ir::ExecutionGraph &graph) const {
  if (!ml::owns_forecast(graph)) {
    return {name(), CheckStatus::NotApplicable, "no forecast model"};
  }
  std::string error;
  if (!ml::parse_forecast_pipeline(graph, error)) {
    return {name(), CheckStatus::Fail, error};
  }
  return {name(), CheckStatus::Pass,
          "temporal folds, past-only features, no calibration, seasonal-naive "
          "baseline; "
          "actual history limits are checked on every series at run time"};
}

} // namespace zetta::pipeline
