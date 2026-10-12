#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "zetta/pipeline/run.hpp"

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
enum class StudyStatus { Running, Stopped, Complete, Failed };
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
    // A workspace methodology's study (lake#689): the target column and the
    // feature columns its trials read. Empty for the catalogue's studies,
    // which resolve both from the dataset contract.
    std::string target;
    std::vector<std::string> features;
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
    std::string failure_reason;
    std::string promoted_model;
    // The tables the study's trials read (zetta#1258), recorded at creation:
    // delete_dataset refuses them while the study runs.
    std::vector<pipeline::TableRead> reads;
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

// Validate the resolved M-1 dataset and its requested validation split before a
// Study spends a trial. The check reads the complete table; the bounded sample is
// used only to infer the column contract. Returns a repair-oriented data reason.
bool validate_study_data(Node& node, const StudySpec& spec,
                         std::string* error = nullptr);

// The name a trial's Model takes: `<study name>-trial-<n>`, the name lowercased
// with other characters folded to `-` (#1242).
std::string study_trial_model_name(const std::string& experiment_name,
                                   const std::string& experiment_id, std::size_t trial_number);

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
    // Appends a running trial's intermediate value. The value changes in
    // memory at once; its replicated capture is coalesced to at most one per
    // kCaptureInterval (#1217), written by the next capture or flush_capture.
    bool report_intermediate(const std::string& run, std::uint64_t step,
                             double value, std::string* error = nullptr);
    static constexpr std::chrono::milliseconds kCaptureInterval{1000};
    bool capture_pending() const { return capture_pending_; }
    // Write a pending capture when its interval has passed (or `force`).
    void flush_capture(bool force = false);
    bool finish_trial(const std::string& run, TrialState state,
                      std::optional<double> objective, double cost,
                      std::string reason = {}, std::string model = {},
                      std::string* error = nullptr);
    bool stop_trial(const std::string& run, std::string reason,
                    std::string* error = nullptr);
    // Preserve immutable parameters while clearing partial retry outputs.
    bool reset_trial_for_retry(const std::string& run,
                               std::string* error = nullptr);
    void stop(std::string reason);
    // Ends the study for `reason`. A study whose trials all ended without a
    // result (at least one failed, none complete) is not complete: it ends
    // failed, with the last failed trial's reason as its failure_reason
    // (#1217), so a run of failures never reads as success. Returns the
    // resulting status.
    StudyStatus set_complete(std::string reason);
    void fail(std::string stop_reason, std::string failure_reason);
    void set_promoted_model(std::string model);
    void set_project_pipeline(std::string project, std::string pipeline);
    void apply_value(StudyValue value);
    nlohmann::json metadata() const;
    void persist_meta() const;

private:
    StudyTrial* find_trial(const std::string& run);
    void capture_and_persist();
    bool capture_pending_ = false;
    std::chrono::steady_clock::time_point last_capture_{};

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

// Drives replicated running studies on their deterministic owner node. The
// driver itself and its trial batches execute on work-stealing schedulers;
// Node state changes are handed back through Node::run_on_owner.
class StudyDriver {
public:
    explicit StudyDriver(Node& node);
    ~StudyDriver();

    StudyDriver(const StudyDriver&) = delete;
    StudyDriver& operator=(const StudyDriver&) = delete;

    void reconcile();
    void request_stop(const std::string& study_id,
                      const std::string& run_id = {});
    void stop();

private:
    struct State;
    std::unique_ptr<State> state_;
};

}  // namespace zetta::ml
