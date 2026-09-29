#include "zetta/ml/study.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <map>
#include <mutex>
#include <numeric>
#include <random>
#include <set>
#include <utility>

#include <nlohmann/json-schema.hpp>

#include "zetta/cluster/replication.hpp"
#include "zetta/compute/work_stealing_scheduler.hpp"
#include "zetta/node.hpp"
#include "zetta/pipeline/run.hpp"
#include "zetta/pubsub/pubsub.hpp"
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
        "status":{"type":"string","enum":["running","stopped","complete"]},
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

}  // namespace

std::string_view to_string(StudyDirection value) {
    return value == StudyDirection::Minimize ? "minimize" : "maximize";
}

std::string_view to_string(StudyStatus value) {
    switch (value) {
        case StudyStatus::Running: return "running";
        case StudyStatus::Stopped: return "stopped";
        case StudyStatus::Complete: return "complete";
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
        if (study.spec().budgets.cost_cap > 0.0 &&
            study.value().spent_cost >= study.spec().budgets.cost_cap) {
            study.stop("cost cap reached");
            break;
        }
        const std::size_t remaining = study.spec().budgets.max_trials -
                                      study.value().trials.size();
        const std::size_t parallel_limit =
            study.spec().budgets.cost_cap > 0.0
                ? 1
                : study.spec().budgets.max_parallel_trials;
        const std::size_t batch_size = std::min(remaining, parallel_limit);
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

}  // namespace zetta::ml
