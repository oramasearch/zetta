#include "zetta/ml/study.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <thread>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/replication.hpp"
#include "zetta/compute/cpu_device.hpp"
#include "zetta/compute/work_stealing_scheduler.hpp"
#include "zetta/data/dataset_path.hpp"
#include "zetta/ddf/local_partitions.hpp"
#include "zetta/ddf/ops.hpp"
#include "zetta/ir/serialization.hpp"
#include "zetta/ml/gbdt.hpp"
#include "zetta/ml/gbdt_train_job.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/project.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pubsub/pubsub.hpp"
#include "zetta/usecases/churn/churn_solution.hpp"
#include "zetta/util/timestamp.hpp"

#include "../catalog.hpp"

namespace zetta::ml {
namespace fs = std::filesystem;

namespace {

void publish_progress(Node& node, const Study& study, std::string event,
                      std::string run = {}, std::uint64_t step = 0,
                      std::optional<double> value = std::nullopt) {
    zetta::pubsub::PubSub* broker = node.pubsub();
    if (broker == nullptr) return;
    const zetta::pubsub::Identity engine{
        zetta::pubsub::Address{"service", "studies"}, {}};
    const zetta::pubsub::Address topic{
        "topic", "study-events." + study.id()};
    if (!broker->topic_settings(topic)) {
        zetta::pubsub::TopicSettings settings;
        settings.exposed = true;
        settings.lifecycle = zetta::pubsub::TopicLifecycle::Ephemeral;
        std::string reason;
        broker->create_topic(engine, topic, settings, &reason);
    }
    nlohmann::json payload = {{"study", study.id()},
                              {"event", std::move(event)},
                              {"run", std::move(run)},
                              {"status", to_string(study.status())},
                              {"time", zetta::util::now_iso8601()}};
    if (step > 0) payload["step"] = step;
    if (value) payload["value"] = *value;
    zetta::pubsub::PublishRequest request;
    request.to = {topic};
    request.encoding = zetta::pubsub::Encoding::Json;
    request.mime_type = "application/json";
    request.correlation_id = study.id();
    request.payload = payload.dump();
    broker->publish(engine, std::move(request));
}

template <class Enum>
std::optional<Enum> parse_enum(std::string_view);

template <>
std::optional<StudyDirection> parse_enum(std::string_view value) {
    if (value == "minimize") return StudyDirection::Minimize;
    if (value == "maximize") return StudyDirection::Maximize;
    return std::nullopt;
}

template <>
std::optional<StudyStatus> parse_enum(std::string_view value) {
    if (value == "running") return StudyStatus::Running;
    if (value == "stopped") return StudyStatus::Stopped;
    if (value == "complete") return StudyStatus::Complete;
    if (value == "failed") return StudyStatus::Failed;
    return std::nullopt;
}

template <>
std::optional<TrialState> parse_enum(std::string_view value) {
    if (value == "running") return TrialState::Running;
    if (value == "pruned") return TrialState::Pruned;
    if (value == "failed") return TrialState::Failed;
    if (value == "complete") return TrialState::Complete;
    return std::nullopt;
}

template <>
std::optional<SamplerKind> parse_enum(std::string_view value) {
    if (value == "grid") return SamplerKind::Grid;
    if (value == "random") return SamplerKind::Random;
    if (value == "tpe") return SamplerKind::Tpe;
    return std::nullopt;
}

template <>
std::optional<PrunerKind> parse_enum(std::string_view value) {
    if (value == "none") return PrunerKind::None;
    if (value == "median") return PrunerKind::Median;
    if (value == "asha") return PrunerKind::Asha;
    return std::nullopt;
}

template <>
std::optional<ParameterKind> parse_enum(std::string_view value) {
    if (value == "int") return ParameterKind::Integer;
    if (value == "float") return ParameterKind::Float;
    if (value == "categorical") return ParameterKind::Categorical;
    return std::nullopt;
}

const nlohmann::json& schema_document() {
    static const nlohmann::json schema = nlohmann::json::parse(R"JSON({
      "$schema":"http://json-schema.org/draft-07/schema#",
      "title":"zetta.Study",
      "type":"object",
      "required":["spec","status","created","updated","trials","spent_cost","stop_reason","promoted_model"],
      "additionalProperties":false,
      "properties":{
        "id":{"type":"string"},
        "spec":{
          "type":"object",
          "required":["name","project","pipeline","dataset","methodology","objective","search_space","sampler","pruner","budgets","seed","differential_privacy","released_metrics"],
          "additionalProperties":false,
          "properties":{
            "name":{"type":"string","minLength":1},
            "project":{"type":"string"},
            "pipeline":{"type":"string"},
            "dataset":{"type":"string"},
            "methodology":{"type":"string","minLength":1},
            "objective":{
              "type":"object",
              "required":["metric","direction","validation_split"],
              "additionalProperties":false,
              "properties":{
                "metric":{"type":"string","minLength":1},
                "direction":{"type":"string","enum":["minimize","maximize"]},
                "validation_split":{"type":"string","minLength":1}
              }
            },
            "search_space":{"type":"array","minItems":1,"items":{
              "type":"object",
              "required":["name","kind","minimum","maximum","step","log","choices","condition_parameter","condition_values"],
              "additionalProperties":false,
              "properties":{
                "name":{"type":"string","minLength":1},
                "kind":{"type":"string","enum":["int","float","categorical"]},
                "minimum":{"type":"number"},
                "maximum":{"type":"number"},
                "step":{"type":"number","minimum":0},
                "log":{"type":"boolean"},
                "choices":{"type":"array","items":{"type":["string","number","integer","boolean"]}},
                "condition_parameter":{"type":"string"},
                "condition_values":{"type":"array","items":{"type":["string","number","integer","boolean"]}}
              }
            }},
            "sampler":{"type":"string","enum":["grid","random","tpe"]},
            "pruner":{"type":"string","enum":["none","median","asha"]},
            "budgets":{
              "type":"object",
              "required":["max_trials","max_wall_time_ms","max_parallel_trials","cost_cap"],
              "additionalProperties":false,
              "properties":{
                "max_trials":{"type":"integer","minimum":1},
                "max_wall_time_ms":{"type":"integer","minimum":0},
                "max_parallel_trials":{"type":"integer","minimum":1},
                "cost_cap":{"type":"number","minimum":0}
              }
            },
            "seed":{"type":"integer","minimum":0},
            "differential_privacy":{"type":"boolean"},
            "released_metrics":{"type":"array","uniqueItems":true,"items":{"type":"string","minLength":1}}
          }
        },
        "status":{"type":"string","enum":["running","stopped","complete","failed"]},
        "created":{"type":"string"},
        "updated":{"type":"string"},
        "trials":{"type":"array","items":{
          "type":"object",
          "required":["number","run","params","state","seed","cost","intermediate","reason","model"],
          "additionalProperties":false,
          "properties":{
            "number":{"type":"integer","minimum":0},
            "run":{"type":"string","minLength":1},
            "params":{"type":"object","patternProperties":{"^[A-Za-z0-9_.-]{1,128}$":{"type":["string","number","integer","boolean"]}},"additionalProperties":false},
            "state":{"type":"string","enum":["running","pruned","failed","complete"]},
            "objective":{"type":"number"},
            "seed":{"type":"integer","minimum":0},
            "cost":{"type":"number","minimum":0},
            "intermediate":{"type":"array","items":{
              "type":"object","required":["step","value"],"additionalProperties":false,
              "properties":{"step":{"type":"integer","minimum":0},"value":{"type":"number"}}
            }},
            "reason":{"type":"string"},
            "model":{"type":"string"}
          }
        }},
        "spent_cost":{"type":"number","minimum":0},
        "stop_reason":{"type":"string"},
        "failure_reason":{"type":"string"},
        "promoted_model":{"type":"string"}
      }
    })JSON");
    return schema;
}

class CollectingErrorHandler : public nlohmann::json_schema::error_handler {
public:
    void error(const nlohmann::json::json_pointer& pointer,
               const nlohmann::json&, const std::string& message) override {
        if (failed) return;
        failed = true;
        reason = (pointer.to_string().empty() ? "/" : pointer.to_string()) +
                 ": " + message;
    }

    bool failed = false;
    std::string reason;
};

nlohmann::json parameter_json(const StudyParameter& parameter) {
    return {{"name", parameter.name},
            {"kind", to_string(parameter.kind)},
            {"minimum", parameter.minimum},
            {"maximum", parameter.maximum},
            {"step", parameter.step},
            {"log", parameter.logarithmic},
            {"choices", parameter.choices},
            {"condition_parameter", parameter.condition_parameter},
            {"condition_values", parameter.condition_values}};
}

nlohmann::json spec_json(const StudySpec& spec) {
    nlohmann::json search_space = nlohmann::json::array();
    for (const StudyParameter& parameter : spec.search_space) {
        search_space.push_back(parameter_json(parameter));
    }
    return {{"name", spec.name},
            {"project", spec.project},
            {"pipeline", spec.pipeline},
            {"dataset", spec.dataset},
            {"methodology", spec.methodology},
            {"objective",
             {{"metric", spec.objective.metric},
              {"direction", to_string(spec.objective.direction)},
              {"validation_split", spec.objective.validation_split}}},
            {"search_space", std::move(search_space)},
            {"sampler", to_string(spec.sampler)},
            {"pruner", to_string(spec.pruner)},
            {"budgets",
             {{"max_trials", spec.budgets.max_trials},
              {"max_wall_time_ms", spec.budgets.max_wall_time_ms},
              {"max_parallel_trials", spec.budgets.max_parallel_trials},
              {"cost_cap", spec.budgets.cost_cap}}},
            {"seed", spec.seed},
            {"differential_privacy", spec.differential_privacy},
            {"released_metrics", spec.released_metrics}};
}

StudySpec parse_spec(const nlohmann::json& value) {
    StudySpec spec;
    spec.name = value.at("name").get<std::string>();
    spec.project = value.at("project").get<std::string>();
    spec.pipeline = value.at("pipeline").get<std::string>();
    spec.dataset = value.at("dataset").get<std::string>();
    spec.methodology = value.at("methodology").get<std::string>();
    const nlohmann::json& objective = value.at("objective");
    spec.objective.metric = objective.at("metric").get<std::string>();
    spec.objective.direction =
        *parse_enum<StudyDirection>(objective.at("direction").get<std::string>());
    spec.objective.validation_split =
        objective.at("validation_split").get<std::string>();
    for (const nlohmann::json& item : value.at("search_space")) {
        StudyParameter parameter;
        parameter.name = item.at("name").get<std::string>();
        parameter.kind =
            *parse_enum<ParameterKind>(item.at("kind").get<std::string>());
        parameter.minimum = item.at("minimum").get<double>();
        parameter.maximum = item.at("maximum").get<double>();
        parameter.step = item.at("step").get<double>();
        parameter.logarithmic = item.at("log").get<bool>();
        parameter.choices = item.at("choices").get<std::vector<nlohmann::json>>();
        parameter.condition_parameter =
            item.at("condition_parameter").get<std::string>();
        parameter.condition_values =
            item.at("condition_values").get<std::vector<nlohmann::json>>();
        spec.search_space.push_back(std::move(parameter));
    }
    spec.sampler = *parse_enum<SamplerKind>(value.at("sampler").get<std::string>());
    spec.pruner = *parse_enum<PrunerKind>(value.at("pruner").get<std::string>());
    const nlohmann::json& budgets = value.at("budgets");
    spec.budgets.max_trials = budgets.at("max_trials").get<std::size_t>();
    spec.budgets.max_wall_time_ms =
        budgets.at("max_wall_time_ms").get<std::uint64_t>();
    spec.budgets.max_parallel_trials =
        budgets.at("max_parallel_trials").get<std::size_t>();
    spec.budgets.cost_cap = budgets.at("cost_cap").get<double>();
    spec.seed = value.at("seed").get<std::uint64_t>();
    spec.differential_privacy = value.at("differential_privacy").get<bool>();
    spec.released_metrics =
        value.at("released_metrics").get<std::vector<std::string>>();
    return spec;
}

bool better(StudyDirection direction, double left, double right) {
    return direction == StudyDirection::Maximize ? left > right : left < right;
}

std::vector<const StudyTrial*> completed_trials(
    const std::vector<StudyTrial>& trials) {
    std::vector<const StudyTrial*> result;
    for (const StudyTrial& trial : trials) {
        if (trial.state == TrialState::Complete && trial.objective) {
            result.push_back(&trial);
        }
    }
    return result;
}

bool active_parameter(const StudyParameter& parameter,
                      const nlohmann::json& sampled) {
    if (parameter.condition_parameter.empty()) return true;
    const auto parent = sampled.find(parameter.condition_parameter);
    if (parent == sampled.end()) return false;
    return std::ranges::any_of(parameter.condition_values,
                               [&](const nlohmann::json& value) {
                                   return value == *parent;
                               });
}

double unit_random(std::mt19937_64& generator) {
    return std::generate_canonical<double, 53>(generator);
}

nlohmann::json random_value(const StudyParameter& parameter,
                            std::mt19937_64& generator) {
    if (parameter.kind == ParameterKind::Categorical) {
        const std::size_t index = static_cast<std::size_t>(
            generator() % parameter.choices.size());
        return parameter.choices[index];
    }
    double value = 0.0;
    if (parameter.logarithmic) {
        const double low = std::log(parameter.minimum);
        const double high = std::log(parameter.maximum);
        value = std::exp(low + unit_random(generator) * (high - low));
    } else {
        value = parameter.minimum +
                unit_random(generator) * (parameter.maximum - parameter.minimum);
    }
    if (parameter.step > 0.0) {
        value = parameter.minimum +
                std::round((value - parameter.minimum) / parameter.step) *
                    parameter.step;
    }
    value = std::clamp(value, parameter.minimum, parameter.maximum);
    if (parameter.kind == ParameterKind::Integer) {
        return static_cast<std::int64_t>(std::llround(value));
    }
    return value;
}

nlohmann::json grid_value(const StudyParameter& parameter,
                          std::size_t& index) {
    if (parameter.kind == ParameterKind::Categorical) {
        const std::size_t cardinality = parameter.choices.size();
        const nlohmann::json value = parameter.choices[index % cardinality];
        index /= cardinality;
        return value;
    }
    const double step = parameter.step > 0.0
                            ? parameter.step
                            : parameter.maximum - parameter.minimum;
    const std::size_t cardinality = static_cast<std::size_t>(
                                        std::floor((parameter.maximum -
                                                    parameter.minimum) /
                                                   step)) +
                                    1;
    const double value = parameter.minimum +
                         static_cast<double>(index % cardinality) * step;
    index /= cardinality;
    if (parameter.kind == ParameterKind::Integer) {
        return static_cast<std::int64_t>(std::llround(value));
    }
    return value;
}

double numeric_value(const nlohmann::json& value) {
    return value.is_number() ? value.get<double>() : 0.0;
}

nlohmann::json tpe_sample(const StudySpec& spec,
                          const std::vector<const StudyTrial*>& completed,
                          std::size_t trial_number) {
    std::vector<const StudyTrial*> ordered = completed;
    std::ranges::sort(ordered, [&](const StudyTrial* left,
                                   const StudyTrial* right) {
        return better(spec.objective.direction, *left->objective,
                      *right->objective);
    });
    const std::size_t good_count =
        std::max<std::size_t>(1, static_cast<std::size_t>(
                                     std::ceil(ordered.size() * 0.2)));
    std::mt19937_64 generator(spec.seed + trial_number * 0x9e3779b97f4a7c15ULL);
    nlohmann::json sampled = nlohmann::json::object();
    for (const StudyParameter& parameter : spec.search_space) {
        if (!active_parameter(parameter, sampled)) continue;
        if (parameter.kind == ParameterKind::Categorical) {
            std::vector<double> weights(parameter.choices.size(), 1.0);
            for (std::size_t index = 0; index < good_count; ++index) {
                const auto found = ordered[index]->params.find(parameter.name);
                if (found == ordered[index]->params.end()) continue;
                for (std::size_t choice = 0; choice < parameter.choices.size();
                     ++choice) {
                    if (*found == parameter.choices[choice]) {
                        weights[choice] += 3.0;
                    }
                }
            }
            std::discrete_distribution<std::size_t> distribution(weights.begin(),
                                                                  weights.end());
            sampled[parameter.name] = parameter.choices[distribution(generator)];
            continue;
        }
        const StudyTrial* center = ordered[generator() % good_count];
        const auto found = center->params.find(parameter.name);
        if (found == center->params.end() || !found->is_number()) {
            sampled[parameter.name] = random_value(parameter, generator);
            continue;
        }
        double low = parameter.minimum;
        double high = parameter.maximum;
        double center_value = found->get<double>();
        if (parameter.logarithmic) {
            low = std::log(low);
            high = std::log(high);
            center_value = std::log(center_value);
        }
        const double deviation =
            (high - low) / (3.0 + std::sqrt(static_cast<double>(good_count)));
        std::normal_distribution<double> distribution(center_value, deviation);
        double value = std::clamp(distribution(generator), low, high);
        if (parameter.logarithmic) value = std::exp(value);
        if (parameter.step > 0.0) {
            value = parameter.minimum +
                    std::round((value - parameter.minimum) / parameter.step) *
                        parameter.step;
        }
        value = std::clamp(value, parameter.minimum, parameter.maximum);
        sampled[parameter.name] = parameter.kind == ParameterKind::Integer
                                      ? nlohmann::json(static_cast<std::int64_t>(
                                            std::llround(value)))
                                      : nlohmann::json(value);
    }
    return sampled;
}

std::optional<double> value_at_step(const StudyTrial& trial,
                                    std::uint64_t step) {
    std::optional<double> value;
    for (const TrialPoint& point : trial.intermediate) {
        if (point.step > step) break;
        value = point.value;
    }
    return value;
}

double pearson_importance(const std::vector<double>& parameter,
                          const std::vector<double>& objective) {
    if (parameter.size() < 2 || parameter.size() != objective.size()) return 0.0;
    const double parameter_mean =
        std::accumulate(parameter.begin(), parameter.end(), 0.0) /
        static_cast<double>(parameter.size());
    const double objective_mean =
        std::accumulate(objective.begin(), objective.end(), 0.0) /
        static_cast<double>(objective.size());
    double covariance = 0.0;
    double parameter_variance = 0.0;
    double objective_variance = 0.0;
    for (std::size_t index = 0; index < parameter.size(); ++index) {
        const double parameter_delta = parameter[index] - parameter_mean;
        const double objective_delta = objective[index] - objective_mean;
        covariance += parameter_delta * objective_delta;
        parameter_variance += parameter_delta * parameter_delta;
        objective_variance += objective_delta * objective_delta;
    }
    if (parameter_variance == 0.0 || objective_variance == 0.0) return 0.0;
    return std::abs(covariance /
                    std::sqrt(parameter_variance * objective_variance));
}

constexpr double kDefaultTrialCostSeconds = 60.0;

double estimated_trial_cost(const StudyValue& value) {
    double estimate = 0.0;
    for (const StudyTrial& trial : value.trials) {
        if (trial.state == TrialState::Complete && trial.cost > 0.0) {
            estimate = std::max(estimate, trial.cost);
        }
    }
    return estimate > 0.0 ? estimate : kDefaultTrialCostSeconds;
}

bool trial_cost_fits(const StudyValue& value,
                     std::size_t reserved_trials = 0) {
    if (value.spec.budgets.cost_cap <= 0.0) return true;
    const double estimate = estimated_trial_cost(value);
    const double reserved =
        estimate * static_cast<double>(reserved_trials + 1);
    return value.spent_cost + reserved <= value.spec.budgets.cost_cap;
}

}  // namespace

std::string_view to_string(StudyDirection value) {
    return value == StudyDirection::Minimize ? "minimize" : "maximize";
}

std::string_view to_string(StudyStatus value) {
    switch (value) {
        case StudyStatus::Running: return "running";
        case StudyStatus::Stopped: return "stopped";
        case StudyStatus::Complete: return "complete";
        case StudyStatus::Failed: return "failed";
    }
    return "stopped";
}

std::string_view to_string(TrialState value) {
    switch (value) {
        case TrialState::Running: return "running";
        case TrialState::Pruned: return "pruned";
        case TrialState::Failed: return "failed";
        case TrialState::Complete: return "complete";
    }
    return "failed";
}

std::string_view to_string(SamplerKind value) {
    switch (value) {
        case SamplerKind::Grid: return "grid";
        case SamplerKind::Random: return "random";
        case SamplerKind::Tpe: return "tpe";
    }
    return "random";
}

std::string_view to_string(PrunerKind value) {
    switch (value) {
        case PrunerKind::None: return "none";
        case PrunerKind::Median: return "median";
        case PrunerKind::Asha: return "asha";
    }
    return "none";
}

std::string_view to_string(ParameterKind value) {
    switch (value) {
        case ParameterKind::Integer: return "int";
        case ParameterKind::Float: return "float";
        case ParameterKind::Categorical: return "categorical";
    }
    return "float";
}

nlohmann::json to_json(const StudyValue& value) {
    nlohmann::json trials = nlohmann::json::array();
    for (const StudyTrial& trial : value.trials) {
        nlohmann::json item{{"number", trial.number},
                            {"run", trial.run},
                            {"params", trial.params},
                            {"state", to_string(trial.state)},
                            {"seed", trial.seed},
                            {"cost", trial.cost},
                            {"intermediate", nlohmann::json::array()},
                            {"reason", trial.reason},
                            {"model", trial.model}};
        if (trial.objective) item["objective"] = *trial.objective;
        for (const TrialPoint& point : trial.intermediate) {
            item["intermediate"].push_back(
                {{"step", point.step}, {"value", point.value}});
        }
        trials.push_back(std::move(item));
    }
    return {{"spec", spec_json(value.spec)},
            {"status", to_string(value.status)},
            {"created", value.created},
            {"updated", value.updated},
            {"trials", std::move(trials)},
            {"spent_cost", value.spent_cost},
            {"stop_reason", value.stop_reason},
            {"failure_reason", value.failure_reason},
            {"promoted_model", value.promoted_model}};
}

const nlohmann::json& study_value_schema() { return schema_document(); }

bool validate_study_value(const nlohmann::json& value, std::string* error) {
    static nlohmann::json_schema::json_validator validator(schema_document());
    CollectingErrorHandler handler;
    validator.validate(value, handler);
    if (!handler.failed) return true;
    if (error != nullptr) *error = handler.reason;
    return false;
}

bool validate_study_spec(const StudySpec& spec, std::string* error) {
    auto fail = [&](std::string reason) {
        if (error != nullptr) *error = std::move(reason);
        return false;
    };
    if (spec.name.empty()) return fail("/name: a study name is required");
    if (spec.methodology.empty()) {
        return fail("/methodology: a methodology is required");
    }
    if (spec.objective.metric.empty()) {
        return fail("/objective/metric: an objective metric is required");
    }
    std::string split = spec.objective.validation_split;
    std::ranges::transform(split, split.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    const bool is_test_split = split == "test" || split.starts_with("test_") ||
                               split.ends_with("_test");
    if (split.empty() || is_test_split) {
        return fail("/objective/validation_split: must name a non-test validation split");
    }
    if (spec.search_space.empty()) {
        return fail("/search_space: at least one parameter is required");
    }
    std::set<std::string> names;
    for (const StudyParameter& parameter : spec.search_space) {
        if (parameter.name.empty() || !names.insert(parameter.name).second) {
            return fail("/search_space: parameter names must be non-empty and unique");
        }
        if (parameter.kind == ParameterKind::Categorical &&
            parameter.choices.empty()) {
            return fail("/search_space/" + parameter.name +
                        "/choices: categorical parameters require choices");
        }
        if (parameter.kind != ParameterKind::Categorical &&
            (!(parameter.minimum < parameter.maximum) ||
             (parameter.logarithmic && parameter.minimum <= 0.0))) {
            return fail("/search_space/" + parameter.name +
                        ": invalid numeric bounds");
        }
        if (!parameter.condition_parameter.empty() &&
            !names.contains(parameter.condition_parameter)) {
            return fail("/search_space/" + parameter.name +
                        "/condition_parameter: parent must appear first");
        }
    }
    if (spec.budgets.max_trials == 0 ||
        spec.budgets.max_parallel_trials == 0) {
        return fail("/budgets: max_trials and max_parallel_trials must be positive");
    }
    if (spec.differential_privacy &&
        !std::ranges::contains(spec.released_metrics, spec.objective.metric)) {
        return fail("/objective/metric: a differentially private study may optimize only a released metric");
    }
    return true;
}

std::optional<StudyValue> parse_study_value(const nlohmann::json& value,
                                            std::string* error) {
    if (!validate_study_value(value, error)) return std::nullopt;
    StudyValue parsed;
    parsed.spec = parse_spec(value.at("spec"));
    if (!validate_study_spec(parsed.spec, error)) return std::nullopt;
    parsed.status = *parse_enum<StudyStatus>(value.at("status").get<std::string>());
    parsed.created = value.at("created").get<std::string>();
    parsed.updated = value.at("updated").get<std::string>();
    parsed.spent_cost = value.at("spent_cost").get<double>();
    parsed.stop_reason = value.at("stop_reason").get<std::string>();
    parsed.failure_reason = value.value("failure_reason", std::string{});
    parsed.promoted_model = value.at("promoted_model").get<std::string>();
    for (const nlohmann::json& item : value.at("trials")) {
        StudyTrial trial;
        trial.number = item.at("number").get<std::size_t>();
        trial.run = item.at("run").get<std::string>();
        trial.params = item.at("params");
        trial.state =
            *parse_enum<TrialState>(item.at("state").get<std::string>());
        if (item.contains("objective")) {
            trial.objective = item.at("objective").get<double>();
        }
        trial.seed = item.at("seed").get<std::uint64_t>();
        trial.cost = item.at("cost").get<double>();
        trial.reason = item.at("reason").get<std::string>();
        trial.model = item.at("model").get<std::string>();
        for (const nlohmann::json& point : item.at("intermediate")) {
            trial.intermediate.push_back(
                {point.at("step").get<std::uint64_t>(),
                 point.at("value").get<double>()});
        }
        parsed.trials.push_back(std::move(trial));
    }
    return parsed;
}

Study::Study(std::string id, StudyValue value, fs::path dir,
             zetta::cluster::ReplicationManager* replication)
    : id_(std::move(id)),
      value_(std::move(value)),
      dir_(std::move(dir)),
      replication_(replication) {}

StudyTrial* Study::find_trial(const std::string& run) {
    const auto found = std::ranges::find(value_.trials, run, &StudyTrial::run);
    return found == value_.trials.end() ? nullptr : &*found;
}

bool Study::add_trial(StudyTrial trial, std::string* error) {
    if (value_.status != StudyStatus::Running) {
        if (error != nullptr) *error = "/study: is not running";
        return false;
    }
    if (trial.run.empty() || find_trial(trial.run) != nullptr) {
        if (error != nullptr) *error = "/run: must name a new Run";
        return false;
    }
    value_.trials.push_back(std::move(trial));
    capture_and_persist();
    return true;
}

bool Study::report_intermediate(const std::string& run, std::uint64_t step,
                                double value, std::string* error) {
    StudyTrial* trial = find_trial(run);
    if (trial == nullptr || trial->state != TrialState::Running) {
        if (error != nullptr) *error = "/run: no running trial with that Run";
        return false;
    }
    if (!std::isfinite(value) ||
        (!trial->intermediate.empty() &&
         step <= trial->intermediate.back().step)) {
        if (error != nullptr) *error = "/step: must increase and value must be finite";
        return false;
    }
    trial->intermediate.push_back({step, value});
    capture_and_persist();
    return true;
}

bool Study::finish_trial(const std::string& run, TrialState state,
                         std::optional<double> objective, double cost,
                         std::string reason, std::string model,
                         std::string* error) {
    StudyTrial* trial = find_trial(run);
    if (trial == nullptr || trial->state != TrialState::Running) {
        if (error != nullptr) *error = "/run: no running trial with that Run";
        return false;
    }
    if (state == TrialState::Running || cost < 0.0 ||
        (objective && !std::isfinite(*objective))) {
        if (error != nullptr) *error = "/trial: invalid terminal result";
        return false;
    }
    trial->state = state;
    trial->objective = objective;
    trial->cost = cost;
    trial->reason = std::move(reason);
    trial->model = std::move(model);
    value_.spent_cost += cost;
    capture_and_persist();
    return true;
}

bool Study::stop_trial(const std::string& run, std::string reason,
                       std::string* error) {
    return finish_trial(run, TrialState::Failed, std::nullopt, 0.0,
                        std::move(reason), {}, error);
}

bool Study::reset_trial_for_retry(const std::string& run,
                                  std::string* error) {
    StudyTrial* trial = find_trial(run);
    if (trial == nullptr || trial->state != TrialState::Running) {
        if (error != nullptr) *error = "/run: no running trial with that Run";
        return false;
    }
    trial->objective.reset();
    trial->cost = 0.0;
    trial->intermediate.clear();
    trial->reason.clear();
    trial->model.clear();
    capture_and_persist();
    return true;
}

void Study::stop(std::string reason) {
    value_.status = StudyStatus::Stopped;
    value_.stop_reason = std::move(reason);
    for (StudyTrial& trial : value_.trials) {
        if (trial.state != TrialState::Running) continue;
        trial.state = TrialState::Failed;
        trial.reason = value_.stop_reason;
    }
    capture_and_persist();
}

void Study::set_complete(std::string reason) {
    value_.status = StudyStatus::Complete;
    value_.stop_reason = std::move(reason);
    capture_and_persist();
}

void Study::fail(std::string stop_reason, std::string failure_reason) {
    value_.status = StudyStatus::Failed;
    value_.stop_reason = std::move(stop_reason);
    value_.failure_reason = std::move(failure_reason);
    for (StudyTrial& trial : value_.trials) {
        if (trial.state != TrialState::Running) continue;
        trial.state = TrialState::Failed;
        trial.reason = value_.failure_reason;
    }
    capture_and_persist();
}

void Study::set_promoted_model(std::string model) {
    value_.promoted_model = std::move(model);
    capture_and_persist();
}

void Study::set_project_pipeline(std::string project,
                                 std::string pipeline) {
    if (value_.spec.project == project && value_.spec.pipeline == pipeline) {
        return;
    }
    value_.spec.project = std::move(project);
    value_.spec.pipeline = std::move(pipeline);
    capture_and_persist();
}

void Study::apply_value(StudyValue value) {
    value_ = std::move(value);
    persist_meta();
}

nlohmann::json Study::metadata() const { return to_json(value_); }

void Study::capture_and_persist() {
    value_.updated = zetta::util::now_iso8601();
    if (replication_ != nullptr) {
        replication_->set(catalog::study_key(id_), metadata());
    }
    persist_meta();
}

void Study::persist_meta() const {
    nlohmann::json value = metadata();
    value["id"] = id_;
    std::ofstream output(dir_ / "study.json", std::ios::trunc);
    output << value.dump(2) << '\n';
}

nlohmann::json StudyOptimizer::sample(const StudySpec& spec,
                                      const std::vector<StudyTrial>& trials,
                                      std::size_t trial_number) {
    if (spec.sampler == SamplerKind::Grid) {
        nlohmann::json sampled = nlohmann::json::object();
        std::size_t index = trial_number;
        for (const StudyParameter& parameter : spec.search_space) {
            if (active_parameter(parameter, sampled)) {
                sampled[parameter.name] = grid_value(parameter, index);
            }
        }
        return sampled;
    }
    const std::vector<const StudyTrial*> completed = completed_trials(trials);
    if (spec.sampler == SamplerKind::Tpe && completed.size() >= 4) {
        return tpe_sample(spec, completed, trial_number);
    }
    std::mt19937_64 generator(spec.seed + trial_number * 0x9e3779b97f4a7c15ULL);
    nlohmann::json sampled = nlohmann::json::object();
    for (const StudyParameter& parameter : spec.search_space) {
        if (active_parameter(parameter, sampled)) {
            sampled[parameter.name] = random_value(parameter, generator);
        }
    }
    return sampled;
}

bool StudyOptimizer::should_prune(const StudySpec& spec,
                                  const std::vector<StudyTrial>& trials,
                                  const StudyTrial& trial) {
    if (spec.pruner == PrunerKind::None || spec.differential_privacy ||
        trial.intermediate.empty()) {
        return false;
    }
    const TrialPoint current = trial.intermediate.back();
    std::vector<double> peers;
    for (const StudyTrial& other : trials) {
        if (other.run == trial.run || other.state == TrialState::Running) continue;
        if (const std::optional<double> value = value_at_step(other, current.step)) {
            peers.push_back(*value);
        }
    }
    if (peers.size() < 3) return false;
    std::ranges::sort(peers);
    if (spec.pruner == PrunerKind::Median) {
        const double median = peers[peers.size() / 2];
        return better(spec.objective.direction, median, current.value);
    }
    std::uint64_t rung = 1;
    while (rung * 3 <= current.step) rung *= 3;
    if (current.step < rung) return false;
    const std::size_t surviving = std::max<std::size_t>(1, peers.size() / 3);
    if (spec.objective.direction == StudyDirection::Maximize) {
        const double cutoff = peers[peers.size() - surviving];
        return current.value < cutoff;
    }
    const double cutoff = peers[surviving - 1];
    return current.value > cutoff;
}

nlohmann::json StudyOptimizer::analysis(const StudyValue& value) {
    std::vector<const StudyTrial*> completed = completed_trials(value.trials);
    std::ranges::sort(completed, [&](const StudyTrial* left,
                                     const StudyTrial* right) {
        return better(value.spec.objective.direction, *left->objective,
                      *right->objective);
    });
    nlohmann::json best_trial = nullptr;
    if (!completed.empty()) {
        best_trial = {{"run", completed.front()->run},
                      {"number", completed.front()->number},
                      {"value", *completed.front()->objective},
                      {"params", completed.front()->params}};
    }
    nlohmann::json history = nlohmann::json::array();
    std::optional<double> incumbent;
    for (const StudyTrial& trial : value.trials) {
        if (!trial.objective) continue;
        if (!incumbent || better(value.spec.objective.direction,
                                 *trial.objective, *incumbent)) {
            incumbent = trial.objective;
        }
        history.push_back({{"number", trial.number},
                           {"run", trial.run},
                           {"value", *trial.objective},
                           {"best", *incumbent}});
    }
    nlohmann::json importances = nlohmann::json::array();
    double importance_total = 0.0;
    for (const StudyParameter& parameter : value.spec.search_space) {
        std::vector<double> parameter_values;
        std::vector<double> objectives;
        std::map<std::string, double> categorical_codes;
        for (const StudyTrial* trial : completed) {
            const auto found = trial->params.find(parameter.name);
            if (found == trial->params.end()) continue;
            if (found->is_number()) {
                parameter_values.push_back(numeric_value(*found));
            } else {
                const std::string key = found->dump();
                if (!categorical_codes.contains(key)) {
                    categorical_codes[key] =
                        static_cast<double>(categorical_codes.size());
                }
                parameter_values.push_back(categorical_codes[key]);
            }
            objectives.push_back(*trial->objective);
        }
        const double score = pearson_importance(parameter_values, objectives);
        importance_total += score;
        importances.push_back({{"parameter", parameter.name}, {"importance", score}});
    }
    if (importance_total > 0.0) {
        for (nlohmann::json& item : importances) {
            item["importance"] =
                item.at("importance").get<double>() / importance_total;
        }
    }
    nlohmann::json coordinates = nlohmann::json::array();
    for (const StudyTrial* trial : completed) {
        coordinates.push_back({{"run", trial->run},
                               {"params", trial->params},
                               {"value", *trial->objective}});
    }
    nlohmann::json leaderboard = nlohmann::json::array();
    for (std::size_t index = 0; index < completed.size(); ++index) {
        const StudyTrial& trial = *completed[index];
        double deviation = 0.0;
        if (trial.intermediate.size() > 1) {
            const double mean = std::accumulate(
                                    trial.intermediate.begin(),
                                    trial.intermediate.end(), 0.0,
                                    [](double sum, const TrialPoint& point) {
                                        return sum + point.value;
                                    }) /
                                static_cast<double>(trial.intermediate.size());
            for (const TrialPoint& point : trial.intermediate) {
                deviation += (point.value - mean) * (point.value - mean);
            }
            deviation = std::sqrt(deviation /
                                  static_cast<double>(trial.intermediate.size() - 1)) /
                        std::sqrt(static_cast<double>(trial.intermediate.size()));
        }
        leaderboard.push_back(
            {{"rank", index + 1},
             {"run", trial.run},
             {"value", *trial.objective},
             {"confidence_low", *trial.objective - 1.96 * deviation},
             {"confidence_high", *trial.objective + 1.96 * deviation}});
    }
    return {{"best_trial", std::move(best_trial)},
            {"optimization_history", std::move(history)},
            {"parameter_importances", std::move(importances)},
            {"parallel_coordinates", std::move(coordinates)},
            {"leaderboard", std::move(leaderboard)}};
}

StudyRunResult run_study(Node& node, Study& study,
                         compute::WorkStealingScheduler& scheduler,
                         const TrialEvaluator& evaluate) {
    StudyRunResult result;
    const auto started = std::chrono::steady_clock::now();
    std::mutex fallback_owner_lock;
    while (study.status() == StudyStatus::Running &&
           study.value().trials.size() < study.spec().budgets.max_trials) {
        if (study.spec().budgets.max_wall_time_ms > 0 &&
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                    .count() >=
                static_cast<std::int64_t>(study.spec().budgets.max_wall_time_ms)) {
            study.stop("max wall time reached");
            break;
        }
        if (!trial_cost_fits(study.value())) {
            study.stop("cost cap reached");
            break;
        }
        const std::size_t remaining = study.spec().budgets.max_trials -
                                      study.value().trials.size();
        const std::size_t desired = std::min(
            remaining, study.spec().budgets.max_parallel_trials);
        std::size_t batch_size = 0;
        while (batch_size < desired &&
               trial_cost_fits(study.value(), batch_size)) {
            ++batch_size;
        }
        struct PendingTrial {
            std::string run;
            nlohmann::json params;
            std::uint64_t seed = 0;
        };
        std::vector<PendingTrial> pending;
        pending.reserve(batch_size);
        for (std::size_t offset = 0; offset < batch_size; ++offset) {
            const std::size_t number = study.value().trials.size();
            const nlohmann::json params = StudyOptimizer::sample(
                study.spec(), study.value().trials, number);
            const std::uint64_t seed = study.spec().seed + number;
            std::shared_ptr<zetta::pipeline::Run> run =
                node.create_run(study.spec().project, study.spec().pipeline);
            if (!run) {
                study.stop("could not create a trial Run");
                break;
            }
            run->set_status(zetta::pipeline::RunStatus::Running);
            nlohmann::json recorded_params = params;
            recorded_params["study.id"] = study.id();
            recorded_params["study.seed"] = seed;
            std::string error;
            run->log_params(recorded_params, &error);
            if (study.spec().differential_privacy) {
                run->suppress_metrics_for_privacy();
            }
            StudyTrial trial;
            trial.number = number;
            trial.run = run->id();
            trial.params = params;
            trial.seed = seed;
            if (!study.add_trial(std::move(trial), &error)) {
                study.stop(error);
                break;
            }
            publish_progress(node, study, "trial_started", run->id());
            pending.push_back({run->id(), params, seed});
            ++result.launched;
        }
        if (pending.empty()) break;
        const std::vector<TrialOutcome> outcomes = scheduler.parallel_map(
            pending, [&](const PendingTrial& pending_trial) {
                auto stop_requested =
                    std::make_shared<std::atomic<bool>>(false);
                auto pruned = std::make_shared<std::atomic<bool>>(false);
                auto wall_time_reached =
                    std::make_shared<std::atomic<bool>>(false);
                const TrialReporter reporter =
                    [&](std::uint64_t step, double value) {
                        bool keep_running = true;
                        std::lock_guard<std::mutex> lock(fallback_owner_lock);
                        node.run_on_owner([&] {
                            if (study.spec().budgets.max_wall_time_ms > 0 &&
                                std::chrono::duration_cast<
                                    std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - started)
                                        .count() >=
                                    static_cast<std::int64_t>(
                                        study.spec().budgets.max_wall_time_ms)) {
                                wall_time_reached->store(true);
                                stop_requested->store(true);
                                keep_running = false;
                                return;
                            }
                            std::string error;
                            if (!study.report_intermediate(
                                    pending_trial.run, step, value, &error)) {
                                keep_running = false;
                                return;
                            }
                            publish_progress(node, study, "trial_progress",
                                             pending_trial.run, step, value);
                            if (!study.spec().differential_privacy) {
                                node.log_run_metrics(
                                    pending_trial.run, step,
                                    {{study.spec().objective.metric, value}}, {},
                                    &error);
                            }
                            const auto found = std::ranges::find(
                                study.value().trials, pending_trial.run,
                                &StudyTrial::run);
                            if (found != study.value().trials.end() &&
                                StudyOptimizer::should_prune(
                                    study.spec(), study.value().trials, *found)) {
                                study.finish_trial(
                                    pending_trial.run, TrialState::Pruned,
                                    std::nullopt, 0.0,
                                    "pruned by " +
                                        std::string(to_string(study.spec().pruner)));
                                if (const auto run =
                                        node.find_run(pending_trial.run)) {
                                    run->add_log(
                                        {zetta::util::now_iso8601(), "info",
                                         "trial pruned at step " +
                                             std::to_string(step)});
                                    run->set_status(
                                        zetta::pipeline::RunStatus::Failed);
                                }
                                pruned->store(true);
                                stop_requested->store(true);
                                keep_running = false;
                                publish_progress(node, study, "trial_pruned",
                                                 pending_trial.run, step, value);
                            }
                        });
                        return keep_running;
                    };
                TrialOutcome outcome = evaluate(
                    pending_trial.params, pending_trial.seed, pending_trial.run,
                    reporter, *stop_requested);
                if (pruned->load()) outcome.pruned = true;
                if (wall_time_reached->load()) {
                    outcome.complete = false;
                    outcome.pruned = false;
                    outcome.objective.reset();
                    outcome.reason = "max wall time reached";
                }
                return outcome;
            });
        for (std::size_t index = 0; index < pending.size(); ++index) {
            const PendingTrial& pending_trial = pending[index];
            const TrialOutcome& outcome = outcomes[index];
            const auto found = std::ranges::find(
                study.value().trials, pending_trial.run, &StudyTrial::run);
            if (found == study.value().trials.end() ||
                found->state != TrialState::Running) {
                if (found != study.value().trials.end() &&
                    found->state == TrialState::Pruned) {
                    ++result.pruned;
                } else {
                    ++result.failed;
                }
                continue;
            }
            TrialState state = TrialState::Failed;
            if (outcome.pruned) state = TrialState::Pruned;
            else if (outcome.complete && outcome.objective) {
                state = TrialState::Complete;
            }
            study.finish_trial(pending_trial.run, state, outcome.objective,
                               outcome.cost, outcome.reason, outcome.model);
            if (const auto run = node.find_run(pending_trial.run)) {
                run->set_status(state == TrialState::Complete
                                    ? zetta::pipeline::RunStatus::Succeeded
                                    : zetta::pipeline::RunStatus::Failed);
            }
            if (state == TrialState::Complete) ++result.completed;
            else if (state == TrialState::Pruned) ++result.pruned;
            else ++result.failed;
            publish_progress(node, study,
                             state == TrialState::Complete
                                 ? "trial_completed"
                                 : state == TrialState::Pruned
                                       ? "trial_pruned"
                                       : "trial_failed",
                             pending_trial.run);
        }
        if (std::ranges::any_of(outcomes, [](const TrialOutcome& outcome) {
                return outcome.reason == "max wall time reached";
            })) {
            study.stop("max wall time reached");
        }
    }
    if (study.status() == StudyStatus::Running) {
        study.set_complete("max trials reached");
    }
    result.stop_reason = study.value().stop_reason;
    return result;
}

namespace {

struct RealTrialPlan {
    ir::ExecutionGraph graph;
    usecases::churn::ChurnTableInput table;
    usecases::churn::ChurnSolutionOptions options;
    bool dataset_level = false;
};

struct ResolvedStudyDataset {
    data::DatasetPath path;
    data::ResolvedDataset dataset;
    std::vector<nlohmann::json> rows;
};

std::shared_ptr<pipeline::Project> study_project(
    Node& node, const std::string& reference) {
    for (const std::shared_ptr<pipeline::Project>& project : node.projects()) {
        if (project &&
            (project->id() == reference || project->name() == reference)) {
            return project;
        }
    }
    return nullptr;
}

double study_holdout_fraction(const StudySpec& spec) {
    double fraction = 0.2;
    const auto parameter = std::ranges::find_if(
        spec.search_space, [](const StudyParameter& candidate) {
            return candidate.name == "holdout_fraction" ||
                   candidate.name == "gbdt.holdout_fraction";
        });
    if (parameter != spec.search_space.end() &&
        parameter->kind == ParameterKind::Float) {
        fraction = std::clamp(fraction, parameter->minimum,
                              parameter->maximum);
    }
    return fraction;
}

std::optional<double> binary_label(const nlohmann::json& value) {
    if (value.is_boolean()) return value.get<bool>() ? 1.0 : 0.0;
    if (!value.is_number()) return std::nullopt;
    const double label = value.get<double>();
    if (label == 0.0 || label == 1.0) return label;
    return std::nullopt;
}

std::string displayed_time(const nlohmann::json& value) {
    return value.is_string() ? value.get<std::string>() : value.dump();
}

std::optional<ResolvedStudyDataset> resolve_study_dataset(
    Node& node, const StudySpec& spec, std::string& error) {
    std::string dataset = spec.dataset;
    if (dataset.empty() && !spec.project.empty() && !spec.pipeline.empty()) {
        const std::shared_ptr<pipeline::Project> project =
            study_project(node, spec.project);
        if (!project) {
            error = "study project was not found";
            return std::nullopt;
        }
        const std::shared_ptr<pipeline::ProjectGraph> stored =
            project->find_graph_by_id(spec.pipeline)
                ? project->find_graph_by_id(spec.pipeline)
                : project->find_graph(spec.pipeline);
        if (!stored) {
            error = "study Pipeline was not found";
            return std::nullopt;
        }
        for (const std::shared_ptr<ir::Node>& source :
             stored->graph().nodes_of_kind(ir::StepKind::Connect)) {
            const nlohmann::json& attributes = source->attributes;
            const std::string database = attributes.is_object()
                                             ? attributes.value(
                                                   "source", std::string{})
                                             : std::string{};
            const std::string table = attributes.is_object()
                                          ? attributes.value(
                                                "table", std::string{})
                                          : std::string{};
            if (database.empty() || table.empty()) continue;
            dataset = database + "." + table;
            break;
        }
    }
    if (dataset.empty()) {
        error = "study dataset was not supplied and the Pipeline names none";
        return std::nullopt;
    }

    ResolvedStudyDataset result;
    result.path = data::parse_dataset_path(dataset);
    if (result.path.kind != data::DatasetPathKind::Table) {
        error = "study datasets must name a landed database.table";
        return std::nullopt;
    }
    const data::DatasetSampler sampler = data::default_dataset_sampler(node);
    const data::SampledDataset sample =
        data::sample_dataset(node, result.path, sampler);
    if (!sample.ok()) {
        error = sample.error;
        return std::nullopt;
    }
    std::string read_error;
    result.rows = sampler(sample.database, sample.table, 0, read_error);
    if (!read_error.empty()) {
        error = "dataset: could not read full table '" + dataset +
                "': " + read_error;
        return std::nullopt;
    }
    if (result.rows.size() != sample.rows) {
        error = "dataset: full table read returned " +
                std::to_string(result.rows.size()) + " of " +
                std::to_string(sample.rows) + " rows";
        return std::nullopt;
    }

    data::DatasetContract overrides;
    if (std::ranges::contains(sample.columns, "observed_at")) {
        bool found_time = false;
        double latest = 0.0;
        for (const nlohmann::json& row : result.rows) {
            const auto value = row.find("observed_at");
            if (value == row.end()) {
                error = "dataset: a row is missing observed_at";
                return std::nullopt;
            }
            const std::optional<double> seconds =
                util::parse_time_seconds(*value);
            if (!seconds) {
                error = "dataset: observed_at contains an invalid time value";
                return std::nullopt;
            }
            latest = found_time ? std::max(latest, *seconds) : *seconds;
            found_time = true;
        }
        if (found_time) {
            overrides.time_column = "observed_at";
            overrides.as_of = latest;
        }
    }
    result.dataset = data::resolve_dataset(
        node, result.path, overrides, sampler);
    if (!result.dataset.ok()) {
        error = result.dataset.error;
        return std::nullopt;
    }
    return result;
}

bool validate_resolved_study_data(const StudySpec& spec,
                                  const ResolvedStudyDataset& resolved,
                                  std::string& error) {
    struct LabelledRow {
        double time = 0.0;
        double label = 0.0;
        nlohmann::json source_time;
    };
    std::vector<LabelledRow> labelled;
    labelled.reserve(resolved.rows.size());
    const data::DatasetContract& contract = resolved.dataset.contract;
    for (const nlohmann::json& row : resolved.rows) {
        const auto label_value = row.find(contract.label_column);
        if (label_value == row.end()) {
            error = "dataset: a row is missing label column '" +
                    contract.label_column + "'";
            return false;
        }
        const std::optional<double> label = binary_label(*label_value);
        if (!label) {
            error = "dataset: label column '" + contract.label_column +
                    "' must contain binary 0/1 or boolean values";
            return false;
        }
        LabelledRow item;
        item.label = *label;
        if (!contract.time_column.empty()) {
            const auto time_value = row.find(contract.time_column);
            if (time_value == row.end()) {
                error = "dataset: a row is missing time column '" +
                        contract.time_column + "'";
                return false;
            }
            const std::optional<double> time =
                util::parse_time_seconds(*time_value);
            if (!time) {
                error = "dataset: time column '" + contract.time_column +
                        "' contains an invalid value";
                return false;
            }
            if (*time > contract.as_of) continue;
            item.time = *time;
            item.source_time = *time_value;
        }
        labelled.push_back(std::move(item));
    }
    if (labelled.size() < 2) {
        error = "the validation split needs at least two point-in-time rows";
        return false;
    }

    const auto count_classes = [](auto first, auto last) {
        std::array<std::size_t, 2> counts{0, 0};
        for (; first != last; ++first) {
            ++counts[first->label > 0.5 ? 1 : 0];
        }
        return counts;
    };
    if (contract.time_column.empty()) {
        const std::array<std::size_t, 2> counts =
            count_classes(labelled.begin(), labelled.end());
        if (counts[0] > 0 && counts[1] > 0) return true;
        error = "the training dataset has " + std::to_string(counts[1]) +
                " positive labels and " + std::to_string(counts[0]) +
                " negative labels";
        return false;
    }

    std::ranges::stable_sort(labelled, {}, &LabelledRow::time);
    const std::size_t held_out = std::clamp<std::size_t>(
        static_cast<std::size_t>(std::ceil(
            static_cast<double>(labelled.size()) *
            study_holdout_fraction(spec))),
        1, labelled.size() - 1);
    const double cutoff = labelled[labelled.size() - held_out].time;
    const auto split = std::ranges::lower_bound(
        labelled, cutoff, {}, &LabelledRow::time);
    const std::array<std::size_t, 2> training =
        count_classes(labelled.begin(), split);
    const std::array<std::size_t, 2> validation =
        count_classes(split, labelled.end());
    if (training[0] == 0 || training[1] == 0) {
        error = "the out-of-time training split has " +
                std::to_string(training[1]) + " positive labels and " +
                std::to_string(training[0]) + " negative labels";
        if (training[1] == 0) {
            const auto first_positive = std::ranges::find_if(
                split, labelled.end(), [](const LabelledRow& row) {
                    return row.label > 0.5;
                });
            if (first_positive != labelled.end()) {
                error += "; positives only appear after " +
                         displayed_time(first_positive->source_time);
            }
        }
        return false;
    }
    if (validation[0] == 0 || validation[1] == 0) {
        error = "the out-of-time validation split has " +
                std::to_string(validation[1]) + " positive labels and " +
                std::to_string(validation[0]) + " negative labels at cutoff " +
                displayed_time(split->source_time);
        return false;
    }
    return true;
}

std::optional<ml::GbdtParams> study_gbdt_params(
    const nlohmann::json& sampled, std::string& error) {
    nlohmann::json values =
        usecases::churn::default_churn_gbdt_params().to_json();
    values["holdout_fraction"] = 0.2;
    for (const auto& [name, value] : sampled.items()) {
        if (name == "model_family") {
            if (!value.is_string() || value.get<std::string>() != "gbdt") {
                error = "model_family: only canonical 'gbdt' is supported";
                return std::nullopt;
            }
            continue;
        }
        std::string parameter = name;
        if (parameter.starts_with("gbdt.")) {
            parameter.erase(0, std::string{"gbdt."}.size());
        }
        static const std::set<std::string> supported = {
            "num_trees", "max_depth", "max_bins", "learning_rate",
            "l2_regularization", "min_split_gain", "min_child_hessian",
            "holdout_fraction"};
        if (!supported.contains(parameter)) {
            error = "unsupported M-1 hyperparameter '" + name + "'";
            return std::nullopt;
        }
        values[parameter] = value;
    }
    std::optional<ml::GbdtParams> parsed = ml::GbdtParams::from_json(values);
    if (!parsed) {
        error = "sampled M-1 hyperparameters are invalid";
    }
    return parsed;
}

std::optional<RealTrialPlan> prepare_real_trial(
    Node& node, const StudySpec& spec, const nlohmann::json& sampled,
    const std::string& run_id, const std::atomic<bool>* cancel,
    std::string& error) {
    RealTrialPlan plan;
    if (!spec.project.empty() || !spec.pipeline.empty()) {
        std::shared_ptr<pipeline::Project> project;
        std::shared_ptr<pipeline::ProjectGraph> stored;
        node.run_on_owner([&] {
            project = study_project(node, spec.project);
            if (!project) return;
            stored = project->find_graph_by_id(spec.pipeline)
                         ? project->find_graph_by_id(spec.pipeline)
                         : project->find_graph(spec.pipeline);
            if (!stored) return;
            const std::optional<ir::ExecutionGraph> copied =
                ir::from_json(ir::to_json(stored->graph()), &error);
            if (!copied) return;
            plan.graph = *copied;
            plan.options.project = project->id();
            plan.options.graph = stored->id();
        });
        if (!project) {
            error = "study project was not found";
            return std::nullopt;
        }
        if (!stored) {
            error = "study Pipeline was not found";
            return std::nullopt;
        }
        if (!error.empty()) return std::nullopt;
    }
    const std::optional<ResolvedStudyDataset> resolved =
        resolve_study_dataset(node, spec, error);
    if (!resolved) return std::nullopt;
    plan.table.table_id = resolved->dataset.table_id;
    plan.table.dataset_name = resolved->dataset.database + "." +
                              resolved->dataset.table;
    plan.table.contract = resolved->dataset.contract;
    plan.table.notes = resolved->dataset.notes;
    plan.dataset_level = spec.project.empty() && spec.pipeline.empty();

    if (plan.graph.empty()) {
        plan.graph.add_node(ir::StepKind::Connect, "Load study dataset",
                            {{"source", resolved->path.database},
                             {"table", resolved->path.table}},
                            "load_study_dataset");
        plan.graph.add_node(ir::StepKind::Clean, "Build study features", {},
                            "build_study_features");
        plan.graph.add_node(
            ir::StepKind::Model, "Train study model",
            {{"model_kind", "gbdt"},
             {"algo", "gbdt"},
             {"target", resolved->dataset.contract.label_column}},
            "train_study_model");
        plan.graph.add_edge("load_study_dataset", "build_study_features");
        plan.graph.add_edge("build_study_features", "train_study_model");
    }

    const std::optional<ml::GbdtParams> params =
        study_gbdt_params(sampled, error);
    if (!params) return std::nullopt;
    plan.options.params = *params;
    plan.options.run_id = run_id;
    plan.options.cancel = cancel;
    plan.options.model_name = "study-" + run_id.substr(0, 8);
    plan.options.coordinate = true;
    return plan;
}

struct DatasetTrialResult {
    std::string model;
    std::string error;
};

DatasetTrialResult run_dataset_trial(Node& node, RealTrialPlan& plan,
                                     const std::string& run_id) {
    DatasetTrialResult outcome;
    compute::JobCoordinator* coordinator = node.job_coordinator();
    if (coordinator == nullptr) {
        outcome.error = "no compute plane attached to this node";
        return outcome;
    }
    if (coordinator->collectives().participants().size() != 1) {
        outcome.error =
            "dataset-level Study trials over partitioned tables currently "
            "require a single-member cluster";
        return outcome;
    }

    ddf::DataFrameSchema schema;
    for (const std::string& feature :
         plan.table.contract.feature_columns) {
        schema.columns.push_back(
            {.name = feature,
             .field = feature,
             .dtype = compute::DataType::Float64});
    }
    schema.columns.push_back(
        {.name = plan.table.contract.label_column,
         .field = plan.table.contract.label_column,
         .dtype = compute::DataType::Int64});
    if (!plan.table.contract.time_column.empty()) {
        schema.columns.push_back(
            {.name = plan.table.contract.time_column,
             .field = plan.table.contract.time_column,
             .dtype = compute::DataType::Float64,
             .timestamp = true});
    }

    compute::CpuDevice device;
    ddf::LocalShardResult shard;
    node.run_on_owner([&] {
        shard = ddf::load_local_partitions(
            node, plan.table.table_id, schema, device);
    });
    if (!shard.ok()) {
        outcome.error = "load full Study table: " + shard.status.message;
        return outcome;
    }

    ddf::DataFrame frame = std::move(shard.frame);
    if (!plan.table.contract.time_column.empty()) {
        ddf::DataFrameResult filtered = ddf::filter(
            frame,
            {.column = plan.table.contract.time_column,
             .comparison = ddf::Comparison::LessEqual,
             .value = plan.table.contract.as_of});
        if (!filtered.ok()) {
            outcome.error = "filter Study table at point in time: " +
                            filtered.status.message;
            return outcome;
        }
        frame = std::move(filtered.frame);
    }

    std::vector<double> validation_times;
    if (!plan.table.contract.time_column.empty()) {
        validation_times.resize(frame.row_count());
        const compute::ComputeStatus time_status = frame.to_host<double>(
            plan.table.contract.time_column, validation_times);
        if (!time_status.ok()) {
            outcome.error = "read Study validation times: " +
                            time_status.message;
            return outcome;
        }
    }

    std::shared_ptr<compute::ComputeJob> job;
    node.run_on_owner([&] {
        job = node.create_job(compute::JobKind::Train,
                              plan.options.project,
                              plan.options.graph, run_id);
    });
    if (!job) {
        outcome.error = "could not create Study train job";
        return outcome;
    }
    const DistributedTrainResult trained = run_train_job(
        *coordinator, *job, frame,
        plan.table.contract.feature_columns,
        plan.table.contract.label_column, plan.options.params, &node,
        plan.options.model_name, "study." + run_id, {},
        kDefaultGbdtTimeout, {}, run_id, plan.options.training_progress,
        validation_times);
    if (!trained.ok()) {
        node.run_on_owner([&] { outcome.error = job->detail(); });
        return outcome;
    }
    outcome.model = trained.model_id;
    node.run_on_owner([&] {
        const std::shared_ptr<Model> model = node.find_model(outcome.model);
        if (model) {
            nlohmann::json metrics = model->metrics();
            metrics["dataset_rows"] = frame.row_count();
            model->set_metrics(std::move(metrics));
        }
        const std::shared_ptr<pipeline::Run> run = node.find_run(run_id);
        if (run) {
            run->set_outputs({pipeline::RunOutput{
                "model", "Train study model", outcome.model}});
            pipeline::RunLineage lineage;
            lineage.sources = {"dataset:" + plan.table.dataset_name};
            run->set_lineage(std::move(lineage));
        }
    });
    return outcome;
}

std::optional<double> trial_objective(Node& node, const StudySpec& spec,
                                      const std::string& model_id,
                                      std::string& error) {
    const std::shared_ptr<Model> model = node.find_model(model_id);
    if (!model) {
        error = "trial completed without a registered model";
        return std::nullopt;
    }
    std::string metric = spec.objective.metric;
    const std::size_t dot = metric.rfind('.');
    if (dot != std::string::npos) metric.erase(0, dot + 1);
    const nlohmann::json& metrics = model->metrics();
    const std::string validation_metric = "held_out_" + metric;
    const auto found = metrics.find(validation_metric);
    if (found == metrics.end() || !found->is_number()) {
        error = "trial model did not report objective metric '" +
                spec.objective.metric + "' on validation split '" +
                spec.objective.validation_split + "'";
        return std::nullopt;
    }
    return found->get<double>();
}

std::size_t stable_hash(std::string_view value) {
    std::size_t hash = 1469598103934665603ull;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ull;
    }
    return hash;
}

bool wall_time_reached(const StudyValue& value) {
    if (value.spec.budgets.max_wall_time_ms == 0) return false;
    const std::optional<double> created =
        util::parse_iso8601_seconds(value.created);
    const std::optional<double> now =
        util::parse_iso8601_seconds(util::now_iso8601());
    return created && now &&
           (*now - *created) * 1000.0 >=
               static_cast<double>(value.spec.budgets.max_wall_time_ms);
}

bool deterministic_data_error(std::string_view reason) {
    return reason.contains("training labels are a single class") ||
           reason.contains("no training rows") ||
           reason.contains("label column") ||
           reason.contains("out-of-time") ||
           reason.contains("study dataset was not supplied");
}

}  // namespace

bool validate_study_data(Node& node, const StudySpec& spec,
                         std::string* error) {
    std::string reason;
    const std::optional<ResolvedStudyDataset> resolved =
        resolve_study_dataset(node, spec, reason);
    if (!resolved && spec.dataset.empty() && !spec.project.empty() &&
        !spec.pipeline.empty() &&
        reason == "study dataset was not supplied and the Pipeline names none") {
        return true;
    }
    if (!resolved ||
        !validate_resolved_study_data(spec, *resolved, reason)) {
        if (error != nullptr) *error = std::move(reason);
        return false;
    }
    return true;
}

struct StudyDriver::State {
    struct TrialResult {
        std::string run;
        std::optional<double> objective;
        double cost = 0.0;
        std::string model;
        std::string error;
        bool pruned = false;
    };

    struct ActiveStudy {
        std::string id;
        std::shared_ptr<std::atomic<bool>> cancel;
        std::shared_ptr<std::atomic<bool>> done;
        std::map<std::string, std::shared_ptr<std::atomic<bool>>> trial_cancels;
    };

    explicit State(Node& owned_node)
        : node(owned_node), driver_scheduler(1), trial_scheduler(4) {}

    Node& node;
    compute::WorkStealingScheduler driver_scheduler;
    compute::WorkStealingScheduler trial_scheduler;
    std::vector<ActiveStudy> active;
    bool stopping = false;

    ActiveStudy* find_active(const std::string& study_id);
    bool owns(const Study& study) const;
    std::vector<StudyTrial> prepare_batch(const std::shared_ptr<Study>& study,
                                          const StudyValue& snapshot);
    TrialResult execute_trial(const std::shared_ptr<Study>& study,
                              const StudyTrial& trial);
    void apply_results(const std::shared_ptr<Study>& study,
                       const std::vector<TrialResult>& results);
    bool finish_for_budget(const std::shared_ptr<Study>& study,
                           const StudyValue& snapshot);
    void drive(const std::string& study_id,
               const std::shared_ptr<std::atomic<bool>>& cancelled,
               const std::shared_ptr<std::atomic<bool>>& done);
    void launch(const std::shared_ptr<Study>& study);
};

StudyDriver::State::ActiveStudy* StudyDriver::State::find_active(
    const std::string& study_id) {
    const auto found = std::ranges::find(active, study_id, &ActiveStudy::id);
    return found == active.end() ? nullptr : &*found;
}

bool StudyDriver::State::owns(const Study& study) const {
    const std::vector<std::string> members = node.cluster_members();
    if (members.empty()) return false;
    const std::string& owner =
        members[stable_hash(study.id()) % members.size()];
    return owner == node.node_id();
}

bool StudyDriver::State::finish_for_budget(
    const std::shared_ptr<Study>& study, const StudyValue& snapshot) {
    std::string reason;
    if (wall_time_reached(snapshot)) reason = "wall_time";
    const bool has_running = std::ranges::any_of(
        snapshot.trials, [](const StudyTrial& trial) {
            return trial.state == TrialState::Running;
        });
    if (reason.empty() && !has_running &&
        !trial_cost_fits(snapshot)) {
        reason = "budget";
    }
    if (reason.empty()) return false;
    node.run_on_owner([&] {
        if (study->status() != StudyStatus::Running) return;
        std::vector<std::string> running;
        for (const StudyTrial& trial : study->value().trials) {
            if (trial.state == TrialState::Running) {
                running.push_back(trial.run);
            }
        }
        for (const std::string& run_id : running) {
            study->stop_trial(run_id, "study budget reached: " + reason);
            if (const std::shared_ptr<pipeline::Run> run =
                    node.find_run(run_id)) {
                run->set_status(pipeline::RunStatus::Failed);
            }
            publish_progress(node, *study, "trial_failed", run_id);
        }
        study->set_complete(reason);
        publish_progress(node, *study, "study_complete");
    });
    return true;
}

std::vector<StudyTrial> StudyDriver::State::prepare_batch(
    const std::shared_ptr<Study>& study, const StudyValue& snapshot) {
    std::vector<StudyTrial> pending;
    for (const StudyTrial& trial : snapshot.trials) {
        if (trial.state == TrialState::Running) pending.push_back(trial);
    }
    const std::size_t remaining =
        snapshot.spec.budgets.max_trials > snapshot.trials.size()
            ? snapshot.spec.budgets.max_trials - snapshot.trials.size()
            : 0;
    const bool has_completed = std::ranges::any_of(
        snapshot.trials, [](const StudyTrial& trial) {
            return trial.state == TrialState::Complete;
        });
    const std::size_t limit = std::min<std::size_t>(
        snapshot.spec.budgets.max_parallel_trials,
        has_completed ? snapshot.spec.budgets.max_parallel_trials : 2);
    const std::size_t available =
        pending.size() < limit ? limit - pending.size() : 0;
    const std::size_t desired = std::min(remaining, available);
    std::size_t add = 0;
    while (add < desired &&
           trial_cost_fits(snapshot, pending.size() + add)) {
        ++add;
    }
    node.run_on_owner([&] {
        for (std::size_t offset = 0; offset < add; ++offset) {
            const std::size_t number = study->value().trials.size();
            const nlohmann::json params = StudyOptimizer::sample(
                study->spec(), study->value().trials, number);
            const std::shared_ptr<pipeline::Run> run =
                node.create_run(study->spec().project,
                                study->spec().pipeline);
            if (!run) {
                study->stop("could not create a trial Run");
                break;
            }
            run->set_status(pipeline::RunStatus::Running);
            StudyTrial trial;
            trial.number = number;
            trial.run = run->id();
            trial.params = params;
            trial.seed = study->spec().seed + number;
            nlohmann::json run_params = params;
            run_params["study.id"] = study->id();
            run_params["study.seed"] = trial.seed;
            std::string error;
            if (!run->log_params(std::move(run_params), &error)) {
                study->stop(error);
                break;
            }
            if (study->spec().differential_privacy) {
                run->suppress_metrics_for_privacy();
            }
            if (!study->add_trial(trial)) break;
            pending.push_back(std::move(trial));
            publish_progress(node, *study, "trial_started", run->id());
        }
    });
    return pending;
}

StudyDriver::State::TrialResult StudyDriver::State::execute_trial(
    const std::shared_ptr<Study>& study, const StudyTrial& trial) {
    TrialResult result;
    result.run = trial.run;
    StudySpec spec;
    std::string study_id;
    node.run_on_owner([&] {
        spec = study->spec();
        study_id = study->id();
    });
    const auto trial_cancel = std::make_shared<std::atomic<bool>>(false);
    node.run_on_owner([&] {
        if (ActiveStudy* found = find_active(study_id)) {
            found->trial_cancels[trial.run] = trial_cancel;
        }
    });
    const auto started = std::chrono::steady_clock::now();
    bool wall_budget_reached = false;
    std::optional<RealTrialPlan> plan;
    node.run_on_owner([&] {
        const std::vector<std::shared_ptr<Model>> models = node.models();
        for (const std::shared_ptr<Model>& model : models) {
            if (model && (model->run() == trial.run ||
                          model->run_id() == trial.run)) {
                node.drop_model(model->id());
            }
        }
        if (const std::shared_ptr<pipeline::Run> run =
                node.find_run(trial.run)) {
            run->reset_metrics_for_retry();
            run->set_status(pipeline::RunStatus::Running);
        }
        study->reset_trial_for_retry(trial.run);
    });
    plan = prepare_real_trial(node, spec, trial.params, trial.run,
                              trial_cancel.get(), result.error);
    if (plan) {
        plan->options.training_progress =
            [&](int step, const std::map<std::string, double>& values) {
                if (trial_cancel->load(std::memory_order_acquire)) return false;
                StudyValue current_value;
                node.run_on_owner([&] { current_value = study->value(); });
                if (wall_time_reached(current_value)) {
                    wall_budget_reached = true;
                    trial_cancel->store(true, std::memory_order_release);
                    return false;
                }
                const auto loss = values.find("train.log_loss");
                if (loss == values.end()) return true;
                const double value = spec.objective.direction ==
                                             StudyDirection::Maximize
                                         ? 1.0 / (1.0 + loss->second)
                                         : loss->second;
                bool keep_running = true;
                node.run_on_owner([&] {
                    std::string report_error;
                    keep_running = study->report_intermediate(
                        trial.run, static_cast<std::uint64_t>(step), value,
                        &report_error);
                    if (!keep_running) return;
                    publish_progress(node, *study, "trial_progress", trial.run,
                                     static_cast<std::uint64_t>(step), value);
                    const auto current = std::ranges::find(
                        study->value().trials, trial.run, &StudyTrial::run);
                    if (current != study->value().trials.end() &&
                        StudyOptimizer::should_prune(
                            spec, study->value().trials, *current)) {
                        result.pruned = true;
                        keep_running = false;
                    }
                });
                return keep_running;
            };
        compute::CpuDevice device;
        if (plan->dataset_level) {
            DatasetTrialResult run =
                run_dataset_trial(node, *plan, trial.run);
            result.model = std::move(run.model);
            result.error = std::move(run.error);
        } else {
            const usecases::churn::ChurnSolutionResult run =
                usecases::churn::run_churn_solution_on_table(
                    node, device, plan->graph, plan->table,
                    plan->options);
            result.model = run.model_id;
            if (!run.ok()) result.error = run.execution.error;
        }
        if (wall_budget_reached) {
            result.error = "study wall_time budget reached";
        }
        if (result.error.empty() && !result.model.empty()) {
            node.run_on_owner([&] {
                result.objective = trial_objective(
                    node, spec, result.model, result.error);
            });
        }
    }
    result.cost = std::max(
        0.001,
        std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                      started)
            .count());
    node.run_on_owner([&] {
        if (ActiveStudy* found = find_active(study_id)) {
            found->trial_cancels.erase(trial.run);
        }
    });
    return result;
}

void StudyDriver::State::apply_results(
    const std::shared_ptr<Study>& study,
    const std::vector<TrialResult>& results) {
    node.run_on_owner([&] {
        for (const TrialResult& result : results) {
            const auto found = std::ranges::find(
                study->value().trials, result.run, &StudyTrial::run);
            if (found == study->value().trials.end() ||
                found->state != TrialState::Running) {
                continue;
            }
            if (result.pruned) {
                study->finish_trial(
                    result.run, TrialState::Pruned, std::nullopt, result.cost,
                    "pruned by " + std::string(to_string(study->spec().pruner)),
                    result.model);
                if (const std::shared_ptr<pipeline::Run> run =
                        node.find_run(result.run)) {
                    run->set_status(pipeline::RunStatus::Failed);
                }
                publish_progress(node, *study, "trial_pruned", result.run);
                continue;
            }
            if (!result.objective) {
                study->finish_trial(result.run, TrialState::Failed,
                                    std::nullopt, result.cost, result.error,
                                    result.model);
                if (const std::shared_ptr<pipeline::Run> run =
                        node.find_run(result.run)) {
                    run->set_status(pipeline::RunStatus::Failed);
                }
                publish_progress(node, *study, "trial_failed", result.run);
                continue;
            }
            if (found->intermediate.empty()) {
                std::string params_error;
                const std::optional<GbdtParams> params =
                    study_gbdt_params(found->params, params_error);
                study->report_intermediate(
                    result.run,
                    static_cast<std::uint64_t>(
                        params ? std::max(1, params->num_trees) : 1),
                    *result.objective);
            }
            study->finish_trial(result.run, TrialState::Complete,
                                result.objective, result.cost, {},
                                result.model);
            if (const std::shared_ptr<pipeline::Run> run =
                    node.find_run(result.run)) {
                run->set_status(pipeline::RunStatus::Succeeded);
            }
            publish_progress(node, *study, "trial_completed", result.run);
        }
        const std::vector<StudyTrial>& trials = study->value().trials;
        if (trials.size() < 2 ||
            trials.back().state != TrialState::Failed ||
            !deterministic_data_error(trials.back().reason)) {
            return;
        }
        const StudyTrial& previous = trials[trials.size() - 2];
        if (previous.state == TrialState::Failed &&
            previous.reason == trials.back().reason) {
            study->fail("data", trials.back().reason);
            publish_progress(node, *study, "study_failed");
        }
    });
}

void StudyDriver::State::drive(
    const std::string& study_id,
    const std::shared_ptr<std::atomic<bool>>& cancelled,
    const std::shared_ptr<std::atomic<bool>>& done) {
    const auto mark_done = [&] {
        done->store(true, std::memory_order_release);
    };
    std::shared_ptr<Study> study;
    node.run_on_owner([&] { study = node.find_study(study_id); });
    if (!study) {
        mark_done();
        return;
    }
    StudySpec initial_spec;
    node.run_on_owner([&] { initial_spec = study->spec(); });
    std::string data_error;
    if (!validate_study_data(node, initial_spec, &data_error)) {
        node.run_on_owner([&] {
            if (study->status() != StudyStatus::Running) return;
            study->fail("data", data_error);
            publish_progress(node, *study, "study_failed");
        });
        mark_done();
        return;
    }
    while (!cancelled->load(std::memory_order_acquire)) {
        StudyValue snapshot;
        node.run_on_owner([&] { snapshot = study->value(); });
        if (snapshot.status != StudyStatus::Running) break;
        if (!owns(*study)) break;
        if (finish_for_budget(study, snapshot)) break;
        const std::vector<StudyTrial> pending =
            prepare_batch(study, snapshot);
        if (pending.empty()) {
            node.run_on_owner([&] {
                if (study->status() != StudyStatus::Running) return;
                study->set_complete("max_trials");
                publish_progress(node, *study, "study_complete");
            });
            break;
        }
        const std::vector<TrialResult> results = trial_scheduler.parallel_map(
            pending, [&](const StudyTrial& trial) {
                return execute_trial(study, trial);
            });
        if (cancelled->load(std::memory_order_acquire)) break;
        apply_results(study, results);
    }
    mark_done();
}

void StudyDriver::State::launch(const std::shared_ptr<Study>& study) {
    ActiveStudy entry;
    entry.id = study->id();
    entry.cancel = std::make_shared<std::atomic<bool>>(false);
    entry.done = std::make_shared<std::atomic<bool>>(false);
    const std::string study_id = entry.id;
    const std::shared_ptr<std::atomic<bool>> cancelled = entry.cancel;
    const std::shared_ptr<std::atomic<bool>> done = entry.done;
    active.push_back(std::move(entry));
    driver_scheduler.submit(
        [this, study_id, cancelled, done] {
            drive(study_id, cancelled, done);
        });
}

StudyDriver::StudyDriver(Node& node)
    : state_(std::make_unique<State>(node)) {}

StudyDriver::~StudyDriver() {
    stop();
}

void StudyDriver::reconcile() {
    if (!state_ || state_->stopping || state_->node.job_coordinator() == nullptr) {
        return;
    }
    std::erase_if(state_->active, [](const State::ActiveStudy& active) {
        return active.done->load(std::memory_order_acquire);
    });
    for (State::ActiveStudy& active : state_->active) {
        const std::shared_ptr<Study> study =
            state_->node.find_study(active.id);
        if (!study || study->status() != StudyStatus::Running ||
            state_->owns(*study)) {
            continue;
        }
        active.cancel->store(true, std::memory_order_release);
        for (const auto& [_, cancel] : active.trial_cancels) {
            cancel->store(true, std::memory_order_release);
        }
    }
    for (const std::shared_ptr<Study>& study : state_->node.studies()) {
        if (!study || study->status() != StudyStatus::Running) continue;
        if (!state_->owns(*study)) continue;
        if (state_->find_active(study->id()) != nullptr) continue;
        state_->launch(study);
    }
}

void StudyDriver::request_stop(const std::string& study_id,
                               const std::string& run_id) {
    if (!state_) return;
    const auto found = std::ranges::find(state_->active, study_id,
                                          &State::ActiveStudy::id);
    if (found == state_->active.end()) return;
    if (run_id.empty()) {
        found->cancel->store(true, std::memory_order_release);
        for (const auto& [_, cancel] : found->trial_cancels) {
            cancel->store(true, std::memory_order_release);
        }
        return;
    }
    const auto trial = found->trial_cancels.find(run_id);
    if (trial != found->trial_cancels.end()) {
        trial->second->store(true, std::memory_order_release);
    }
}

void StudyDriver::stop() {
    if (!state_ || state_->stopping) return;
    state_->stopping = true;
    for (State::ActiveStudy& active : state_->active) {
        active.cancel->store(true, std::memory_order_release);
        for (const auto& [_, cancel] : active.trial_cancels) {
            cancel->store(true, std::memory_order_release);
        }
    }
    while (std::ranges::any_of(
        state_->active, [](const State::ActiveStudy& active) {
            return !active.done->load(std::memory_order_acquire);
        })) {
        state_->node.drain_owner_work();
        std::this_thread::yield();
    }
}

}  // namespace zetta::ml
