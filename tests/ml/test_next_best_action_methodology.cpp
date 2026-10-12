#include <gtest/gtest.h>

#include "zetta/ml/next_best_action_methodology.hpp"
#include "zetta/pipeline/graph_checks.hpp"

namespace {

zetta::ir::ExecutionGraph
shape(nlohmann::json hints = nlohmann::json::object()) {
  zetta::ml::ShapeRequest request;
  request.hints = std::move(hints);
  return zetta::ml::find_methodology("M-8")->build_shape(request);
}

void refuses(nlohmann::json hints) {
  const auto graph = shape(std::move(hints));
  const auto result =
      zetta::pipeline::NextBestActionGraphCheck{}.evaluate(graph);
  EXPECT_EQ(result.status, zetta::pipeline::CheckStatus::Fail);
  EXPECT_FALSE(result.reason.empty());
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}

} // namespace

TEST(NextBestActionMethodology, OwnPipelineIsRunnableAndNotChurn) {
  const auto *methodology = zetta::ml::find_methodology("S-11");
  ASSERT_NE(methodology, nullptr);
  EXPECT_EQ(methodology->id, "M-8");
  EXPECT_TRUE(methodology->runnable);
  const auto graph = shape({{"source", "offers"}, {"table", "candidates"}});
  EXPECT_EQ(graph.node_count(), 5u);
  EXPECT_TRUE(methodology->owns(graph));
  EXPECT_FALSE(zetta::ml::find_methodology("M-1")->owns(graph));
  EXPECT_FALSE(zetta::pipeline::default_gate().run(graph).block_deploy);
  std::string error;
  const auto parsed = zetta::ml::parse_next_best_action_pipeline(graph, error);
  ASSERT_TRUE(parsed) << error;
  EXPECT_EQ(parsed->database, "offers");
  EXPECT_EQ(parsed->table, "candidates");
}

TEST(NextBestActionGraphCheck, RefusesSoftEligibilityOrContactCaps) {
  refuses({{"hard_constraints", false}});
  refuses({{"hard_constraints", "yes"}});
  refuses({{"apply_eligibility", false}});
  refuses({{"apply_contact_caps", false}});
  refuses({{"selection", "highest_score"}});
}

TEST(NextBestActionGraphCheck, RefusesNonChronologicalInteractionEvaluation) {
  refuses({{"time_based_holdout", false}});
  refuses({{"interaction_time_column", ""}});
}

TEST(NextBestActionGraphCheck, RefusesGuessedPropensitiesOrCausalClaims) {
  refuses({{"offline_evaluation", "infer_propensities"}});
  refuses({{"propensity_interpretation", "causal_lift"}});
}

TEST(NextBestActionGraphCheck, RefusesInvalidBudgetAndParameters) {
  refuses({{"budget", -1}});
  refuses({{"factors", 0}});
  refuses({{"als_iterations", 0}});
  refuses({{"minimum_logging_propensity", 0}});
  refuses({{"maximum_importance_weight", 0}});
}

TEST(NextBestActionGraphCheck, RefusesMissingOrOverlappingRoles) {
  refuses({{"entity_column", ""}});
  refuses({{"label_column", "entity_id"}});
  refuses({{"feature_columns", nlohmann::json::array()}});
  refuses({{"feature_columns", {"response"}}});
  refuses({{"source", 42}});
  refuses({{"model_kind", "gbdt"}});
}

TEST(NextBestActionGraphCheck, RefusesDisconnectedOrExtraStages) {
  auto graph = shape();
  graph.remove_edge("recommendation_model", "recommendation_policy");
  graph.add_edge("recommendation_features", "recommendation_policy");
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
  graph = shape();
  graph.add_node(zetta::ir::StepKind::Clean, "unregistered transform");
  EXPECT_TRUE(zetta::pipeline::default_gate().run(graph).block_deploy);
}
