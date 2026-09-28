#include "zetta/ml/methodology.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "zetta/data/dataset_path.hpp"
#include "zetta/ml/bandit.hpp"
#include "zetta/ml/run_features.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/project_graph.hpp"

namespace zetta::ml {
namespace {

bool is_bandit_kind(std::string_view value) {
  return value == "bandit" || value == "bandits" ||
         value == "sequential_decisioning";
}

bool owns_bandit_graph(const ir::ExecutionGraph &graph) {
  for (const auto &node : graph.nodes_of_kind(ir::StepKind::Model)) {
    if (!node || !node->attributes.is_object()) {
      continue;
    }
    const auto kind = node->attributes.find("model_kind");
    if (kind != node->attributes.end() && kind->is_string() &&
        is_bandit_kind(kind->get<std::string>())) {
      return true;
    }
  }
  return false;
}

ir::ExecutionGraph build_bandit_shape(const ShapeRequest &request) {
  const std::string source = request.hints.value("source", "datasets");
  const std::string table = request.hints.value("table", "decisions");
  const std::string algorithm = request.hints.value("algorithm", "thompson");
  const double floor = request.hints.value("exploration_floor", 0.05);
  const double default_cap = request.hints.value("default_arm_cap", 1.0);
  const nlohmann::json arms =
      request.hints.value("arms", nlohmann::json::array());
  const nlohmann::json caps =
      request.hints.value("arm_caps", nlohmann::json::object());

  ir::ExecutionGraph graph;
  graph.add_node(
      ir::StepKind::Connect, "Read decision history",
      {{"source", source},
       {"table", table},
       {"note", "decisions, treatments, rewards, and optional context"}},
      "read_bandit_decisions");
  graph.add_node(ir::StepKind::Clean, "Prepare decision context",
                 {{"note", "preserve decision order and numeric context"}},
                 "prepare_bandit_context");
  nlohmann::json model_attributes = {
      {"model_kind", "bandit"},
      {"algorithm", algorithm},
      {"decision_column", "decision_id"},
      {"arm_column", "arm"},
      {"reward_column", "reward"},
      {"exploration_floor", floor},
      {"default_arm_cap", default_cap},
      {"arm_caps", caps},
      {"note", "update per-arm native policy statistics"}};
  if (!arms.empty()) {
    model_attributes["arms"] = arms;
  }
  graph.add_node(ir::StepKind::Model, "Learn allocation policy",
                 std::move(model_attributes), "train_bandit_policy");
  graph.add_node(ir::StepKind::Serve, "Write allocation policy",
                 {{"output", "policy_table"},
                  {"note", "arm probabilities and expected rewards"}},
                 "write_bandit_policy");
  graph.add_node(ir::StepKind::Act, "Log decision propensities",
                 {{"output", "decision_log"},
                  {"note", "chosen arm and its actual selection probability"}},
                 "log_bandit_propensities");
  graph.add_edge("read_bandit_decisions", "prepare_bandit_context");
  graph.add_edge("prepare_bandit_context", "train_bandit_policy");
  graph.add_edge("train_bandit_policy", "write_bandit_policy");
  graph.add_edge("write_bandit_policy", "log_bandit_propensities");
  return graph;
}

const ir::Node *bandit_model_node(const ir::ExecutionGraph &graph) {
  const auto models = graph.nodes_of_kind(ir::StepKind::Model);
  return models.empty() ? nullptr : models.front().get();
}

std::string pipeline_dataset_path(const ir::ExecutionGraph &graph) {
  const auto sources = graph.nodes_of_kind(ir::StepKind::Connect);
  if (sources.empty() || !sources.front()->attributes.is_object()) {
    return {};
  }
  const nlohmann::json &attributes = sources.front()->attributes;
  const auto source = attributes.find("source");
  const auto table = attributes.find("table");
  if (source == attributes.end() || table == attributes.end() ||
      !source->is_string() || !table->is_string()) {
    return {};
  }
  return source->get<std::string>() + "." + table->get<std::string>();
}

BanditDatasetParams bandit_params(const nlohmann::json &input,
                                  const ir::Node &model) {
  const nlohmann::json &attributes = model.attributes;
  BanditDatasetParams params;
  const std::string algorithm =
      input.value("algorithm", attributes.value("algorithm", "thompson"));
  if (const auto parsed = bandit_algorithm_from_string(algorithm)) {
    params.policy.algorithm = *parsed;
  }
  params.policy.arms =
      input.value("arms", attributes.value("arms", std::vector<std::string>{}));
  params.policy.exploration_floor = input.value(
      "exploration_floor", attributes.value("exploration_floor", 0.05));
  params.policy.default_arm_cap =
      input.value("default_arm_cap", attributes.value("default_arm_cap", 1.0));
  params.policy.arm_caps = input.value(
      "arm_caps",
      attributes.value("arm_caps", std::map<std::string, double>{}));
  params.policy.ridge = input.value("ridge", 1.0);
  params.policy.confidence = input.value("confidence", 1.0);
  params.policy.seed = input.value("seed", std::uint64_t{1});
  params.policy.baseline_arm = input.value("baseline_arm", "");
  params.decision_column = input.value(
      "decision_column", attributes.value("decision_column", "decision_id"));
  params.arm_column =
      input.value("arm_column", attributes.value("arm_column", "arm"));
  params.reward_column =
      input.value("reward_column", attributes.value("reward_column", "reward"));
  params.context_columns =
      input.value("context_columns", std::vector<std::string>{});
  params.model_name = input.value("model_name", "bandit_policy");
  return params;
}

api::ApiResult run_bandit_methodology(RunRequest &request) {
  if (request.context == nullptr || request.project == nullptr ||
      request.graph == nullptr) {
    return api::ApiResult::failure(
        "engine_error", "M-15 run request is missing its engine context");
  }
  const ir::Node *model = bandit_model_node(request.graph->graph());
  if (model == nullptr) {
    return api::ApiResult::failure("engine_error",
                                   "M-15 pipeline has no model step");
  }
  const std::string path_text = request.input.value(
      "dataset_path", pipeline_dataset_path(request.graph->graph()));
  if (path_text.empty()) {
    return api::ApiResult::failure("invalid_input",
                                   "M-15 pipeline has no source table");
  }
  BanditDatasetParams params = bandit_params(request.input, *model);
  data::DatasetContract overrides;
  if (const auto given = request.input.find("dataset");
      given != request.input.end()) {
    std::string reason;
    const auto parsed = data::dataset_contract_from_json(*given, &reason);
    if (!parsed) {
      return api::ApiResult::failure("invalid_input", reason);
    }
    overrides = *parsed;
  }
  if (overrides.entity_column.empty()) {
    overrides.entity_column = params.decision_column;
  }
  if (overrides.label_column.empty()) {
    overrides.label_column = params.reward_column;
  }
  if (overrides.feature_columns.empty() && !params.context_columns.empty()) {
    overrides.feature_columns = params.context_columns;
  }
  data::ResolvedDataset resolved = data::resolve_dataset(
      request.context->node, data::parse_dataset_path(path_text), overrides,
      data::default_dataset_sampler(request.context->node,
                                    request.context->cluster));
  if (!resolved.ok()) {
    resolved.contract = overrides;
    return run_dataset_failure(request, "M-15", resolved);
  }
  if (!bandit_algorithm_from_string(request.input.value(
          "algorithm", model->attributes.value("algorithm", "thompson")))) {
    return api::ApiResult::failure(
        "invalid_input",
        "bandit: algorithm must be thompson, linucb, or linear_thompson");
  }
  if (params.context_columns.empty() &&
      params.policy.algorithm != BanditAlgorithm::Thompson) {
    params.context_columns = resolved.contract.feature_columns;
    std::erase(params.context_columns, params.arm_column);
  }
  data::FeaturePlanOptions feature_options;
  feature_options.entity_column = params.decision_column;
  feature_options.label_column = params.reward_column;
  feature_options.group_column = params.arm_column;
  feature_options.native_entries.push_back(
      {{params.arm_column, params.reward_column}, {"arm_policy_statistics"},
       "group observed rewards by arm; update native posterior statistics",
       "the allocation policy learns separate reward statistics for each available arm"});
  for (const auto &column : params.context_columns) {
    feature_options.native_entries.push_back(
        {{column}, {column}, "native numeric context; per-arm policy update",
         "the bandit runner consumes this decision-time context column"});
  }
  const auto feature_plan =
      plan_run_features(request, "M-15", resolved, feature_options);
  if (const auto reason = feature_plan_error(feature_plan); !reason.empty()) {
    return api::ApiResult::failure("invalid_input", reason);
  }
  const BanditDatasetResult result =
      run_bandit_dataset(request.context->node, resolved, params,
                         request.project->id(), request.graph->id());
  if (!result.ok()) {
    return api::ApiResult::failure("engine_error", result.error);
  }
  record_run_features(request.context->node, result.run_id, feature_plan);
  nlohmann::json output;
  output["run_id"] = result.run_id;
  output["engine"] = "bandit";
  output["status"] = "succeeded";
  output["dataset"] = data::to_json(resolved.contract);
  output["dataset_notes"] = resolved.notes;
  return api::ApiResult::success(std::move(output));
}

} // namespace

Methodology make_m15_methodology() {
  Methodology methodology;
  methodology.id = "M-15";
  methodology.name = "Bandits & sequential decisioning";
  methodology.family = "Action & decision";
  methodology.model_kinds = {"bandit", "bandits", "sequential_decisioning"};
  methodology.solutions = {};
  methodology.keywords = {"bandit", "explore", "sequential decision",
                          "next best action", "adaptive allocation"};
  methodology.inputs = {{"entity", true, "decision identifier"},
                        {"treatment", true, "arm chosen"},
                        {"label", true, "observed reward"},
                        {"feature", false, "numeric decision context"}};
  methodology.outputs = {"allocation policy", "decision propensities"};
  methodology.metrics = {"cumulative reward", "regret", "exploration share"};
  methodology.runnable = true;
  methodology.build_shape = build_bandit_shape;
  methodology.owns = owns_bandit_graph;
  methodology.run = run_bandit_methodology;
  return methodology;
}

} // namespace zetta::ml
