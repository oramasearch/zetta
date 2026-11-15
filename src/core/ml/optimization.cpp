// SPDX-License-Identifier: MIT

#include "zetta/ml/optimization.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <map>
#include <numeric>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>

#include "zetta/data/database.hpp"
#include "zetta/data/table.hpp"
#include "zetta/ml/model.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::ml {
namespace {

using Matrix = std::vector<std::vector<double>>;

struct StandardRow {
  std::vector<double> coefficients;
  OptimizationSense sense = OptimizationSense::LessEqual;
  double right_hand_side = 0.0;
};

struct StandardProblem {
  Matrix matrix;
  std::vector<double> right_hand_side;
  std::vector<double> objective;
  std::vector<std::size_t> basis;
  std::vector<bool> artificial;
  std::vector<double> lower_bounds;
  std::size_t original_variable_count = 0;
  double objective_offset = 0.0;
};

struct RevisedSimplexResult {
  OptimizationStatus status = OptimizationStatus::InvalidModel;
  std::vector<double> values;
  double objective = 0.0;
  std::size_t iterations = 0;
  std::string error;
};

struct RelaxationResult {
  OptimizationStatus status = OptimizationStatus::InvalidModel;
  std::vector<double> values;
  double objective = 0.0;
  std::size_t iterations = 0;
  std::string error;
};

struct BranchNode {
  std::vector<double> lower_bounds;
  std::vector<double> upper_bounds;
  double bound = std::numeric_limits<double>::infinity();
  std::uint64_t sequence = 0;
};

struct LowerBranchPriority {
  bool operator()(const BranchNode &left, const BranchNode &right) const {
    if (left.bound != right.bound) {
      return left.bound < right.bound;
    }
    return left.sequence > right.sequence;
  }
};

using BranchQueue = std::priority_queue<BranchNode, std::vector<BranchNode>,
                                        LowerBranchPriority>;

bool finite(double value) { return std::isfinite(value); }

double dot(const std::vector<double> &left, const std::vector<double> &right) {
  double result = 0.0;
  for (std::size_t index = 0; index < left.size(); ++index) {
    result += left[index] * right[index];
  }
  return result;
}

std::vector<double> multiply(const Matrix &matrix,
                             const std::vector<double> &vector) {
  std::vector<double> result(matrix.size(), 0.0);
  for (std::size_t row = 0; row < matrix.size(); ++row) {
    result[row] = dot(matrix[row], vector);
  }
  return result;
}

std::optional<Matrix> inverse(Matrix matrix, double tolerance) {
  const std::size_t size = matrix.size();
  Matrix augmented(size, std::vector<double>(size * 2, 0.0));
  for (std::size_t row = 0; row < size; ++row) {
    if (matrix[row].size() != size) {
      return std::nullopt;
    }
    for (std::size_t column = 0; column < size; ++column) {
      augmented[row][column] = matrix[row][column];
    }
    augmented[row][size + row] = 1.0;
  }

  for (std::size_t pivot = 0; pivot < size; ++pivot) {
    std::size_t best = pivot;
    for (std::size_t row = pivot + 1; row < size; ++row) {
      if (std::abs(augmented[row][pivot]) > std::abs(augmented[best][pivot])) {
        best = row;
      }
    }
    if (std::abs(augmented[best][pivot]) <= tolerance) {
      return std::nullopt;
    }
    if (best != pivot) {
      std::swap(augmented[best], augmented[pivot]);
    }
    const double divisor = augmented[pivot][pivot];
    for (double &value : augmented[pivot]) {
      value /= divisor;
    }
    for (std::size_t row = 0; row < size; ++row) {
      if (row == pivot) {
        continue;
      }
      const double factor = augmented[row][pivot];
      if (std::abs(factor) <= tolerance) {
        continue;
      }
      for (std::size_t column = 0; column < size * 2; ++column) {
        augmented[row][column] -= factor * augmented[pivot][column];
      }
    }
  }

  Matrix result(size, std::vector<double>(size, 0.0));
  for (std::size_t row = 0; row < size; ++row) {
    for (std::size_t column = 0; column < size; ++column) {
      result[row][column] = augmented[row][size + column];
    }
  }
  return result;
}

Matrix basis_matrix(const Matrix &matrix,
                    const std::vector<std::size_t> &basis) {
  Matrix result(matrix.size(), std::vector<double>(basis.size(), 0.0));
  for (std::size_t row = 0; row < matrix.size(); ++row) {
    for (std::size_t column = 0; column < basis.size(); ++column) {
      result[row][column] = matrix[row][basis[column]];
    }
  }
  return result;
}

std::vector<double> matrix_column(const Matrix &matrix, std::size_t column) {
  std::vector<double> result(matrix.size(), 0.0);
  for (std::size_t row = 0; row < matrix.size(); ++row) {
    result[row] = matrix[row][column];
  }
  return result;
}

bool valid_problem(const OptimizationProblem &problem, std::string &error) {
  if (problem.variables.empty()) {
    error = "optimization: the model has no variables";
    return false;
  }
  for (const OptimizationVariable &variable : problem.variables) {
    if (variable.name.empty()) {
      error = "optimization: a variable has no name";
      return false;
    }
    if (!finite(variable.objective) || !finite(variable.lower_bound) ||
        std::isnan(variable.upper_bound) || !finite(variable.current_value)) {
      error = "optimization: variable '" + variable.name +
              "' has a non-finite value";
      return false;
    }
    if (variable.upper_bound < variable.lower_bound) {
      error = "optimization: variable '" + variable.name +
              "' has upper_bound below lower_bound";
      return false;
    }
  }
  for (const OptimizationConstraint &constraint : problem.constraints) {
    if (constraint.name.empty()) {
      error = "optimization: a constraint has no name";
      return false;
    }
    if (constraint.coefficients.size() != problem.variables.size()) {
      error = "optimization: constraint '" + constraint.name +
              "' has the wrong coefficient count";
      return false;
    }
    if (!finite(constraint.right_hand_side) ||
        std::ranges::any_of(constraint.coefficients,
                            [](double value) { return !finite(value); })) {
      error = "optimization: constraint '" + constraint.name +
              "' has a non-finite value";
      return false;
    }
  }
  return true;
}

OptimizationSense flipped(OptimizationSense sense) {
  if (sense == OptimizationSense::LessEqual) {
    return OptimizationSense::GreaterEqual;
  }
  if (sense == OptimizationSense::GreaterEqual) {
    return OptimizationSense::LessEqual;
  }
  return OptimizationSense::Equal;
}

StandardProblem standardize(const OptimizationProblem &problem,
                            const std::vector<double> &lower_bounds,
                            const std::vector<double> &upper_bounds,
                            double tolerance) {
  StandardProblem standard;
  standard.original_variable_count = problem.variables.size();
  standard.lower_bounds = lower_bounds;
  standard.objective.resize(problem.variables.size(), 0.0);
  for (std::size_t index = 0; index < problem.variables.size(); ++index) {
    const double coefficient = problem.variables[index].objective;
    standard.objective[index] = problem.maximize ? coefficient : -coefficient;
    standard.objective_offset +=
        standard.objective[index] * lower_bounds[index];
  }

  std::vector<StandardRow> rows;
  rows.reserve(problem.constraints.size() + problem.variables.size());
  for (const OptimizationConstraint &constraint : problem.constraints) {
    StandardRow row;
    row.coefficients = constraint.coefficients;
    row.sense = constraint.sense;
    row.right_hand_side = constraint.right_hand_side;
    for (std::size_t index = 0; index < lower_bounds.size(); ++index) {
      row.right_hand_side -= row.coefficients[index] * lower_bounds[index];
    }
    if (row.right_hand_side < -tolerance) {
      row.right_hand_side = -row.right_hand_side;
      row.sense = flipped(row.sense);
      for (double &coefficient : row.coefficients) {
        coefficient = -coefficient;
      }
    }
    if (std::abs(row.right_hand_side) <= tolerance) {
      row.right_hand_side = 0.0;
    }
    rows.push_back(std::move(row));
  }
  for (std::size_t index = 0; index < upper_bounds.size(); ++index) {
    if (!finite(upper_bounds[index])) {
      continue;
    }
    StandardRow row;
    row.coefficients.assign(problem.variables.size(), 0.0);
    row.coefficients[index] = 1.0;
    row.right_hand_side = upper_bounds[index] - lower_bounds[index];
    row.sense = OptimizationSense::LessEqual;
    rows.push_back(std::move(row));
  }

  std::size_t columns = problem.variables.size();
  for (const StandardRow &row : rows) {
    columns += row.sense == OptimizationSense::Equal ? 1 : 1;
    if (row.sense == OptimizationSense::GreaterEqual) {
      ++columns;
    }
  }
  standard.matrix.assign(rows.size(), std::vector<double>(columns, 0.0));
  standard.right_hand_side.resize(rows.size(), 0.0);
  standard.objective.resize(columns, 0.0);
  standard.artificial.assign(columns, false);
  standard.basis.resize(rows.size(), 0);

  std::size_t next_column = problem.variables.size();
  for (std::size_t row_index = 0; row_index < rows.size(); ++row_index) {
    const StandardRow &row = rows[row_index];
    std::copy(row.coefficients.begin(), row.coefficients.end(),
              standard.matrix[row_index].begin());
    standard.right_hand_side[row_index] = row.right_hand_side;
    if (row.sense == OptimizationSense::LessEqual) {
      standard.matrix[row_index][next_column] = 1.0;
      standard.basis[row_index] = next_column;
      ++next_column;
      continue;
    }
    if (row.sense == OptimizationSense::GreaterEqual) {
      standard.matrix[row_index][next_column] = -1.0;
      ++next_column;
    }
    standard.matrix[row_index][next_column] = 1.0;
    standard.artificial[next_column] = true;
    standard.basis[row_index] = next_column;
    ++next_column;
  }
  return standard;
}

RevisedSimplexResult revised_simplex(const Matrix &matrix,
                                     const std::vector<double> &right_hand_side,
                                     const std::vector<double> &objective,
                                     std::vector<std::size_t> &basis,
                                     const std::vector<bool> &excluded,
                                     const OptimizationParams &params,
                                     std::size_t remaining_iterations) {
  RevisedSimplexResult result;
  if (matrix.empty()) {
    result.status = OptimizationStatus::Optimal;
    result.values.assign(objective.size(), 0.0);
    result.objective = 0.0;
    return result;
  }

  const std::size_t row_count = matrix.size();
  const std::size_t column_count = matrix.front().size();
  for (std::size_t iteration = 0; iteration < remaining_iterations;
       ++iteration) {
    const std::optional<Matrix> basis_inverse = inverse(
        basis_matrix(matrix, basis), params.feasibility_tolerance * 0.01);
    if (!basis_inverse) {
      result.status = OptimizationStatus::NumericalFailure;
      result.error = "optimization: basis refactorization is singular";
      result.iterations = iteration;
      return result;
    }
    std::vector<double> basic_values =
        multiply(*basis_inverse, right_hand_side);
    for (double &value : basic_values) {
      if (std::abs(value) <= params.feasibility_tolerance) {
        value = 0.0;
      }
      if (value < -params.feasibility_tolerance) {
        result.status = OptimizationStatus::NumericalFailure;
        result.error =
            "optimization: refactorized basis lost primal feasibility";
        result.iterations = iteration;
        return result;
      }
    }

    std::vector<double> basic_cost(row_count, 0.0);
    for (std::size_t row = 0; row < row_count; ++row) {
      basic_cost[row] = objective[basis[row]];
    }
    std::vector<double> dual(column_count, 0.0);
    for (std::size_t column = 0; column < column_count; ++column) {
      for (std::size_t row = 0; row < row_count; ++row) {
        for (std::size_t inner = 0; inner < row_count; ++inner) {
          dual[column] += basic_cost[row] * (*basis_inverse)[row][inner] *
                          matrix[inner][column];
        }
      }
    }

    std::set<std::size_t> basic_set(basis.begin(), basis.end());
    std::optional<std::size_t> entering;
    for (std::size_t column = 0; column < column_count; ++column) {
      if (basic_set.contains(column) || excluded[column]) {
        continue;
      }
      const double reduced_cost = objective[column] - dual[column];
      if (reduced_cost > params.feasibility_tolerance) {
        entering = column;
        break;
      }
    }
    if (!entering) {
      result.status = OptimizationStatus::Optimal;
      result.values.assign(column_count, 0.0);
      for (std::size_t row = 0; row < row_count; ++row) {
        result.values[basis[row]] = basic_values[row];
      }
      result.objective = dot(objective, result.values);
      result.iterations = iteration;
      return result;
    }

    const std::vector<double> direction =
        multiply(*basis_inverse, matrix_column(matrix, *entering));
    std::optional<std::size_t> leaving_row;
    double best_ratio = std::numeric_limits<double>::infinity();
    for (std::size_t row = 0; row < row_count; ++row) {
      if (direction[row] <= params.feasibility_tolerance) {
        continue;
      }
      const double ratio = basic_values[row] / direction[row];
      if (ratio < best_ratio - params.feasibility_tolerance ||
          (std::abs(ratio - best_ratio) <= params.feasibility_tolerance &&
           (!leaving_row || basis[row] < basis[*leaving_row]))) {
        best_ratio = ratio;
        leaving_row = row;
      }
    }
    if (!leaving_row) {
      result.status = OptimizationStatus::Unbounded;
      result.iterations = iteration + 1;
      return result;
    }
    basis[*leaving_row] = *entering;
  }

  result.status = OptimizationStatus::IterationLimit;
  result.iterations = remaining_iterations;
  result.error = "optimization: revised simplex iteration limit reached";
  return result;
}

bool checks_pass(const OptimizationProblem &problem,
                 const std::vector<double> &values, double tolerance,
                 bool check_integrality,
                 std::vector<ConstraintCheck> *checks = nullptr) {
  if (values.size() != problem.variables.size()) {
    return false;
  }
  bool feasible = true;
  for (std::size_t index = 0; index < values.size(); ++index) {
    const OptimizationVariable &variable = problem.variables[index];
    if (values[index] < variable.lower_bound - tolerance ||
        values[index] > variable.upper_bound + tolerance) {
      feasible = false;
    }
    if (check_integrality && variable.integer &&
        std::abs(values[index] - std::round(values[index])) > tolerance) {
      feasible = false;
    }
  }
  std::vector<ConstraintCheck> local_checks;
  local_checks.reserve(problem.constraints.size());
  for (const OptimizationConstraint &constraint : problem.constraints) {
    ConstraintCheck check;
    check.constraint = constraint.name;
    check.activity = dot(constraint.coefficients, values);
    check.right_hand_side = constraint.right_hand_side;
    if (constraint.sense == OptimizationSense::LessEqual) {
      check.violation = std::max(0.0, check.activity - check.right_hand_side);
    } else if (constraint.sense == OptimizationSense::GreaterEqual) {
      check.violation = std::max(0.0, check.right_hand_side - check.activity);
    } else {
      check.violation = std::abs(check.activity - check.right_hand_side);
    }
    check.satisfied = check.violation <= tolerance;
    feasible = feasible && check.satisfied;
    local_checks.push_back(std::move(check));
  }
  if (checks != nullptr) {
    *checks = std::move(local_checks);
  }
  return feasible;
}

RelaxationResult solve_relaxation(const OptimizationProblem &problem,
                                  const std::vector<double> &lower_bounds,
                                  const std::vector<double> &upper_bounds,
                                  const OptimizationParams &params) {
  RelaxationResult result;
  const StandardProblem standard = standardize(
      problem, lower_bounds, upper_bounds, params.feasibility_tolerance);
  if (standard.matrix.empty()) {
    result.values = lower_bounds;
    for (std::size_t index = 0; index < standard.objective.size(); ++index) {
      if (standard.objective[index] > params.feasibility_tolerance) {
        result.status = OptimizationStatus::Unbounded;
        result.values.clear();
        return result;
      }
    }
    result.status = OptimizationStatus::Optimal;
    result.objective = 0.0;
    for (std::size_t index = 0; index < problem.variables.size(); ++index) {
      result.objective +=
          problem.variables[index].objective * result.values[index];
    }
    return result;
  }
  std::vector<std::size_t> basis = standard.basis;
  std::vector<double> phase_one_objective(standard.objective.size(), 0.0);
  std::vector<bool> include_everything(standard.objective.size(), false);
  for (std::size_t column = 0; column < standard.artificial.size(); ++column) {
    if (standard.artificial[column]) {
      phase_one_objective[column] = -1.0;
    }
  }
  const RevisedSimplexResult phase_one = revised_simplex(
      standard.matrix, standard.right_hand_side, phase_one_objective, basis,
      include_everything, params, params.iteration_limit);
  result.iterations += phase_one.iterations;
  if (phase_one.status != OptimizationStatus::Optimal) {
    result.status = phase_one.status;
    result.error = phase_one.error;
    return result;
  }
  if (phase_one.objective < -params.feasibility_tolerance) {
    result.status = OptimizationStatus::Infeasible;
    return result;
  }

  const std::size_t remaining = params.iteration_limit > result.iterations
                                    ? params.iteration_limit - result.iterations
                                    : 0;
  const RevisedSimplexResult phase_two = revised_simplex(
      standard.matrix, standard.right_hand_side, standard.objective, basis,
      standard.artificial, params, remaining);
  result.iterations += phase_two.iterations;
  result.status = phase_two.status;
  result.error = phase_two.error;
  if (phase_two.status != OptimizationStatus::Optimal) {
    return result;
  }
  result.values.resize(problem.variables.size(), 0.0);
  for (std::size_t index = 0; index < problem.variables.size(); ++index) {
    result.values[index] =
        standard.lower_bounds[index] + phase_two.values[index];
  }
  if (!checks_pass(problem, result.values, params.feasibility_tolerance,
                   false)) {
    result.status = OptimizationStatus::NumericalFailure;
    result.values.clear();
    result.error =
        "optimization: independent verification rejected the LP plan";
    return result;
  }
  result.objective = 0.0;
  for (std::size_t index = 0; index < problem.variables.size(); ++index) {
    result.objective +=
        problem.variables[index].objective * result.values[index];
  }
  return result;
}

double normalized_objective(const OptimizationProblem &problem,
                            double objective) {
  return problem.maximize ? objective : -objective;
}

double relative_gap(double incumbent, double bound) {
  return std::max(0.0, bound - incumbent) / std::max(1.0, std::abs(incumbent));
}

std::pair<std::vector<double>, std::vector<double>>
variable_bounds(const OptimizationProblem &problem) {
  std::vector<double> lower_bounds;
  std::vector<double> upper_bounds;
  lower_bounds.reserve(problem.variables.size());
  upper_bounds.reserve(problem.variables.size());
  for (const OptimizationVariable &variable : problem.variables) {
    lower_bounds.push_back(variable.lower_bound);
    upper_bounds.push_back(variable.upper_bound);
  }
  return {std::move(lower_bounds), std::move(upper_bounds)};
}

OptimizationResult
solve_linear_problem(const OptimizationProblem &problem,
                     const OptimizationParams &params,
                     const std::vector<double> &lower_bounds,
                     const std::vector<double> &upper_bounds) {
  const RelaxationResult relaxation =
      solve_relaxation(problem, lower_bounds, upper_bounds, params);
  OptimizationResult result;
  result.status = relaxation.status;
  result.values = relaxation.values;
  result.objective = relaxation.objective;
  result.iterations = relaxation.iterations;
  result.explored_nodes = 1;
  result.error = relaxation.error;
  if (result.has_plan()) {
    result.constraint_checks = verify_optimization_plan(
        problem, result.values, params.feasibility_tolerance);
  }
  return result;
}

std::optional<std::size_t>
first_fractional_variable(const OptimizationProblem &problem,
                          const std::vector<double> &values, double tolerance) {
  for (std::size_t index = 0; index < problem.variables.size(); ++index) {
    if (!problem.variables[index].integer) {
      continue;
    }
    if (std::abs(values[index] - std::round(values[index])) > tolerance) {
      return index;
    }
  }
  return std::nullopt;
}

void enqueue_branches(BranchQueue &queue, const BranchNode &parent,
                      std::size_t variable, double value, double bound,
                      double tolerance, std::uint64_t &sequence) {
  BranchNode down = parent;
  down.upper_bounds[variable] =
      std::min(down.upper_bounds[variable], std::floor(value));
  down.bound = bound;
  down.sequence = sequence++;
  if (down.lower_bounds[variable] <= down.upper_bounds[variable] + tolerance) {
    queue.push(std::move(down));
  }

  BranchNode up = parent;
  up.lower_bounds[variable] =
      std::max(up.lower_bounds[variable], std::ceil(value));
  up.bound = bound;
  up.sequence = sequence++;
  if (up.lower_bounds[variable] <= up.upper_bounds[variable] + tolerance) {
    queue.push(std::move(up));
  }
}

void verify_mip_plan(const OptimizationProblem &problem,
                     const OptimizationParams &params,
                     OptimizationResult &result) {
  result.constraint_checks = verify_optimization_plan(
      problem, result.values, params.feasibility_tolerance);
  if (checks_pass(problem, result.values, params.feasibility_tolerance, true)) {
    return;
  }
  result.status = OptimizationStatus::NumericalFailure;
  result.values.clear();
  result.constraint_checks.clear();
  result.error = "optimization: independent verification rejected the MIP plan";
}

OptimizationResult solve_mixed_integer_problem(
    const OptimizationProblem &problem, const OptimizationParams &params,
    std::vector<double> lower_bounds, std::vector<double> upper_bounds) {
  OptimizationResult result;
  BranchQueue queue;
  queue.push(BranchNode{std::move(lower_bounds), std::move(upper_bounds),
                        std::numeric_limits<double>::infinity(), 0});
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t sequence = 1;
  std::optional<double> incumbent_normalized;
  std::vector<double> incumbent_values;
  double incumbent_objective = 0.0;
  OptimizationStatus limit_status = OptimizationStatus::Optimal;

  while (!queue.empty()) {
    if (result.explored_nodes >= params.node_limit) {
      limit_status = OptimizationStatus::NodeLimit;
      break;
    }
    if (std::chrono::steady_clock::now() - started >= params.time_limit) {
      limit_status = OptimizationStatus::TimeLimit;
      break;
    }
    BranchNode node = queue.top();
    queue.pop();
    const RelaxationResult relaxation =
        solve_relaxation(problem, node.lower_bounds, node.upper_bounds, params);
    ++result.explored_nodes;
    result.iterations += relaxation.iterations;
    if (relaxation.status == OptimizationStatus::Infeasible) {
      continue;
    }
    if (relaxation.status == OptimizationStatus::Unbounded) {
      if (result.explored_nodes == 1) {
        result.status = OptimizationStatus::Unbounded;
        return result;
      }
      continue;
    }
    if (relaxation.status != OptimizationStatus::Optimal) {
      result.status = relaxation.status;
      result.error = relaxation.error;
      return result;
    }

    const double bound = normalized_objective(problem, relaxation.objective);
    if (incumbent_normalized &&
        bound <= *incumbent_normalized + params.feasibility_tolerance) {
      continue;
    }
    const std::optional<std::size_t> branch_variable =
        first_fractional_variable(problem, relaxation.values,
                                  params.integrality_tolerance);
    if (!branch_variable) {
      incumbent_normalized = bound;
      incumbent_objective = relaxation.objective;
      incumbent_values = relaxation.values;
      continue;
    }
    enqueue_branches(queue, node, *branch_variable,
                     relaxation.values[*branch_variable], bound,
                     params.feasibility_tolerance, sequence);
  }

  if (!incumbent_normalized) {
    result.status = limit_status == OptimizationStatus::Optimal
                        ? OptimizationStatus::Infeasible
                        : limit_status;
    result.optimality_gap = 1.0;
    return result;
  }
  result.values = std::move(incumbent_values);
  result.objective = incumbent_objective;
  result.status = limit_status;
  const double best_bound =
      queue.empty() ? *incumbent_normalized : queue.top().bound;
  result.optimality_gap = result.status == OptimizationStatus::Optimal
                              ? 0.0
                              : relative_gap(*incumbent_normalized, best_bound);
  verify_mip_plan(problem, params, result);
  return result;
}

std::optional<double> number_field(const nlohmann::json &row,
                                   std::string_view field) {
  const auto found = row.find(field);
  if (found == row.end()) {
    return std::nullopt;
  }
  if (!found->is_number()) {
    return std::nullopt;
  }
  const double value = found->get<double>();
  if (!finite(value)) {
    return std::nullopt;
  }
  return value;
}

std::optional<std::string> string_field(const nlohmann::json &row,
                                        std::string_view field) {
  const auto found = row.find(field);
  if (found == row.end() || !found->is_string()) {
    return std::nullopt;
  }
  return found->get<std::string>();
}

std::optional<OptimizationSense> parse_sense(std::string_view value) {
  if (value == "<=" || value == "less_equal") {
    return OptimizationSense::LessEqual;
  }
  if (value == "=" || value == "==" || value == "equal") {
    return OptimizationSense::Equal;
  }
  if (value == ">=" || value == "greater_equal") {
    return OptimizationSense::GreaterEqual;
  }
  return std::nullopt;
}

struct VariableRecord {
  OptimizationVariable variable;
  bool objective_seen = false;
  bool lower_seen = false;
  bool upper_seen = false;
  bool integer_seen = false;
  bool current_seen = false;
};

struct ConstraintRecord {
  OptimizationSense sense = OptimizationSense::LessEqual;
  double right_hand_side = 0.0;
  bool metadata_seen = false;
  std::map<std::string, double> coefficients;
};

class OptimizationTableParser {
public:
  OptimizationTableParser(const std::vector<nlohmann::json> &rows,
                          bool maximize, std::string *error)
      : rows_(rows), maximize_(maximize), error_(error) {}

  std::optional<OptimizationProblem> parse() {
    if (rows_.empty()) {
      return reject("optimization dataset: the table has no rows");
    }
    for (std::size_t index = 0; index < rows_.size(); ++index) {
      if (!parse_row(rows_[index], index)) {
        return std::nullopt;
      }
    }
    for (const std::string &name : variable_order_) {
      const VariableRecord &record = variables_[name];
      if (!record.objective_seen || !record.integer_seen) {
        return reject("optimization dataset: variable '" + name +
                      "' needs objective and integer metadata");
      }
    }
    return build_problem();
  }

private:
  std::optional<OptimizationProblem> reject(std::string reason) {
    fail(std::move(reason));
    return std::nullopt;
  }

  bool fail(std::string reason) {
    if (error_ != nullptr) {
      *error_ = std::move(reason);
    }
    return false;
  }

  bool validate_row_id(const nlohmann::json &row, std::size_t index) {
    const auto found = row.find("row_id");
    if (found == row.end() || !found->is_number_integer()) {
      return fail("optimization dataset: row " + std::to_string(index) +
                  " needs an integer row_id");
    }
    const std::int64_t id = found->get<std::int64_t>();
    if (!row_ids_.insert(id).second) {
      return fail("optimization dataset: duplicate row_id " +
                  std::to_string(id));
    }
    return true;
  }

  bool validate_prediction(const nlohmann::json &row, std::size_t index) {
    const auto prediction_kind = string_field(row, "prediction_kind");
    if (prediction_kind && *prediction_kind == "point") {
      return fail("optimization dataset: row " + std::to_string(index) +
                  " uses a point forecast; provide a scenario or quantile");
    }
    const auto scenario = string_field(row, "scenario");
    const auto quantile = number_field(row, "quantile");
    if ((!scenario || scenario->empty()) && !quantile) {
      return fail("optimization dataset: row " + std::to_string(index) +
                  " has no scenario or quantile marker");
    }
    if (quantile && (*quantile < 0.0 || *quantile > 1.0)) {
      return fail("optimization dataset: row " + std::to_string(index) +
                  " has quantile outside [0, 1]");
    }
    return true;
  }

  bool assign_number(const nlohmann::json &row, std::string_view field,
                     double &target, bool &seen) {
    const auto found = row.find(field);
    if (found == row.end()) {
      return true;
    }
    const auto value = number_field(row, field);
    if (!value || (seen && std::abs(target - *value) > 1e-12)) {
      return false;
    }
    target = *value;
    seen = true;
    return true;
  }

  bool parse_variable_metadata(const nlohmann::json &row,
                               const std::string &name) {
    VariableRecord &record = variables_[name];
    if (!assign_number(row, "objective", record.variable.objective,
                       record.objective_seen) ||
        !assign_number(row, "lower_bound", record.variable.lower_bound,
                       record.lower_seen) ||
        !assign_number(row, "upper_bound", record.variable.upper_bound,
                       record.upper_seen) ||
        !assign_number(row, "current_value", record.variable.current_value,
                       record.current_seen)) {
      return fail("optimization dataset: variable '" + name +
                  "' has inconsistent numeric metadata");
    }
    const auto integer = row.find("integer");
    if (integer == row.end()) {
      return true;
    }
    if (!integer->is_boolean()) {
      return fail("optimization dataset: integer must be boolean");
    }
    const bool value = integer->get<bool>();
    if (record.integer_seen && record.variable.integer != value) {
      return fail("optimization dataset: variable '" + name +
                  "' has inconsistent integer metadata");
    }
    record.variable.integer = value;
    record.integer_seen = true;
    return true;
  }

  bool parse_constraint(const nlohmann::json &row, std::size_t index,
                        const std::string &variable_name) {
    const auto name = string_field(row, "constraint");
    if (!name || name->empty()) {
      return true;
    }
    const auto coefficient = number_field(row, "coefficient");
    const auto sense_text = string_field(row, "sense");
    const auto right_hand_side = number_field(row, "rhs");
    if (!coefficient || !sense_text || !right_hand_side) {
      return fail("optimization dataset: constraint row " +
                  std::to_string(index) +
                  " needs numeric coefficient/rhs and a sense");
    }
    const auto sense = parse_sense(*sense_text);
    if (!sense) {
      return fail("optimization dataset: constraint '" + *name +
                  "' has unknown sense '" + *sense_text + "'");
    }
    if (!constraints_.contains(*name)) {
      constraint_order_.push_back(*name);
    }
    ConstraintRecord &constraint = constraints_[*name];
    if (constraint.metadata_seen &&
        (constraint.sense != *sense ||
         std::abs(constraint.right_hand_side - *right_hand_side) > 1e-12)) {
      return fail("optimization dataset: constraint '" + *name +
                  "' has inconsistent metadata");
    }
    constraint.sense = *sense;
    constraint.right_hand_side = *right_hand_side;
    constraint.metadata_seen = true;
    constraint.coefficients[variable_name] += *coefficient;
    return true;
  }

  bool parse_row(const nlohmann::json &row, std::size_t index) {
    if (!row.is_object()) {
      return fail("optimization dataset: row " + std::to_string(index) +
                  " is not an object");
    }
    if (!validate_row_id(row, index) || !validate_prediction(row, index)) {
      return false;
    }
    const auto variable_name = string_field(row, "variable");
    if (!variable_name || variable_name->empty()) {
      return fail("optimization dataset: row " + std::to_string(index) +
                  " has no variable name");
    }
    if (!variables_.contains(*variable_name)) {
      variable_order_.push_back(*variable_name);
      variables_[*variable_name].variable.name = *variable_name;
    }
    return parse_variable_metadata(row, *variable_name) &&
           parse_constraint(row, index, *variable_name);
  }

  std::optional<OptimizationProblem> build_problem() {
    OptimizationProblem problem;
    problem.maximize = maximize_;
    for (const std::string &name : variable_order_) {
      problem.variables.push_back(variables_[name].variable);
    }
    for (const std::string &name : constraint_order_) {
      OptimizationConstraint constraint;
      constraint.name = name;
      constraint.sense = constraints_[name].sense;
      constraint.right_hand_side = constraints_[name].right_hand_side;
      constraint.coefficients.reserve(variable_order_.size());
      for (const std::string &variable : variable_order_) {
        constraint.coefficients.push_back(
            constraints_[name].coefficients[variable]);
      }
      problem.constraints.push_back(std::move(constraint));
    }
    std::string validation_error;
    if (!valid_problem(problem, validation_error)) {
      return reject(std::move(validation_error));
    }
    return problem;
  }

  const std::vector<nlohmann::json> &rows_;
  bool maximize_;
  std::string *error_;
  std::vector<std::string> variable_order_;
  std::map<std::string, VariableRecord> variables_;
  std::vector<std::string> constraint_order_;
  std::map<std::string, ConstraintRecord> constraints_;
  std::set<std::int64_t> row_ids_;
};

std::string table_suffix(std::string id) {
  std::ranges::replace(id, '-', '_');
  return id;
}

std::shared_ptr<data::Table>
create_output_table(Node &node, const std::string &database_name,
                    const std::string &table_name) {
  std::shared_ptr<data::Database> database = node.find_database(database_name);
  if (!database) {
    database = node.create_database(database_name);
  }
  if (!database) {
    return nullptr;
  }
  return database->create_table(table_name);
}

bool fail_run(OptimizationRunResult &result, std::string error) {
  result.error = std::move(error);
  result.run->add_log({util::now_iso8601(), "error", result.error});
  result.run->set_status(pipeline::RunStatus::Failed);
  return false;
}

void apply_distributed_verification(const OptimizationParams &params,
                                    const OptimizationAllReduce &allreduce,
                                    OptimizationResult &solution) {
  if (!solution.has_plan() || !allreduce) {
    return;
  }
  double local_max_violation = 0.0;
  double local_failures = 0.0;
  for (const ConstraintCheck &check : solution.constraint_checks) {
    local_max_violation = std::max(local_max_violation, check.violation);
    if (!check.satisfied) {
      local_failures += 1.0;
    }
  }
  const std::vector<double> maximum =
      allreduce("optimization.verify.max", {local_max_violation},
                OptimizationReduce::Max);
  const std::vector<double> failures =
      allreduce("optimization.verify.failures", {local_failures},
                OptimizationReduce::Sum);
  if (maximum.size() != 1 || failures.size() != 1) {
    solution.status = OptimizationStatus::NumericalFailure;
    solution.values.clear();
    solution.constraint_checks.clear();
    solution.error = "optimization: distributed verification collective failed";
    return;
  }
  if (maximum.front() <= params.feasibility_tolerance &&
      failures.front() <= 0.0) {
    return;
  }
  solution.status = OptimizationStatus::NumericalFailure;
  solution.values.clear();
  solution.constraint_checks.clear();
  solution.error = "optimization: a member rejected the candidate plan";
}

bool terminal_without_plan(OptimizationStatus status) {
  return status == OptimizationStatus::Infeasible ||
         status == OptimizationStatus::Unbounded ||
         status == OptimizationStatus::NodeLimit ||
         status == OptimizationStatus::TimeLimit ||
         status == OptimizationStatus::IterationLimit;
}

bool write_result_tables(Node &node, const OptimizationProblem &problem,
                         const OptimizationRunOptions &options,
                         OptimizationRunResult &result,
                         std::vector<pipeline::RunOutput> &outputs) {
  if (!result.solution.has_plan()) {
    return true;
  }
  const std::string suffix = table_suffix(result.run->id());
  const std::string plan_name = "plan_" + suffix;
  const std::shared_ptr<data::Table> plan =
      create_output_table(node, options.output_database, plan_name);
  if (!plan) {
    return fail_run(result, "optimization: could not create the plan table");
  }
  for (std::size_t index = 0; index < problem.variables.size(); ++index) {
    const OptimizationVariable &variable = problem.variables[index];
    plan->add_row({{"variable", variable.name},
                   {"decision", result.solution.values[index]},
                   {"current_value", variable.current_value},
                   {"objective_coefficient", variable.objective},
                   {"integer", variable.integer}});
  }
  result.plan_table = options.output_database + "." + plan_name;
  outputs.push_back({"dataset", "optimization plan", result.plan_table});

  const std::string constraints_name = "constraints_" + suffix;
  const std::shared_ptr<data::Table> constraints =
      create_output_table(node, options.output_database, constraints_name);
  if (!constraints) {
    return fail_run(
        result, "optimization: could not create the constraint trace table");
  }
  for (const ConstraintCheck &check : result.solution.constraint_checks) {
    constraints->add_row({{"constraint", check.constraint},
                          {"activity", check.activity},
                          {"rhs", check.right_hand_side},
                          {"violation", check.violation},
                          {"satisfied", check.satisfied}});
  }
  result.constraint_table = options.output_database + "." + constraints_name;
  outputs.push_back({"dataset", "constraint trace", result.constraint_table});
  return true;
}

bool register_result_model(Node &node, const data::ResolvedDataset &dataset,
                           const OptimizationProblem &problem,
                           const OptimizationParams &params,
                           const OptimizationRunOptions &options,
                           std::size_t input_rows,
                           OptimizationRunResult &result,
                           std::vector<pipeline::RunOutput> &outputs) {
  ModelValue model_value;
  model_value.name = options.model_name;
  model_value.kind = "optimization";
  model_value.project = options.project;
  model_value.run = result.run->id();
  model_value.feature_schema = {
      {"solver", "native-revised-simplex-branch-and-bound"},
      {"status", std::string(to_string(result.solution.status))},
      {"variables", problem.variables.size()},
      {"constraints", problem.constraints.size()},
      {"input", dataset.database + "." + dataset.table}};
  model_value.metrics = {
      {"objective_value", result.solution.objective},
      {"current_plan_objective", result.solution.current_plan_objective},
      {"objective_delta",
       result.solution.objective - result.solution.current_plan_objective},
      {"feasible", result.solution.feasible() ? 1.0 : 0.0},
      {"optimality_gap", result.solution.optimality_gap},
      {"constraint_violations",
       static_cast<double>(std::ranges::count_if(
           result.solution.constraint_checks,
           [](const ConstraintCheck &check) { return !check.satisfied; }))},
      {"iterations", static_cast<double>(result.solution.iterations)},
      {"explored_nodes", static_cast<double>(result.solution.explored_nodes)},
      {"input_rows", static_cast<double>(input_rows)}};
  const std::shared_ptr<Model> model =
      node.create_model(std::move(model_value));
  if (!model) {
    return fail_run(result,
                    "optimization: could not register the solver result");
  }
  model->set_weights(nlohmann::json{
      {"variables", problem.variables.size()},
      {"constraints", problem.constraints.size()},
      {"seed",
       params.seed}}.dump());
  result.model_id = model->id();
  outputs.push_back({"model", "optimization solver result", result.model_id});
  outputs.push_back({"report", "optimization report", result.run->id()});
  return true;
}

void finish_run(const data::ResolvedDataset &dataset,
                std::vector<pipeline::RunOutput> outputs,
                OptimizationRunResult &result) {
  pipeline::RunLineage lineage;
  lineage.sources = {"dataset:" + dataset.database + "." + dataset.table};
  lineage.steps = {"read scenario table", "solve optimization model",
                   "verify bounds and every constraint", "write plan tables"};
  lineage.freshness = util::now_iso8601();
  lineage.confidence = result.solution.has_plan() ? 1.0 : 0.0;
  result.run->set_outputs(std::move(outputs));
  result.run->set_lineage(std::move(lineage));
  for (const std::string &note : dataset.notes) {
    result.run->add_log({util::now_iso8601(), "info", "dataset: " + note});
  }
  result.run->add_log(
      {util::now_iso8601(), "info",
       "solver status: " + std::string(to_string(result.solution.status)) +
           ", gap " + std::to_string(result.solution.optimality_gap)});
  result.run->set_status(pipeline::RunStatus::Succeeded);
}

} // namespace

std::string_view to_string(OptimizationStatus status) {
  switch (status) {
  case OptimizationStatus::Optimal:
    return "optimal";
  case OptimizationStatus::Feasible:
    return "feasible";
  case OptimizationStatus::Infeasible:
    return "infeasible";
  case OptimizationStatus::Unbounded:
    return "unbounded";
  case OptimizationStatus::NodeLimit:
    return "node_limit";
  case OptimizationStatus::TimeLimit:
    return "time_limit";
  case OptimizationStatus::IterationLimit:
    return "iteration_limit";
  case OptimizationStatus::NumericalFailure:
    return "numerical_failure";
  case OptimizationStatus::InvalidModel:
    return "invalid_model";
  }
  return "invalid_model";
}

bool OptimizationResult::feasible() const noexcept {
  if (!has_plan()) {
    return false;
  }
  return std::ranges::all_of(
      constraint_checks,
      [](const ConstraintCheck &check) { return check.satisfied; });
}

std::vector<ConstraintCheck>
verify_optimization_plan(const OptimizationProblem &problem,
                         const std::vector<double> &values, double tolerance) {
  std::vector<ConstraintCheck> checks;
  checks_pass(problem, values, tolerance, true, &checks);
  return checks;
}

OptimizationResult solve_optimization(const OptimizationProblem &problem,
                                      const OptimizationParams &params) {
  OptimizationResult result;
  if (!valid_problem(problem, result.error)) {
    result.status = OptimizationStatus::InvalidModel;
    return result;
  }
  if (params.iteration_limit == 0 || params.node_limit == 0 ||
      params.time_limit.count() <= 0 || params.feasibility_tolerance <= 0.0 ||
      params.integrality_tolerance <= 0.0) {
    result.status = OptimizationStatus::InvalidModel;
    result.error =
        "optimization: solver limits and tolerances must be positive";
    return result;
  }
  const auto [lower_bounds, upper_bounds] = variable_bounds(problem);
  const bool mixed_integer = std::ranges::any_of(
      problem.variables,
      [](const OptimizationVariable &variable) { return variable.integer; });
  result =
      mixed_integer
          ? solve_mixed_integer_problem(problem, params, lower_bounds,
                                        upper_bounds)
          : solve_linear_problem(problem, params, lower_bounds, upper_bounds);
  for (const OptimizationVariable &variable : problem.variables) {
    result.current_plan_objective +=
        variable.objective * variable.current_value;
  }
  return result;
}

std::optional<OptimizationProblem>
optimization_problem_from_rows(const std::vector<nlohmann::json> &rows,
                               bool maximize, std::string *error) {
  OptimizationTableParser parser(rows, maximize, error);
  return parser.parse();
}

bool OptimizationRunResult::ok() const noexcept {
  return run != nullptr && error.empty();
}

OptimizationRunResult run_optimization(Node &node,
                                       const data::ResolvedDataset &dataset,
                                       const data::DatasetSampler &sampler,
                                       const OptimizationParams &params,
                                       const OptimizationRunOptions &options) {
  OptimizationRunResult result;
  result.run = node.create_run(options.project, options.graph);
  if (!result.run) {
    result.error = "optimization: could not create the Run record";
    return result;
  }
  // What it reads, before it reads it, so delete_dataset refuses it (zetta#1272).
  result.run->add_reads({{dataset.database, dataset.table, dataset.table_id}});
  result.run->set_status(pipeline::RunStatus::Running);
  result.run->add_log({util::now_iso8601(), "info",
                       "optimization run started on dataset:" +
                           dataset.database + "." + dataset.table});

  std::string read_error;
  const std::vector<nlohmann::json> rows =
      sampler(dataset.database, dataset.table,
              std::numeric_limits<std::size_t>::max(), read_error);
  if (!read_error.empty()) {
    fail_run(result, "optimization dataset: " + read_error);
    return result;
  }

  std::string parse_error;
  const std::optional<OptimizationProblem> problem =
      optimization_problem_from_rows(rows, params.maximize, &parse_error);
  if (!problem) {
    fail_run(result, std::move(parse_error));
    return result;
  }
  result.solution = solve_optimization(*problem, params);
  apply_distributed_verification(params, options.allreduce, result.solution);
  if (!result.solution.has_plan() &&
      !terminal_without_plan(result.solution.status)) {
    fail_run(result, result.solution.error.empty()
                         ? "optimization solver ended without a plan"
                         : result.solution.error);
    return result;
  }

  std::vector<pipeline::RunOutput> outputs;
  if (!write_result_tables(node, *problem, options, result, outputs)) {
    return result;
  }
  if (!register_result_model(node, dataset, *problem, params, options,
                             rows.size(), result, outputs)) {
    return result;
  }
  finish_run(dataset, std::move(outputs), result);
  return result;
}

} // namespace zetta::ml
