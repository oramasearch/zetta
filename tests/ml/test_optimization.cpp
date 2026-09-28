#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "temp_dir.hpp"
#include "zetta/ml/optimization.hpp"
#include "zetta/node.hpp"

namespace {

using zetta::ml::OptimizationConstraint;
using zetta::ml::OptimizationParams;
using zetta::ml::OptimizationProblem;
using zetta::ml::OptimizationSense;
using zetta::ml::OptimizationStatus;
using zetta::ml::OptimizationVariable;

OptimizationConstraint less_equal(std::string name,
                                  std::vector<double> coefficients,
                                  double right_hand_side) {
  return OptimizationConstraint{std::move(name), std::move(coefficients),
                                OptimizationSense::LessEqual, right_hand_side};
}

OptimizationConstraint greater_equal(std::string name,
                                     std::vector<double> coefficients,
                                     double right_hand_side) {
  return OptimizationConstraint{std::move(name), std::move(coefficients),
                                OptimizationSense::GreaterEqual,
                                right_hand_side};
}

} // namespace

TEST(RevisedSimplex, SolvesTextbookLinearProgram) {
  OptimizationProblem problem;
  problem.variables = {
      {"x", 3.0, 0.0, std::numeric_limits<double>::infinity(), false, 0.0},
      {"y", 2.0, 0.0, std::numeric_limits<double>::infinity(), false, 0.0},
  };
  problem.constraints = {
      less_equal("capacity", {1.0, 1.0}, 4.0),
      less_equal("x_limit", {1.0, 0.0}, 2.0),
  };

  const auto result = zetta::ml::solve_optimization(problem);
  ASSERT_EQ(result.status, OptimizationStatus::Optimal) << result.error;
  ASSERT_EQ(result.values.size(), 2u);
  EXPECT_NEAR(result.values[0], 2.0, 1e-8);
  EXPECT_NEAR(result.values[1], 2.0, 1e-8);
  EXPECT_NEAR(result.objective, 10.0, 1e-8);
  EXPECT_TRUE(result.feasible());
}

TEST(RevisedSimplex, BlandRuleHandlesDegenerateOptimum) {
  OptimizationProblem problem;
  problem.variables = {
      {"x", 1.0, 0.0, 1.0, false, 0.0},
      {"y", 1.0, 0.0, 1.0, false, 0.0},
  };
  problem.constraints = {
      less_equal("shared", {1.0, 1.0}, 1.0),
  };

  const auto result = zetta::ml::solve_optimization(problem);
  ASSERT_EQ(result.status, OptimizationStatus::Optimal) << result.error;
  EXPECT_NEAR(result.objective, 1.0, 1e-8);
  EXPECT_TRUE(result.feasible());
}

TEST(RevisedSimplex, SolvesBoundedMinimizationWithEquality) {
  OptimizationProblem problem;
  problem.maximize = false;
  problem.variables = {
      {"x", 1.0, 0.0, 4.0, false, 0.0},
      {"y", 1.0, 0.0, 4.0, false, 0.0},
  };
  problem.constraints = {
      greater_equal("minimum", {1.0, 1.0}, 3.0),
      {"balance", {1.0, -1.0}, OptimizationSense::Equal, 1.0},
  };

  const auto result = zetta::ml::solve_optimization(problem);
  ASSERT_EQ(result.status, OptimizationStatus::Optimal) << result.error;
  ASSERT_EQ(result.values.size(), 2u);
  EXPECT_NEAR(result.values[0], 2.0, 1e-8);
  EXPECT_NEAR(result.values[1], 1.0, 1e-8);
  EXPECT_NEAR(result.objective, 3.0, 1e-8);
  EXPECT_TRUE(result.feasible());
}

TEST(RevisedSimplex, ReportsInfeasibleWithoutAPlan) {
  OptimizationProblem problem;
  problem.variables = {
      {"x", 1.0, 0.0, std::numeric_limits<double>::infinity(), false, 0.0}};
  problem.constraints = {
      less_equal("upper", {1.0}, 1.0),
      greater_equal("lower", {1.0}, 2.0),
  };

  const auto result = zetta::ml::solve_optimization(problem);
  EXPECT_EQ(result.status, OptimizationStatus::Infeasible) << result.error;
  EXPECT_FALSE(result.has_plan());
}

TEST(RevisedSimplex, ReportsUnboundedWithoutAPlan) {
  OptimizationProblem problem;
  problem.variables = {
      {"x", 1.0, 0.0, std::numeric_limits<double>::infinity(), false, 0.0}};

  const auto result = zetta::ml::solve_optimization(problem);
  EXPECT_EQ(result.status, OptimizationStatus::Unbounded) << result.error;
  EXPECT_FALSE(result.has_plan());
}

TEST(BranchBoundMip, SolvesKnapsackToOptimalityAndReportsZeroGap) {
  OptimizationProblem problem;
  problem.variables = {
      {"item_a", 10.0, 0.0, 1.0, true, 0.0},
      {"item_b", 6.0, 0.0, 1.0, true, 0.0},
      {"item_c", 4.0, 0.0, 1.0, true, 0.0},
  };
  problem.constraints = {
      less_equal("weight", {4.0, 2.0, 1.0}, 5.0),
  };

  const auto result = zetta::ml::solve_optimization(problem);
  ASSERT_EQ(result.status, OptimizationStatus::Optimal) << result.error;
  ASSERT_EQ(result.values.size(), 3u);
  EXPECT_NEAR(result.values[0], 1.0, 1e-8);
  EXPECT_NEAR(result.values[1], 0.0, 1e-8);
  EXPECT_NEAR(result.values[2], 1.0, 1e-8);
  EXPECT_NEAR(result.objective, 14.0, 1e-8);
  EXPECT_DOUBLE_EQ(result.optimality_gap, 0.0);
  EXPECT_TRUE(result.feasible());
}

TEST(BranchBoundMip, NodeLimitReportsGapAndNeverInventsAPlan) {
  OptimizationProblem problem;
  problem.variables = {
      {"x", 1.0, 0.0, 1.0, true, 0.0},
      {"y", 1.0, 0.0, 1.0, true, 0.0},
  };
  problem.constraints = {
      less_equal("fractional", {1.0, 1.0}, 1.5),
  };
  OptimizationParams params;
  params.node_limit = 1;

  const auto result = zetta::ml::solve_optimization(problem, params);
  EXPECT_EQ(result.status, OptimizationStatus::NodeLimit);
  EXPECT_FALSE(result.has_plan());
  EXPECT_DOUBLE_EQ(result.optimality_gap, 1.0);
}

TEST(OptimizationVerification, RejectsEveryConstraintViolationIndependently) {
  OptimizationProblem problem;
  problem.variables = {{"x", 1.0, 0.0, 10.0, false, 0.0}};
  problem.constraints = {less_equal("capacity", {1.0}, 3.0)};

  const auto checks = zetta::ml::verify_optimization_plan(problem, {4.0}, 1e-8);
  ASSERT_EQ(checks.size(), 1u);
  EXPECT_FALSE(checks.front().satisfied);
  EXPECT_NEAR(checks.front().violation, 1.0, 1e-8);
}

TEST(OptimizationRunner, DistributedVerifierCanVetoCandidatePlan) {
  zetta::test::TempDir directory;
  zetta::NodeConfig config;
  config.data_dir = directory.path().string();
  zetta::Node node(config);
  ASSERT_TRUE(node.start());

  zetta::data::ResolvedDataset dataset;
  dataset.database = "operations";
  dataset.table = "decisions";
  dataset.rows = 1;
  const zetta::data::DatasetSampler sampler =
      [](const std::string &, const std::string &, std::size_t, std::string &) {
        return std::vector<nlohmann::json>{
            {{"row_id", 1},
             {"variable", "x"},
             {"objective", 1.0},
             {"lower_bound", 0.0},
             {"upper_bound", 1.0},
             {"integer", false},
             {"scenario", "base"}},
        };
      };
  zetta::ml::OptimizationRunOptions options;
  options.project = "project";
  options.graph = "graph";
  options.allreduce = [](const std::string &tag,
                         const std::vector<double> &local,
                         zetta::ml::OptimizationReduce) {
    if (tag == "optimization.verify.failures") {
      return std::vector<double>{1.0};
    }
    return local;
  };

  const auto result = zetta::ml::run_optimization(
      node, dataset, sampler, OptimizationParams{}, options);
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.solution.status, OptimizationStatus::NumericalFailure);
  EXPECT_FALSE(result.solution.has_plan());
  EXPECT_TRUE(result.plan_table.empty());
  ASSERT_NE(result.run, nullptr);
  EXPECT_EQ(result.run->status(), zetta::pipeline::RunStatus::Failed);
  node.stop();
}

TEST(OptimizationTable, RequiresScenarioOrQuantileInsteadOfPointForecast) {
  const std::vector<nlohmann::json> rows = {
      {{"row_id", 1},
       {"variable", "x"},
       {"objective", 1.0},
       {"integer", false},
       {"prediction_kind", "point"}},
  };
  std::string error;
  const auto problem =
      zetta::ml::optimization_problem_from_rows(rows, true, &error);
  EXPECT_FALSE(problem.has_value());
  EXPECT_NE(error.find("point forecast"), std::string::npos) << error;
}

TEST(OptimizationTable, RequiresDeclaredVariableMetadataAndUniqueRowIds) {
  const std::vector<nlohmann::json> missing_metadata = {
      {{"row_id", 1}, {"variable", "x"}, {"scenario", "base"}},
  };
  std::string error;
  EXPECT_FALSE(zetta::ml::optimization_problem_from_rows(missing_metadata, true,
                                                         &error));
  EXPECT_NE(error.find("objective and integer"), std::string::npos) << error;

  const std::vector<nlohmann::json> duplicate_rows = {
      {{"row_id", 1},
       {"variable", "x"},
       {"objective", 1.0},
       {"integer", false},
       {"scenario", "base"}},
      {{"row_id", 1},
       {"variable", "y"},
       {"objective", 2.0},
       {"integer", false},
       {"scenario", "base"}},
  };
  error.clear();
  EXPECT_FALSE(
      zetta::ml::optimization_problem_from_rows(duplicate_rows, true, &error));
  EXPECT_NE(error.find("duplicate row_id"), std::string::npos) << error;
}

TEST(OptimizationTable, BuildsVariablesAndConstraintsFromRows) {
  const std::vector<nlohmann::json> rows = {
      {{"row_id", 1},
       {"variable", "x"},
       {"objective", 3.0},
       {"integer", false},
       {"current_value", 1.0},
       {"scenario", "base"},
       {"constraint", "capacity"},
       {"coefficient", 1.0},
       {"sense", "<="},
       {"rhs", 4.0}},
      {{"row_id", 2},
       {"variable", "y"},
       {"objective", 2.0},
       {"integer", false},
       {"scenario", "base"},
       {"constraint", "capacity"},
       {"coefficient", 1.0},
       {"sense", "<="},
       {"rhs", 4.0}},
  };
  std::string error;
  const auto problem =
      zetta::ml::optimization_problem_from_rows(rows, true, &error);
  ASSERT_TRUE(problem.has_value()) << error;
  ASSERT_EQ(problem->variables.size(), 2u);
  ASSERT_EQ(problem->constraints.size(), 1u);
  EXPECT_EQ(problem->constraints.front().coefficients,
            (std::vector<double>{1.0, 1.0}));
}
