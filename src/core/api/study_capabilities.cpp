// AutoML studies on the control surface (#1138; docs/ml/studies.md).

#include <algorithm>
#include <set>

#include "zetta/api/capabilities.hpp"
#include "zetta/api/registry.hpp"
#include "zetta/ml/methodology.hpp"
#include "zetta/ml/study.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/gate.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/util/timestamp.hpp"

namespace zetta::api {
namespace {

using zetta::governance::CapabilityDomain;
using zetta::governance::Operation;

nlohmann::json study_reference() {
    return {{"type", "string"}, {"minLength", 1}};
}

const nlohmann::json& create_study_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document =
            zetta::ml::study_value_schema().at("properties").at("spec");
        document["required"] = {"name", "methodology", "objective", "budgets"};
        document["properties"]["from_defaults"] = {{"type", "boolean"}};
        return document;
    }();
    return schema;
}

const nlohmann::json& study_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = zetta::ml::study_value_schema();
        document["required"].push_back("id");
        return document;
    }();
    return schema;
}

const nlohmann::json& get_study_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study"}},
        {"additionalProperties", false},
        {"properties", {{"study", study_reference()}}}};
    return schema;
}

const nlohmann::json& list_studies_schema() {
    static const nlohmann::json schema = {
        {"type", "object"}, {"additionalProperties", false}, {"properties", nlohmann::json::object()}};
    return schema;
}

const nlohmann::json& list_studies_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"studies"}},
        {"additionalProperties", false},
        {"properties",
         {{"studies",
           {{"type", "array"}, {"items", study_output_schema()}}}}}};
    return schema;
}

nlohmann::json trial_schema() {
    return zetta::ml::study_value_schema()
        .at("properties")
        .at("trials")
        .at("items");
}

const nlohmann::json& list_trials_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"state",
           {{"type", "string"},
            {"enum", {"running", "pruned", "failed", "complete"}}}}}}};
    return schema;
}

const nlohmann::json& list_trials_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "trials"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"trials", {{"type", "array"}, {"items", trial_schema()}}}}}};
    return schema;
}

const nlohmann::json& stop_study_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "status", "reason"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"status", {{"type", "string"}, {"const", "stopped"}}},
          {"reason", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& stop_trial_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()}, {"run", study_reference()}}}};
    return schema;
}

const nlohmann::json& stop_trial_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run", "state"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"run", study_reference()},
          {"state", {{"type", "string"}, {"const", "failed"}}}}}};
    return schema;
}

const nlohmann::json& promote_trial_output_schema() {
    static const nlohmann::json schema = {
        {"type", "object"},
        {"required", {"study", "run", "model", "project"}},
        {"additionalProperties", false},
        {"properties",
         {{"study", study_reference()},
          {"run", study_reference()},
          {"model", study_reference()},
          {"project", {{"type", "string"}}}}}};
    return schema;
}

const nlohmann::json& analysis_output_schema() {
    static const nlohmann::json schema = [] {
        nlohmann::json document = nlohmann::json::parse(R"JSON({
      "type":"object",
      "required":["best_trial","optimization_history","parameter_importances","parallel_coordinates","leaderboard"],
      "additionalProperties":false,
      "properties":{
        "best_trial":{"type":["object","null"],"additionalProperties":false,"properties":{"run":{"type":"string"},"number":{"type":"integer"},"value":{"type":"number"},"params":{"type":"object"}},"required":["run","number","value","params"]},
        "optimization_history":{"type":"array","items":{"type":"object","required":["number","run","value","best"],"additionalProperties":false,"properties":{"number":{"type":"integer"},"run":{"type":"string"},"value":{"type":"number"},"best":{"type":"number"}}}},
        "parameter_importances":{"type":"array","items":{"type":"object","required":["parameter","importance"],"additionalProperties":false,"properties":{"parameter":{"type":"string"},"importance":{"type":"number"}}}},
        "parallel_coordinates":{"type":"array","items":{"type":"object","required":["run","params","value"],"additionalProperties":false,"properties":{"run":{"type":"string"},"params":{"type":"object"},"value":{"type":"number"}}}},
        "leaderboard":{"type":"array","items":{"type":"object","required":["rank","run","value","confidence_low","confidence_high"],"additionalProperties":false,"properties":{"rank":{"type":"integer"},"run":{"type":"string"},"value":{"type":"number"},"confidence_low":{"type":"number"},"confidence_high":{"type":"number"}}}}
      }
    })JSON");
        const nlohmann::json params = trial_schema().at("properties").at("params");
        document["properties"]["best_trial"]["properties"]["params"] = params;
        document["properties"]["parallel_coordinates"]["items"]
                ["properties"]["params"] = params;
        return document;
    }();
    return schema;
}

nlohmann::json with_id(const std::shared_ptr<zetta::ml::Study>& study) {
    nlohmann::json output = study->metadata();
    output["id"] = study->id();
    return output;
}

std::vector<zetta::ml::StudyParameter> default_space(
    const zetta::ml::Methodology& methodology) {
    zetta::ml::StudyParameter family;
    family.name = "model_family";
    family.kind = zetta::ml::ParameterKind::Categorical;
    const std::set<std::string> unique(methodology.model_kinds.begin(),
                                       methodology.model_kinds.end());
    for (const std::string& model : unique) family.choices.push_back(model);
    if (family.choices.empty()) family.choices.push_back(methodology.id);
    if (methodology.id != "M-1") return {std::move(family)};

    family.choices = {"gbdt", "linear"};
    zetta::ml::StudyParameter trees;
    trees.name = "gbdt.num_trees";
    trees.kind = zetta::ml::ParameterKind::Integer;
    trees.minimum = 10;
    trees.maximum = 200;
    trees.step = 10;
    trees.condition_parameter = "model_family";
    trees.condition_values = {"gbdt"};
    zetta::ml::StudyParameter depth;
    depth.name = "gbdt.max_depth";
    depth.kind = zetta::ml::ParameterKind::Integer;
    depth.minimum = 2;
    depth.maximum = 8;
    depth.step = 1;
    depth.condition_parameter = "model_family";
    depth.condition_values = {"gbdt"};
    zetta::ml::StudyParameter learning_rate;
    learning_rate.name = "learning_rate";
    learning_rate.minimum = 0.001;
    learning_rate.maximum = 0.5;
    learning_rate.logarithmic = true;
    zetta::ml::StudyParameter regularization;
    regularization.name = "linear.l2";
    regularization.minimum = 0.000001;
    regularization.maximum = 10.0;
    regularization.logarithmic = true;
    regularization.condition_parameter = "model_family";
    regularization.condition_values = {"linear"};
    return {std::move(family), std::move(trees), std::move(depth),
            std::move(learning_rate), std::move(regularization)};
}

std::optional<zetta::ml::StudySpec> spec_from_input(
    const nlohmann::json& input, std::string& reason) {
    const std::string methodology_id =
        input.at("methodology").get<std::string>();
    const zetta::ml::Methodology* methodology =
        zetta::ml::find_methodology(methodology_id);
    if (methodology == nullptr || !methodology->runnable) {
        reason = "/methodology: no runnable methodology named '" +
                 methodology_id + "'";
        return std::nullopt;
    }
    nlohmann::json spec = input;
    spec.erase("from_defaults");
    spec["project"] = input.value("project", std::string{});
    spec["pipeline"] = input.value("pipeline", std::string{});
    spec["dataset"] = input.value("dataset", std::string{});
    spec["sampler"] = input.value("sampler", std::string{"tpe"});
    spec["pruner"] = input.value("pruner", std::string{"asha"});
    spec["seed"] = input.value("seed", std::uint64_t{0});
    spec["differential_privacy"] =
        input.value("differential_privacy", false) || methodology->id == "M-20";
    spec["released_metrics"] =
        input.value("released_metrics", std::vector<std::string>{});
    if (!input.contains("search_space")) {
        if (!input.value("from_defaults", false)) {
            reason = "/search_space: required unless from_defaults is true";
            return std::nullopt;
        }
        nlohmann::json parameters = nlohmann::json::array();
        for (const zetta::ml::StudyParameter& parameter :
             default_space(*methodology)) {
            parameters.push_back(
                {{"name", parameter.name},
                 {"kind", zetta::ml::to_string(parameter.kind)},
                 {"minimum", parameter.minimum},
                 {"maximum", parameter.maximum},
                 {"step", parameter.step},
                 {"log", parameter.logarithmic},
                 {"choices", parameter.choices},
                 {"condition_parameter", parameter.condition_parameter},
                 {"condition_values", parameter.condition_values}});
        }
        spec["search_space"] = std::move(parameters);
    }
    nlohmann::json document = {
        {"spec", std::move(spec)},
        {"status", "running"},
        {"created", zetta::util::now_iso8601()},
        {"updated", zetta::util::now_iso8601()},
        {"trials", nlohmann::json::array()},
        {"spent_cost", 0.0},
        {"stop_reason", ""},
        {"promoted_model", ""}};
    const std::optional<zetta::ml::StudyValue> parsed =
        zetta::ml::parse_study_value(document, &reason);
    if (!parsed) return std::nullopt;
    return parsed->spec;
}

std::shared_ptr<zetta::pipeline::Project> project_for(
    zetta::Node& node, const std::string& reference) {
    for (const auto& project : node.projects()) {
        if (project->id() == reference || project->name() == reference) {
            return project;
        }
    }
    return nullptr;
}

void publish_study_event(zetta::Node& node,
                         const std::shared_ptr<zetta::ml::Study>& study,
                         std::string event, std::string run = {}) {
    zetta::pubsub::PubSub* broker = node.pubsub();
    if (broker == nullptr) return;
    const zetta::pubsub::Identity engine{
        zetta::pubsub::Address{"service", "studies"}, {}};
    const zetta::pubsub::Address topic{
        "topic", "study-events." + study->id()};
    if (!broker->topic_settings(topic)) {
        zetta::pubsub::TopicSettings settings;
        settings.exposed = true;
        settings.lifecycle = zetta::pubsub::TopicLifecycle::Ephemeral;
        std::string reason;
        broker->create_topic(engine, topic, settings, &reason);
    }
    zetta::pubsub::PublishRequest request;
    request.to = {topic};
    request.encoding = zetta::pubsub::Encoding::Json;
    request.mime_type = "application/json";
    request.correlation_id = study->id();
    request.payload = nlohmann::json{{"study", study->id()},
                                     {"event", std::move(event)},
                                     {"run", std::move(run)},
                                     {"status", zetta::ml::to_string(study->status())},
                                     {"time", zetta::util::now_iso8601()}}
                          .dump();
    broker->publish(engine, std::move(request));
}

ApiResult handle_create_study(const nlohmann::json& input,
                              ApiContext& context) {
    std::string reason;
    const std::optional<zetta::ml::StudySpec> spec =
        spec_from_input(input, reason);
    if (!spec) return ApiResult::failure("invalid_input", reason);
    if (!spec->project.empty() || !spec->pipeline.empty()) {
        const auto project = project_for(context.node, spec->project);
        if (!project) {
            return ApiResult::failure("not_found", "project was not found",
                                      "/project");
        }
        const auto graph = project->find_graph_by_id(spec->pipeline)
                               ? project->find_graph_by_id(spec->pipeline)
                               : project->find_graph(spec->pipeline);
        if (!graph) {
            return ApiResult::failure("not_found", "pipeline was not found",
                                      "/pipeline");
        }
        const zetta::pipeline::ValidationReport report =
            zetta::pipeline::default_gate().run(graph->graph());
        if (report.block_deploy) {
            return ApiResult::failure(
                "invalid_input",
                "the pipeline failed Verification and cannot be studied",
                "/pipeline");
        }
    }
    const auto study = context.node.create_study(*spec, &reason);
    if (!study) return ApiResult::failure("invalid_input", reason);
    publish_study_event(context.node, study, "created");
    return ApiResult::success(with_id(study));
}

ApiResult handle_get_study(const nlohmann::json& input,
                           ApiContext& context) {
    const auto study =
        context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    return ApiResult::success(with_id(study));
}

ApiResult handle_list_studies(const nlohmann::json&, ApiContext& context) {
    nlohmann::json studies = nlohmann::json::array();
    for (const auto& study : context.node.studies()) studies.push_back(with_id(study));
    return ApiResult::success({{"studies", std::move(studies)}});
}

ApiResult handle_list_trials(const nlohmann::json& input,
                             ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    const std::string state = input.value("state", std::string{});
    nlohmann::json trials = nlohmann::json::array();
    for (const nlohmann::json& trial : study->metadata().at("trials")) {
        if (state.empty() || trial.at("state") == state) trials.push_back(trial);
    }
    return ApiResult::success({{"study", study->id()}, {"trials", std::move(trials)}});
}

ApiResult handle_stop_study(const nlohmann::json& input,
                            ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    std::vector<std::string> running;
    for (const zetta::ml::StudyTrial& trial : study->value().trials) {
        if (trial.state == zetta::ml::TrialState::Running) {
            running.push_back(trial.run);
        }
    }
    study->stop("stopped by caller");
    for (const std::string& run_id : running) {
        if (const auto run = context.node.find_run(run_id)) {
            run->set_status(zetta::pipeline::RunStatus::Failed);
        }
    }
    publish_study_event(context.node, study, "stopped");
    return ApiResult::success({{"study", study->id()},
                               {"status", "stopped"},
                               {"reason", study->value().stop_reason}});
}

ApiResult handle_stop_trial(const nlohmann::json& input,
                            ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    const std::string run_id = input.at("run").get<std::string>();
    std::string reason;
    if (!study->stop_trial(run_id, "stopped by caller", &reason)) {
        return ApiResult::failure("invalid_input", reason, "/run");
    }
    if (const auto run = context.node.find_run(run_id)) {
        run->set_status(zetta::pipeline::RunStatus::Failed);
    }
    publish_study_event(context.node, study, "trial_stopped", run_id);
    return ApiResult::success(
        {{"study", study->id()}, {"run", run_id}, {"state", "failed"}});
}

ApiResult handle_promote_trial(const nlohmann::json& input,
                               ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    const std::string run_id = input.at("run").get<std::string>();
    const auto trial = std::ranges::find(study->value().trials, run_id,
                                          &zetta::ml::StudyTrial::run);
    if (trial == study->value().trials.end() ||
        trial->state != zetta::ml::TrialState::Complete) {
        return ApiResult::failure("invalid_input", "only a complete trial can be promoted", "/run");
    }
    std::shared_ptr<zetta::ml::Model> model;
    if (!trial->model.empty()) model = context.node.find_model(trial->model);
    if (!model) {
        const std::vector<std::shared_ptr<zetta::ml::Model>> models =
            context.node.models();
        const auto found = std::ranges::find_if(
            models, [&](const auto& candidate) {
                return candidate->run() == run_id;
            });
        if (found != models.end()) model = *found;
    }
    if (!model) {
        return ApiResult::failure("not_found", "the trial Run has no registered model", "/run");
    }
    if (!study->spec().project.empty() &&
        model->project() != study->spec().project) {
        return ApiResult::failure("invalid_input", "the trial model belongs to another project", "/run");
    }
    study->set_promoted_model(model->id());
    publish_study_event(context.node, study, "trial_promoted", run_id);
    return ApiResult::success({{"study", study->id()},
                               {"run", run_id},
                               {"model", model->id()},
                               {"project", model->project()}});
}

ApiResult handle_analysis(const nlohmann::json& input, ApiContext& context) {
    const auto study = context.node.find_study(input.at("study").get<std::string>());
    if (!study) return ApiResult::failure("not_found", "study was not found", "/study");
    return ApiResult::success(zetta::ml::StudyOptimizer::analysis(study->value()));
}

}  // namespace

void register_study_capabilities(Registry& registry) {
    registry.add(Capability{
        "create_study", "Create study",
        "Create a replicated AutoML study after validating its non-test objective and Pipeline; this mutates state.",
        create_study_schema(), study_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "create_study"}, true, false,
        handle_create_study});
    registry.add(Capability{
        "get_study", "Get study",
        "Read one study's objective, search configuration, budgets and enforced usage. No side effects.",
        get_study_schema(), study_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_get_study});
    registry.add(Capability{
        "list_studies", "List studies",
        "Read every study and its current budget status. No side effects.",
        list_studies_schema(), list_studies_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_list_studies});
    registry.add(Capability{
        "list_trials", "List trials",
        "Read the Runs belonging to a study, optionally filtered by trial state. No side effects.",
        list_trials_schema(), list_trials_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_list_trials});
    registry.add(Capability{
        "stop_study", "Stop study",
        "Stop a running study and cooperatively stop its running trial Runs; this mutates state.",
        get_study_schema(), stop_study_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "stop_study"}, true, false,
        handle_stop_study});
    registry.add(Capability{
        "stop_trial", "Stop trial",
        "Stop one running trial Run without stopping its study; this mutates state.",
        stop_trial_schema(), stop_trial_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "stop_trial"}, true, false,
        handle_stop_trial});
    registry.add(Capability{
        "promote_trial", "Promote trial",
        "Register a complete trial's existing model as the study project's promoted model version; this requires the dedicated promotion permission.",
        stop_trial_schema(), promote_trial_output_schema(), CapabilityKind::Action,
        Operation{CapabilityDomain::Orchestration, "promote_trial"}, true, false,
        handle_promote_trial});
    registry.add(Capability{
        "get_study_analysis", "Get study analysis",
        "Read the best trial, optimization history, parameter importances, parallel-coordinate data and confidence leaderboard. No side effects.",
        get_study_schema(), analysis_output_schema(), CapabilityKind::Read,
        Operation{CapabilityDomain::Api, "query"}, false, false,
        handle_analysis});
}

}  // namespace zetta::api
