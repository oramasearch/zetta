#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace zetta {
class Node;
}

namespace zetta::cluster {
class ReplicationManager;
}

namespace zetta::compute {
class WorkStealingScheduler;
}

namespace zetta::ml {

enum class StudyDirection { Minimize, Maximize };
enum class StudyStatus { Running, Stopped, Complete };
enum class TrialState { Running, Pruned, Failed, Complete };
enum class SamplerKind { Grid, Random, Tpe };
enum class PrunerKind { None, Median, Asha };
enum class ParameterKind { Integer, Float, Categorical };

std::string_view to_string(StudyDirection value);
std::string_view to_string(StudyStatus value);
std::string_view to_string(TrialState value);
std::string_view to_string(SamplerKind value);
std::string_view to_string(PrunerKind value);
std::string_view to_string(ParameterKind value);

struct StudyObjective {
    std::string metric;
    StudyDirection direction = StudyDirection::Maximize;
    std::string validation_split;
};

struct StudyParameter {
    std::string name;
    ParameterKind kind = ParameterKind::Float;
    double minimum = 0.0;
    double maximum = 1.0;
    double step = 0.0;
    bool logarithmic = false;
    std::vector<nlohmann::json> choices;
    std::string condition_parameter;
    std::vector<nlohmann::json> condition_values;
};

struct StudyBudgets {
    std::size_t max_trials = 1;
    std::uint64_t max_wall_time_ms = 0;
    std::size_t max_parallel_trials = 1;
    double cost_cap = 0.0;
};

struct StudySpec {
    std::string name;
    std::string project;
    std::string pipeline;
    std::string dataset;
    std::string methodology;
    StudyObjective objective;
    std::vector<StudyParameter> search_space;
    SamplerKind sampler = SamplerKind::Tpe;
    PrunerKind pruner = PrunerKind::Asha;
    StudyBudgets budgets;
    std::uint64_t seed = 0;
    bool differential_privacy = false;
    std::vector<std::string> released_metrics;
};

struct TrialPoint {
    std::uint64_t step = 0;
    double value = 0.0;
};

struct StudyTrial {
    std::size_t number = 0;
    std::string run;
    nlohmann::json params = nlohmann::json::object();
    TrialState state = TrialState::Running;
    std::optional<double> objective;
    std::uint64_t seed = 0;
    double cost = 0.0;
    std::vector<TrialPoint> intermediate;
    std::string reason;
    std::string model;
};

struct StudyValue {
    StudySpec spec;
    StudyStatus status = StudyStatus::Running;
    std::string created;
    std::string updated;
    std::vector<StudyTrial> trials;
    double spent_cost = 0.0;
    std::string stop_reason;
    std::string promoted_model;
};

struct StudyDeleteResult {
    std::string id;
    std::string name;
    std::vector<std::string> removed_runs;
    std::string kept_model;
    std::string kept_run;
};

nlohmann::json to_json(const StudyValue& value);
const nlohmann::json& study_value_schema();
bool validate_study_value(const nlohmann::json& value,
                          std::string* error = nullptr);
std::optional<StudyValue> parse_study_value(const nlohmann::json& value,
                                            std::string* error = nullptr);
bool validate_study_spec(const StudySpec& spec, std::string* error = nullptr);

class Study {
public:
    Study(std::string id, StudyValue value, std::filesystem::path dir,
          zetta::cluster::ReplicationManager* replication = nullptr);

    const std::string& id() const { return id_; }
    const StudyValue& value() const { return value_; }
    const StudySpec& spec() const { return value_.spec; }
    StudyStatus status() const { return value_.status; }
    const std::filesystem::path& dir() const { return dir_; }

    bool add_trial(StudyTrial trial, std::string* error = nullptr);
    bool report_intermediate(const std::string& run, std::uint64_t step,
                             double value, std::string* error = nullptr);
    bool finish_trial(const std::string& run, TrialState state,
                      std::optional<double> objective, double cost,
                      std::string reason = {}, std::string model = {},
                      std::string* error = nullptr);
    bool stop_trial(const std::string& run, std::string reason,
                    std::string* error = nullptr);
    void stop(std::string reason);
    void set_complete(std::string reason);
    void set_promoted_model(std::string model);
    void apply_value(StudyValue value);
    nlohmann::json metadata() const;
    void persist_meta() const;

private:
    StudyTrial* find_trial(const std::string& run);
    void capture_and_persist();

    std::string id_;
    StudyValue value_;
    std::filesystem::path dir_;
    zetta::cluster::ReplicationManager* replication_;
};

class StudyOptimizer {
public:
    static nlohmann::json sample(const StudySpec& spec,
                                 const std::vector<StudyTrial>& trials,
                                 std::size_t trial_number);
    static bool should_prune(const StudySpec& spec,
                             const std::vector<StudyTrial>& trials,
                             const StudyTrial& trial);
    static nlohmann::json analysis(const StudyValue& value);
};

struct TrialOutcome {
    bool complete = true;
    bool pruned = false;
    std::optional<double> objective;
    double cost = 0.0;
    std::string reason;
    std::string model;
};

using TrialReporter = std::function<bool(std::uint64_t step, double value)>;
using TrialEvaluator = std::function<TrialOutcome(
    const nlohmann::json& params, std::uint64_t seed,
    const std::string& run, const TrialReporter& report,
    const std::atomic<bool>& stop_requested)>;

struct StudyRunResult {
    std::size_t launched = 0;
    std::size_t completed = 0;
    std::size_t pruned = 0;
    std::size_t failed = 0;
    std::string stop_reason;
};

StudyRunResult run_study(Node& node, Study& study,
                         compute::WorkStealingScheduler& scheduler,
                         const TrialEvaluator& evaluate);

}  // namespace zetta::ml
