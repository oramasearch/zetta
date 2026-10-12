#include "zetta/pipeline/graph_checks.hpp"

#include <utility>

#include "zetta/ml/next_best_action_methodology.hpp"

namespace zetta::pipeline {

CheckResult
NextBestActionGraphCheck::evaluate(const ir::ExecutionGraph &graph) const {
  if (!ml::owns_next_best_action(graph)) {
    return {name(), CheckStatus::NotApplicable, "no next-best-action model"};
  }
  std::string error;
  if (!ml::parse_next_best_action_pipeline(graph, error)) {
    return {name(), CheckStatus::Fail, std::move(error)};
  }
  return {name(), CheckStatus::Pass,
          "chronological holdout and logging-propensity refusal are declared; "
          "eligibility and contact caps are hard constraints before budget"};
}

} // namespace zetta::pipeline
