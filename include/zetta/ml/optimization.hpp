#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/data/dataset_path.hpp"

namespace zetta {
class Node;
}

namespace zetta::pipeline {
class Run;
}

// Native linear and mixed-integer optimization for M-16 (#1055).
//
// The LP primitive is a two-phase revised simplex implementation. It keeps an
// explicit basis, refactorizes that basis with partial pivoting, and uses
// Bland's rule for deterministic anti-cycling. The MIP primitive places LP
// relaxations in a best-bound branch-and-bound queue. No external solver or
// foreign runtime participates.
//
// See docs/ml/optimization.md.
namespace zetta::ml {

enum class OptimizationSense {
  LessEqual,
  Equal,
  GreaterEqual,
};

enum class OptimizationStatus {
  Optimal,
  Feasible,
  Infeasible,
  Unbounded,
  NodeLimit,
  TimeLimit,
  IterationLimit,
  NumericalFailure,
  InvalidModel,
};

std::string_view to_string(OptimizationStatus status);

struct OptimizationVariable {
  std::string name;
  double objective = 0.0;
  double lower_bound = 0.0;
  double upper_bound = std::numeric_limits<double>::infinity();
  bool integer = false;
  double current_value = 0.0;
};

struct OptimizationConstraint {
  std::string name;
  std::vector<double> coefficients;
  OptimizationSense sense = OptimizationSense::LessEqual;
  double right_hand_side = 0.0;
};

struct OptimizationProblem {
  std::vector<OptimizationVariable> variables;
  std::vector<OptimizationConstraint> constraints;
  bool maximize = true;
};

struct OptimizationParams {
  bool maximize = true;
  std::size_t iteration_limit = 10'000;
  std::size_t node_limit = 10'000;
  std::chrono::milliseconds time_limit{30'000};
  double feasibility_tolerance = 1e-8;
  double integrality_tolerance = 1e-8;
  std::uint64_t seed = 0;
};

struct ConstraintCheck {
  std::string constraint;
  double activity = 0.0;
  double right_hand_side = 0.0;
  double violation = 0.0;
  bool satisfied = false;
};

struct OptimizationResult {
  OptimizationStatus status = OptimizationStatus::InvalidModel;
  std::vector<double> values;
  std::vector<ConstraintCheck> constraint_checks;
  double objective = 0.0;
  double current_plan_objective = 0.0;
  double optimality_gap = 0.0;
  std::size_t iterations = 0;
  std::size_t explored_nodes = 0;
  std::string error;

  bool has_plan() const noexcept { return !values.empty(); }
  bool feasible() const noexcept;
};

// Optional cross-member reduction for independently-computed verification
// statistics. The vector is reduced elementwise using the named operation.
// The identity callback is used by the single-member runner. Multi-member
// callers use the cluster collective, matching the GBDT AllReduce injection.
enum class OptimizationReduce {
  Sum,
  Max,
};
using OptimizationAllReduce = std::function<std::vector<double>(
    const std::string &tag, const std::vector<double> &local,
    OptimizationReduce operation)>;

OptimizationResult solve_optimization(const OptimizationProblem &problem,
                                      const OptimizationParams &params = {});

// Independently check bounds, integrality, and every original constraint.
// This never trusts the simplex tableau or branch-and-bound bookkeeping.
std::vector<ConstraintCheck>
verify_optimization_plan(const OptimizationProblem &problem,
                         const std::vector<double> &values,
                         double tolerance = 1e-8);

// Decode the row-oriented table contract documented in optimization.md.
// Every row must carry a scenario or quantile marker; a point forecast is
// rejected at this boundary. On failure returns nullopt and fills `error`.
std::optional<OptimizationProblem>
optimization_problem_from_rows(const std::vector<nlohmann::json> &rows,
                               bool maximize, std::string *error = nullptr);

struct OptimizationRunOptions {
  std::string project;
  std::string graph;
  std::string model_name = "optimization-plan";
  std::string output_database = "optimization_results";
  OptimizationAllReduce allreduce;
};

struct OptimizationRunResult {
  std::shared_ptr<pipeline::Run> run;
  OptimizationResult solution;
  std::string plan_table;
  std::string constraint_table;
  std::string model_id;
  std::string error;

  bool ok() const noexcept;
};

// Phase-A runner contract used by the methodology adapter: a resolved live
// table plus parameters in, durable plan/constraint tables and lineage out.
// `sampler` must read the complete resolved table; the registry adapter passes
// the cluster-aware dataset sampler so partition-owned rows are gathered from
// their owners before the coordinator performs basis/branch decisions.
OptimizationRunResult
run_optimization(Node &node, const data::ResolvedDataset &dataset,
                 const data::DatasetSampler &sampler,
                 const OptimizationParams &params,
                 const OptimizationRunOptions &options = {});

} // namespace zetta::ml
